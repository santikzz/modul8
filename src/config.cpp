#include "config.h"

#include <fstream>
#include <string>

// plain key=value file next to the working dir. simple on purpose.
static const char* kPath = "modul8.cfg";

Config config::load() {
  Config c;
  std::ifstream f(kPath);
  if (!f) return c;

  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    if (!val.empty() && val.back() == '\r') val.pop_back();  // tolerate crlf

    if (key == "input") {
      c.inputDevice = val;
    } else if (key == "output") {
      c.outputDevice = val;
    } else if (key == "buffer") {
      try {
        c.bufferFrames = std::stoi(val);
      } catch (...) {
      }
    } else if (key == "console") {
      c.showConsole = (val == "1" || val == "true");
    }
  }

  if (c.bufferFrames < 32) c.bufferFrames = 32;
  if (c.bufferFrames > 1024) c.bufferFrames = 1024;
  return c;
}

bool config::save(const Config& c) {
  std::ofstream f(kPath, std::ios::trunc);
  if (!f) return false;
  f << "input=" << c.inputDevice << "\n";
  f << "output=" << c.outputDevice << "\n";
  f << "buffer=" << c.bufferFrames << "\n";
  f << "console=" << (c.showConsole ? 1 : 0) << "\n";
  return true;
}
