#pragma once

class AudioEngine;

// owns the os window + imgui render loop. blocks until the window is closed.
namespace gui {

int run(AudioEngine& engine);

}  // namespace gui
