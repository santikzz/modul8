#include <iostream>

#include "audio.h"
#include "gui.h"
#include "lua_effect.h"

int main() {
  AudioEngine engine;
  if (!engine.hasDevices()) {
    std::cerr << "no audio devices found\n";
    return 1;
  }
  for (const auto& err : luafx::registerAll()) std::cerr << "[lua] " << err << "\n";
  return gui::run(engine);
}
