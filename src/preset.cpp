#include "preset.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "effect.h"
#include "graph.h"

namespace preset {

namespace {

const char* kFolder = "presets";
const char* kExt = ".preset";
const char* kMagic = "guitarpedal-preset";

std::string path(const std::string& name) { return dir() + "\\" + name + kExt; }

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r' && c != '\n') {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

// invalid characters for a filename; the ui also blocks these but guard anyway.
bool validName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  return name.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

}  // namespace

std::string dir() {
  ::CreateDirectoryA(kFolder, nullptr);  // no-op if it already exists
  return kFolder;
}

std::vector<std::string> list() {
  std::vector<std::string> out;
  std::string pattern = dir() + "\\*" + kExt;
  WIN32_FIND_DATAA fd;
  HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string file = fd.cFileName;
    size_t dot = file.rfind(kExt);
    if (dot != std::string::npos) out.push_back(file.substr(0, dot));
  } while (::FindNextFileA(h, &fd));
  ::FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

bool exists(const std::string& name) {
  return ::GetFileAttributesA(path(name).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool save(const std::string& name, const Graph& g) {
  if (!validName(name)) return false;
  std::ofstream f(path(name));
  if (!f) return false;

  std::map<int, int> idx;  // node id -> save index
  int i = 0;
  for (auto& n : g.nodes()) idx[n->id] = i++;

  f << kMagic << "|1\n";
  for (auto& n : g.nodes()) {
    bool byp = n->bypass.load(std::memory_order_relaxed);
    f << "node|" << idx[n->id] << "|" << n->type << "|" << n->x << "|" << n->y << "|"
      << (byp ? 1 : 0) << "\n";
  }
  for (auto& n : g.nodes()) {
    if (!n->fx) continue;
    for (auto& p : n->fx->params())
      f << "param|" << idx[n->id] << "|" << p.name << "|"
        << p.value->load(std::memory_order_relaxed) << "\n";
  }
  for (auto& w : g.wires()) f << "wire|" << idx[w.from] << "|" << idx[w.to] << "\n";
  return true;
}

bool load(const std::string& name, Graph& g) {
  std::ifstream f(path(name));
  if (!f) return false;

  std::string header;
  std::getline(f, header);
  if (split(header)[0] != kMagic) return false;

  g.clear();
  std::map<int, int> saveToId;  // save index -> live node id

  std::string line;
  while (std::getline(f, line)) {
    auto t = split(line);
    if (t[0] == "node" && t.size() >= 6) {
      int si = std::atoi(t[1].c_str());
      const std::string& type = t[2];
      float x = (float)std::atof(t[3].c_str());
      float y = (float)std::atof(t[4].c_str());
      bool byp = t[5] == "1";
      GraphNode* n = nullptr;
      if (type == "in")
        n = g.node(Graph::kInput);
      else if (type == "out")
        n = g.node(Graph::kOutput);
      else
        n = g.addEffect(type, x, y);
      if (!n) continue;
      n->x = x;
      n->y = y;
      n->bypass.store(byp, std::memory_order_relaxed);
      saveToId[si] = n->id;
    } else if (t[0] == "param" && t.size() >= 4) {
      auto it = saveToId.find(std::atoi(t[1].c_str()));
      if (it == saveToId.end()) continue;
      GraphNode* n = g.node(it->second);
      if (!n || !n->fx) continue;
      for (auto& p : n->fx->params())
        if (t[2] == p.name) p.value->store((float)std::atof(t[3].c_str()));
    } else if (t[0] == "wire" && t.size() >= 3) {
      auto a = saveToId.find(std::atoi(t[1].c_str()));
      auto b = saveToId.find(std::atoi(t[2].c_str()));
      if (a != saveToId.end() && b != saveToId.end()) g.addWire(a->second, b->second);
    }
  }
  return true;
}

bool remove(const std::string& name) {
  if (!validName(name)) return false;
  return ::DeleteFileA(path(name).c_str()) != 0;
}

}  // namespace preset
