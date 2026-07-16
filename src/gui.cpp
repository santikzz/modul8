#include "gui.h"

#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "audio.h"
#include "config.h"
#include "graph.h"
#include "icons.h"
#include "lua_effect.h"
#include "preset.h"
#include "registry.h"

// ---- dx11 device + swap chain (isolated backend plumbing) ----

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_targetView = nullptr;
static bool g_occluded = false;
static UINT g_resizeW = 0, g_resizeH = 0;

// ---- borderless window state (shared between draw code and wndProc) ----

static HWND g_hwnd = nullptr;
static int g_titleBarH = 32;   // caption strip height, in pixels
static int g_captionRight = 0; // window-relative x where the drag area ends (window buttons start)

static void createTarget() {
  ID3D11Texture2D* back = nullptr;
  g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
  g_device->CreateRenderTargetView(back, nullptr, &g_targetView);
  back->Release();
}

static void releaseTarget() {
  if (g_targetView) {
    g_targetView->Release();
    g_targetView = nullptr;
  }
}

static bool createDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate = {60, 1};
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc = {1, 0};
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
                                             &g_device, &got, &g_context);
  if (hr == DXGI_ERROR_UNSUPPORTED)
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                       D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got,
                                       &g_context);
  if (hr != S_OK) return false;
  createTarget();
  return true;
}

static void cleanupDevice() {
  releaseTarget();
  if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
  if (g_context) { g_context->Release(); g_context = nullptr; }
  if (g_device) { g_device->Release(); g_device = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// hit-test for the borderless main window: resize edges + custom drag caption.
static LRESULT hitTest(HWND hwnd, LPARAM lParam) {
  const int border = 8;
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  RECT rc;
  ::GetWindowRect(hwnd, &rc);

  bool left = pt.x < rc.left + border;
  bool right = pt.x >= rc.right - border;
  bool top = pt.y < rc.top + border;
  bool bottom = pt.y >= rc.bottom - border;

  if (top && left) return HTTOPLEFT;
  if (top && right) return HTTOPRIGHT;
  if (bottom && left) return HTBOTTOMLEFT;
  if (bottom && right) return HTBOTTOMRIGHT;
  if (left) return HTLEFT;
  if (right) return HTRIGHT;
  if (top) return HTTOP;
  if (bottom) return HTBOTTOM;

  int cx = pt.x - rc.left;
  int cy = pt.y - rc.top;
  if (cy < g_titleBarH && cx < g_captionRight) return HTCAPTION;  // drag; excludes window buttons
  return HTCLIENT;
}

static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wParam) {
        // strip the native frame so the client fills the whole window.
        // when maximized, pull the client in by the frame or it overflows the monitor.
        if (::IsZoomed(hwnd)) {
          int fx = ::GetSystemMetrics(SM_CXFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
          int fy = ::GetSystemMetrics(SM_CYFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
          auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
          p->rgrc[0].left += fx;
          p->rgrc[0].top += fy;
          p->rgrc[0].right -= fx;
          p->rgrc[0].bottom -= fy;
        }
        return 0;
      }
      break;
    case WM_NCHITTEST:
      return hitTest(hwnd, lParam);
    case WM_SIZE:
      if (wParam == SIZE_MINIMIZED) return 0;
      g_resizeW = LOWORD(lParam);
      g_resizeH = HIWORD(lParam);
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // disable alt menu
      break;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- ui state + panels ----

namespace {

// ---- windows 98 look: palette, 3d bevels, custom chrome ----

namespace w98 {

const ImU32 kFace = IM_COL32(192, 192, 192, 255);
const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kLight = IM_COL32(223, 223, 223, 255);
const ImU32 kGray = IM_COL32(128, 128, 128, 255);
const ImU32 kDark = IM_COL32(10, 10, 10, 255);
const ImU32 kBlack = IM_COL32(0, 0, 0, 255);
const ImU32 kNavy = IM_COL32(0, 0, 128, 255);
const ImU32 kNavyLit = IM_COL32(16, 132, 208, 255);
const ImU32 kCapText = IM_COL32(255, 255, 255, 255);

// window chrome geometry (pixels)
const float kFrame = 4.0f;    // raised sizing border
const float kCaption = 20.0f; // title bar height
const float kMenuBar = 19.0f; // menu strip height
const float kToolbar = 32.0f; // actions bar height
const float kStatus = 22.0f;  // status bar height

// widget geometry (pixels), matching the classic control metrics
const float kButtonH = 23.0f;
const float kComboH = 20.0f;
const float kCheckbox = 13.0f;
const float kSliderTrackH = 22.0f;
const float kThumbW = 11.0f;
const float kThumbBodyH = 13.0f;
const float kThumbPointH = 5.0f;
const float kTickH = 3.0f;

void fillFace(ImDrawList* dl, ImVec2 a, ImVec2 b) { dl->AddRectFilled(a, b, kFace); }

// two-ring 3d edge, colors per 98.css. a=top-left, b=bottom-right.
void bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 outerTL, ImU32 innerTL, ImU32 outerBR,
           ImU32 innerBR) {
  dl->AddRectFilled(a, ImVec2(b.x, a.y + 1), outerTL);                          // top
  dl->AddRectFilled(a, ImVec2(a.x + 1, b.y), outerTL);                          // left
  dl->AddRectFilled(ImVec2(a.x, b.y - 1), b, outerBR);                          // bottom
  dl->AddRectFilled(ImVec2(b.x - 1, a.y), b, outerBR);                          // right
  dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + 2), innerTL);
  dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(a.x + 2, b.y - 1), innerTL);
  dl->AddRectFilled(ImVec2(a.x + 1, b.y - 2), ImVec2(b.x - 1, b.y - 1), innerBR);
  dl->AddRectFilled(ImVec2(b.x - 2, a.y + 1), ImVec2(b.x - 1, b.y - 1), innerBR);
}

void raised(ImDrawList* dl, ImVec2 a, ImVec2 b) { bevel(dl, a, b, kWhite, kLight, kDark, kGray); }
void sunken(ImDrawList* dl, ImVec2 a, ImVec2 b) { bevel(dl, a, b, kGray, kDark, kWhite, kLight); }
void pressed(ImDrawList* dl, ImVec2 a, ImVec2 b) { bevel(dl, a, b, kDark, kGray, kWhite, kLight); }

// classic caption glyphs, drawn as 1px black shapes centered in [a,b]. kind:
// 0=minimize 1=maximize 2=restore 3=close
void glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, int kind, ImVec2 off) {
  float cx = (a.x + b.x) * 0.5f + off.x;
  float cy = (a.y + b.y) * 0.5f + off.y;
  switch (kind) {
    case 0:
      dl->AddRectFilled(ImVec2(cx - 4, cy + 3), ImVec2(cx + 3, cy + 5), kBlack);
      break;
    case 1:
      dl->AddRect(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), kBlack);
      dl->AddRectFilled(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy - 3), kBlack);
      break;
    case 2:
      dl->AddRect(ImVec2(cx - 3, cy - 5), ImVec2(cx + 5, cy + 3), kBlack);
      dl->AddRectFilled(ImVec2(cx - 3, cy - 5), ImVec2(cx + 5, cy - 3), kBlack);
      dl->AddRectFilled(ImVec2(cx - 5, cy - 3), ImVec2(cx + 3, cy + 5), kFace);
      dl->AddRect(ImVec2(cx - 5, cy - 3), ImVec2(cx + 3, cy + 5), kBlack);
      dl->AddRectFilled(ImVec2(cx - 5, cy - 3), ImVec2(cx + 3, cy - 1), kBlack);
      break;
    case 3:
      dl->AddLine(ImVec2(cx - 4, cy - 4), ImVec2(cx + 4, cy + 4), kBlack, 1.4f);
      dl->AddLine(ImVec2(cx - 4, cy + 4), ImVec2(cx + 4, cy - 4), kBlack, 1.4f);
      break;
  }
}

// small speaker icon for the caption, thematic app glyph. origin is top-left.
void appIcon(ImDrawList* dl, ImVec2 o) {
  float x = o.x, cy = o.y + 7;
  dl->AddRectFilled(ImVec2(x, cy - 2), ImVec2(x + 3, cy + 3), kBlack);
  dl->AddTriangleFilled(ImVec2(x + 3, cy - 4), ImVec2(x + 3, cy + 5), ImVec2(x + 8, cy), kBlack);
  dl->AddLine(ImVec2(x + 10, cy - 3), ImVec2(x + 10, cy + 3), kBlack, 1.2f);
  dl->AddLine(ImVec2(x + 12, cy - 5), ImVec2(x + 12, cy + 5), kBlack, 1.2f);
}

// ---- disabled scope (grays custom widgets, blocks interaction) ----

bool g_disStack[16];
int g_disTop = 0;

void beginDisabled(bool d) {
  ImGui::BeginDisabled(d);
  bool eff = d || (g_disTop > 0 && g_disStack[g_disTop - 1]);
  if (g_disTop < 16) g_disStack[g_disTop++] = eff;
}
void endDisabled() {
  ImGui::EndDisabled();
  if (g_disTop > 0) g_disTop--;
}
bool disabled() { return g_disTop > 0 && g_disStack[g_disTop - 1]; }

// ---- custom immediate-mode widgets, drawn to match windows 98 ----

bool button(const char* label, float width = 0.0f, float height = kButtonH) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 ts = ImGui::CalcTextSize(label);
  float w = width > 0.0f ? width : ts.x + 18.0f;
  bool clicked = ImGui::InvisibleButton(label, ImVec2(w, height));
  bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 a = pos, b(pos.x + w, pos.y + height);
  fillFace(dl, a, b);
  if (held)
    pressed(dl, a, b);
  else
    raised(dl, a, b);
  float push = held ? 1.0f : 0.0f;
  dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f + push, (a.y + b.y - ts.y) * 0.5f + push),
              disabled() ? kGray : kBlack, label);
  return clicked;
}

