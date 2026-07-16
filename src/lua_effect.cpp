#include "lua_effect.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

#include <lua.hpp>

#include "effect.h"
#include "registry.h"

namespace luafx {

namespace {

const char* kFolder = "effects";

// wraps the ffi cast and param table so user scripts stay clean. `process` may be
// jit-compiled; the wrapper is the only per-block lua entry point the host calls.
const char* kShim = R"(
local ffi = require('ffi')
local user = process
local p = __params
__process = function(ptr, n) user(ffi.cast('float*', ptr), n, p) end
)";

// one lua_State per instance: two copies of the same script never share state.
// the state is created and prepared on the ui thread; after the stream starts
// only the audio thread touches it. gc is stopped at load and stepped manually
// once per block so collection never pauses long enough to drop a buffer.
class LuaEffect : public Effect {
public:
  explicit LuaEffect(const std::string& path) : path_(path) {
    // fallback display name if the script breaks after it was registered.
    size_t slash = path.find_last_of("\\/");
    size_t dot = path.rfind('.');
    name_ = path.substr(slash + 1, dot - slash - 1);
    load();
  }

  ~LuaEffect() override {
    if (L_) lua_close(L_);
  }

  bool failedToLoad() const { return !loadError_.empty(); }
  const std::string& loadError() const { return loadError_; }

  void prepare(double sampleRate, int maxBlock) override {
    if (!L_ || failed_) return;
    lua_getglobal(L_, "prepare");
    if (!lua_isfunction(L_, -1)) {
      lua_pop(L_, 1);
      return;
    }
    lua_pushnumber(L_, sampleRate);
    lua_pushinteger(L_, maxBlock);
    if (lua_pcall(L_, 2, 0, 0) != 0) fail("prepare");
  }

  void process(float* buffer, int numFrames) override {
    if (failed_) return;  // script errored: act as a passthrough

    // publish the ui-side knob values into the (preallocated) param table.
    // string keys are interned at load time, so setfield never allocates here.
    lua_rawgeti(L_, LUA_REGISTRYINDEX, paramsRef_);
    for (auto& s : slots_) {
      float v = s->value.load(std::memory_order_relaxed);
      if (s->kind != Param::Float) v = std::round(v);
      lua_pushnumber(L_, v);
      lua_setfield(L_, -2, s->name.c_str());
    }
    lua_pop(L_, 1);

    lua_rawgeti(L_, LUA_REGISTRYINDEX, processRef_);
    lua_pushlightuserdata(L_, buffer);
    lua_pushinteger(L_, numFrames);
    if (lua_pcall(L_, 2, 0, 0) != 0) {
      fail("process");
      return;
    }
    lua_gc(L_, LUA_GCSTEP, 1);
  }

  const char* name() const override { return name_.c_str(); }

private:
  // a knob backed by the atomic the ui writes; heap-allocated so the pointer
  // handed to Effect::param() stays stable while the vector grows.
  struct Slot {
    std::string name;
    std::atomic<float> value;
    Param::Kind kind;
  };

  void fail(const char* stage) {
    failed_ = true;
    std::fprintf(stderr, "[lua] %s: %s() error: %s\n", path_.c_str(), stage,
                 lua_tostring(L_, -1));
    lua_pop(L_, 1);
  }

  void loadFail(const std::string& msg) {
    loadError_ = msg;
    failed_ = true;
  }

