#include "console_ui.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>

#include "audio.h"
#include "registry.h"

namespace console_ui {

unsigned int pickDevice(AudioEngine& engine, bool wantInput) {
  auto list = wantInput ? engine.inputDevices() : engine.outputDevices();
  if (list.empty()) {
    std::cerr << "no suitable " << (wantInput ? "input" : "output") << " device\n";
    std::exit(1);
  }

  std::cout << "\n" << (wantInput ? "input" : "output") << " devices:\n";
  for (size_t i = 0; i < list.size(); i++) {
    const auto& d = list[i];
    std::cout << "  [" << i << "] " << d.name << " (" << d.channels << " ch)"
              << (d.isDefault ? " *default" : "") << "\n";
  }

  std::cout << "pick number: ";
  size_t choice = 0;
  std::cin >> choice;
  if (choice >= list.size()) choice = 0;
  return list[choice].id;
}

std::vector<std::string> pickEffects() {
  std::cout << "\navailable effects:\n";
  for (auto& n : Registry::names()) std::cout << "  " << n << "\n";
  std::cout << "enter chain in order (space separated), e.g. distortion reverb:\n> ";
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  std::string line;
  std::getline(std::cin, line);

  std::vector<std::string> chain;
  std::istringstream iss(line);
  std::string tok;
  while (iss >> tok) chain.push_back(tok);
  return chain;
}

}  // namespace console_ui