bool smallButton(const char* label) {
  ImVec2 ts = ImGui::CalcTextSize(label);
  return button(label, ts.x + 12.0f, ImGui::GetTextLineHeight() + 5.0f);
}

bool checkbox(const char* label, bool* v) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 ts = ImGui::CalcTextSize(label);
  float h = kCheckbox > ts.y ? kCheckbox : ts.y;
  bool clicked = ImGui::InvisibleButton(label, ImVec2(kCheckbox + 6.0f + ts.x, h));
  if (clicked) *v = !*v;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 boxMin(pos.x, pos.y + (h - kCheckbox) * 0.5f);
  ImVec2 boxMax(boxMin.x + kCheckbox, boxMin.y + kCheckbox);
  dl->AddRectFilled(boxMin, boxMax, kWhite);
  sunken(dl, boxMin, boxMax);
  ImU32 fg = disabled() ? kGray : kBlack;
  if (*v) {
    float x = boxMin.x, y = boxMin.y;
    dl->AddLine(ImVec2(x + 3, y + 6), ImVec2(x + 5, y + 8), fg, 1.6f);
    dl->AddLine(ImVec2(x + 5, y + 8), ImVec2(x + 9, y + 3), fg, 1.6f);
  }
  dl->AddText(ImVec2(boxMax.x + 6, pos.y + (h - ts.y) * 0.5f), fg, label);
  return clicked;
}

void separator() {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y = pos.y + 3.0f;
  dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + w, y), kGray, 1.0f);
  dl->AddLine(ImVec2(pos.x, y + 1), ImVec2(pos.x + w, y + 1), kWhite, 1.0f);
  ImGui::Dummy(ImVec2(w, 7));
}

void separatorText(const char* label) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImVec2 ts = ImGui::CalcTextSize(label);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(pos, kBlack, label);
  float lx = pos.x + ts.x + 6.0f, y = pos.y + ts.y * 0.5f;
  dl->AddLine(ImVec2(lx, y), ImVec2(pos.x + w, y), kGray, 1.0f);
  dl->AddLine(ImVec2(lx, y + 1), ImVec2(pos.x + w, y + 1), kWhite, 1.0f);
  ImGui::Dummy(ImVec2(w, ts.y));
}

void dottedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
  for (float x = a.x; x < b.x; x += 2.0f) {
    dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x + 1, a.y + 1), col);
    dl->AddRectFilled(ImVec2(x, b.y - 1), ImVec2(x + 1, b.y), col);
  }
  for (float y = a.y; y < b.y; y += 2.0f) {
    dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + 1, y + 1), col);
    dl->AddRectFilled(ImVec2(b.x - 1, y), ImVec2(b.x, y + 1), col);
  }
}

// classic trackbar: recessed groove, tick marks, pentagon thumb with a 3d edge.
void trackbar(ImDrawList* dl, ImVec2 origin, float w, float t, bool focused) {
  float left = origin.x, right = origin.x + w, trackY = origin.y;
  if (focused)
    dottedRect(dl, ImVec2(left - 2, trackY - 2), ImVec2(right + 2, trackY + kSliderTrackH), kBlack);

  float grooveY = trackY + 6.0f;
  dl->AddLine(ImVec2(left, grooveY), ImVec2(right, grooveY), kGray, 1.0f);
  dl->AddLine(ImVec2(left, grooveY + 1), ImVec2(right, grooveY + 1), kWhite, 1.0f);

  float tickTop = trackY + kThumbBodyH + kThumbPointH + 1.0f;
  int ticks = (int)(w / 16.0f);
  if (ticks < 4) ticks = 4;
  for (int i = 0; i <= ticks; i++) {
    float tx = left + (w - 1.0f) * (float)i / (float)ticks;
    dl->AddLine(ImVec2(tx, tickTop), ImVec2(tx, tickTop + kTickH), kGray, 1.0f);
  }

  float cx = left + kThumbW * 0.5f + t * (w - kThumbW);
  float half = kThumbW * 0.5f, bodyB = trackY + kThumbBodyH;
  ImVec2 tl(cx - half, trackY), tr(cx + half, trackY), br(cx + half, bodyB);
  ImVec2 pt(cx, bodyB + kThumbPointH), bl(cx - half, bodyB);
  ImVec2 poly[5] = {tl, tr, br, pt, bl};
  dl->AddConvexPolyFilled(poly, 5, kFace);
  dl->AddRectFilled(ImVec2(tl.x, tl.y), ImVec2(tr.x, tl.y + 1), kWhite);
  dl->AddRectFilled(ImVec2(tl.x, tl.y), ImVec2(tl.x + 1, bl.y), kWhite);
  dl->AddRectFilled(ImVec2(tl.x + 1, tl.y + 1), ImVec2(tr.x - 1, tl.y + 2), kLight);
  dl->AddRectFilled(ImVec2(tl.x + 1, tl.y + 1), ImVec2(tl.x + 2, bl.y), kLight);
  dl->AddRectFilled(ImVec2(tr.x - 1, tr.y), ImVec2(tr.x, br.y), kDark);
  dl->AddRectFilled(ImVec2(tr.x - 2, tr.y + 1), ImVec2(tr.x - 1, br.y), kGray);
  dl->AddLine(ImVec2(bl.x + 0.5f, bl.y), pt, kWhite, 1.0f);
  dl->AddLine(ImVec2(br.x - 0.5f, br.y), pt, kDark, 1.0f);
}

bool sliderInt(const char* label, int* v, int mn, int mx) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  float lineH = ImGui::GetTextLineHeight();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool dis = disabled();

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", *v);
  ImVec2 vts = ImGui::CalcTextSize(buf);

  bool changed = false;
  ImGui::InvisibleButton(label, ImVec2(w, lineH + 3.0f + kSliderTrackH));
  bool active = ImGui::IsItemActive();
  bool hovered = ImGui::IsItemHovered();

  int span = mx - mn;
  float t = span != 0 ? (float)(*v - mn) / (float)span : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  if (active) {
    float rel = (ImGui::GetMousePos().x - (pos.x + kThumbW * 0.5f)) / (w - kThumbW);
    if (rel < 0.0f) rel = 0.0f;
    if (rel > 1.0f) rel = 1.0f;
    int nv = mn + (int)(rel * span + 0.5f);
    if (nv != *v) {
      *v = nv;
      changed = true;
    }
    t = rel;
  }

  ImU32 tc = dis ? kGray : kBlack;
  dl->AddText(pos, tc, label);
  dl->AddText(ImVec2(pos.x + w - vts.x, pos.y), tc, buf);
  trackbar(dl, ImVec2(pos.x, pos.y + lineH + 3.0f), w, t, (hovered || active) && !dis);
  return changed;
}

bool sliderFloat(const char* label, float* v, float mn, float mx) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  float lineH = ImGui::GetTextLineHeight();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool dis = disabled();

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", *v);
  ImVec2 vts = ImGui::CalcTextSize(buf);

  bool changed = false;
  ImGui::InvisibleButton(label, ImVec2(w, lineH + 3.0f + kSliderTrackH));
  bool active = ImGui::IsItemActive();
  bool hovered = ImGui::IsItemHovered();

  float span = mx - mn;
  float t = span != 0.0f ? (*v - mn) / span : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  if (active) {
    float rel = (ImGui::GetMousePos().x - (pos.x + kThumbW * 0.5f)) / (w - kThumbW);
    if (rel < 0.0f) rel = 0.0f;
    if (rel > 1.0f) rel = 1.0f;
    float nv = mn + rel * span;
    if (nv != *v) {
      *v = nv;
      changed = true;
    }
    t = rel;
  }

  ImU32 tc = dis ? kGray : kBlack;
  dl->AddText(pos, tc, label);
  dl->AddText(ImVec2(pos.x + w - vts.x, pos.y), tc, buf);
  trackbar(dl, ImVec2(pos.x, pos.y + lineH + 3.0f), w, t, (hovered || active) && !dis);
  return changed;
}

// compact raised toolbar button; caller draws its glyph from the item rect.
bool toolButton(const char* id, float w, float h, bool sunkenLook) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 a = pos, b(pos.x + w, pos.y + h);
  fillFace(dl, a, b);
  if (held || sunkenLook)
    pressed(dl, a, b);
  else
    raised(dl, a, b);
  return clicked;
}

// short horizontal slider for the toolbar: sunken groove, small raised thumb.
bool toolSlider(const char* id, float* v, float mn, float mx, float w) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  const float h = 18.0f;
  bool changed = false;
  ImGui::InvisibleButton(id, ImVec2(w, h));
  bool active = ImGui::IsItemActive();

  float span = mx - mn;
  float t = span != 0.0f ? (*v - mn) / span : 0.0f;
  if (active) {
    float rel = (ImGui::GetMousePos().x - (pos.x + 6.0f)) / (w - 12.0f);
    if (rel < 0.0f) rel = 0.0f;
    if (rel > 1.0f) rel = 1.0f;
    float nv = mn + rel * span;
    if (nv != *v) {
      *v = nv;
      changed = true;
    }
    t = rel;
  }
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  float gy = pos.y + h * 0.5f;
  dl->AddLine(ImVec2(pos.x + 3, gy), ImVec2(pos.x + w - 3, gy), kGray, 1.0f);
  dl->AddLine(ImVec2(pos.x + 3, gy + 1), ImVec2(pos.x + w - 3, gy + 1), kWhite, 1.0f);
  float cx = pos.x + 6.0f + t * (w - 12.0f);
  ImVec2 ta(cx - 5, pos.y + 2), tb(cx + 5, pos.y + h - 2);
  fillFace(dl, ta, tb);
  raised(dl, ta, tb);
  return changed;
}

