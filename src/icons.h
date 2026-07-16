#pragma once

#include "imgui.h"

struct ID3D11Device;

// loads a handful of real windows shell icons (SHGetStockIconInfo) into dx11
// textures so the ui can show native iconography. every getter returns 0 when the
// icon could not be loaded, so callers must keep a drawn-glyph fallback.
namespace icons {

enum Id {
  Presets,   // open folder
  Settings,  // application / properties
  About,     // info bubble
  Search,    // find
  Audio,     // audio files (speaker)
  Remove,    // delete (recycle)
  Warning,   // caution triangle
  Count
};

void init(ID3D11Device* device);
void shutdown();

// small = 16px source, big = 32px source. draw at any size; scaling is fine.
ImTextureID get(Id id, bool big = false);

}  // namespace icons
