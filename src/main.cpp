#include <iostream>

#include "audio.h"
#include "gui.h"

int main() {
  AudioEngine engine;
  if (!engine.hasDevices()) {
    std::cerr << "no audio devices found\n";
    return 1;
  }
  return gui::run(engine);
}