// classic menu entry: full-width row, navy highlight, closes the popup on click.
bool menuItem(const char* label) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  const float h = 17.0f;
  bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool hot = ImGui::IsItemHovered();
  if (hot) dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), kNavy);
  dl->AddText(ImVec2(pos.x + 20.0f, pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
              hot ? kCapText : kBlack, label);
  if (clicked) ImGui::CloseCurrentPopup();
  return clicked;
}

// classic group box: etched frame with the label breaking the top line. content goes
// into a child sized by the explicit height; pair with endGroupBox().
ImVec2 g_gbPos;
float g_gbH;

void beginGroupBox(const char* label, float h) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  g_gbPos = pos;
  g_gbH = h;

  ImVec2 a(pos.x, pos.y + 6.0f);
  ImVec2 b(pos.x + w, pos.y + h);
  dl->AddRect(ImVec2(a.x + 1, a.y + 1), b, kWhite);
  dl->AddRect(a, ImVec2(b.x - 1, b.y - 1), kGray);

  ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddRectFilled(ImVec2(pos.x + 8, pos.y), ImVec2(pos.x + 14 + ts.x, pos.y + ts.y), kFace);
  dl->AddText(ImVec2(pos.x + 11, pos.y), disabled() ? kGray : kBlack, label);

  ImGui::SetCursorScreenPos(ImVec2(pos.x + 10, pos.y + ts.y + 8.0f));
  ImGui::BeginChild(label, ImVec2(w - 20.0f, h - ts.y - 16.0f), false);
}

void endGroupBox() {
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(g_gbPos.x, g_gbPos.y + g_gbH + 8.0f));
}

}  // namespace w98

// a caption button: raised, sinks while held, returns true on click (release inside).
bool captionButton(const char* id, ImVec2 tl, ImVec2 br, int kind) {
  ImGui::SetCursorScreenPos(tl);
  bool clicked = ImGui::InvisibleButton(id, ImVec2(br.x - tl.x, br.y - tl.y));
  bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(tl, br, w98::kFace);
  if (held)
    w98::pressed(dl, tl, br);
  else
    w98::raised(dl, tl, br);
  w98::glyph(dl, tl, br, kind, held ? ImVec2(1, 1) : ImVec2(0, 0));
  return clicked;
}

// fixed-size floating dialog with win98 chrome: raised frame, navy caption, close
// button. call endWindow98() only when this returns true.
bool beginWindow98(const char* title, bool* open, ImVec2 size) {
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  bool visible = ImGui::Begin(title, nullptr, flags);
  ImGui::PopStyleVar(2);
  if (!visible) {
    ImGui::End();
    return false;
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetWindowPos();
  ImVec2 br(pos.x + size.x, pos.y + size.y);
  dl->AddRectFilled(pos, br, w98::kFace);
  w98::raised(dl, pos, br);

  const float f = 3.0f;
  ImVec2 a(pos.x + f, pos.y + f);
  ImVec2 b(br.x - f, a.y + w98::kCaption);
  dl->AddRectFilledMultiColor(a, b, w98::kNavy, w98::kNavyLit, w98::kNavyLit, w98::kNavy);
  float textY = a.y + (w98::kCaption - ImGui::GetTextLineHeight()) * 0.5f;
  dl->AddText(ImVec2(a.x + 5, textY), w98::kCapText, title);

  const float bw = 16.0f, bh = 14.0f;
  float by = a.y + (w98::kCaption - bh) * 0.5f;
  if (captionButton("##close", ImVec2(b.x - 2 - bw, by), ImVec2(b.x - 2, by + bh), 3))
    *open = false;

  ImGui::SetCursorPos(ImVec2(f, f + w98::kCaption));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
  ImGui::BeginChild("##body", ImVec2(size.x - 2 * f, size.y - 2 * f - w98::kCaption), false);
  ImGui::PopStyleVar();
  return true;
}

void endWindow98() {
  ImGui::EndChild();
  ImGui::End();
}

void applyWin98Style() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.0f;
  s.PopupRounding = s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.0f;
  s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = s.PopupBorderSize = 1.0f;
  s.TabBorderSize = 0.0f;
  s.WindowPadding = ImVec2(8, 8);
  s.FramePadding = ImVec2(6, 3);
  s.ItemSpacing = ImVec2(6, 4);
  s.ItemInnerSpacing = ImVec2(4, 4);
  s.ScrollbarSize = 16.0f;
  s.GrabMinSize = 16.0f;
  s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

  auto rgb = [](int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
  };
  const ImVec4 face = rgb(192, 192, 192);
  const ImVec4 white = rgb(255, 255, 255);
  const ImVec4 light = rgb(223, 223, 223);
  const ImVec4 gray = rgb(128, 128, 128);
  const ImVec4 navy = rgb(0, 0, 128);
  const ImVec4 field = rgb(255, 255, 255);
  const ImVec4 black = rgb(0, 0, 0);

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = black;
  c[ImGuiCol_TextDisabled] = gray;
  c[ImGuiCol_TextSelectedBg] = navy;
  c[ImGuiCol_WindowBg] = face;
  c[ImGuiCol_ChildBg] = face;
  c[ImGuiCol_PopupBg] = face;
  c[ImGuiCol_Border] = white;      // paired with BorderShadow to fake the raised edge
  c[ImGuiCol_BorderShadow] = gray;
  c[ImGuiCol_FrameBg] = field;     // inputs/combos/checkboxes/sliders are sunken white fields
  c[ImGuiCol_FrameBgHovered] = field;
  c[ImGuiCol_FrameBgActive] = field;
  c[ImGuiCol_TitleBg] = gray;
  c[ImGuiCol_TitleBgActive] = navy;
  c[ImGuiCol_TitleBgCollapsed] = gray;
  c[ImGuiCol_MenuBarBg] = face;
  c[ImGuiCol_ScrollbarBg] = light;
  c[ImGuiCol_ScrollbarGrab] = face;
  c[ImGuiCol_ScrollbarGrabHovered] = face;
  c[ImGuiCol_ScrollbarGrabActive] = face;
  c[ImGuiCol_CheckMark] = black;
  c[ImGuiCol_SliderGrab] = face;
  c[ImGuiCol_SliderGrabActive] = face;
  c[ImGuiCol_Button] = face;
  c[ImGuiCol_ButtonHovered] = face;
  c[ImGuiCol_ButtonActive] = light;
  c[ImGuiCol_Header] = navy;
  c[ImGuiCol_HeaderHovered] = navy;
  c[ImGuiCol_HeaderActive] = navy;
  c[ImGuiCol_Separator] = gray;
  c[ImGuiCol_SeparatorHovered] = gray;
  c[ImGuiCol_SeparatorActive] = gray;
  c[ImGuiCol_ResizeGrip] = face;
  c[ImGuiCol_ResizeGripHovered] = light;
  c[ImGuiCol_ResizeGripActive] = white;
  c[ImGuiCol_Tab] = face;
  c[ImGuiCol_TabHovered] = light;
  c[ImGuiCol_TabActive] = light;
  c[ImGuiCol_TabUnfocused] = face;
  c[ImGuiCol_TabUnfocusedActive] = light;
}

// registry names are lowercase; capitalize each word for display only. the stored
// type stays lowercase so the registry and preset files keep matching.
std::string titleCase(const std::string& s) {
  std::string r = s;
  bool up = true;
  for (char& c : r) {
    if (c == ' ') {
      up = true;
    } else if (up) {
      c = (char)std::toupper((unsigned char)c);
      up = false;
    }
  }
  return r;
}

// friendly one-liners shown under each effect in the picker, for non-technical users.
const char* effectBlurb(const std::string& type) {
  if (type == "distortion") return "Warm, crunchy overdrive";
  if (type == "fuzz") return "Thick, gnarly fuzz tone";
  if (type == "gain") return "Boost or lower the volume";
  if (type == "reverb") return "Adds space, like a room";
  if (type == "echo") return "Repeats your notes back";
  if (type == "flanger") return "Sweeping, jet-plane swirl";
  if (type == "noise gate") return "Silences hiss between notes";
  return "Sound effect";
}

// draw a shell icon texture centered at `c`, scaled to `sz`px. grays when disabled.
void drawIcon(ImDrawList* dl, ImTextureID tex, ImVec2 c, float sz, bool dis = false) {
  if (!tex) return;
  ImVec2 a(c.x - sz * 0.5f, c.y - sz * 0.5f), b(a.x + sz, a.y + sz);
  dl->AddImage(tex, a, b, ImVec2(0, 0), ImVec2(1, 1),
               dis ? IM_COL32(255, 255, 255, 96) : IM_COL32(255, 255, 255, 255));
}

// vector fallbacks for actions with no native shell icon (plus, folder).
void plusGlyph(ImDrawList* dl, ImVec2 c, ImU32 col) {
  dl->AddRectFilled(ImVec2(c.x - 5, c.y - 1), ImVec2(c.x + 5, c.y + 1), col);
  dl->AddRectFilled(ImVec2(c.x - 1, c.y - 5), ImVec2(c.x + 1, c.y + 5), col);
}

void folderGlyph(ImDrawList* dl, ImVec2 c, ImU32 col) {
  ImVec2 a(c.x - 6, c.y - 4), b(c.x + 6, c.y + 5);
  dl->AddRect(a, b, col);
  dl->AddLine(ImVec2(a.x, a.y + 2), ImVec2(c.x - 1, a.y + 2), col);
  dl->AddLine(ImVec2(c.x - 1, a.y + 2), ImVec2(c.x + 1, a.y), col);
  dl->AddLine(ImVec2(c.x + 1, a.y), ImVec2(b.x, a.y), col);
}

