#include "gui.h"

#include <d3d11.h>
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "audio.h"
#include "config.h"
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

struct UiState {
  std::vector<AudioDevice> inputs;
  std::vector<AudioDevice> outputs;
  std::vector<std::string> available;  // effect names from the registry
  std::vector<std::string> chain;      // effect names the user has queued, in order
  int inSel = -1;   // -1 = nothing selected
  int outSel = -1;
  int bufferFrames = 256;
  bool running = false;
  bool showSettings = false;
  bool showAbout = false;
  bool showConsole = false;
  std::string status;
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
  std::vector<std::string> unknown;
  engine.setChain(ui.chain, unknown);
  if (!engine.start(ui.inputs[ui.inSel].id, ui.outputs[ui.outSel].id, 48000,
                    (unsigned int)ui.bufferFrames)) {
    ui.status = "failed to open stream";
    return;
  }
  ui.running = true;
  ui.status = "running @ " + std::to_string(engine.sampleRate()) + " Hz, " +
              std::to_string(engine.bufferFrames()) + " frames";
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

void drawContent(AudioEngine& engine, UiState& ui) {
  w98::separatorText("effect chain");
  w98::beginDisabled(ui.running);  // chain is rebuilt on start; lock it while live
  ImGui::TextDisabled("available:");
  for (auto& name : ui.available) {
    ImGui::SameLine();
    if (w98::smallButton(("+ " + name).c_str())) ui.chain.push_back(name);
  }
  if (ui.chain.empty()) {
    ImGui::TextDisabled("(chain empty - dry signal passthrough)");
  } else {
    for (int i = 0; i < (int)ui.chain.size(); i++) {
      ImGui::PushID(i);
      ImGui::Text("%d. %s", i + 1, ui.chain[i].c_str());
      ImGui::SameLine();
      if (w98::smallButton("x")) {
        ui.chain.erase(ui.chain.begin() + i);
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
  }
  w98::endDisabled();

  w98::separatorText("transport");
  if (!ui.running) {
    if (w98::button("Start", 120.0f)) startEngine(engine, ui);
  } else {
    if (w98::button("Stop", 120.0f)) {
      engine.stop();
      ui.running = false;
      ui.status = "stopped";
    }
    ImGui::SameLine();
    ImGui::Text("xruns: %u", engine.xruns());
  }
}

// sunken status strip at the window bottom: message cell, state cell, resize gripper.
void drawStatusBar(ImVec2 pos, float w, float y0, const UiState& ui, AudioEngine& engine) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float f = w98::kFrame;
  ImVec2 a(pos.x + f, pos.y + y0);
  ImVec2 b(pos.x + w - f, pos.y + y0 + w98::kStatus);
  dl->AddRectFilled(a, b, w98::kFace);

  float th = ImGui::GetTextLineHeight();
  float ty = a.y + (w98::kStatus - th) * 0.5f;

  ImVec2 c1a(a.x, a.y + 1), c1b(b.x - 132, b.y - 1);
  w98::sunken(dl, c1a, c1b);
  std::string msg = ui.status.empty() ? "Ready" : ui.status;
  dl->PushClipRect(ImVec2(c1a.x + 3, c1a.y), ImVec2(c1b.x - 3, c1b.y), true);
  dl->AddText(ImVec2(c1a.x + 5, ty), w98::kBlack, msg.c_str());
  dl->PopClipRect();

  ImVec2 c2a(b.x - 128, a.y + 1), c2b(b.x - 16, b.y - 1);
  w98::sunken(dl, c2a, c2b);
  char state[64];
  if (ui.running)
    snprintf(state, sizeof(state), "running  xruns: %u", engine.xruns());
  else
    snprintf(state, sizeof(state), "stopped");
  dl->AddText(ImVec2(c2a.x + 5, ty), w98::kBlack, state);

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

  float top = f + w98::kCaption + w98::kMenuBar;
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

  UiState ui;
  refreshDevices(engine, ui);

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
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  cleanupDevice();
  ::DestroyWindow(g_hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

}  // namespace gui
