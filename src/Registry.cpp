#include "registry.h"

// function-local static avoids static init order problems: the map is built
// on first REGISTER_EFFECT call, whatever order the effect files init in.
std::map<std::string, EffectFactory>& Registry::table() {
  static std::map<std::string, EffectFactory> t;
  return t;
}

bool Registry::add(const std::string& name, EffectFactory factory) {
  table()[name] = std::move(factory);
  return true;
}

std::unique_ptr<Effect> Registry::create(const std::string& name) {
  auto it = table().find(name);
  return it == table().end() ? nullptr : it->second();
}

std::vector<std::string> Registry::names() {
  std::vector<std::string> out;
  for (auto& kv : table()) out.push_back(kv.first);
  return out;
}