struct UiState {
  std::vector<AudioDevice> inputs;
  std::vector<AudioDevice> outputs;
  std::vector<std::string> available;  // effect names from the registry
  Graph graph;                         // the live signal graph shown on the canvas
  int inSel = -1;   // -1 = nothing selected
  int outSel = -1;
  int bufferFrames = 256;
  bool running = false;
  bool showSettings = false;
  bool showAbout = false;
  bool showConsole = false;
  bool showAddEffect = false;
  int wireFrom = -1;  // node id an in-progress cable is being dragged from, -1 = none
  int addCount = 0;   // nodes added, used to stagger new card positions
  int raiseId = -1;   // node to pull to the top of the draw order next frame
  float inVol = 1.0f;
  float outVol = 1.0f;
  bool bypassAll = false;
  char addSearch[64] = "";     // add-effect dialog filter text
  std::string addSelected;     // effect name highlighted in the dialog
  std::string status;

  // presets
  std::vector<std::string> presets;  // names on disk, refreshed when the menu opens
  std::string currentPreset;         // last loaded/saved preset, "" = unsaved rig
  bool showSavePreset = false;
  char presetName[64] = "";

  // generic yes/no confirmation modal
  bool showConfirm = false;
  int confirmAction = 0;  // 1 = new empty rig, 2 = delete current preset
  std::string confirmTitle, confirmText, confirmYes, confirmArg;
};

void applyConsole(bool show) {
  HWND c = ::GetConsoleWindow();
  if (c) ::ShowWindow(c, show ? SW_SHOW : SW_HIDE);
}

int indexOfDevice(const std::vector<AudioDevice>& devs, const std::string& name) {
  if (name.empty()) return -1;
  for (int i = 0; i < (int)devs.size(); i++)
    if (devs[i].name == name) return i;
  return -1;
}

void saveConfig(const UiState& ui) {
  Config c;
  c.inputDevice = ui.inSel >= 0 ? ui.inputs[ui.inSel].name : "";
  c.outputDevice = ui.outSel >= 0 ? ui.outputs[ui.outSel].name : "";
  c.bufferFrames = ui.bufferFrames;
  c.showConsole = ui.showConsole;
  config::save(c);
}

void refreshDevices(AudioEngine& engine, UiState& ui) {
  ui.inputs = engine.inputDevices();
  ui.outputs = engine.outputDevices();
  ui.available = Registry::names();
  if (ui.inSel >= (int)ui.inputs.size()) ui.inSel = -1;
  if (ui.outSel >= (int)ui.outputs.size()) ui.outSel = -1;
}

// win98 caption: navy gradient bar, app icon, title, beveled min/max/close buttons.
// sets g_titleBarH / g_captionRight so wndProc knows the drag region vs the buttons.
void drawTitleBar(ImVec2 pos, float w) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float f = w98::kFrame;
  const float capTop = pos.y + f;
  const float capBot = capTop + w98::kCaption;
  g_titleBarH = (int)(f + w98::kCaption);

  ImVec2 a(pos.x + f, capTop);
  ImVec2 b(pos.x + w - f, capBot);
  dl->AddRectFilledMultiColor(a, b, w98::kNavy, w98::kNavyLit, w98::kNavyLit, w98::kNavy);

  bool active = ::GetForegroundWindow() == g_hwnd;
  if (!active) dl->AddRectFilled(a, b, w98::kGray);

  w98::appIcon(dl, ImVec2(a.x + 4, a.y + (w98::kCaption - 14) * 0.5f));
  float textY = a.y + (w98::kCaption - ImGui::GetTextLineHeight()) * 0.5f;
  dl->AddText(ImVec2(a.x + 22, textY), w98::kCapText, "guitarpedal");

  const float bw = 16.0f, bh = 14.0f;
  float by = capTop + (w98::kCaption - bh) * 0.5f;
  float closeR = pos.x + w - f - 2;
  ImVec2 cTL(closeR - bw, by), cBR(closeR, by + bh);
  ImVec2 mxTL(cTL.x - 2 - bw, by), mxBR(cTL.x - 2, by + bh);
  ImVec2 mnTL(mxTL.x - bw, by), mnBR(mxTL.x, by + bh);
  g_captionRight = (int)(mnTL.x - pos.x);

  bool zoomed = ::IsZoomed(g_hwnd);
  if (captionButton("##min", mnTL, mnBR, 0)) ::ShowWindow(g_hwnd, SW_MINIMIZE);
  if (captionButton("##max", mxTL, mxBR, zoomed ? 2 : 1))
    ::ShowWindow(g_hwnd, zoomed ? SW_RESTORE : SW_MAXIMIZE);
  if (captionButton("##close", cTL, cBR, 3)) ::PostMessage(g_hwnd, WM_CLOSE, 0, 0);
}

// custom win98 combo: label above a sunken field with a raised drop arrow; the list is
// a borderless popup with navy selection and a classic scrollbar (via imgui) when long.
void deviceCombo(const char* label, const std::vector<AudioDevice>& devs, int& sel) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  float lineH = ImGui::GetTextLineHeight();
  bool dis = w98::disabled();
  bool wasOpen = ImGui::IsPopupOpen(label);

  bool clicked = ImGui::InvisibleButton(label, ImVec2(w, lineH + 2.0f + w98::kComboH));
  bool held = ImGui::IsItemActive();
  ImU32 tc = dis ? w98::kGray : w98::kBlack;

  dl->AddText(pos, tc, label);
  ImVec2 boxMin(pos.x, pos.y + lineH + 2.0f);
  ImVec2 boxMax(pos.x + w, boxMin.y + w98::kComboH);
  dl->AddRectFilled(boxMin, boxMax, w98::kWhite);
  w98::sunken(dl, boxMin, boxMax);

  ImVec2 arMin(boxMax.x - w98::kComboH + 2.0f, boxMin.y + 2.0f);
  ImVec2 arMax(boxMax.x - 2.0f, boxMax.y - 2.0f);
  w98::fillFace(dl, arMin, arMax);
  if (wasOpen || held)
    w98::pressed(dl, arMin, arMax);
  else
    w98::raised(dl, arMin, arMax);
  ImVec2 ac((arMin.x + arMax.x) * 0.5f, (arMin.y + arMax.y) * 0.5f);
  dl->AddTriangleFilled(ImVec2(ac.x - 3, ac.y - 1.5f), ImVec2(ac.x + 3, ac.y - 1.5f),
                        ImVec2(ac.x, ac.y + 2.5f), tc);

  const char* preview = (sel >= 0 && sel < (int)devs.size()) ? devs[sel].name.c_str()
                        : (devs.empty() ? "(none)" : "(select)");
  dl->PushClipRect(ImVec2(boxMin.x + 4.0f, boxMin.y), ImVec2(arMin.x - 1.0f, boxMax.y), true);
  dl->AddText(ImVec2(boxMin.x + 5.0f, boxMin.y + (w98::kComboH - lineH) * 0.5f), tc, preview);
  dl->PopClipRect();

  if (clicked && !dis && !wasOpen) ImGui::OpenPopup(label);

  float itemH = w98::kComboH - 2.0f;
  ImGui::SetNextWindowPos(ImVec2(boxMin.x, boxMax.y));
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, itemH * 8.0f + 2.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, w98::kWhite);
  ImGui::PushStyleColor(ImGuiCol_Border, w98::kBlack);
  if (ImGui::BeginPopup(label, ImGuiWindowFlags_NoMove)) {
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)devs.size(); i++) {
      ImGui::PushID(i);
      ImVec2 rp = ImGui::GetCursorScreenPos();
      float iw = ImGui::GetContentRegionAvail().x;
      bool rc = ImGui::InvisibleButton("##it", ImVec2(iw, itemH));
      bool navy = ImGui::IsItemHovered() || i == sel;
      if (navy) pdl->AddRectFilled(rp, ImVec2(rp.x + iw, rp.y + itemH), w98::kNavy);
      std::string item = devs[i].name + (devs[i].isDefault ? " *default" : "");
      pdl->AddText(ImVec2(rp.x + 4.0f, rp.y + (itemH - lineH) * 0.5f),
                   navy ? w98::kCapText : w98::kBlack, item.c_str());
      if (rc) {
        sel = i;
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

void drawSettings(AudioEngine& engine, UiState& ui) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!beginWindow98("Settings", &ui.showSettings, ImVec2(340, 330))) return;

  w98::beginGroupBox("Audio", 192.0f);
  w98::beginDisabled(ui.running);  // devices + buffer are locked while streaming
  deviceCombo("input", ui.inputs, ui.inSel);
  deviceCombo("output", ui.outputs, ui.outSel);
  if (w98::smallButton("refresh devices")) refreshDevices(engine, ui);
  w98::sliderInt("buffer frames", &ui.bufferFrames, 32, 1024);
  w98::endDisabled();
  ImGui::TextUnformatted("sample rate: 48000 Hz");
  w98::endGroupBox();

  w98::beginGroupBox("App", 52.0f);
  if (w98::checkbox("show debug console", &ui.showConsole)) applyConsole(ui.showConsole);
  w98::endGroupBox();

  const float saveW = 75.0f;
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - saveW - 10.0f);
  if (w98::button("Save", saveW)) {
    saveConfig(ui);
    ui.status = "config saved";
  }
  endWindow98();
}

void drawAbout(UiState& ui) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!beginWindow98("About guitarpedal", &ui.showAbout, ImVec2(280, 140))) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  w98::appIcon(dl, pos);
  dl->AddText(ImVec2(pos.x + 22, pos.y), w98::kBlack, "guitarpedal");
  ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight() + 4.0f));
  ImGui::TextUnformatted("real-time guitar effects host");
  ImGui::TextUnformatted("version 1.0");
  w98::separator();

  const float okW = 75.0f;
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - okW) * 0.5f);
  if (w98::button("OK", okW)) ui.showAbout = false;
  endWindow98();
}

