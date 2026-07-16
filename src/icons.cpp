#include "icons.h"

#include <d3d11.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace icons {

namespace {

ID3D11Device* g_dev = nullptr;
ImTextureID g_small[Count] = {};
ImTextureID g_big[Count] = {};
std::vector<ID3D11ShaderResourceView*> g_owned;  // freed on shutdown
std::map<std::string, Banner> g_banners;         // path -> texture, failures cached too

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

// decode an image file to 32-bit bgra with wic and upload it as a texture.
Banner loadBanner(const std::string& path) {
  Banner out = {};
  if (!g_dev) return out;

  // safe if com is already up on this thread; only bail on a hard failure.
  HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(co) && co != RPC_E_CHANGED_MODE) return out;

  IWICImagingFactory* factory = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;

  do {
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
      break;

    wchar_t wide[MAX_PATH];
    if (!::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, MAX_PATH)) break;
    if (FAILED(factory->CreateDecoderFromFilename(wide, nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
      break;
    if (FAILED(decoder->GetFrame(0, &frame))) break;

    if (FAILED(factory->CreateFormatConverter(&converter))) break;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
      break;

    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) break;
    std::vector<unsigned char> pixels((size_t)w * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data()))) break;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex)) && tex) {
      ID3D11ShaderResourceView* srv = nullptr;
      if (SUCCEEDED(g_dev->CreateShaderResourceView(tex, nullptr, &srv)) && srv) {
        g_owned.push_back(srv);
        out.tex = (ImTextureID)srv;
        out.w = (int)w;
        out.h = (int)h;
      }
      tex->Release();
    }
  } while (false);

  if (converter) converter->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (factory) factory->Release();
  return out;
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
  g_banners.clear();
  g_dev = nullptr;
}

ImTextureID get(Id id, bool big) {
  if (id < 0 || id >= Count) return (ImTextureID)0;
  return big ? g_big[id] : g_small[id];
}

// decode an image file with wic, scale it to size x size, and wrap it in an hicon.
HICON__* loadIconFile(const char* path, int size) {
  if (!path || !*path || size <= 0) return nullptr;

  HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(co) && co != RPC_E_CHANGED_MODE) return nullptr;

  IWICImagingFactory* factory = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICFormatConverter* converter = nullptr;
  HICON icon = nullptr;

  do {
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
      break;

    wchar_t wide[MAX_PATH];
    if (!::MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH)) break;
    if (FAILED(factory->CreateDecoderFromFilename(wide, nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
      break;
    if (FAILED(decoder->GetFrame(0, &frame))) break;

    if (FAILED(factory->CreateBitmapScaler(&scaler))) break;
    if (FAILED(scaler->Initialize(frame, (UINT)size, (UINT)size,
                                  WICBitmapInterpolationModeFant)))
      break;
    if (FAILED(factory->CreateFormatConverter(&converter))) break;
    if (FAILED(converter->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
      break;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // top-down, matches wic row order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
      if (color) ::DeleteObject(color);
      break;
    }

    if (SUCCEEDED(converter->CopyPixels(nullptr, (UINT)size * 4,
                                        (UINT)(size * size * 4), (BYTE*)bits))) {
      HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
      if (mask) {
        ICONINFO ii = {TRUE, 0, 0, mask, color};
        icon = ::CreateIconIndirect(&ii);
        ::DeleteObject(mask);
      }
    }
    ::DeleteObject(color);
  } while (false);

  if (converter) converter->Release();
  if (scaler) scaler->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (factory) factory->Release();
  return icon;
}

const Banner& banner(const char* path) {
  static const Banner none = {};
  if (!path || !*path) return none;
  auto it = g_banners.find(path);
  if (it == g_banners.end()) it = g_banners.emplace(path, loadBanner(path)).first;
  return it->second;
}

}  // namespace icons