  bool readParams() {
    lua_getglobal(L_, "params");
    if (lua_isnil(L_, -1)) {
      lua_pop(L_, 1);
      return true;  // a script with no knobs is fine
    }
    if (!lua_istable(L_, -1)) {
      lua_pop(L_, 1);
      loadFail("'params' must be a table");
      return false;
    }
    int count = (int)lua_objlen(L_, -1);
    for (int i = 1; i <= count; i++) {
      lua_rawgeti(L_, -1, i);
      if (!lua_istable(L_, -1)) {
        lua_pop(L_, 2);
        loadFail("params[" + std::to_string(i) + "] must be a table");
        return false;
      }
      auto num = [&](const char* key, float def) {
        lua_getfield(L_, -1, key);
        float v = lua_isnumber(L_, -1) ? (float)lua_tonumber(L_, -1) : def;
        lua_pop(L_, 1);
        return v;
      };

      lua_getfield(L_, -1, "name");
      if (!lua_isstring(L_, -1)) {
        lua_pop(L_, 3);
        loadFail("params[" + std::to_string(i) + "] needs a 'name' string");
        return false;
      }
      auto slot = std::make_unique<Slot>();
      slot->name = lua_tostring(L_, -1);
      lua_pop(L_, 1);

      slot->kind = Param::Float;
      lua_getfield(L_, -1, "kind");
      if (lua_isstring(L_, -1)) {
        std::string k = lua_tostring(L_, -1);
        if (k == "int") slot->kind = Param::Int;
        if (k == "bool") slot->kind = Param::Bool;
      }
      lua_pop(L_, 1);

      float mn = num("min", 0.0f);
      float mx = num("max", 1.0f);
      if (slot->kind == Param::Bool) {
        mn = 0.0f;
        mx = 1.0f;
      }
      if (mn >= mx) {
        lua_pop(L_, 2);
        loadFail("param '" + slot->name + "': min must be < max");
        return false;
      }
      float def = num("default", mn);
      if (def < mn) def = mn;
      if (def > mx) def = mx;
      slot->value.store(def, std::memory_order_relaxed);

      param(slot->name.c_str(), &slot->value, mn, mx, slot->kind);
      slots_.push_back(std::move(slot));
      lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return true;
  }

  void load() {
    L_ = luaL_newstate();
    if (!L_) {
      loadFail("could not create lua state");
      return;
    }
    luaL_openlibs(L_);

    if (luaL_dofile(L_, path_.c_str()) != 0) {
      loadFail(lua_tostring(L_, -1));
      lua_pop(L_, 1);
      return;
    }

    lua_getglobal(L_, "name");
    if (!lua_isstring(L_, -1)) {
      lua_pop(L_, 1);
      loadFail("script must set a global 'name' string");
      return;
    }
    name_ = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    lua_getglobal(L_, "process");
    bool hasProcess = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    if (!hasProcess) {
      loadFail("script must define a global process(buf, n, p) function");
      return;
    }

    if (!readParams()) return;

    // the table the shim hands to process(); fields refreshed every block.
    lua_newtable(L_);
    for (auto& s : slots_) {
      lua_pushnumber(L_, s->value.load(std::memory_order_relaxed));
      lua_setfield(L_, -2, s->name.c_str());
    }
    lua_pushvalue(L_, -1);
    paramsRef_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    lua_setglobal(L_, "__params");

    if (luaL_dostring(L_, kShim) != 0) {
      loadFail(lua_tostring(L_, -1));
      lua_pop(L_, 1);
      return;
    }
    lua_getglobal(L_, "__process");
    processRef_ = luaL_ref(L_, LUA_REGISTRYINDEX);

    lua_gc(L_, LUA_GCSTOP, 0);
  }

  std::string path_;
  std::string name_;
  std::string loadError_;
  lua_State* L_ = nullptr;
  bool failed_ = false;
  int paramsRef_ = LUA_NOREF;
  int processRef_ = LUA_NOREF;
  std::vector<std::unique_ptr<Slot>> slots_;
};

}  // namespace

std::string dir() {
  ::CreateDirectoryA(kFolder, nullptr);  // no-op if it already exists
  return kFolder;
}

std::vector<std::string> registerAll() {
  // names present before any script registered; scripts may not shadow these.
  static const std::set<std::string> builtins = [] {
    auto v = Registry::names();
    return std::set<std::string>(v.begin(), v.end());
  }();

  std::vector<std::string> errors;
  std::set<std::string> seen;  // names claimed earlier in this same scan
  std::string pattern = dir() + "\\*.lua";
  WIN32_FIND_DATAA fd;
  HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return errors;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string path = std::string(kFolder) + "\\" + fd.cFileName;

    // load once now to validate the script and learn its registry name.
    LuaEffect probe(path);
    if (probe.failedToLoad()) {
      errors.push_back(path + ": " + probe.loadError());
      continue;
    }
    std::string name = probe.name();
    if (builtins.count(name)) {
      errors.push_back(path + ": name '" + name + "' is taken by a built-in effect");
      continue;
    }
    if (!seen.insert(name).second) {
      errors.push_back(path + ": name '" + name + "' is already used by another script");
      continue;
    }
    Registry::add(name, [path] { return std::make_unique<LuaEffect>(path); });
  } while (::FindNextFileA(h, &fd));
  ::FindClose(h);
  return errors;
}

}  // namespace luafx