void startEngine(AudioEngine& engine, UiState& ui) {
  if (ui.inSel < 0 || ui.inSel >= (int)ui.inputs.size() || ui.outSel < 0 ||
      ui.outSel >= (int)ui.outputs.size()) {
    ui.status = "select an input and output device";
    return;
  }
  if (!engine.start(ui.inputs[ui.inSel].id, ui.outputs[ui.outSel].id, ui.graph, 48000,
                    (unsigned int)ui.bufferFrames)) {
    ui.status = "failed to open stream";
    return;
  }
  ui.running = true;
  ui.status = "running @ " + std::to_string(engine.sampleRate()) + " Hz, " +
              std::to_string(engine.bufferFrames()) + " frames";
}

// after a structural edit (node/wire added or removed) push a fresh plan to the
// audio thread. no-op when stopped: the plan is built from scratch on start.
void rebuildIfRunning(AudioEngine& engine, UiState& ui) {
  if (engine.running()) engine.swapPlan(ui.graph.buildPlan((int)engine.bufferFrames()));
}

// swapping the whole graph (preset load / new rig) frees every effect at once, so
// the stream must be stopped first. it is transparently restarted afterwards.
void withStoppedGraph(AudioEngine& engine, UiState& ui, void (*edit)(UiState&)) {
  bool wasRunning = ui.running;
  if (wasRunning) {
    engine.stop();
    ui.running = false;
  }
  edit(ui);
  ui.addCount = 0;
  if (wasRunning) startEngine(engine, ui);
}

void loadPreset(AudioEngine& engine, UiState& ui, const std::string& name) {
  bool wasRunning = ui.running;
  if (wasRunning) {
    engine.stop();
    ui.running = false;
  }
  if (!preset::load(name, ui.graph)) {
    ui.status = "could not load preset: " + name;
    return;
  }
  ui.currentPreset = name;
  ui.addCount = 0;
  if (wasRunning) startEngine(engine, ui);
  ui.status = "loaded preset: " + name;
}

void newRig(AudioEngine& engine, UiState& ui) {
  withStoppedGraph(engine, ui, [](UiState& u) {
    u.graph.clear();
    u.graph.addWire(Graph::kInput, Graph::kOutput);  // restore the dry passthrough
    u.currentPreset.clear();
  });
  ui.status = "started a new rig";
}

void savePreset(UiState& ui, const std::string& name) {
  if (preset::save(name, ui.graph)) {
    ui.currentPreset = name;
    ui.presets = preset::list();
    ui.status = "saved preset: " + name;
  } else {
    ui.status = "could not save preset";
  }
}

// classic menu bar under the caption: File / Settings / About. an open menu
// highlights navy and hovering a sibling switches to it, like the real thing.
void drawMenuBar(ImVec2 pos, float w, UiState& ui) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float f = w98::kFrame;
  ImVec2 a(pos.x + f, pos.y + f + w98::kCaption);
  ImVec2 b(pos.x + w - f, a.y + w98::kMenuBar);
  dl->AddRectFilled(a, b, w98::kFace);

  const char* labels[3] = {"File", "Settings", "About"};
  bool anyOpen = false;
  for (const char* l : labels)
    if (ImGui::IsPopupOpen(l)) anyOpen = true;

  float x = a.x + 1.0f;
  for (int i = 0; i < 3; i++) {
    ImVec2 ts = ImGui::CalcTextSize(labels[i]);
    float itemW = ts.x + 14.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, a.y + 1.0f));
    ImGui::PushID(i);
    bool clicked = ImGui::InvisibleButton("##menu", ImVec2(itemW, w98::kMenuBar - 2.0f));
    ImGui::PopID();
    bool hovered = ImGui::IsItemHovered();
    bool open = ImGui::IsPopupOpen(labels[i]);

    if (open)
      dl->AddRectFilled(ImVec2(x, a.y + 1), ImVec2(x + itemW, b.y - 1), w98::kNavy);
    dl->AddText(ImVec2(x + 7.0f, a.y + (w98::kMenuBar - ts.y) * 0.5f),
                open ? w98::kCapText : w98::kBlack, labels[i]);

    if ((clicked && !open) || (anyOpen && hovered && !open)) ImGui::OpenPopup(labels[i]);

    ImGui::SetNextWindowPos(ImVec2(x, b.y));
    ImGui::SetNextWindowSize(ImVec2(160, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (ImGui::BeginPopup(labels[i], ImGuiWindowFlags_NoMove)) {
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      ImVec2 wp = ImGui::GetWindowPos();
      ImVec2 ws = ImGui::GetWindowSize();
      pdl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), w98::kFace);
      w98::raised(pdl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
      switch (i) {
        case 0:
          if (w98::menuItem("Reload Lua Effects")) {
            auto errors = luafx::registerAll();
            ui.available = Registry::names();
            ui.status = errors.empty()
                            ? "lua effects reloaded"
                            : "lua load error: " + errors.front();
          }
          if (w98::menuItem("Open Effects Folder"))
            ::ShellExecuteA(nullptr, "open", luafx::dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          if (w98::menuItem("Close")) ::PostMessage(g_hwnd, WM_CLOSE, 0, 0);
          break;
        case 1:
          if (w98::menuItem("Manage settings...")) ui.showSettings = true;
          break;
        case 2:
          if (w98::menuItem("About guitarpedal...")) ui.showAbout = true;
          break;
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    x += itemW;
  }
}

// ---- canvas: pedal cards + cables ----

const float kNodeW = 156.0f;
const float kIoW = 60.0f;
const float kTitleH = 18.0f;
const float kPortR = 5.0f;

float paramRowH(const Param& p) { return p.kind == Param::Bool ? 20.0f : 44.0f; }

float nodeHeight(const GraphNode* n) {
  if (n->isIo()) return 40.0f;
  float h = kTitleH + 8.0f;
  for (auto& p : n->fx->params()) h += paramRowH(p);
  return h < kTitleH + 24.0f ? kTitleH + 24.0f : h;
}

struct NodeBox {
  ImVec2 min, max, inPort, outPort;
  bool hasIn, hasOut;
};

float distSq(ImVec2 a, ImVec2 b) {
  float dx = a.x - b.x, dy = a.y - b.y;
  return dx * dx + dy * dy;
}

// cubic bezier point, used to hit-test cables for click-to-delete.
ImVec2 bezier(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float t) {
  float u = 1.0f - t;
  float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
  return ImVec2(a * p0.x + b * c0.x + c * c1.x + d * p1.x,
                a * p0.y + b * c0.y + c * c1.y + d * p1.y);
}

// draws one effect/io card: bg, navy title (grayed when bypassed), bypass led +
// close button, param widgets. moves the node while its title is dragged.
void drawNode(AudioEngine& engine, UiState& ui, GraphNode* n, const NodeBox& box, int& closeId) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(n->id);

  // soft drop shadow so overlapping cards read as clearly stacked, not fighting.
  dl->AddRectFilled(ImVec2(box.min.x + 3, box.min.y + 3), ImVec2(box.max.x + 3, box.max.y + 3),
                    IM_COL32(0, 0, 0, 48));
  w98::fillFace(dl, box.min, box.max);
  w98::raised(dl, box.min, box.max);

  ImVec2 tMin = box.min, tMax(box.max.x, box.min.y + kTitleH);
  bool bypassed = n->bypass.load(std::memory_order_relaxed);
  ImU32 capA = bypassed ? w98::kGray : w98::kNavy;
  ImU32 capB = bypassed ? w98::kGray : w98::kNavyLit;
  dl->AddRectFilledMultiColor(tMin, tMax, capA, capB, capB, capA);
  // border under the drag bar: a beveled edge that marks it as the grab handle.
  dl->AddLine(ImVec2(tMin.x, tMax.y - 1), ImVec2(tMax.x, tMax.y - 1), w98::kDark, 1.0f);
  dl->AddLine(ImVec2(tMin.x, tMax.y), ImVec2(tMax.x, tMax.y), w98::kWhite, 1.0f);
  float textY = tMin.y + (kTitleH - ImGui::GetTextLineHeight()) * 0.5f;
  std::string disp = titleCase(n->type);
  dl->AddText(ImVec2(tMin.x + 5, textY), w98::kCapText, disp.c_str());

  float dragW = tMax.x - tMin.x;
  if (!n->isIo()) {
    const float bs = 12.0f;
    ImVec2 xMin(tMax.x - 3 - bs, tMin.y + 3), xMax(tMax.x - 3, tMin.y + 3 + bs);
    ImGui::SetCursorScreenPos(xMin);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("close", ImVec2(bs, bs))) closeId = n->id;
    if (ImGui::IsItemActivated()) ui.raiseId = n->id;
    w98::fillFace(dl, xMin, xMax);
    w98::raised(dl, xMin, xMax);
    float cx = (xMin.x + xMax.x) * 0.5f, cy = (xMin.y + xMax.y) * 0.5f;
    dl->AddLine(ImVec2(cx - 3, cy - 3), ImVec2(cx + 3, cy + 3), w98::kBlack, 1.2f);
    dl->AddLine(ImVec2(cx - 3, cy + 3), ImVec2(cx + 3, cy - 3), w98::kBlack, 1.2f);

    ImVec2 lMin(xMin.x - 4 - bs, tMin.y + 3), lMax(xMin.x - 4, tMin.y + 3 + bs);
    ImGui::SetCursorScreenPos(lMin);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("byp", ImVec2(bs, bs)))
      n->bypass.store(!bypassed, std::memory_order_relaxed);
    if (ImGui::IsItemActivated()) ui.raiseId = n->id;
    dl->AddRectFilled(lMin, lMax, w98::kBlack);
    ImU32 led = bypassed ? w98::kGray : IM_COL32(64, 224, 96, 255);
    dl->AddRectFilled(ImVec2(lMin.x + 2, lMin.y + 2), ImVec2(lMax.x - 2, lMax.y - 2), led);
    dragW = lMin.x - tMin.x;
  }

  ImGui::SetCursorScreenPos(tMin);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("drag", ImVec2(dragW, kTitleH));
  if (ImGui::IsItemActivated()) ui.raiseId = n->id;
  if (ImGui::IsItemActive()) {
    ImVec2 d = ImGui::GetIO().MouseDelta;
    n->x += d.x;
    n->y += d.y;
  }

  if (!n->isIo() && !n->fx->params().empty()) {
    ImGui::SetCursorScreenPos(ImVec2(box.min.x + 7, box.min.y + kTitleH + 4));
    ImGui::BeginChild("body", ImVec2(kNodeW - 14, box.max.y - box.min.y - kTitleH - 8), false);
    auto& params = n->fx->params();
    for (int pi = 0; pi < (int)params.size(); pi++) {
      const Param& pr = params[pi];
      ImGui::PushID(pi);
      float val = pr.value->load(std::memory_order_relaxed);
      if (pr.kind == Param::Bool) {
        bool b = val >= 0.5f;
        if (w98::checkbox(pr.name, &b)) pr.value->store(b ? 1.0f : 0.0f);
      } else if (pr.kind == Param::Int) {
        int iv = (int)lroundf(val);
        if (w98::sliderInt(pr.name, &iv, (int)pr.min, (int)pr.max)) pr.value->store((float)iv);
      } else {
        float f = val;
        if (w98::sliderFloat(pr.name, &f, pr.min, pr.max)) pr.value->store(f);
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
  }

  if (box.hasIn) {
    dl->AddCircleFilled(box.inPort, kPortR, w98::kWhite);
    dl->AddCircle(box.inPort, kPortR, w98::kBlack);
    ImGui::SetCursorScreenPos(ImVec2(box.inPort.x - kPortR - 2, box.inPort.y - kPortR - 2));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("in", ImVec2((kPortR + 2) * 2, (kPortR + 2) * 2));
  }
  if (box.hasOut) {
    dl->AddCircleFilled(box.outPort, kPortR, w98::kWhite);
    dl->AddCircle(box.outPort, kPortR, w98::kBlack);
    ImGui::SetCursorScreenPos(ImVec2(box.outPort.x - kPortR - 2, box.outPort.y - kPortR - 2));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("out", ImVec2((kPortR + 2) * 2, (kPortR + 2) * 2));
    if (ImGui::IsItemActivated()) {
      ui.wireFrom = n->id;
      ui.raiseId = n->id;
    }
  }

  ImGui::PopID();
}

void drawCanvas(AudioEngine& engine, UiState& ui) {
  ImGui::BeginChild("canvas", ImVec2(0, 0), true);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 origin = ImGui::GetWindowPos();
  ImVec2 csize = ImGui::GetWindowSize();
  ImVec2 mouse = ImGui::GetMousePos();

  // pegboard dots
  for (float gy = 12; gy < csize.y; gy += 18)
    for (float gx = 12; gx < csize.x; gx += 18)
      dl->AddRectFilled(ImVec2(origin.x + gx, origin.y + gy),
                        ImVec2(origin.x + gx + 1, origin.y + gy + 1), w98::kGray);

  std::map<int, NodeBox> boxes;
  for (auto& n : ui.graph.nodes()) {
    float w = n->isIo() ? kIoW : kNodeW;
    float h = nodeHeight(n.get());
    ImVec2 mn(origin.x + n->x, origin.y + n->y), mx(mn.x + w, mn.y + h);
    NodeBox b;
    b.min = mn;
    b.max = mx;
    b.hasIn = n->id != Graph::kInput;
    b.hasOut = n->id != Graph::kOutput;
    b.inPort = ImVec2(mn.x, (mn.y + mx.y) * 0.5f);
    b.outPort = ImVec2(mx.x, (mn.y + mx.y) * 0.5f);
    boxes[n->id] = b;
  }

  // front-most card under the cursor (nodes are drawn back-to-front, so the last
  // match in order is on top). used both to gate cable hover and to raise on click.
  int hoverTop = -1;
  for (auto& n : ui.graph.nodes()) {
    const NodeBox& bx = boxes[n->id];
    if (mouse.x >= bx.min.x && mouse.x <= bx.max.x && mouse.y >= bx.min.y && mouse.y <= bx.max.y)
      hoverTop = n->id;
  }
  bool overNode = hoverTop >= 0;

  // cables (behind nodes); hover highlights, click deletes
  int delFrom = -1, delTo = -1;
  for (auto& w : ui.graph.wires()) {
    ImVec2 a = boxes[w.from].outPort, b = boxes[w.to].inPort;
    float dx = fabsf(b.x - a.x) * 0.5f;
    if (dx < 40.0f) dx = 40.0f;
    ImVec2 c0(a.x + dx, a.y), c1(b.x - dx, b.y);
    bool hov = false;
    if (!overNode && ui.wireFrom < 0) {
      for (int i = 0; i <= 24; i++) {
        if (distSq(bezier(a, c0, c1, b, i / 24.0f), mouse) < 36.0f) {
          hov = true;
          break;
        }
      }
    }
    dl->AddBezierCubic(a, c0, c1, b, hov ? w98::kNavy : w98::kBlack, hov ? 3.0f : 2.0f);
    if (hov && ImGui::IsMouseClicked(0)) {
      delFrom = w.from;
      delTo = w.to;
    }
  }
  if (delFrom >= 0) {
    ui.graph.removeWire(delFrom, delTo);
    rebuildIfRunning(engine, ui);
  }

  int closeId = -1;
  for (auto& n : ui.graph.nodes()) drawNode(engine, ui, n.get(), boxes[n->id], closeId);

  // in-progress cable follows the mouse; drop on an input port to connect
  if (ui.wireFrom >= 0) {
    ImVec2 a = boxes[ui.wireFrom].outPort;
    float dx = fabsf(mouse.x - a.x) * 0.5f;
    if (dx < 40.0f) dx = 40.0f;
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(mouse.x - dx, mouse.y), mouse, w98::kNavy,
                       2.0f);
    if (ImGui::IsMouseReleased(0)) {
      int target = -1;
      for (auto& kv : boxes)
        if (kv.second.hasIn && distSq(kv.second.inPort, mouse) < 100.0f) {
          target = kv.first;
          break;
        }
      if (target >= 0 && ui.graph.addWire(ui.wireFrom, target)) rebuildIfRunning(engine, ui);
      ui.wireFrom = -1;
    }
  }

  // clicking any part of the top card pulls it forward, even a param on a covered one.
  if (hoverTop >= 0 && ui.wireFrom < 0 && ImGui::IsMouseClicked(0)) ui.raiseId = hoverTop;

  if (closeId >= 0) {
    auto dead = ui.graph.removeNode(closeId);
    if (dead) rebuildIfRunning(engine, ui);  // dead frees after the plan swap returns
  }
  if (ui.raiseId >= 0) {
    ui.graph.raiseNode(ui.raiseId);  // clicked node goes on top next frame
    ui.raiseId = -1;
  }

  ImGui::EndChild();
}

