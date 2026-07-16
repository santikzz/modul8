#include "gui.h"

#include <d3d11.h>
#include <windows.h>
#include <windowsx.h>

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

bool titleButton(const char* label, float w, float h) {
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  bool clicked = ImGui::Button(label, ImVec2(w, h));
  ImGui::PopStyleColor();
  return clicked;
}

// custom caption: title text + min/maximize/close. sets g_titleBarH / g_captionRight
// so wndProc knows the drag region vs the window buttons.
void drawTitleBar() {
  const float barH = 32.0f;
  g_titleBarH = (int)barH;

  ImVec2 pos = ImGui::GetWindowPos();
  float w = ImGui::GetWindowWidth();
  ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + barH),
                                            IM_COL32(30, 30, 36, 255));

  ImGui::SetCursorPos(ImVec2(12, (barH - ImGui::GetTextLineHeight()) * 0.5f));
  ImGui::TextUnformatted("guitarpedal");

  const float btnW = 46.0f;
  const int nBtn = 3;
  float start = w - btnW * nBtn;
  g_captionRight = (int)start;

  ImGui::SetCursorPos(ImVec2(start, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  if (titleButton("_", btnW, barH)) ::ShowWindow(g_hwnd, SW_MINIMIZE);
  ImGui::SameLine(0, 0);
  bool zoomed = ::IsZoomed(g_hwnd);
  if (titleButton(zoomed ? "[o]" : "[ ]", btnW, barH))
    ::ShowWindow(g_hwnd, zoomed ? SW_RESTORE : SW_MAXIMIZE);
  ImGui::SameLine(0, 0);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 40, 40, 255));
  if (titleButton("X", btnW, barH)) ::PostMessage(g_hwnd, WM_CLOSE, 0, 0);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  ImGui::SetCursorPos(ImVec2(0, barH));
}

void deviceCombo(const char* label, const std::vector<AudioDevice>& devs, int& sel) {
  const char* preview = (sel >= 0 && sel < (int)devs.size()) ? devs[sel].name.c_str()
                        : (devs.empty() ? "(none)" : "(select)");
  if (!ImGui::BeginCombo(label, preview)) return;
  for (int i = 0; i < (int)devs.size(); i++) {
    bool selected = i == sel;
    std::string item = devs[i].name + (devs[i].isDefault ? " *default" : "");
    if (ImGui::Selectable(item.c_str(), selected)) sel = i;
    if (selected) ImGui::SetItemDefaultFocus();
  }
  ImGui::EndCombo();
}

void drawSettings(UiState& ui) {
  if (!ImGui::Begin("Settings", &ui.showSettings)) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("audio");
  ImGui::BeginDisabled(ui.running);  // devices + buffer are locked while streaming
  deviceCombo("input", ui.inputs, ui.inSel);
  deviceCombo("output", ui.outputs, ui.outSel);
  ImGui::SliderInt("buffer frames", &ui.bufferFrames, 32, 1024);
  ImGui::EndDisabled();
  ImGui::TextUnformatted("sample rate: 48000 Hz");

  ImGui::Separator();
  ImGui::TextDisabled("app");
  if (ImGui::Checkbox("show debug console", &ui.showConsole)) applyConsole(ui.showConsole);

  ImGui::Separator();
  if (ImGui::Button("Save")) {
    saveConfig(ui);
    ui.status = "config saved";
  }
  ImGui::End();
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

void drawContent(AudioEngine& engine, UiState& ui) {
  if (ImGui::Button(ui.showSettings ? "hide settings" : "settings"))
    ui.showSettings = !ui.showSettings;

  ImGui::SeparatorText("devices");
  ImGui::BeginDisabled(ui.running);  // can't switch devices mid-stream
  deviceCombo("input", ui.inputs, ui.inSel);
  deviceCombo("output", ui.outputs, ui.outSel);
  if (ImGui::Button("refresh")) refreshDevices(engine, ui);
  ImGui::EndDisabled();

  ImGui::SeparatorText("effect chain");
  ImGui::BeginDisabled(ui.running);  // chain is rebuilt on start; lock it while live
  ImGui::TextDisabled("available:");
  for (auto& name : ui.available) {
    ImGui::SameLine();
    if (ImGui::SmallButton(("+ " + name).c_str())) ui.chain.push_back(name);
  }
  if (ui.chain.empty()) {
    ImGui::TextDisabled("(chain empty - dry signal passthrough)");
  } else {
    for (int i = 0; i < (int)ui.chain.size(); i++) {
      ImGui::PushID(i);
      ImGui::Text("%d. %s", i + 1, ui.chain[i].c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        ui.chain.erase(ui.chain.begin() + i);
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
  }
  ImGui::EndDisabled();

  ImGui::SeparatorText("transport");
  if (!ui.running) {
    if (ImGui::Button("Start", ImVec2(120, 0))) startEngine(engine, ui);
  } else {
    if (ImGui::Button("Stop", ImVec2(120, 0))) {
      engine.stop();
      ui.running = false;
      ui.status = "stopped";
    }
    ImGui::SameLine();
    ImGui::Text("xruns: %u", engine.xruns());
  }
  if (!ui.status.empty()) ImGui::TextUnformatted(ui.status.c_str());
}

void drawMain(AudioEngine& engine, UiState& ui) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoBringToFrontOnFocus;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool open = ImGui::Begin("guitarpedal", nullptr, flags);
  ImGui::PopStyleVar();
  if (!open) {
    ImGui::End();
    return;
  }

  drawTitleBar();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
  ImGui::BeginChild("content", ImVec2(0, 0), false);
  drawContent(engine, ui);
  ImGui::EndChild();
  ImGui::PopStyleVar();

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

  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;  // keep torn-out os windows crisp
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }

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
    if (ui.showSettings) drawSettings(ui);

    ImGui::Render();
    const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
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
