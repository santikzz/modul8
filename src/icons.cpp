#include "icons.h"

#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>

#include <vector>

namespace icons {

namespace {

ID3D11Device* g_dev = nullptr;
ImTextureID g_small[Count] = {};
ImTextureID g_big[Count] = {};
std::vector<ID3D11ShaderResourceView*> g_owned;  // freed on shutdown

SHSTOCKICONID stockId(Id id) {
  switch (id) {
    case Presets: return SIID_FOLDEROPEN;
    case Settings: return SIID_APPLICATION;
    case About: return SIID_INFO;
    case Search: return SIID_FIND;
    case Audio: return SIID_AUDIOFILES;
    case Remove: return SIID_DELETE;
    case Warning: return SIID_WARNING;
    default: return SIID_APPLICATION;
  }
}

// rasterize an hicon into a top-down 32-bit dib and upload it as a bgra texture.
ImTextureID textureFromIcon(HICON ic, int size) {
  if (!ic || !g_dev) return (ImTextureID)0;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;  // negative = top-down, matches texture rows
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ImTextureID out = (ImTextureID)0;

  if (dib && bits) {
    HGDIOBJ prev = ::SelectObject(mem, dib);
    ::memset(bits, 0, (size_t)size * size * 4);
    ::DrawIconEx(mem, 0, 0, ic, size, size, 0, nullptr, DI_NORMAL);
    ::SelectObject(mem, prev);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = size;
    td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = bits;
    sd.SysMemPitch = size * 4;

    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex)) && tex) {
      ID3D11ShaderResourceView* srv = nullptr;
      if (SUCCEEDED(g_dev->CreateShaderResourceView(tex, nullptr, &srv)) && srv) {
        g_owned.push_back(srv);
        out = (ImTextureID)srv;
      }
      tex->Release();
    }
  }

  if (dib) ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return out;
}

ImTextureID loadStock(Id id, bool big) {
  SHSTOCKICONINFO sii = {sizeof(sii)};
  UINT flags = SHGSI_ICON | (big ? SHGSI_LARGEICON : SHGSI_SMALLICON);
  if (::SHGetStockIconInfo(stockId(id), flags, &sii) != S_OK) return (ImTextureID)0;
  ImTextureID t = textureFromIcon(sii.hIcon, big ? 32 : 16);
  ::DestroyIcon(sii.hIcon);
  return t;
}

}  // namespace

void init(ID3D11Device* device) {
  g_dev = device;
  for (int i = 0; i < Count; i++) {
    g_small[i] = loadStock((Id)i, false);
    g_big[i] = loadStock((Id)i, true);
  }
}

void shutdown() {
  for (auto* srv : g_owned)
    if (srv) srv->Release();
  g_owned.clear();
  g_dev = nullptr;
}

ImTextureID get(Id id, bool big) {
  if (id < 0 || id >= Count) return (ImTextureID)0;
  return big ? g_big[id] : g_small[id];
}

}  // namespace icons