bool containsCI(const std::string& hay, const char* needle) {
  if (!needle || !needle[0]) return true;
  std::string h, n;
  for (char c : hay) h.push_back((char)std::tolower((unsigned char)c));
  for (const char* p = needle; *p; p++) n.push_back((char)std::tolower((unsigned char)*p));
  return h.find(n) != std::string::npos;
}

void addSelectedEffect(AudioEngine& engine, UiState& ui) {
  if (ui.addSelected.empty()) return;
  float x = 220.0f + (ui.addCount % 5) * 24.0f, y = 40.0f + (ui.addCount % 5) * 24.0f;
  GraphNode* n = ui.graph.addEffect(ui.addSelected, x, y);
  if (!n) return;
  ui.addCount++;
  if (engine.running()) n->fx->prepare(engine.sampleRate(), (int)engine.bufferFrames());
  rebuildIfRunning(engine, ui);
  ui.status = "added " + titleCase(ui.addSelected);
}

// module picker: friendly two-line rows (name + plain-english blurb), live search,
// single selection. double clicking a row, or Add, drops it on the board.
void drawAddEffect(AudioEngine& engine, UiState& ui) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!beginWindow98("Add an Effect", &ui.showAddEffect, ImVec2(300, 372))) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::TextUnformatted("Pick an effect to add to your board:");
  ImGui::Dummy(ImVec2(1, 4));

  // search row: native find icon sits left of the input.
  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImTextureID find = icons::get(icons::Search);
  float ind = find ? 22.0f : 0.0f;
  if (find) drawIcon(dl, find, ImVec2(sp.x + 8, sp.y + 10), 16.0f);
  if (ImGui::IsWindowAppearing()) {
    ui.addSearch[0] = '\0';
    ui.addSelected.clear();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetCursorScreenPos(ImVec2(sp.x + ind, sp.y));
  ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ind);
  ImGui::InputTextWithHint("##search", "type to filter...", ui.addSearch, sizeof(ui.addSearch));
  ImGui::PopItemWidth();
  ImGui::Dummy(ImVec2(1, 4));

  float btnRow = w98::kButtonH + 12.0f;
  float listH = ImGui::GetContentRegionAvail().y - btnRow;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 1));
  ImGui::BeginChild("list", ImVec2(0, listH), true);
  ImDrawList* ldl = ImGui::GetWindowDrawList();
  ImVec2 listTop = ImGui::GetCursorScreenPos();
  float lineH = ImGui::GetTextLineHeight();
  int shown = 0;
  for (auto& name : ui.available) {
    if (!containsCI(name, ui.addSearch)) continue;
    shown++;
    ImVec2 rp = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    const float h = 32.0f;
    bool clicked = ImGui::InvisibleButton(name.c_str(), ImVec2(w, h));
    bool hl = name == ui.addSelected || ImGui::IsItemHovered();
    if (hl) ldl->AddRectFilled(rp, ImVec2(rp.x + w, rp.y + h), w98::kNavy);
    std::string t = titleCase(name);
    ldl->AddText(ImVec2(rp.x + 8, rp.y + 4), hl ? w98::kCapText : w98::kBlack, t.c_str());
    ldl->AddText(ImVec2(rp.x + 8, rp.y + 4 + lineH + 1), hl ? w98::kLight : w98::kGray,
                 effectBlurb(name));
    ldl->AddLine(ImVec2(rp.x, rp.y + h - 1), ImVec2(rp.x + w, rp.y + h - 1), w98::kLight, 1.0f);
    if (clicked) ui.addSelected = name;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
      ui.addSelected = name;
      addSelectedEffect(engine, ui);
    }
  }
  if (shown == 0)
    ldl->AddText(ImVec2(listTop.x + 4, listTop.y + 4), w98::kGray, "No effects match your search.");
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::Dummy(ImVec2(1, 4));
  const float addW = 78.0f, cancelW = 78.0f, gap = 8.0f;
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - addW - cancelW - gap - 10.0f);
  if (w98::button("Cancel", cancelW)) ui.showAddEffect = false;
  ImGui::SameLine(0.0f, gap);
  w98::beginDisabled(ui.addSelected.empty());
  if (w98::button("Add", addW)) addSelectedEffect(engine, ui);
  w98::endDisabled();
  endWindow98();
}

