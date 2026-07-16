#pragma once

#include "imgui.h"

struct ID3D11Device;
struct HICON__;

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

// an image file (jpg/png/bmp, decoded via wic) uploaded as a texture. tex is 0
// when the file is missing or undecodable; results are cached per path.
struct Banner {
  ImTextureID tex;
  int w;
  int h;
};
const Banner& banner(const char* path);

// decode an image file (jpg/png/bmp) with wic and build an hicon for wm_seticon.
// returns nullptr when the file is missing or undecodable; the caller owns the icon.
HICON__* loadIconFile(const char* path, int size);

}  // namespace icons
