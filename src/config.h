#pragma once

#include <string>

// persisted app settings. devices are stored by name because rtaudio device
// ids are not stable across reboots / device changes.
struct Config {
  std::string inputDevice;
  std::string outputDevice;
  int bufferFrames = 256;
  bool showConsole = false;
};

namespace config {

Config load();  // returns defaults if the file is missing or unreadable
bool save(const Config& c);

}  // namespace config