// small text-entry modal for naming a preset before saving. warns on overwrite.
void drawSavePreset(AudioEngine& engine, UiState& ui) {
  (void)engine;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!beginWindow98("Save Preset", &ui.showSavePreset, ImVec2(300, 178))) return;

  ImGui::TextUnformatted("Give your sound a name:");
  ImGui::Dummy(ImVec2(1, 4));
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::PushItemWidth(-1);
  bool entered = ImGui::InputTextWithHint("##pname", "e.g. My Crunch", ui.presetName,
                                          sizeof(ui.presetName),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopItemWidth();

  std::string name = ui.presetName;
  bool valid = !name.empty() && name.find_first_of("\\/:*?\"<>|") == std::string::npos;
  ImGui::Dummy(ImVec2(1, 2));
  if (!name.empty() && !valid)
    ImGui::TextUnformatted("Please avoid \\ / : * ? \" < > | in the name.");
  else if (valid && preset::exists(name))
    ImGui::TextUnformatted("This name exists. Saving replaces it.");
  else
    ImGui::TextUnformatted("Saved to the presets folder, ready to share.");

  const float w = 78.0f, gap = 8.0f;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - w98::kButtonH - 14.0f);
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 2 * w - gap - 10.0f);
  if (w98::button("Cancel", w)) ui.showSavePreset = false;
  ImGui::SameLine(0.0f, gap);
  w98::beginDisabled(!valid);
  if ((w98::button("Save", w) || (entered && valid))) {
    savePreset(ui, name);
    ui.showSavePreset = false;
  }
  w98::endDisabled();
  endWindow98();
}

// generic yes/no modal with a native warning icon; runs the pending action on Yes.
void drawConfirm(AudioEngine& engine, UiState& ui) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!beginWindow98(ui.confirmTitle.c_str(), &ui.showConfirm, ImVec2(300, 150))) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImTextureID warn = icons::get(icons::Warning, true);
  float ind = 0.0f;
  if (warn) {
    drawIcon(dl, warn, ImVec2(p.x + 16, p.y + 16), 32.0f);
    ind = 44.0f;
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x + ind, p.y + 4));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 4.0f);
  ImGui::TextWrapped("%s", ui.confirmText.c_str());
  ImGui::PopTextWrapPos();

  const float w = 78.0f, gap = 8.0f;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - w98::kButtonH - 14.0f);
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 2 * w - gap - 10.0f);
  if (w98::button("Cancel", w)) ui.showConfirm = false;
  ImGui::SameLine(0.0f, gap);
  if (w98::button(ui.confirmYes.c_str(), w)) {
    if (ui.confirmAction == 1) newRig(engine, ui);
    if (ui.confirmAction == 2 && preset::remove(ui.confirmArg)) {
      if (ui.currentPreset == ui.confirmArg) ui.currentPreset.clear();
      ui.presets = preset::list();
      ui.status = "deleted preset: " + ui.confirmArg;
    }
    ui.showConfirm = false;
  }
  endWindow98();
}

void drawContent(AudioEngine& engine, UiState& ui) { drawCanvas(engine, ui); }

// the presets dropdown: load any saved rig, save the current one, or start fresh.
// friendly, one obvious action per row; destructive rows route through a confirm.
void drawPresetsMenu(AudioEngine& engine, UiState& ui) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3, 3));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  if (ImGui::BeginPopup("##presetmenu", ImGuiWindowFlags_NoMove)) {
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    pdl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), w98::kFace);
    w98::raised(pdl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));

    if (ui.presets.empty()) {
      w98::beginDisabled(true);
      w98::menuItem("(no presets saved yet)");
      w98::endDisabled();
    } else {
      for (auto& p : ui.presets)
        if (w98::menuItem(titleCase(p).c_str())) loadPreset(engine, ui, p);
    }
    w98::separator();
    if (w98::menuItem("Save as New...")) {
      ui.presetName[0] = '\0';
      ui.showSavePreset = true;
    }
    if (!ui.currentPreset.empty()) {
      std::string upd = "Update \"" + titleCase(ui.currentPreset) + "\"";
      if (w98::menuItem(upd.c_str())) savePreset(ui, ui.currentPreset);
      std::string del = "Delete \"" + titleCase(ui.currentPreset) + "\"...";
      if (w98::menuItem(del.c_str())) {
        ui.confirmTitle = "Delete Preset";
        ui.confirmText = "Delete the preset \"" + titleCase(ui.currentPreset) +
                         "\"? This removes the file for good.";
        ui.confirmYes = "Delete";
        ui.confirmAction = 2;
        ui.confirmArg = ui.currentPreset;
        ui.showConfirm = true;
      }
    }
    w98::separator();
    if (w98::menuItem("New Empty Rig...")) {
      ui.confirmTitle = "New Rig";
      ui.confirmText = "Clear the board and start over? Save first if you want to keep this sound.";
      ui.confirmYes = "Start New";
      ui.confirmAction = 1;
      ui.confirmArg.clear();
      ui.showConfirm = true;
    }
    if (w98::menuItem("Open Presets Folder"))
      ::ShellExecuteA(nullptr, "open", preset::dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(3);
}

// actions bar under the menu: transport, presets, add effect, in/out volume, bypass.
void drawToolbar(ImVec2 pos, float w, AudioEngine& engine, UiState& ui) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float f = w98::kFrame;
  ImVec2 a(pos.x + f, pos.y + f + w98::kCaption + w98::kMenuBar);
  ImVec2 b(pos.x + w - f, a.y + w98::kToolbar);
  dl->AddRectFilled(a, b, w98::kFace);
  dl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), w98::kGray, 1.0f);

  float lineH = ImGui::GetTextLineHeight();
  float midY = (a.y + b.y) * 0.5f;
  const float bh = 24.0f;
  float by = a.y + (w98::kToolbar - bh) * 0.5f;
  float x = a.x + 6.0f;

  auto vsep = [&](float sx) {
    dl->AddLine(ImVec2(sx, a.y + 5), ImVec2(sx, b.y - 5), w98::kGray, 1.0f);
    dl->AddLine(ImVec2(sx + 1, a.y + 5), ImVec2(sx + 1, b.y - 5), w98::kWhite, 1.0f);
  };
  auto tip = [](const char* t) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t);
  };

  // start / stop
  ImGui::SetCursorScreenPos(ImVec2(x, by));
  bool run = ui.running;
  if (w98::toolButton("##ss", 40.0f, bh, false)) {
    if (run) {
      engine.stop();
      ui.running = false;
      ui.status = "stopped";
    } else {
      startEngine(engine, ui);
    }
  }
  tip(run ? "Stop audio" : "Start audio");
  float gx = x + 20.0f, gy = by + bh * 0.5f;
  if (run)
    dl->AddRectFilled(ImVec2(gx - 4, gy - 4), ImVec2(gx + 4, gy + 4), IM_COL32(176, 0, 0, 255));
  else
    dl->AddTriangleFilled(ImVec2(gx - 4, gy - 5), ImVec2(gx - 4, gy + 5), ImVec2(gx + 5, gy),
                          IM_COL32(0, 128, 0, 255));
  x += 40.0f + 8.0f;
  vsep(x);
  x += 8.0f;

  // add effect: plus glyph + label
  {
    const char* label = "Add Effect";
    ImVec2 ts = ImGui::CalcTextSize(label);
    float bw = 8.0f + 12.0f + 6.0f + ts.x + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    bool clicked = w98::toolButton("##add", bw, bh, false);
    tip("Add an effect to your board");
    bool held = ImGui::IsItemActive();
    float push = held ? 1.0f : 0.0f;
    plusGlyph(dl, ImVec2(x + 14 + push, by + bh * 0.5f + push), w98::kBlack);
    dl->AddText(ImVec2(x + 26 + push, by + (bh - ts.y) * 0.5f + push), w98::kBlack, label);
    if (clicked) ui.showAddEffect = true;
    x += bw + 6.0f;
  }

  // presets: folder icon + label + drop arrow, opens the dropdown
  {
    const char* label = "Presets";
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImTextureID folder = icons::get(icons::Presets);
    float bw = 8.0f + 16.0f + 6.0f + ts.x + 6.0f + 9.0f + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    bool wasOpen = ImGui::IsPopupOpen("##presetmenu");
    bool clicked = w98::toolButton("##presets", bw, bh, wasOpen);
    tip("Load, save or share your sounds");
    float push = (ImGui::IsItemActive() || wasOpen) ? 1.0f : 0.0f;
    if (folder)
      drawIcon(dl, folder, ImVec2(x + 16 + push, by + bh * 0.5f + push), 16.0f);
    else
      folderGlyph(dl, ImVec2(x + 16 + push, by + bh * 0.5f + push), w98::kBlack);
    float tx = x + 26 + push;
    dl->AddText(ImVec2(tx, by + (bh - ts.y) * 0.5f + push), w98::kBlack, label);
    float ax = tx + ts.x + 8.0f, ay = by + bh * 0.5f + push;
    dl->AddTriangleFilled(ImVec2(ax - 3, ay - 1.5f), ImVec2(ax + 3, ay - 1.5f), ImVec2(ax, ay + 2.5f),
                          w98::kBlack);
    if (clicked && !wasOpen) {
      ui.presets = preset::list();
      ImGui::OpenPopup("##presetmenu");
    }
    ImGui::SetNextWindowPos(ImVec2(x, b.y));
    ImGui::SetNextWindowSize(ImVec2(bw > 172.0f ? bw : 172.0f, 0));
    drawPresetsMenu(engine, ui);
    x += bw + 8.0f;
  }

  vsep(x);
  x += 8.0f;

  // input / output volume, with a native audio icon and a live percentage readout
  ImTextureID au = icons::get(icons::Audio);
  if (au) {
    drawIcon(dl, au, ImVec2(x + 8, midY), 16.0f);
    x += 20.0f;
  }

  auto volControl = [&](const char* id, const char* label, float* v, void (AudioEngine::*set)(float)) {
    dl->AddText(ImVec2(x, midY - lineH * 0.5f), w98::kBlack, label);
    x += ImGui::CalcTextSize(label).x + 5.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, a.y + (w98::kToolbar - 18.0f) * 0.5f));
    if (w98::toolSlider(id, v, 0.0f, 2.0f, 66.0f)) (engine.*set)(*v);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s volume: %d%%", label, (int)(*v * 100.0f + 0.5f));
    x += 66.0f + 4.0f;
    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(*v * 100.0f + 0.5f));
    dl->AddText(ImVec2(x, midY - lineH * 0.5f), w98::kBlack, pct);
    x += 34.0f + 8.0f;
  };
  volControl("##invol", "In", &ui.inVol, &AudioEngine::setInputGain);
  volControl("##outvol", "Out", &ui.outVol, &AudioEngine::setOutputGain);

  // global bypass, pinned right
  const float bypW = 72.0f;
  float bypX = b.x - 6.0f - bypW;
  ImGui::SetCursorScreenPos(ImVec2(bypX, by));
  if (w98::toolButton("##bypass", bypW, bh, ui.bypassAll)) {
    ui.bypassAll = !ui.bypassAll;
    engine.setBypass(ui.bypassAll);
  }
  tip(ui.bypassAll ? "Effects off - hearing your dry signal" : "Turn all effects off at once");
  ImVec2 lts = ImGui::CalcTextSize("Bypass");
  dl->AddText(ImVec2(bypX + (bypW - lts.x) * 0.5f + (ui.bypassAll ? 1.0f : 0.0f),
                     by + (bh - lts.y) * 0.5f + (ui.bypassAll ? 1.0f : 0.0f)),
              ui.bypassAll ? IM_COL32(176, 0, 0, 255) : w98::kBlack, "Bypass");
}

