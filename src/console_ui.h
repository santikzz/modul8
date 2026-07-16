#pragma once

#include <string>
#include <vector>

class AudioEngine;

// temporary console prompts for device + effect selection. imgui replaces these later.
namespace console_ui {

unsigned int pickDevice(AudioEngine& engine, bool wantInput);
std::vector<std::string> pickEffects();

}  // namespace console_ui
