#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Effect.h"

using EffectFactory = std::function<std::unique_ptr<Effect>()>;

class Registry {
public:
  static bool add(const std::string& name, EffectFactory factory);
  static std::unique_ptr<Effect> create(const std::string& name);
  static std::vector<std::string> names();

private:
  static std::map<std::string, EffectFactory>& table();
};

// one line at the bottom of an effect .cpp registers it under a name.
#define REGISTER_EFFECT(Type, Name) \
  static const bool _registered_##Type = \
    Registry::add(Name, [] { return std::make_unique<Type>(); });