// sunken status strip at the window bottom: message, latency and state cells, gripper.
void drawStatusBar(ImVec2 pos, float w, float y0, const UiState& ui, AudioEngine& engine) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float f = w98::kFrame;
  ImVec2 a(pos.x + f, pos.y + y0);
  ImVec2 b(pos.x + w - f, pos.y + y0 + w98::kStatus);
  dl->AddRectFilled(a, b, w98::kFace);

  float th = ImGui::GetTextLineHeight();
  float ty = a.y + (w98::kStatus - th) * 0.5f;

  // round-trip latency the player actually feels is ~two buffer periods (in + out).
  double sr = ui.running ? (double)engine.sampleRate() : 48000.0;
  double frames = ui.running ? (double)engine.bufferFrames() : (double)ui.bufferFrames;
  double latencyMs = sr > 0.0 ? 2000.0 * frames / sr : 0.0;

  ImVec2 c1a(a.x, a.y + 1), c1b(b.x - 238, b.y - 1);
  w98::sunken(dl, c1a, c1b);
  std::string msg = ui.status.empty() ? "Ready" : ui.status;
  dl->PushClipRect(ImVec2(c1a.x + 3, c1a.y), ImVec2(c1b.x - 3, c1b.y), true);
  dl->AddText(ImVec2(c1a.x + 5, ty), w98::kBlack, msg.c_str());
  dl->PopClipRect();

  ImVec2 c2a(b.x - 234, a.y + 1), c2b(b.x - 138, b.y - 1);
  w98::sunken(dl, c2a, c2b);
  char lat[32];
  snprintf(lat, sizeof(lat), "Latency: %.1f ms", latencyMs);
  dl->AddText(ImVec2(c2a.x + 5, ty), w98::kBlack, lat);

  ImVec2 c3a(b.x - 134, a.y + 1), c3b(b.x - 16, b.y - 1);
  w98::sunken(dl, c3a, c3b);
  char state[64];
  if (ui.running)
    snprintf(state, sizeof(state), "Running  xruns: %u", engine.xruns());
  else
    snprintf(state, sizeof(state), "Stopped");
  dl->AddText(ImVec2(c3a.x + 5, ty), w98::kBlack, state);

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j <= i; j++) {
      float gx = b.x - 4 - i * 4;
      float gy = b.y - 4 - j * 4;
      dl->AddRectFilled(ImVec2(gx + 1, gy + 1), ImVec2(gx + 3, gy + 3), w98::kWhite);
      dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + 2, gy + 2), w98::kGray);
    }
  }
}

void drawMain(AudioEngine& engine, UiState& ui) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoBringToFrontOnFocus;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  bool open = ImGui::Begin("guitarpedal", nullptr, flags);
  ImGui::PopStyleVar(2);
  if (!open) {
    ImGui::End();
    return;
  }

  ImVec2 pos = ImGui::GetWindowPos();
  ImVec2 size = ImGui::GetWindowSize();
  const float f = w98::kFrame;

  ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                            w98::kFace);
  w98::raised(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + size.x, pos.y + size.y));

  drawTitleBar(pos, size.x);
  drawMenuBar(pos, size.x, ui);
  drawToolbar(pos, size.x, engine, ui);

  float top = f + w98::kCaption + w98::kMenuBar + w98::kToolbar;
  float childH = size.y - top - w98::kStatus - f;
  ImGui::SetCursorPos(ImVec2(f, top));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
  ImGui::BeginChild("content", ImVec2(size.x - 2 * f, childH), false);
  drawContent(engine, ui);
  ImGui::EndChild();
  ImGui::PopStyleVar();

  drawStatusBar(pos, size.x, top + childH, ui, engine);

  ImGui::End();
}

}  // namespace

// ---- entry point ----

namespace gui {

int run(AudioEngine& engine) {
  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, ::GetModuleHandle(nullptr),
                    nullptr, nullptr, nullptr, nullptr, L"guitarpedal", nullptr};
  ::RegisterClassExW(&wc);
  // WS_POPUP drops the overlapped frame; WS_THICKFRAME/WS_CAPTION keep native resize,
  // snap, drop shadow and min/max animations while WM_NCCALCSIZE hides the visual frame.
  DWORD winStyle = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MAXIMIZEBOX |
                   WS_MINIMIZEBOX;
  g_hwnd = ::CreateWindowW(wc.lpszClassName, L"guitarpedal", winStyle, 100, 100, 720, 560, nullptr,
                           nullptr, wc.hInstance, nullptr);
  // force WM_NCCALCSIZE to re-run so the frame is stripped before the window is shown.
  ::SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

  if (!createDevice(g_hwnd)) {
    cleanupDevice();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(g_hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // windows can leave the root window

  // native win98 ui font; falls back to the built-in font if unavailable.
  const char* uiFonts[] = {"C:\\Windows\\Fonts\\micross.ttf", "C:\\Windows\\Fonts\\tahoma.ttf"};
  for (const char* path : uiFonts)
    if (io.Fonts->AddFontFromFileTTF(path, 13.0f)) break;

  applyWin98Style();
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg].w = 1.0f;  // keep torn-out os windows opaque

  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);
  icons::init(g_device);

  UiState ui;
  refreshDevices(engine, ui);
  ui.presets = preset::list();

  Config cfg = config::load();  // empty selection if the file does not exist
  ui.bufferFrames = cfg.bufferFrames;
  ui.showConsole = cfg.showConsole;
  ui.inSel = indexOfDevice(ui.inputs, cfg.inputDevice);
  ui.outSel = indexOfDevice(ui.outputs, cfg.outputDevice);
  applyConsole(ui.showConsole);

  bool done = false;
  while (!done) {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;

    if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
      ::Sleep(10);
      continue;
    }
    g_occluded = false;

    if (g_resizeW != 0 && g_resizeH != 0) {
      releaseTarget();
      g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
      g_resizeW = g_resizeH = 0;
      createTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    drawMain(engine, ui);
    if (ui.showSettings) drawSettings(engine, ui);
    if (ui.showAbout) drawAbout(ui);
    if (ui.showAddEffect) drawAddEffect(engine, ui);
    if (ui.showSavePreset) drawSavePreset(engine, ui);
    if (ui.showConfirm) drawConfirm(engine, ui);

    ImGui::Render();
    const float clear[4] = {0.0f, 0.5f, 0.5f, 1.0f};  // teal 98 desktop behind torn-out windows
    g_context->OMSetRenderTargets(1, &g_targetView, nullptr);
    g_context->ClearRenderTargetView(g_targetView, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
    }

    HRESULT hr = g_swapChain->Present(1, 0);  // vsync
    g_occluded = (hr == DXGI_STATUS_OCCLUDED);
  }

  engine.stop();
  icons::shutdown();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  cleanupDevice();
  ::DestroyWindow(g_hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

}  // namespace gui
