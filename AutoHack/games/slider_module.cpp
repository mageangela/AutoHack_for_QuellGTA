#define NOMINMAX

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cwchar>

namespace gta5::games::slider {
namespace {

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT WM_APP_WORKER_DONE = WM_APP + 3;
constexpr int kHotkeyToggleId = 2001;
constexpr double kBaselineScreenHeightPx = 1080.0;
constexpr double kEdgeActionWindowSeconds = 0.035;
constexpr double kEdgeTriggerZonePx = 80.0;
constexpr double kMinUsableVelocityPxPerSec = 25.0;
constexpr double kLateGraceSeconds = 0.010;
constexpr double kInvalidPredictionSeconds = 999.0;
constexpr int kHudWidth = 300;
constexpr int kHudCollapsedHeight = 88;
constexpr int kHudExpandedHeight = 88;
constexpr int kHudMargin = 18;
constexpr int kHudTopMargin = 118;
constexpr int kCursorSize = 64;
constexpr int kCursorArrowTopOffset = 14;
constexpr int kCursorArrowBottomOffset = 14;
constexpr int kCursorArrowRightOffset = 14;

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

int ScaledPx(double value, double scale) {
  return std::max(1, static_cast<int>(std::round(value * std::clamp(scale, 0.45, 2.25))));
}

struct RectI {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct RedBar {
  bool ok = false;
  int x1 = 0;
  int x2 = 0;
  int y1 = 0;
  int y2 = 0;
  int centerY = 0;
  int height = 0;
};

struct BarMeasure {
  bool ok = false;
  int x1 = 0;
  int x2 = 0;
  int topY1 = 0;
  int topY2 = 0;
  int bottomY1 = 0;
  int bottomY2 = 0;
  double gapCenterY = 0.0;
};

struct FrameAnalysis {
  bool ok = false;
  bool inMinigame = false;
  RedBar red;
  std::vector<BarMeasure> bars;
  std::wstring minigameStatus = L"waiting minigame";
  std::wstring minigameLog;
};

struct YellowMeasure {
  bool ok = false;
  int index = -1;
  int score = 0;
  int topBottomY = 0;
  int bottomTopY = 0;
  double gapCenterY = 0.0;
};

struct SearchCells {
  bool ok = false;
  std::vector<std::pair<int, int>> xRanges;
};

struct CaptureFrame {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  std::vector<uint32_t> bgra;
};

struct TrackSample {
  double center = 0.0;
  std::chrono::steady_clock::time_point time;
};

struct TrackSlot {
  bool valid = false;
  double lastCenter = 0.0;
  double velocity = 0.0;
  std::vector<TrackSample> history;
};

struct PreviewBar {
  int x1 = 0;
  int x2 = 0;
  int topY1 = 0;
  int topY2 = 0;
  int bottomY1 = 0;
  int bottomY2 = 0;
  bool active = false;
  bool moving = false;
};

struct PreviewState {
  bool hasFrame = false;
  bool hasRed = false;
  bool hasYellow = false;
  bool running = false;
  RedBar red;
  YellowMeasure yellow;
  std::vector<PreviewBar> bars;
  std::wstring status;
  std::wstring lastLog;
  double edgeError = 0.0;
  double triggerTimeSec = kInvalidPredictionSeconds;
  double leadSec = 0.0;
  double velocity = 0.0;
  double scale = 1.0;
  int screenX = 0;
  int screenY = 0;
  int screenW = 0;
  int screenH = 0;
};

HWND g_mainWnd = nullptr;
HWND g_overlayWnd = nullptr;
HWND g_cursorWnd = nullptr;
HWND g_marksWnd = nullptr;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stopWorker{false};
std::atomic<bool> g_overlayRepaintPending{false};
std::atomic<bool> g_marksRepaintPending{false};
std::thread g_worker;
std::mutex g_previewMutex;
PreviewState g_preview;
std::atomic<int> g_hudActiveBar{0};
std::atomic<int> g_hudEdgePx{0};
std::atomic<int> g_hudTtcMs{-1};
std::atomic<int> g_hudDtMs{0};
std::atomic<int> g_hudLeadMs{0};
std::atomic<int> g_hudVelocityPx{0};
std::atomic<int> g_hudScale100{100};
std::atomic<int> g_hotkeyVk{VK_F6};
std::atomic<int> g_pendingHotkeyVk{VK_F6};
std::atomic<int> g_tapHoldMs{8};
std::atomic<int> g_tapGapMs{18};
std::atomic<bool> g_listeningHotkey{false};
std::atomic<bool> g_settingsExpanded{false};
std::atomic<bool> g_overlayCursorEnabled{true};
std::atomic<bool> g_cursorVisible{false};
std::atomic<int> g_cursorX{0};
std::atomic<int> g_cursorY{0};
std::atomic<int> g_cursorTargetY{0};
std::atomic<bool> g_cursorInZone{false};
std::atomic<int> g_cursorBar{0};
bool g_hudUserPlaced = false;
POINT g_hudPos{0, 0};
bool g_hudDragging = false;
POINT g_hudDragOffset{0, 0};

int CurrentHudHeight() {
  return g_settingsExpanded.load(std::memory_order_relaxed) ? kHudExpandedHeight : kHudCollapsedHeight;
}

RECT MonitorRectFromHandle(HMONITOR monitor) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (monitor && GetMonitorInfoW(monitor, &mi)) return mi.rcMonitor;
  RECT primary{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  return primary;
}

// All tunable pixel constants are authored for a 1080p game image.
// Runtime scaling uses the physical monitor height (rcMonitor), not rcWork,
// so taskbars and reserved desktop areas never shrink the game geometry.
double GeometryScaleFromScreenPoint(int screenX, int screenY) {
  POINT p{screenX, screenY};
  RECT mr = MonitorRectFromHandle(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST));
  const int h = static_cast<int>(mr.bottom - mr.top);
  return h > 0 ? std::clamp(h / kBaselineScreenHeightPx, 0.45, 2.25) : 1.0;
}

double GeometryScaleFromRedScreen(const RedBar& red) {
  if (!red.ok) return std::clamp(GetSystemMetrics(SM_CYSCREEN) / kBaselineScreenHeightPx, 0.45, 2.25);
  return GeometryScaleFromScreenPoint((red.x1 + red.x2) / 2, red.centerY);
}

std::wstring FileNameOnly(const wchar_t* path) {
  if (!path) return L"";
  const wchar_t* slash = wcsrchr(path, L'\\');
  const wchar_t* fwd = wcsrchr(path, L'/');
  const wchar_t* base = std::max(slash ? slash + 1 : path, fwd ? fwd + 1 : path);
  return base;
}

std::wstring ExeSiblingPath(const wchar_t* fileName) {
  wchar_t path[MAX_PATH * 4]{};
  DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return fileName;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (slash) *(slash + 1) = L'\0';
  return std::wstring(path) + fileName;
}

bool IsValidHotkeyVk(int vk) {
  if (vk < 8 || vk > 254) return false;
  if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) return false;
  return vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU;
}

int ClampTapMs(int value) {
  return std::clamp(value, 1, 250);
}

std::wstring KeyName(int vk) {
  if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
  if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
  if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));
  UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  wchar_t name[64]{};
  if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return name;
  return L"VK" + std::to_wstring(vk);
}

void SaveSettings() {
    const std::wstring path = ExeSiblingPath(L"QuellGTA.ini");

    // 使用 Windows API 写入 INI 文件
    WritePrivateProfileStringW(L"AutoHack", L"overlay_cursor",
        g_overlayCursorEnabled.load(std::memory_order_relaxed) ? L"1" : L"0",
        path.c_str());
    WritePrivateProfileStringW(L"AutoHack", L"tap_hold_ms",
        std::to_wstring(g_tapHoldMs.load(std::memory_order_relaxed)).c_str(),
        path.c_str());
    WritePrivateProfileStringW(L"AutoHack", L"tap_gap_ms",
        std::to_wstring(g_tapGapMs.load(std::memory_order_relaxed)).c_str(),
        path.c_str());
}

void LoadSettings() {
    int overlay = 1;
    int tapHold = 8;
    int tapGap = 18;
    bool needsSave = false;

    const std::wstring path = ExeSiblingPath(L"QuellGTA.ini");

    wchar_t buffer[64] = { 0 };

    GetPrivateProfileStringW(L"AutoHack", L"overlay_cursor", L"1", buffer, 64, path.c_str());
    overlay = _wtoi(buffer);

    GetPrivateProfileStringW(L"AutoHack", L"tap_hold_ms", L"16", buffer, 64, path.c_str());
    tapHold = _wtoi(buffer);

    GetPrivateProfileStringW(L"AutoHack", L"tap_gap_ms", L"36", buffer, 64, path.c_str());
    tapGap = _wtoi(buffer);

    if ((overlay != 0 && overlay != 1)) {
        overlay = 1;
        needsSave = true;
    }
    if (tapHold < 1 || tapHold > 250) {
        tapHold = 8;
        needsSave = true;
    }
    if (tapGap < 1 || tapGap > 250) {
        tapGap = 18;
        needsSave = true;
    }

    g_overlayCursorEnabled.store(overlay != 0, std::memory_order_relaxed);
    g_tapHoldMs.store(ClampTapMs(tapHold), std::memory_order_relaxed);
    g_tapGapMs.store(ClampTapMs(tapGap), std::memory_order_relaxed);

    if (needsSave) SaveSettings();
}

void ApplyHotkeySetting(HWND hwnd) {
  if (!hwnd) return;
  UnregisterHotKey(hwnd, kHotkeyToggleId);
  RegisterHotKey(hwnd, kHotkeyToggleId, MOD_NOREPEAT, static_cast<UINT>(g_hotkeyVk.load(std::memory_order_relaxed)));
}

bool ProcessExeNameEquals(DWORD pid, const wchar_t* exeName) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  wchar_t imagePath[MAX_PATH * 4]{};
  DWORD size = static_cast<DWORD>(std::size(imagePath));
  const bool ok = QueryFullProcessImageNameW(process, 0, imagePath, &size) &&
                  lstrcmpiW(FileNameOnly(imagePath).c_str(), exeName) == 0;
  CloseHandle(process);
  return ok;
}

struct WindowByExeSearch {
  const wchar_t* exeName = nullptr;
  HWND hwnd = nullptr;
  RECT rect{};
};

BOOL CALLBACK EnumWindowByExeProc(HWND hwnd, LPARAM lParam) {
  auto* search = reinterpret_cast<WindowByExeSearch*>(lParam);
  if (!IsWindowVisible(hwnd) || hwnd == g_mainWnd || hwnd == g_overlayWnd || hwnd == g_cursorWnd || hwnd == g_marksWnd ||
      GetWindow(hwnd, GW_OWNER)) {
    return TRUE;
  }
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr) || wr.right - wr.left < 100 || wr.bottom - wr.top < 100) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid && ProcessExeNameEquals(pid, search->exeName)) {
    search->hwnd = hwnd;
    search->rect = wr;
    return FALSE;
  }
  return TRUE;
}

bool FindWindowRectByExe(const wchar_t* exeName, RECT* rect) {
  WindowByExeSearch search{exeName};
  EnumWindows(EnumWindowByExeProc, reinterpret_cast<LPARAM>(&search));
  if (!search.hwnd) return false;
  if (rect) *rect = search.rect;
  return true;
}

RECT VirtualDesktopRect() {
  return RECT{
      GetSystemMetrics(SM_XVIRTUALSCREEN),
      GetSystemMetrics(SM_YVIRTUALSCREEN),
      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
  };
}

RECT ClampHudScreenRect(RECT panel) {
  RECT vd = VirtualDesktopRect();
  const int panelW = panel.right - panel.left;
  const int panelH = panel.bottom - panel.top;
  const int minLeft = static_cast<int>(vd.left) + kHudMargin;
  const int minTop = static_cast<int>(vd.top) + kHudMargin;
  const int maxLeft = std::max(minLeft, static_cast<int>(vd.right) - panelW - kHudMargin);
  const int maxTop = std::max(minTop, static_cast<int>(vd.bottom) - panelH - kHudMargin);
  const int left = std::clamp(static_cast<int>(panel.left), minLeft, maxLeft);
  const int top = std::clamp(static_cast<int>(panel.top), minTop, maxTop);
  panel.left = left;
  panel.top = top;
  panel.right = left + panelW;
  panel.bottom = top + panelH;
  return panel;
}

RECT HudPanelRect(const PreviewState& state, const RECT& client) {
  (void)state;
  (void)client;
  return RECT{0, 0, kHudWidth, CurrentHudHeight()};
}

RECT InitialHudScreenRect() {
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);

  if (g_hudUserPlaced) {
    return ClampHudScreenRect(RECT{g_hudPos.x, g_hudPos.y, g_hudPos.x + kHudWidth, g_hudPos.y + CurrentHudHeight()});
  }

  constexpr int kHudTopDrop = 50;
  constexpr int kHudRightExtraInset = 28;
  HMONITOR monitor = nullptr;
  if (vw == 5760) {
    const int middleRight = vx + 1920 * 2;
    const int topBase = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int left = middleRight - kHudWidth - kHudMargin - kHudRightExtraInset;
    const int top = topBase + kHudTopDrop;
    return ClampHudScreenRect(RECT{left, top, left + kHudWidth, top + CurrentHudHeight()});
  }

  RECT gtaRect{};
  if (FindWindowRectByExe(L"gta5.exe", &gtaRect)) {
    monitor = MonitorFromRect(&gtaRect, MONITOR_DEFAULTTONEAREST);
  }
  if (!monitor) {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_mainWnd && fg != g_overlayWnd) {
      RECT wr{};
      if (GetWindowRect(fg, &wr)) monitor = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
    }
  }
  if (!monitor) {
    POINT cursor{};
    GetCursorPos(&cursor);
    monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  }

  RECT mr = MonitorRectFromHandle(monitor);
  const int left = mr.right - kHudWidth - kHudMargin - kHudRightExtraInset;
  const int top = mr.top + kHudTopDrop;
  RECT panel{
      left,
      top,
      left + kHudWidth,
      top + CurrentHudHeight(),
  };

  return ClampHudScreenRect(panel);
}

RECT HudExitButtonRect(const RECT& panel) {
  return RECT{panel.right - 36, panel.top + 10, panel.right - 14, panel.top + 32};
}

RECT HudHeaderRect(const RECT& panel) {
  return RECT{panel.left, panel.top, panel.right, panel.top + 42};
}

RECT HudSettingsToggleRect(const RECT& panel) {
  return RECT{panel.right - 42, panel.top + 84, panel.right - 16, panel.top + 104};
}

RECT HudHotkeyRect(const RECT& panel) {
  return RECT{panel.left + 92, panel.top + 112, panel.left + 190, panel.top + 140};
}

RECT HudConfirmRect(const RECT& panel) {
  return RECT{panel.left + 204, panel.top + 112, panel.left + 286, panel.top + 140};
}

RECT HudOverlayCheckRect(const RECT& panel) {
  return RECT{panel.left + 18, panel.top + 150, panel.left + 36, panel.top + 168};
}

RECT HudTapHoldMinusRect(const RECT& panel) {
  return RECT{panel.left + 162, panel.top + 190, panel.left + 184, panel.top + 214};
}

RECT HudTapHoldValueRect(const RECT& panel) {
  return RECT{panel.left + 188, panel.top + 190, panel.left + 226, panel.top + 214};
}

RECT HudTapHoldPlusRect(const RECT& panel) {
  return RECT{panel.left + 230, panel.top + 190, panel.left + 252, panel.top + 214};
}

RECT HudTapGapMinusRect(const RECT& panel) {
  return RECT{panel.left + 162, panel.top + 220, panel.left + 184, panel.top + 244};
}

RECT HudTapGapValueRect(const RECT& panel) {
  return RECT{panel.left + 188, panel.top + 220, panel.left + 226, panel.top + 244};
}

RECT HudTapGapPlusRect(const RECT& panel) {
  return RECT{panel.left + 230, panel.top + 220, panel.left + 252, panel.top + 244};
}

PreviewState SnapshotPreviewState() {
  static PreviewState cached;
  std::unique_lock<std::mutex> lock(g_previewMutex, std::try_to_lock);
  if (lock.owns_lock()) cached = g_preview;
  return cached;
}

void RequestOverlayRepaint() {
  bool expected = false;
  if (g_overlayWnd && g_overlayRepaintPending.compare_exchange_strong(expected, true)) {
    InvalidateRect(g_overlayWnd, nullptr, FALSE);
  }
  expected = false;
  if (g_marksWnd && g_marksRepaintPending.compare_exchange_strong(expected, true)) {
    InvalidateRect(g_marksWnd, nullptr, FALSE);
  }
}

std::wstring NowTime() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[32];
  swprintf(buf, 32, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

void PostLog(const std::wstring& text) {
  if (text.rfind(L"Timing:", 0) == 0 ||
      text.rfind(L"Press Enter:", 0) == 0 ||
      text.rfind(L"active bar:", 0) == 0 ||
      text == L"yellow outline not found" ||
      text == L"not in minigame") {
    return;
  }
  auto* payload = new std::wstring(text);
  if (g_mainWnd) {
    PostMessageW(g_mainWnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(payload));
  } else {
    OutputDebugStringW((payload->c_str()));
    OutputDebugStringW(L"\n");
    delete payload;
  }
}

void PostStatus(const std::wstring& text) {
  auto* payload = new std::wstring(text);
  if (g_mainWnd) {
    PostMessageW(g_mainWnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(payload));
  } else {
    delete payload;
  }
}

uint8_t R(uint32_t px) { return static_cast<uint8_t>((px >> 16) & 0xff); }
uint8_t G(uint32_t px) { return static_cast<uint8_t>((px >> 8) & 0xff); }
uint8_t B(uint32_t px) { return static_cast<uint8_t>(px & 0xff); }

bool IsRed(uint32_t px) {
  const int r = R(px), g = G(px), b = B(px);
  return r >= 165 && g <= 95 && b <= 95 && r >= g + 70 && r >= b + 70;
}

bool IsWhite(uint32_t px) {
  const int r = R(px), g = G(px), b = B(px);
  return r >= 210 && g >= 210 && b >= 210 && std::abs(r - g) <= 38 && std::abs(r - b) <= 38;
}

bool IsYellow(uint32_t px) {
  const int r = R(px), g = G(px), b = B(px);
  return r >= 185 && g >= 145 && b <= 95 && r >= b + 90 && g >= b + 55 && std::abs(r - g) <= 95;
}

bool CaptureScreenRegion(CaptureFrame& frame, const RectI* region) {
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  if (region) {
    const int x1 = std::max(vx, region->x);
    const int y1 = std::max(vy, region->y);
    const int x2 = std::min(vx + vw, region->x + region->w);
    const int y2 = std::min(vy + vh, region->y + region->h);
    frame.x = x1;
    frame.y = y1;
    frame.w = std::max(1, x2 - x1);
    frame.h = std::max(1, y2 - y1);
  } else {
    frame.x = vx;
    frame.y = vy;
    frame.w = vw;
    frame.h = vh;
  }
  if (frame.w <= 0 || frame.h <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  if (!screen || !mem) {
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    return false;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = frame.w;
  bmi.bmiHeader.biHeight = -frame.h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return false;
  }

  HGDIOBJ old = SelectObject(mem, dib);
  BOOL copied = BitBlt(mem, 0, 0, frame.w, frame.h, screen, frame.x, frame.y, SRCCOPY);
  frame.bgra.resize(static_cast<size_t>(frame.w) * frame.h);
  if (copied) {
    std::memcpy(frame.bgra.data(), bits, frame.bgra.size() * sizeof(uint32_t));
  }

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return copied == TRUE;
}

uint32_t Pixel(const CaptureFrame& f, int x, int y) {
  return f.bgra[static_cast<size_t>(y) * f.w + x];
}

std::vector<std::pair<int, int>> GroupRuns(const std::vector<int>& values, int begin, int end, int threshold, int minWidth);

RedBar LocateRedBar(const CaptureFrame& f) {
  RedBar red;
  const int cx1 = std::max(0, f.w / 32);
  const int cx2 = std::min(f.w, f.w - f.w / 32);
  const int cy1 = std::max(0, f.h / 12);
  const int cy2 = std::min(f.h, f.h - f.h / 12);

  std::vector<int> rowCount(f.h, 0);
  for (int y = cy1; y < cy2; ++y) {
    int count = 0;
    for (int x = cx1; x < cx2; ++x) {
      if (IsRed(Pixel(f, x, y))) ++count;
    }
    rowCount[y] = count;
  }

  int bestY = cy1;
  int bestCount = 0;
  for (int y = cy1; y < cy2; ++y) {
    if (rowCount[y] > bestCount) {
      bestCount = rowCount[y];
      bestY = y;
    }
  }

  const int minRowCount = std::max(120, bestCount / 2);
  if (bestCount < minRowCount) return red;

  auto redRuns = GroupRuns(rowCount, cy1, cy2, minRowCount, 3);
  if (redRuns.empty()) return red;
  auto chosen = *std::min_element(redRuns.begin(), redRuns.end(), [screenMid = f.h / 2](auto a, auto b) {
    return std::abs((a.first + a.second) / 2 - screenMid) < std::abs((b.first + b.second) / 2 - screenMid);
  });
  int y1 = chosen.first;
  int y2 = chosen.second;

  int x1 = f.w, x2 = 0;
  for (int y = y1; y <= y2; ++y) {
    for (int x = cx1; x < cx2; ++x) {
      if (IsRed(Pixel(f, x, y))) {
        x1 = std::min(x1, x);
        x2 = std::max(x2, x);
      }
    }
  }

  if (x2 - x1 < 240 || y2 - y1 < 4) return red;
  red.ok = true;
  red.x1 = x1;
  red.x2 = x2;
  red.y1 = y1;
  red.y2 = y2;
  red.centerY = (y1 + y2) / 2;
  red.height = y2 - y1 + 1;
  return red;
}

std::vector<std::pair<int, int>> GroupRuns(const std::vector<int>& values, int begin, int end, int threshold, int minWidth) {
  std::vector<std::pair<int, int>> runs;
  int i = begin;
  while (i < end) {
    while (i < end && values[i] < threshold) ++i;
    int s = i;
    while (i < end && values[i] >= threshold) ++i;
    int e = i - 1;
    if (e >= s && e - s + 1 >= minWidth) runs.push_back({s, e});
  }
  return runs;
}

std::vector<BarMeasure> LocateWhiteBars(const CaptureFrame& f, const RedBar& red, double scale) {
  std::vector<BarMeasure> bars;
  if (!red.ok) return bars;

  RectI roi;
  roi.x = std::max(0, red.x1 - ScaledPx(25, scale));
  roi.w = std::min(f.w - 1, red.x2 + ScaledPx(25, scale)) - roi.x + 1;
  roi.y = std::max(0, red.centerY - ScaledPx(360, scale));
  roi.h = std::min(f.h - 1, red.centerY + ScaledPx(360, scale)) - roi.y + 1;

  std::vector<int> colCount(f.w, 0);
  for (int x = roi.x; x < roi.x + roi.w; ++x) {
    int count = 0;
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
      if (IsWhite(Pixel(f, x, y))) ++count;
    }
    colCount[x] = count;
  }

  const int colThreshold = std::max(ScaledPx(28, scale), roi.h / 13);
  auto xRuns = GroupRuns(colCount, roi.x, roi.x + roi.w, colThreshold, ScaledPx(5, scale));

  for (auto [x1, x2] : xRuns) {
    if (x2 - x1 + 1 > ScaledPx(80, scale)) continue;

    std::vector<int> rowCount(f.h, 0);
    const int width = x2 - x1 + 1;
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
      int count = 0;
      for (int x = x1; x <= x2; ++x) {
        if (IsWhite(Pixel(f, x, y))) ++count;
      }
      rowCount[y] = count;
    }

    auto yRuns = GroupRuns(rowCount, roi.y, roi.y + roi.h, std::max(ScaledPx(3, scale), width / 3), ScaledPx(12, scale));
    if (yRuns.size() < 2) continue;

    std::pair<int, int> top{};
    std::pair<int, int> bottom{};
    int bestScore = -1000000;
    for (size_t i = 0; i + 1 < yRuns.size(); ++i) {
      for (size_t j = i + 1; j < yRuns.size(); ++j) {
        const int verticalGap = yRuns[j].first - yRuns[i].second - 1;
        const int topHeight = yRuns[i].second - yRuns[i].first + 1;
        const int bottomHeight = yRuns[j].second - yRuns[j].first + 1;
        if (verticalGap < ScaledPx(6, scale) || verticalGap > ScaledPx(70, scale)) continue;
        if (topHeight < ScaledPx(28, scale) || bottomHeight < ScaledPx(28, scale)) continue;

        const int pairCenter = (yRuns[i].second + yRuns[j].first) / 2;
        const int heightBalancePenalty = std::abs(topHeight - bottomHeight);
        const int centerPenalty = std::abs(pairCenter - red.centerY) / 3;
        const int score = topHeight + bottomHeight - heightBalancePenalty - centerPenalty;
        if (score > bestScore) {
          bestScore = score;
          top = yRuns[i];
          bottom = yRuns[j];
        }
      }
    }
    if (bestScore < 0) continue;

    const double gapCenter = (top.second + bottom.first) / 2.0;

    BarMeasure m;
    m.ok = true;
    m.x1 = x1;
    m.x2 = x2;
    m.topY1 = top.first;
    m.topY2 = top.second;
    m.bottomY1 = bottom.first;
    m.bottomY2 = bottom.second;
    m.gapCenterY = gapCenter;
    bars.push_back(m);
  }

  std::sort(bars.begin(), bars.end(), [](const BarMeasure& a, const BarMeasure& b) {
    return (a.x1 + a.x2) < (b.x1 + b.x2);
  });

  if (bars.size() > 8) {
    std::vector<BarMeasure> best;
    for (const auto& b : bars) {
      if (static_cast<int>(best.size()) < 8) best.push_back(b);
    }
    bars.swap(best);
  }

  return bars;
}

bool MeasureBarAtX(const CaptureFrame& f, int x1, int x2, int y1, int y2, double scale, BarMeasure& out) {
  x1 = std::clamp(x1, 0, f.w - 1);
  x2 = std::clamp(x2, 0, f.w - 1);
  y1 = std::clamp(y1, 0, f.h - 1);
  y2 = std::clamp(y2, 0, f.h - 1);
  if (x2 <= x1 || y2 <= y1) return false;

  std::vector<int> rowCount(f.h, 0);
  const int width = x2 - x1 + 1;
  for (int y = y1; y <= y2; ++y) {
    int count = 0;
    for (int x = x1; x <= x2; ++x) {
      if (IsWhite(Pixel(f, x, y))) ++count;
    }
    rowCount[y] = count;
  }

  auto yRuns = GroupRuns(rowCount, y1, y2 + 1, std::max(ScaledPx(3, scale), width / 3), ScaledPx(12, scale));
  if (yRuns.size() < 2) return false;

  std::pair<int, int> top{};
  std::pair<int, int> bottom{};
  int bestScore = -1000000;
  for (size_t i = 0; i + 1 < yRuns.size(); ++i) {
    for (size_t j = i + 1; j < yRuns.size(); ++j) {
      const int verticalGap = yRuns[j].first - yRuns[i].second - 1;
      const int topHeight = yRuns[i].second - yRuns[i].first + 1;
      const int bottomHeight = yRuns[j].second - yRuns[j].first + 1;
      if (verticalGap < ScaledPx(6, scale) || verticalGap > ScaledPx(70, scale)) continue;
      if (topHeight < ScaledPx(28, scale) || bottomHeight < ScaledPx(28, scale)) continue;
      const int heightBalancePenalty = std::abs(topHeight - bottomHeight);
      const int score = topHeight + bottomHeight - heightBalancePenalty;
      if (score > bestScore) {
        bestScore = score;
        top = yRuns[i];
        bottom = yRuns[j];
      }
    }
  }
  if (bestScore < 0) return false;

  out.ok = true;
  out.x1 = x1;
  out.x2 = x2;
  out.topY1 = top.first;
  out.topY2 = top.second;
  out.bottomY1 = bottom.first;
  out.bottomY2 = bottom.second;
  out.gapCenterY = (top.second + bottom.first) / 2.0;
  return true;
}

std::vector<BarMeasure> LocateWhiteBarsAtKnownX(const CaptureFrame& f, const RedBar& red, const std::vector<std::pair<int, int>>& knownXRuns, double scale) {
  std::vector<BarMeasure> bars;
  if (!red.ok || knownXRuns.size() != 8) return bars;

  const int scanY1 = std::max(0, red.centerY - ScaledPx(290, scale));
  const int scanY2 = std::min(f.h - 1, red.centerY + ScaledPx(290, scale));
  for (auto [kx1, kx2] : knownXRuns) {
    BarMeasure m;
    if (MeasureBarAtX(f, kx1 - ScaledPx(3, scale), kx2 + ScaledPx(3, scale), scanY1, scanY2, scale, m)) {
      bars.push_back(m);
    }
  }
  return bars;
}

SearchCells BuildSearchCellsFromWhiteBars(const std::vector<BarMeasure>& bars, double scale) {
  SearchCells cells;
  if (bars.size() < 8) return cells;

  std::vector<double> centers;
  std::vector<double> widths;
  for (int i = 1; i < 8; ++i) {
    centers.push_back((bars[i].x1 + bars[i].x2) / 2.0);
    widths.push_back(static_cast<double>(bars[i].x2 - bars[i].x1 + 1));
  }
  std::sort(widths.begin(), widths.end());
  const double medianWidth = widths[widths.size() / 2];

  std::vector<double> gaps;
  for (size_t i = 1; i < centers.size(); ++i) {
    gaps.push_back(centers[i] - centers[i - 1]);
  }
  std::sort(gaps.begin(), gaps.end());
  const double medianGap = gaps.empty() ? medianWidth * 2.0 : gaps[gaps.size() / 2];
  const double c1 = centers.front();
  const double firstCenter = c1 - medianGap;
  const double searchWidth = std::clamp(medianWidth + ScaledPx(16, scale), medianWidth, medianGap - ScaledPx(4, scale));

  cells.xRanges.reserve(8);
  for (int i = 0; i < 8; ++i) {
    const double c = firstCenter + medianGap * i;
    cells.xRanges.push_back({static_cast<int>(std::round(c - searchWidth / 2.0)),
                             static_cast<int>(std::round(c + searchWidth / 2.0))});
  }
  cells.ok = true;
  return cells;
}

bool MeasureYellowGapForBar(const CaptureFrame& f, const RedBar& red, int index, int cellLeftScreen, int cellRightScreen, double scale, YellowMeasure& out) {
  const int cellLeft = cellLeftScreen - f.x;
  const int cellRight = cellRightScreen - f.x;
  const int y1 = std::max(0, red.centerY - f.y - ScaledPx(320, scale));
  const int y2 = std::min(f.h - 1, red.centerY - f.y + ScaledPx(320, scale));
  if (cellRight < 0 || cellLeft >= f.w || y2 <= y1) return false;

  std::vector<int> rowCount(f.h, 0);
  const int scanX1 = std::clamp(cellLeft, 0, f.w - 1);
  const int scanX2 = std::clamp(cellRight, 0, f.w - 1);
  for (int y = y1; y <= y2; ++y) {
    int count = 0;
    for (int x = scanX1; x <= scanX2; ++x) {
      if (IsYellow(Pixel(f, x, y))) ++count;
    }
    rowCount[y] = count;
  }

  auto runs = GroupRuns(rowCount, y1, y2 + 1, ScaledPx(2, scale), ScaledPx(6, scale));
  if (runs.size() < 2) return false;

  std::pair<int, int> topRun{};
  std::pair<int, int> bottomRun{};
  int bestScore = -1000000;
  for (size_t i = 0; i + 1 < runs.size(); ++i) {
    for (size_t j = i + 1; j < runs.size(); ++j) {
      const int verticalGap = runs[j].first - runs[i].second - 1;
      const int topHeight = runs[i].second - runs[i].first + 1;
      const int bottomHeight = runs[j].second - runs[j].first + 1;
      if (verticalGap < ScaledPx(8, scale) || verticalGap > ScaledPx(95, scale)) continue;
      if (topHeight < ScaledPx(10, scale) || bottomHeight < ScaledPx(10, scale)) continue;

      int yellowPixels = 0;
      for (int y = runs[i].first; y <= runs[i].second; ++y) yellowPixels += rowCount[y];
      for (int y = runs[j].first; y <= runs[j].second; ++y) yellowPixels += rowCount[y];
      const int balancePenalty = std::abs(topHeight - bottomHeight) * 2;
      const int gapPenalty = std::abs(verticalGap - ScaledPx(24, scale));
      const int score = yellowPixels + topHeight + bottomHeight - balancePenalty - gapPenalty;
      if (score > bestScore) {
        bestScore = score;
        topRun = runs[i];
        bottomRun = runs[j];
      }
    }
  }
  if (bestScore < 0) return false;

  out.ok = true;
  out.index = index;
  out.topBottomY = topRun.second + f.y;
  out.bottomTopY = bottomRun.first + f.y;
  out.gapCenterY = (out.topBottomY + out.bottomTopY) / 2.0;
  int score = 0;
  for (int y = topRun.first; y <= topRun.second; ++y) score += rowCount[y];
  for (int y = bottomRun.first; y <= bottomRun.second; ++y) score += rowCount[y];
  out.score = score;
  return true;
}

YellowMeasure FindActiveYellowMeasure(const CaptureFrame& f, const RedBar& red, const SearchCells& cells, double scale) {
  YellowMeasure best;
  if (!red.ok || !cells.ok || cells.xRanges.size() != 8) return best;
  for (int i = 0; i < 8; ++i) {
    YellowMeasure m;
    if (MeasureYellowGapForBar(f, red, i, cells.xRanges[i].first, cells.xRanges[i].second, scale, m)) {
      if (!best.ok || m.score > best.score) best = m;
    }
  }
  if (best.ok && best.score < ScaledPx(20, scale)) best.ok = false;
  return best;
}

YellowMeasure FindActiveYellowMeasureCandidates(const CaptureFrame& f, const RedBar& red, const SearchCells& cells, const std::vector<int>& candidates, double scale) {
  YellowMeasure best;
  if (!red.ok || !cells.ok || cells.xRanges.size() != 8) return best;
  for (int rawIndex : candidates) {
    const int i = std::clamp(rawIndex, 0, 7);
    YellowMeasure m;
    if (MeasureYellowGapForBar(f, red, i, cells.xRanges[i].first, cells.xRanges[i].second, scale, m)) {
      if (!best.ok || m.score > best.score) best = m;
    }
  }
  if (best.ok && best.score < ScaledPx(20, scale)) best.ok = false;
  return best;
}

FrameAnalysis AnalyzeFrame(const CaptureFrame& f, const std::vector<std::pair<int, int>>* knownXRuns = nullptr) {
  FrameAnalysis analysis;
  analysis.red = LocateRedBar(f);
  if (!analysis.red.ok) {
    analysis.minigameStatus = L"searching minigame";
    analysis.minigameLog = L"not in minigame: red line not found";
    return analysis;
  }
  const double scale = GeometryScaleFromScreenPoint(f.x + (analysis.red.x1 + analysis.red.x2) / 2,
                                                    f.y + analysis.red.centerY);
  if (knownXRuns && knownXRuns->size() == 8) {
    analysis.bars = LocateWhiteBarsAtKnownX(f, analysis.red, *knownXRuns, scale);
  }
  if (analysis.bars.size() < 8) {
    analysis.bars = LocateWhiteBars(f, analysis.red, scale);
  }
  analysis.ok = analysis.bars.size() >= 8;
  analysis.inMinigame = analysis.ok;

  analysis.red.x1 += f.x;
  analysis.red.x2 += f.x;
  analysis.red.y1 += f.y;
  analysis.red.y2 += f.y;
  analysis.red.centerY += f.y;
  for (auto& bar : analysis.bars) {
    bar.x1 += f.x;
    bar.x2 += f.x;
    bar.topY1 += f.y;
    bar.topY2 += f.y;
    bar.bottomY1 += f.y;
    bar.bottomY2 += f.y;
    bar.gapCenterY += f.y;
  }
  if (analysis.inMinigame) {
    analysis.minigameStatus = L"in minigame";
  } else {
    analysis.minigameStatus = L"searching minigame";
    std::wstringstream ss;
    ss << L"not in minigame: white bars " << analysis.bars.size() << L" / 8";
    analysis.minigameLog = ss.str();
  }
  return analysis;
}

FrameAnalysis AnalyzeLockedGeometry(const CaptureFrame& f, const RedBar& lockedRedScreen, const std::vector<BarMeasure>& lockedBarsScreen) {
  FrameAnalysis analysis;
  if (!lockedRedScreen.ok || lockedBarsScreen.size() < 8) return analysis;

  RedBar localRed = lockedRedScreen;
  localRed.x1 -= f.x;
  localRed.x2 -= f.x;
  localRed.y1 -= f.y;
  localRed.y2 -= f.y;
  localRed.centerY -= f.y;
  if (localRed.x2 < 0 || localRed.x1 >= f.w || localRed.centerY < 0 || localRed.centerY >= f.h) {
    return analysis;
  }

  analysis.ok = true;
  analysis.inMinigame = true;
  analysis.minigameStatus = L"in minigame";
  analysis.red = lockedRedScreen;
  analysis.bars = lockedBarsScreen;
  return analysis;
}

void PressEnter() {
  constexpr WORD kMainEnterScanCode = 0x1C;

  INPUT down{};
  down.type = INPUT_KEYBOARD;
  down.ki.wScan = kMainEnterScanCode;
  down.ki.dwFlags = KEYEVENTF_SCANCODE;
  SendInput(1, &down, sizeof(INPUT));

  Sleep(42);

  INPUT up{};
  up.type = INPUT_KEYBOARD;
  up.ki.wScan = kMainEnterScanCode;
  up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
  SendInput(1, &up, sizeof(INPUT));
}

void WaitUntilPrecise(std::chrono::steady_clock::time_point target) {
  while (!g_stopWorker.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= target) break;
    const auto remain = target - now;
    if (remain > std::chrono::milliseconds(2)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
      std::this_thread::yield();
    }
  }
}

bool IsMoving(const TrackSlot& slot) {
  if (slot.history.size() < 4) return false;
  const size_t begin = slot.history.size() > 6 ? slot.history.size() - 6 : 0;
  auto firstSample = slot.history.begin() + static_cast<std::ptrdiff_t>(begin);
  auto [minIt, maxIt] = std::minmax_element(firstSample, slot.history.end(), [](const TrackSample& a, const TrackSample& b) {
    return a.center < b.center;
  });
  return (maxIt->center - minIt->center) >= 3.0;
}

void ResetTrack(TrackSlot& slot) {
  slot.valid = false;
  slot.lastCenter = 0.0;
  slot.velocity = 0.0;
  slot.history.clear();
}

double EstimateVelocity(const TrackSlot& slot) {
  if (slot.history.size() < 2) return 0.0;
  const size_t begin = slot.history.size() > 4 ? slot.history.size() - 4 : 0;
  const auto tLast = slot.history.back().time;

  double sumW = 0.0;
  double sumT = 0.0;
  double sumE = 0.0;
  std::vector<double> times;
  std::vector<double> errors;
  std::vector<double> weights;
  times.reserve(slot.history.size() - begin);
  errors.reserve(slot.history.size() - begin);
  weights.reserve(slot.history.size() - begin);
  for (size_t i = begin; i < slot.history.size(); ++i) {
    const double t = std::chrono::duration<double>(slot.history[i].time - tLast).count();
    const double recency = std::clamp(1.0 + t / 0.10, 0.0, 1.0);
    const double w = 0.50 + recency * recency * 2.50;
    times.push_back(t);
    errors.push_back(slot.history[i].center);
    weights.push_back(w);
    sumW += w;
    sumT += w * t;
    sumE += w * slot.history[i].center;
  }
  if (sumW <= 0.0) return 0.0;
  const double meanT = sumT / sumW;
  const double meanE = sumE / sumW;
  double num = 0.0;
  double den = 0.0;
  for (size_t i = 0; i < times.size(); ++i) {
    num += weights[i] * (times[i] - meanT) * (errors[i] - meanE);
    den += weights[i] * (times[i] - meanT) * (times[i] - meanT);
  }
  return den > 1e-6 ? num / den : 0.0;
}

bool AddTrackSample(TrackSlot& slot, double observedError, std::chrono::steady_clock::time_point sampleTime) {
  slot.lastCenter = observedError;
  if (!slot.history.empty()) {
    const double delta = std::abs(observedError - slot.history.back().center);
    const double ageMs = std::chrono::duration<double, std::milli>(sampleTime - slot.history.back().time).count();
    if (delta < 0.75 && ageMs < 80.0) return false;
  }
  slot.history.push_back({observedError, sampleTime});
  while (slot.history.size() > 32) slot.history.erase(slot.history.begin());
  slot.velocity = EstimateVelocity(slot);
  return true;
}

bool IsUsableTiming(double seconds) {
  return std::isfinite(seconds) && seconds > 0.0 && seconds < 1.0;
}

double EstimateEdgeTriggerTime(double edgeError, double velocity, double scale) {
  const double speed = std::abs(velocity);
  if (std::abs(edgeError) > ScaledPx(kEdgeTriggerZonePx, scale) || speed < kMinUsableVelocityPxPerSec || edgeError * velocity >= 0.0) {
    return kInvalidPredictionSeconds;
  }
  return std::abs(edgeError) / speed;
}

double EdgeTriggerError(const YellowMeasure& yellow, const RedBar& red, double velocity) {
  if (velocity > 0.0) {
    return yellow.gapCenterY - red.y1;
  }
  if (velocity < 0.0) {
    return yellow.gapCenterY - red.y2;
  }
  return yellow.gapCenterY - red.centerY;
}

void UpdatePreview(const CaptureFrame& frame,
                   const FrameAnalysis& analysis,
                   const std::vector<TrackSlot>& tracks,
                   int active,
                   const std::wstring& status,
                   const YellowMeasure* yellow = nullptr,
                   double edgeError = 0.0,
                   double triggerTimeSec = kInvalidPredictionSeconds,
                   double leadSec = 0.0,
                   double velocity = 0.0,
                   double scale = 1.0) {
  PreviewState next;
  next.hasFrame = true;
  next.hasRed = analysis.red.ok;
  next.hasYellow = yellow && yellow->ok;
  next.running = g_running.load();
  next.red = analysis.red;
  next.yellow = yellow ? *yellow : YellowMeasure{};
  next.status = status;
  next.edgeError = edgeError;
  next.triggerTimeSec = triggerTimeSec;
  next.leadSec = leadSec;
  next.velocity = velocity;
  next.scale = scale;
  next.screenX = frame.x;
  next.screenY = frame.y;
  next.screenW = frame.w;
  next.screenH = frame.h;

  for (size_t i = 0; i < analysis.bars.size(); ++i) {
    const auto& b = analysis.bars[i];
    PreviewBar ob;
    ob.x1 = b.x1;
    ob.x2 = b.x2;
    ob.topY1 = b.topY1;
    ob.topY2 = b.topY2;
    ob.bottomY1 = b.bottomY1;
    ob.bottomY2 = b.bottomY2;
    ob.active = static_cast<int>(i) == active;
    ob.moving = i < tracks.size() && IsMoving(tracks[i]);
    next.bars.push_back(ob);
  }

  std::unique_lock<std::mutex> lock(g_previewMutex, std::try_to_lock);
  if (lock.owns_lock()) {
    next.lastLog = g_preview.lastLog;
    g_preview = std::move(next);
    RequestOverlayRepaint();
  }
}

void WorkerLoop() {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  PostLog(L"Start: keep the game visible. Using vision to detect the red line and white bars.");
  std::vector<TrackSlot> tracks(8);
  auto lastEnter = std::chrono::steady_clock::now() - std::chrono::seconds(2);
  int frameNo = 0;
  int lastLoggedActive = -2;
  int missFrames = 0;
  auto lastFrameTime = std::chrono::steady_clock::now();
  int expectedIndex = 0;
  int lastYellowIndex = -1;
  int trackedActive = -1;
  int yellowMissFrames = 0;
  bool hasTrackingRegion = false;
  RectI trackingRegion{};
  RedBar lockedRedScreen{};
  std::vector<std::pair<int, int>> knownBarXRunsScreen;
  std::vector<BarMeasure> lastBarsScreen;
  SearchCells searchCellsScreen;
  bool cellsLocked = false;
  double geometryScale = 1.0;
  auto lastUiUpdate = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  const auto activeSearchStart = std::chrono::steady_clock::now();
  bool activeFoundOnce = false;
  bool finishPendingAfter8 = false;
  auto finishConfirmStart = std::chrono::steady_clock::now();
  int slowFrameStreak = 0;
  while (!g_stopWorker.load()) {
    const auto frameTime = std::chrono::steady_clock::now();
    const int frameDtMs = static_cast<int>(std::round(std::chrono::duration<double, std::milli>(frameTime - lastFrameTime).count()));
    lastFrameTime = frameTime;
    if (frameDtMs > 30) {
      ++slowFrameStreak;
      if (slowFrameStreak >= 3) {
        PostLog(L"Error: dt > 30ms for 3 consecutive frames; stopping.");
        PostStatus(L"analysis latency too high; stopped");
        g_stopWorker.store(true);
        break;
      }
      std::this_thread::yield();
      ++frameNo;
      continue;
    }
    slowFrameStreak = 0;
    CaptureFrame frame;
    if (!CaptureScreenRegion(frame, hasTrackingRegion ? &trackingRegion : nullptr)) {
      PostStatus(L"capture failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    const auto sampleTime = std::chrono::steady_clock::now();

    const bool canUseLockedGeometry = hasTrackingRegion && lockedRedScreen.ok && lastBarsScreen.size() >= 8 && searchCellsScreen.ok;
    bool usedLockedGeometry = false;
    FrameAnalysis a;
    if (canUseLockedGeometry) {
      a = AnalyzeLockedGeometry(frame, lockedRedScreen, lastBarsScreen);
      usedLockedGeometry = a.inMinigame;
    }
    if (!usedLockedGeometry) {
      std::vector<std::pair<int, int>> knownBarXRunsLocal;
      if (knownBarXRunsScreen.size() == 8) {
        for (auto [x1, x2] : knownBarXRunsScreen) {
          knownBarXRunsLocal.push_back({x1 - frame.x, x2 - frame.x});
        }
      }
      a = AnalyzeFrame(frame, knownBarXRunsLocal.size() == 8 ? &knownBarXRunsLocal : nullptr);
    }
    if (!a.inMinigame) {
      if (++missFrames % 15 == 1) {
        PostLog(a.minigameLog.empty() ? L"not in minigame" : a.minigameLog);
      }
      if (missFrames >= 3) {
        hasTrackingRegion = false;
        lockedRedScreen = {};
        knownBarXRunsScreen.clear();
        searchCellsScreen = {};
        cellsLocked = false;
        lastBarsScreen.clear();
        geometryScale = 1.0;
      }
      PostStatus(a.minigameStatus);
      UpdatePreview(frame, a, tracks, -1, a.minigameStatus);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      continue;
    }
    missFrames = 0;
    lockedRedScreen = a.red;
    lastBarsScreen = a.bars;
    geometryScale = GeometryScaleFromRedScreen(a.red);
    if (a.bars.size() >= 8) {
      knownBarXRunsScreen.clear();
      for (int i = 0; i < 8; ++i) {
        knownBarXRunsScreen.push_back({a.bars[i].x1, a.bars[i].x2});
      }
      if (!cellsLocked || !searchCellsScreen.ok) {
        searchCellsScreen = BuildSearchCellsFromWhiteBars(a.bars, geometryScale);
        cellsLocked = searchCellsScreen.ok;
      }
    }
    hasTrackingRegion = true;
    int minBarX = a.red.x1;
    int maxBarX = a.red.x2;
    if (searchCellsScreen.ok && searchCellsScreen.xRanges.size() == 8) {
      minBarX = searchCellsScreen.xRanges.front().first;
      maxBarX = searchCellsScreen.xRanges.back().second;
    } else if (knownBarXRunsScreen.size() == 8) {
      minBarX = knownBarXRunsScreen.front().first;
      maxBarX = knownBarXRunsScreen.back().second;
    }
    trackingRegion.x = minBarX - ScaledPx(45, geometryScale);
    trackingRegion.y = a.red.centerY - ScaledPx(300, geometryScale);
    trackingRegion.w = (maxBarX - minBarX) + ScaledPx(90, geometryScale);
    trackingRegion.h = ScaledPx(600, geometryScale);

    std::vector<int> yellowCandidates;
    if (lastYellowIndex >= 0) {
      yellowCandidates = {lastYellowIndex, lastYellowIndex - 1, lastYellowIndex + 1};
    } else {
      yellowCandidates = {expectedIndex, expectedIndex + 1, expectedIndex - 1};
    }
    YellowMeasure yellow = FindActiveYellowMeasureCandidates(frame, a.red, searchCellsScreen, yellowCandidates, geometryScale);
    if (!yellow.ok) {
      yellow = FindActiveYellowMeasure(frame, a.red, searchCellsScreen, geometryScale);
    }
    int active = yellow.ok ? yellow.index : -1;
    if (active >= 0 && active != trackedActive) {
      ResetTrack(tracks[active]);
      trackedActive = active;
    }
    if (yellow.ok) {
      yellowMissFrames = 0;
      lastYellowIndex = active;
      tracks[active].valid = true;
      const double observedError = yellow.gapCenterY - a.red.centerY;
      AddTrackSample(tracks[active], observedError, sampleTime);
      expectedIndex = active;
      int cursorBaseX = yellow.index >= 0 && yellow.index < static_cast<int>(a.bars.size())
                            ? a.bars[yellow.index].x1 - ScaledPx(16, geometryScale) - kCursorArrowRightOffset
                            : minBarX - ScaledPx(16, geometryScale) - kCursorArrowRightOffset;
      if (searchCellsScreen.ok && searchCellsScreen.xRanges.size() == 8) {
        cursorBaseX = searchCellsScreen.xRanges[active].first - ScaledPx(16, geometryScale) - kCursorArrowRightOffset;
      }
      g_cursorX.store(cursorBaseX, std::memory_order_relaxed);
      g_cursorY.store(static_cast<int>(std::round(yellow.gapCenterY)), std::memory_order_relaxed);
      g_cursorTargetY.store(a.red.centerY, std::memory_order_relaxed);
      g_cursorInZone.store(false, std::memory_order_relaxed);
      g_cursorBar.store(active + 1, std::memory_order_relaxed);
      g_cursorVisible.store(true, std::memory_order_relaxed);
    }

    if (active == -1) {
      if (!activeFoundOnce && std::chrono::steady_clock::now() - activeSearchStart > std::chrono::seconds(10)) {
        PostLog(L"Error: active bar not found within 10s; stopping.");
        PostStatus(L"active bar timeout; stopped");
        g_stopWorker.store(true);
        break;
      }
      g_cursorVisible.store(false, std::memory_order_relaxed);
      g_cursorInZone.store(false, std::memory_order_relaxed);
      if (finishPendingAfter8 && std::chrono::steady_clock::now() - finishConfirmStart > std::chrono::milliseconds(650)) {
        PostLog(L"Completed: bar 8 confirmed without rollback; stopping.");
        PostStatus(L"completed; stopped");
        g_stopWorker.store(true);
        break;
      }
      if (++yellowMissFrames >= 3) lastYellowIndex = -1;
      if (yellowMissFrames >= 3) trackedActive = -1;
      PostStatus(L"waiting yellow outline");
      UpdatePreview(frame, a, tracks, -1, L"waiting yellow outline", nullptr, 0.0, kInvalidPredictionSeconds, 0.0, 0.0, geometryScale);
      if (lastLoggedActive != -1 && frameNo > 12) {
        PostLog(L"yellow outline not found");
        lastLoggedActive = -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      ++frameNo;
      continue;
    }
    activeFoundOnce = true;

    if (finishPendingAfter8) {
      if (active == 6) {
        PostLog(L"bar 8 rolled back to bar 7; continuing.");
        finishPendingAfter8 = false;
      } else if (std::chrono::steady_clock::now() - finishConfirmStart > std::chrono::milliseconds(900)) {
        PostLog(L"Completed: bar 8 confirmed; stopping.");
        PostStatus(L"completed; stopped");
        g_stopWorker.store(true);
        break;
      }
    }

    if (active != lastLoggedActive) {
      std::wstringstream ss;
      ss << L"active bar: " << (active + 1);
      PostLog(ss.str());
      lastLoggedActive = active;
    }

    const double error = tracks[active].lastCenter;
    const double velocity = tracks[active].velocity;
    const double edgeError = EdgeTriggerError(yellow, a.red, velocity);
    const int cursorTargetY = velocity >= 0.0 ? a.red.y1 : a.red.y2;
    g_cursorTargetY.store(cursorTargetY, std::memory_order_relaxed);
    g_cursorInZone.store(std::abs(edgeError) <= ScaledPx(kEdgeTriggerZonePx * 2.0, geometryScale), std::memory_order_relaxed);
    const double triggerTimeSec = EstimateEdgeTriggerTime(edgeError, velocity, geometryScale);
    const auto now = std::chrono::steady_clock::now();
    const bool cooledDown = now - lastEnter > std::chrono::milliseconds(170);
    const bool modelReady = tracks[active].history.size() >= 2;
    const bool triggerReady = modelReady && IsUsableTiming(triggerTimeSec);
    const double leadSec = std::clamp(frameDtMs / 1000.0, 0.0, kEdgeActionWindowSeconds);
    const double pressDelaySec = triggerTimeSec - leadSec;
    const int triggerMs = IsUsableTiming(triggerTimeSec) ? static_cast<int>(std::round(triggerTimeSec * 1000.0)) : -1;
    const int leadMs = static_cast<int>(std::round(leadSec * 1000.0));
    if (now - lastUiUpdate >= std::chrono::milliseconds(50)) {
      g_hudActiveBar.store(active + 1, std::memory_order_relaxed);
      g_hudEdgePx.store(static_cast<int>(std::round(edgeError)), std::memory_order_relaxed);
      g_hudTtcMs.store(triggerMs, std::memory_order_relaxed);
      g_hudDtMs.store(frameDtMs, std::memory_order_relaxed);
      g_hudLeadMs.store(leadMs, std::memory_order_relaxed);
      g_hudVelocityPx.store(static_cast<int>(std::round(velocity)), std::memory_order_relaxed);
      g_hudScale100.store(static_cast<int>(std::round(geometryScale * 100.0)), std::memory_order_relaxed);
      UpdatePreview(frame, a, tracks, active, a.minigameStatus.empty() ? L"in minigame" : a.minigameStatus,
                    &yellow, edgeError, triggerTimeSec, leadSec, velocity, geometryScale);
      lastUiUpdate = now;
    }
    const bool pressReady = triggerReady && cooledDown && pressDelaySec <= kEdgeActionWindowSeconds && pressDelaySec >= -std::max(kLateGraceSeconds, leadSec);
    if (pressReady) {
      const auto predictedAt = sampleTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                            std::chrono::duration<double>(pressDelaySec));
      WaitUntilPrecise(predictedAt);
      PressEnter();
      lastEnter = std::chrono::steady_clock::now();
      std::wstringstream ss;
      ss << L"Press Enter: bar=" << (active + 1) << L" schedErr=" << static_cast<int>(std::round(error)) << L"px";
      PostLog(ss.str());
      std::wstringstream timingLog;
      timingLog << std::fixed << std::setprecision(1);
      timingLog << L"Timing: bar=" << (active + 1)
                << L" edge=" << static_cast<int>(std::round(edgeError))
                << L"px copy=" << triggerMs
                << L" lead=" << leadMs
                << L"ms vel=" << static_cast<int>(std::round(velocity))
                << L"px/s dt=" << frameDtMs
                << L"ms";
      PostLog(timingLog.str());
      ResetTrack(tracks[active]);
      trackedActive = -1;
      expectedIndex = std::min(7, active + 1);
      lastYellowIndex = -1;
      if (active == 7) {
        finishPendingAfter8 = true;
        finishConfirmStart = std::chrono::steady_clock::now();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      ++frameNo;
      continue;
    }

    std::this_thread::yield();
    ++frameNo;
  }

  g_cursorVisible.store(false, std::memory_order_relaxed);
  g_cursorInZone.store(false, std::memory_order_relaxed);
  if (g_marksWnd) ShowWindow(g_marksWnd, SW_HIDE);
  {
    std::lock_guard<std::mutex> lock(g_previewMutex);
    const std::wstring status = g_preview.status;
    const std::wstring lastLog = g_preview.lastLog;
    g_preview = PreviewState{};
    g_preview.status = status;
    g_preview.lastLog = lastLog;
    g_preview.running = false;
  }
  PostStatus(L"stopped");
  PostLog(L"stopped");
}

void StartWorker() {
  if (g_running.load()) return;
  g_cursorVisible.store(false, std::memory_order_relaxed);
  g_cursorInZone.store(false, std::memory_order_relaxed);
  if (g_marksWnd) ShowWindow(g_marksWnd, SW_HIDE);
  g_hudActiveBar.store(0, std::memory_order_relaxed);
  g_hudTtcMs.store(-1, std::memory_order_relaxed);
  g_hudDtMs.store(0, std::memory_order_relaxed);
  PostStatus(L"searching minigame");
  g_stopWorker.store(false);
  g_running.store(true);
  g_worker = std::thread([] {
    WorkerLoop();
    g_running.store(false);
    PostMessageW(g_mainWnd, WM_APP_WORKER_DONE, 0, 0);
  });
}

void StopWorker() {
  if (!g_running.load()) return;
  g_cursorVisible.store(false, std::memory_order_relaxed);
  g_cursorInZone.store(false, std::memory_order_relaxed);
  if (g_cursorWnd) ShowWindow(g_cursorWnd, SW_HIDE);
  if (g_marksWnd) ShowWindow(g_marksWnd, SW_HIDE);
  g_stopWorker.store(true);
  if (g_worker.joinable()) {
    if (std::this_thread::get_id() == g_worker.get_id()) return;
    g_worker.join();
  }
  g_running.store(false);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CLOSE:
      if (g_mainWnd && hwnd != g_mainWnd) {
        PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_CREATE:
      SetTimer(hwnd, 1, 100, nullptr);
      return 0;
    case WM_TIMER:
      if (wParam == 1) {
        if (g_listeningHotkey.load(std::memory_order_relaxed)) {
          for (int vk = 8; vk <= 254; ++vk) {
            if (!IsValidHotkeyVk(vk)) continue;
            if (GetAsyncKeyState(vk) & 1) {
              g_pendingHotkeyVk.store(vk, std::memory_order_relaxed);
              break;
            }
          }
        }
        RequestOverlayRepaint();
        return 0;
      }
      return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_LBUTTONDOWN: {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      PreviewState state = SnapshotPreviewState();
      RECT panel = HudPanelRect(state, rc);
      RECT exitButton = HudExitButtonRect(panel);
      RECT header = HudHeaderRect(panel);
      RECT settingsToggle = HudSettingsToggleRect(panel);
      RECT hotkeyBox = HudHotkeyRect(panel);
      RECT confirmBox = HudConfirmRect(panel);
      RECT overlayBox = HudOverlayCheckRect(panel);
      RECT holdMinus = HudTapHoldMinusRect(panel);
      RECT holdPlus = HudTapHoldPlusRect(panel);
      RECT gapMinus = HudTapGapMinusRect(panel);
      RECT gapPlus = HudTapGapPlusRect(panel);
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (PtInRect(&settingsToggle, pt)) {
        const bool expanded = !g_settingsExpanded.load(std::memory_order_relaxed);
        g_settingsExpanded.store(expanded, std::memory_order_relaxed);
        g_listeningHotkey.store(false, std::memory_order_relaxed);
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        RECT desired{wr.left, wr.top, wr.left + kHudWidth, wr.top + CurrentHudHeight()};
        RECT clamped = ClampHudScreenRect(desired);
        g_hudPos = POINT{clamped.left, clamped.top};
        g_hudUserPlaced = true;
        SetWindowPos(hwnd, HWND_TOPMOST, clamped.left, clamped.top, kHudWidth, CurrentHudHeight(),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        RequestOverlayRepaint();
        return 0;
      }
      if (g_settingsExpanded.load(std::memory_order_relaxed) && PtInRect(&confirmBox, pt)) {
        if (!g_listeningHotkey.load(std::memory_order_relaxed)) {
          g_pendingHotkeyVk.store(g_hotkeyVk.load(std::memory_order_relaxed), std::memory_order_relaxed);
          g_listeningHotkey.store(true, std::memory_order_relaxed);
        } else {
          const int pending = g_pendingHotkeyVk.load(std::memory_order_relaxed);
          if (IsValidHotkeyVk(pending)) {
            g_hotkeyVk.store(pending, std::memory_order_relaxed);
            ApplyHotkeySetting(g_mainWnd);
            SaveSettings();
          }
          g_listeningHotkey.store(false, std::memory_order_relaxed);
        }
        RequestOverlayRepaint();
        return 0;
      }
      if (g_settingsExpanded.load(std::memory_order_relaxed) && PtInRect(&overlayBox, pt)) {
        const bool enabled = !g_overlayCursorEnabled.load(std::memory_order_relaxed);
        g_overlayCursorEnabled.store(enabled, std::memory_order_relaxed);
        if (!enabled && g_cursorWnd) ShowWindow(g_cursorWnd, SW_HIDE);
        SaveSettings();
        RequestOverlayRepaint();
        return 0;
      }
      if (g_settingsExpanded.load(std::memory_order_relaxed) &&
          (PtInRect(&holdMinus, pt) || PtInRect(&holdPlus, pt) || PtInRect(&gapMinus, pt) || PtInRect(&gapPlus, pt))) {
        if (PtInRect(&holdMinus, pt)) {
          g_tapHoldMs.store(ClampTapMs(g_tapHoldMs.load(std::memory_order_relaxed) - 1), std::memory_order_relaxed);
        } else if (PtInRect(&holdPlus, pt)) {
          g_tapHoldMs.store(ClampTapMs(g_tapHoldMs.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
        } else if (PtInRect(&gapMinus, pt)) {
          g_tapGapMs.store(ClampTapMs(g_tapGapMs.load(std::memory_order_relaxed) - 1), std::memory_order_relaxed);
        } else if (PtInRect(&gapPlus, pt)) {
          g_tapGapMs.store(ClampTapMs(g_tapGapMs.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
        }
        SaveSettings();
        RequestOverlayRepaint();
        return 0;
      }
      if (PtInRect(&header, pt) && !PtInRect(&exitButton, pt)) {
        g_hudDragging = true;
        g_hudDragOffset = POINT{pt.x - panel.left, pt.y - panel.top};
        SetCapture(hwnd);
        return 0;
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (g_hudDragging) {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd, &pt);
        RECT desired{pt.x - g_hudDragOffset.x, pt.y - g_hudDragOffset.y,
                     pt.x - g_hudDragOffset.x + kHudWidth, pt.y - g_hudDragOffset.y + CurrentHudHeight()};
        RECT clamped = ClampHudScreenRect(desired);
        g_hudUserPlaced = true;
        g_hudPos = POINT{clamped.left, clamped.top};
        SetWindowPos(hwnd, HWND_TOPMOST, clamped.left, clamped.top, kHudWidth, CurrentHudHeight(),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return 0;
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (g_hudDragging) {
        g_hudDragging = false;
        ReleaseCapture();
        return 0;
      }
      RECT rc{};
      GetClientRect(hwnd, &rc);
      PreviewState state = SnapshotPreviewState();
      RECT panel = HudPanelRect(state, rc);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      g_overlayRepaintPending.store(false);
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(hdc, &rc, clearBrush);
      DeleteObject(clearBrush);

      PreviewState state = SnapshotPreviewState();

      SetBkMode(hdc, TRANSPARENT);
      HPEN redPen = CreatePen(PS_SOLID, 2, RGB(70, 255, 120));
      HBRUSH panelBrush = CreateSolidBrush(RGB(10, 14, 20));
      HBRUSH panelHeaderBrush = CreateSolidBrush(RGB(18, 28, 42));
      HBRUSH runningBrush = CreateSolidBrush(RGB(255, 150, 45));
      HBRUSH readyBrush = CreateSolidBrush(RGB(80, 255, 140));
      HBRUSH dimPillBrush = CreateSolidBrush(RGB(30, 42, 54));
      HGDIOBJ oldPen = SelectObject(hdc, redPen);
      HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

      HFONT font = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
      HGDIOBJ oldFont = SelectObject(hdc, font);

      RECT panel = HudPanelRect(state, rc);
      const int px = panel.left;
      const int py = panel.top;
      RECT hotkeyBox = HudHotkeyRect(panel);
      RECT confirmBox = HudConfirmRect(panel);
      RECT overlayBox = HudOverlayCheckRect(panel);
      RECT holdMinus = HudTapHoldMinusRect(panel);
      RECT holdValue = HudTapHoldValueRect(panel);
      RECT holdPlus = HudTapHoldPlusRect(panel);
      RECT gapMinus = HudTapGapMinusRect(panel);
      RECT gapValue = HudTapGapValueRect(panel);
      RECT gapPlus = HudTapGapPlusRect(panel);
      SelectObject(hdc, panelBrush);
      RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom, 14, 14);
      SelectObject(hdc, panelHeaderBrush);
      RoundRect(hdc, panel.left, panel.top, panel.right, panel.top + 42, 14, 14);
      SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
      SelectObject(hdc, redPen);
      RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom, 14, 14);

      RECT led{px + 14, py + 13, px + 26, py + 25};
      const bool isRunning = g_running.load(std::memory_order_relaxed);
      SelectObject(hdc, isRunning ? readyBrush : readyBrush);
      Ellipse(hdc, led.left, led.top, led.right, led.bottom);

      const wchar_t* stateTitle = isRunning ? L"AutoHack for QuellGTA" : L"AutoHack for QuellGTA";
      SetTextColor(hdc, isRunning ? RGB(145, 255, 175) : RGB(145, 255, 175));
      TextOutW(hdc, px + 36, py + 10, stateTitle, static_cast<int>(wcslen(stateTitle)));

      SetTextColor(hdc, RGB(230, 230, 230));
      TextOutW(hdc, px + 14, py + 54, state.status.c_str(), static_cast<int>(std::min<size_t>(state.status.size(), 48)));

      const bool listening = g_listeningHotkey.load(std::memory_order_relaxed);
      const bool settingsExpanded = g_settingsExpanded.load(std::memory_order_relaxed);
      RECT settingsToggle = HudSettingsToggleRect(panel);

      HBRUSH confirmBrush = CreateSolidBrush(RGB(22, 48, 44));

      SelectObject(hdc, oldFont);
      SelectObject(hdc, oldBrush);
      SelectObject(hdc, oldPen);
      DeleteObject(font);
      DeleteObject(redPen);
      DeleteObject(panelBrush);
      DeleteObject(panelHeaderBrush);
      DeleteObject(runningBrush);
      DeleteObject(readyBrush);
      DeleteObject(dimPillBrush);
      DeleteObject(confirmBrush);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

LRESULT CALLBACK MarksProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      SetTimer(hwnd, 1, 50, nullptr);
      return 0;
    case WM_TIMER: {
      if (wParam != 1) return 0;
      PreviewState state = SnapshotPreviewState();
      if (!g_overlayCursorEnabled.load(std::memory_order_relaxed) ||
          !g_running.load(std::memory_order_relaxed) || state.bars.empty()) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }

      int minX = state.bars.front().x1;
      int maxX = state.bars.front().x2;
      for (const auto& b : state.bars) {
        minX = std::min(minX, b.x1);
        maxX = std::max(maxX, b.x2);
      }

      const int infoW = ScaledPx(96, state.scale);
      const int infoH = ScaledPx(44, state.scale);
      const int gap = ScaledPx(12, state.scale);
      const int pad = ScaledPx(12, state.scale);
      const int infoRight = state.red.x1 - gap;
      const int markerScreenY = state.red.centerY;
      RECT desired{std::min(infoRight - infoW, minX) - pad, markerScreenY - infoH / 2 - pad,
                   std::max(infoRight, maxX) + pad, markerScreenY + infoH / 2 + pad};
      RECT clamped = ClampHudScreenRect(desired);
      SetWindowPos(hwnd, HWND_TOPMOST, clamped.left, clamped.top, clamped.right - clamped.left, clamped.bottom - clamped.top,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      g_marksRepaintPending.store(false, std::memory_order_relaxed);
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(hdc, &rc, clearBrush);
      DeleteObject(clearBrush);

      PreviewState state = SnapshotPreviewState();
      if (!g_overlayCursorEnabled.load(std::memory_order_relaxed) ||
          !g_running.load(std::memory_order_relaxed) || state.bars.empty()) {
        EndPaint(hwnd, &ps);
        return 0;
      }

      RECT wr{};
      GetWindowRect(hwnd, &wr);
      SetBkMode(hdc, TRANSPARENT);
      HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
      HGDIOBJ oldFont = SelectObject(hdc, font);
      HPEN glowPen = CreatePen(PS_SOLID, 4, RGB(18, 66, 50));
      HPEN activePen = CreatePen(PS_SOLID, 2, RGB(112, 255, 198));
      HPEN donePen = CreatePen(PS_SOLID, 2, RGB(82, 160, 220));
      HPEN dimPen = CreatePen(PS_SOLID, 2, RGB(58, 70, 84));
      HBRUSH activeBrush = CreateSolidBrush(RGB(112, 255, 198));
      HBRUSH doneBrush = CreateSolidBrush(RGB(50, 112, 158));
      HBRUSH dimBrush = CreateSolidBrush(RGB(28, 38, 50));
      HBRUSH infoBrush = CreateSolidBrush(RGB(8, 14, 18));
      HGDIOBJ oldPen = SelectObject(hdc, glowPen);
      HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

      const int activeBar = g_hudActiveBar.load(std::memory_order_relaxed);
      const int markerW = ScaledPx(28, state.scale);
      const int markerH = ScaledPx(8, state.scale);
      const int markerLineY = state.red.centerY - wr.top;
      for (size_t i = 0; i < state.bars.size(); ++i) {
        const auto& b = state.bars[i];
        const int cx = (b.x1 + b.x2) / 2 - wr.left;
        RECT mark{cx - markerW / 2, markerLineY - markerH / 2, cx + markerW / 2, markerLineY + markerH / 2};
        SelectObject(hdc, glowPen);
        SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(hdc, mark.left - 2, mark.top - 2, mark.right + 2, mark.bottom + 2, markerH + 4, markerH + 4);
        if (activeBar == static_cast<int>(i) + 1) {
          SelectObject(hdc, activePen);
          SelectObject(hdc, activeBrush);
        } else if (activeBar > static_cast<int>(i) + 1) {
          SelectObject(hdc, donePen);
          SelectObject(hdc, doneBrush);
        } else {
          SelectObject(hdc, dimPen);
          SelectObject(hdc, dimBrush);
        }
        RoundRect(hdc, mark.left, mark.top, mark.right, mark.bottom, markerH, markerH);
      }

      const int infoW = ScaledPx(96, state.scale);
      const int infoH = ScaledPx(44, state.scale);
      const int gap = ScaledPx(12, state.scale);
      const int infoRight = state.red.x1 - gap - wr.left;
      const int infoCenterY = state.red.centerY - wr.top;
      const int dtMs = g_hudDtMs.load(std::memory_order_relaxed);
      const int ttcMs = g_hudTtcMs.load(std::memory_order_relaxed);
      std::wstring dtText = L"dt " + std::to_wstring(dtMs) + L"ms";
      std::wstring ttcText = L"ttc " + std::to_wstring(ttcMs) + L"ms";
      RECT info{infoRight - infoW, infoCenterY - infoH / 2, infoRight, infoCenterY + infoH / 2};
      SelectObject(hdc, infoBrush);
      SelectObject(hdc, activePen);
      RoundRect(hdc, info.left, info.top, info.right, info.bottom, 8, 8);
      SetTextColor(hdc, RGB(180, 255, 218));
      const int textX = info.left + ScaledPx(8, state.scale);
      TextOutW(hdc, textX, info.top + ScaledPx(5, state.scale), dtText.c_str(), static_cast<int>(dtText.size()));
      TextOutW(hdc, textX, info.top + ScaledPx(23, state.scale), ttcText.c_str(), static_cast<int>(ttcText.size()));

      SelectObject(hdc, oldBrush);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldFont);
      DeleteObject(font);
      DeleteObject(glowPen);
      DeleteObject(activePen);
      DeleteObject(donePen);
      DeleteObject(dimPen);
      DeleteObject(activeBrush);
      DeleteObject(doneBrush);
      DeleteObject(dimBrush);
      DeleteObject(infoBrush);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

LRESULT CALLBACK CursorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      SetTimer(hwnd, 1, 33, nullptr);
      return 0;
    case WM_TIMER: {
      if (wParam != 1) return 0;
      if (!g_overlayCursorEnabled.load(std::memory_order_relaxed) ||
          !g_cursorVisible.load(std::memory_order_relaxed) ||
          !g_running.load(std::memory_order_relaxed)) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      const int cursorX = g_cursorX.load(std::memory_order_relaxed);
      const int cursorY = g_cursorY.load(std::memory_order_relaxed);
      const bool inZone = g_cursorInZone.load(std::memory_order_relaxed);
      int targetY = g_cursorTargetY.load(std::memory_order_relaxed);
      if (targetY == 0) targetY = cursorY;
      const int top = inZone ? std::min(cursorY - kCursorSize / 2, targetY - 10) : cursorY - kCursorSize / 2;
      const int bottom = inZone ? std::max(cursorY + kCursorSize / 2, targetY + 10) : cursorY + kCursorSize / 2;
      const int x = cursorX - kCursorSize / 2;
      RECT desired{x, top, x + kCursorSize, bottom};
      RECT clamped = ClampHudScreenRect(desired);
      SetWindowPos(hwnd, HWND_TOPMOST, clamped.left, clamped.top, kCursorSize, clamped.bottom - clamped.top,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(hdc, &rc, clearBrush);
      DeleteObject(clearBrush);

      SetBkMode(hdc, TRANSPARENT);
      HPEN glowPen = CreatePen(PS_SOLID, 8, RGB(20, 70, 18));
      HPEN arrowPen = CreatePen(PS_SOLID, 5, RGB(72, 255, 36));
      HPEN linkGlowPen = CreatePen(PS_SOLID, 5, RGB(16, 62, 22));
      HPEN linkPen = CreatePen(PS_SOLID, 2, RGB(72, 255, 36));
      HGDIOBJ oldPen = SelectObject(hdc, glowPen);
      HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

      RECT wr{};
      GetWindowRect(hwnd, &wr);
      const int cursorY = g_cursorY.load(std::memory_order_relaxed);
      const int targetY = g_cursorTargetY.load(std::memory_order_relaxed);
      const int cy = cursorY - wr.top;
      const int ty = targetY - wr.top;
      const int arrowTop = cy - kCursorArrowTopOffset;
      const int arrowBottom = cy + kCursorArrowBottomOffset;
      const bool targetAbove = targetY < cursorY;
      const int edgeY = targetAbove ? arrowTop : arrowBottom;
      const bool docked = std::abs(ty - edgeY) <= 2;
      const bool inZone = g_cursorInZone.load(std::memory_order_relaxed);

      POINT arrow[] = {
          {35, arrowTop},
          {18, arrowTop},
          {29, cy},
          {18, arrowBottom},
          {35, arrowBottom},
          {46, cy},
          {35, arrowTop},
      };

      if (inZone && !docked) {
        const int lx = 50;
        SelectObject(hdc, linkGlowPen);
        MoveToEx(hdc, lx, edgeY, nullptr);
        LineTo(hdc, lx, ty);
        MoveToEx(hdc, 40, ty, nullptr);
        LineTo(hdc, 62, ty);
        SelectObject(hdc, linkPen);
        MoveToEx(hdc, lx, edgeY, nullptr);
        LineTo(hdc, lx, ty);
        MoveToEx(hdc, 40, ty, nullptr);
        LineTo(hdc, 62, ty);
      }

      SelectObject(hdc, glowPen);
      Polyline(hdc, arrow, static_cast<int>(sizeof(arrow) / sizeof(arrow[0])));
      SelectObject(hdc, arrowPen);
      Polyline(hdc, arrow, static_cast<int>(sizeof(arrow) / sizeof(arrow[0])));

      SelectObject(hdc, oldBrush);
      SelectObject(hdc, oldPen);
      DeleteObject(glowPen);
      DeleteObject(arrowPen);
      DeleteObject(linkGlowPen);
      DeleteObject(linkPen);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      ApplyHotkeySetting(hwnd);
      return 0;
    case WM_HOTKEY:
      if (wParam == kHotkeyToggleId) {
        if (g_listeningHotkey.load(std::memory_order_relaxed)) return 0;
        if (g_running.load()) {
          StopWorker();
        } else {
          StartWorker();
        }
        return 0;
      }
      return 0;
    case WM_APP_LOG: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      OutputDebugStringW(text->c_str());
      OutputDebugStringW(L"\n");
      {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        g_preview.lastLog = *text;
        g_preview.running = g_running.load();
      }
      RequestOverlayRepaint();
      return 0;
    }
    case WM_APP_STATUS: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        g_preview.status = *text;
        g_preview.running = g_running.load();
      }
      RequestOverlayRepaint();
      return 0;
    }
    case WM_APP_WORKER_DONE:
      {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        g_preview.running = false;
      }
      RequestOverlayRepaint();
      if (g_worker.joinable()) g_worker.detach();
      return 0;
    case WM_CLOSE:
      StopWorker();
      if (g_marksWnd) DestroyWindow(g_marksWnd);
      if (g_cursorWnd) DestroyWindow(g_cursorWnd);
      if (g_overlayWnd) DestroyWindow(g_overlayWnd);
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      StopWorker();
      UnregisterHotKey(hwnd, kHotkeyToggleId);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

}  // namespace

void SetHostWindow(HWND hwnd) { g_mainWnd = hwnd; }
HWND HudWindow() { return g_overlayWnd; }
HWND CursorWindow() { return g_cursorWnd; }
HWND MarksWindow() { return g_marksWnd; }
void SetHudWindow(HWND hwnd) { g_overlayWnd = hwnd; }
void SetCursorWindow(HWND hwnd) { g_cursorWnd = hwnd; }
void SetMarksWindow(HWND hwnd) { g_marksWnd = hwnd; }
bool OverlayEnabled() { return g_overlayCursorEnabled.load(std::memory_order_relaxed); }
void SetOverlayEnabled(bool enabled) { g_overlayCursorEnabled.store(enabled, std::memory_order_relaxed); SaveSettings(); }
int TapHoldMs() { return g_tapHoldMs.load(std::memory_order_relaxed); }
int TapGapMs() { return g_tapGapMs.load(std::memory_order_relaxed); }
int HotkeyVk() { return g_hotkeyVk.load(std::memory_order_relaxed); }
std::wstring HotkeyName() { return KeyName(HotkeyVk()); }
void LoadPersistentSettings() { LoadSettings(); }
void ApplyHotkey(HWND hwnd) { ApplyHotkeySetting(hwnd); }
bool Running() { return g_running.load(std::memory_order_relaxed); }
void RequestStop() { g_stopWorker.store(true, std::memory_order_relaxed); }
void MarkRunning(bool running) { g_running.store(running, std::memory_order_relaxed); }
void ResetStopFlag() { g_stopWorker.store(false, std::memory_order_relaxed); }
bool StopRequested() { return g_stopWorker.load(std::memory_order_relaxed); }
std::thread& WorkerThread() { return g_worker; }
void PostModuleLog(const std::wstring& text) { PostLog(text); }
void PostModuleStatus(const std::wstring& text) { PostStatus(text); }
void RepaintHud() { RequestOverlayRepaint(); }
void ClearOverlayState() {
  g_cursorVisible.store(false, std::memory_order_relaxed);
  g_cursorInZone.store(false, std::memory_order_relaxed);
  g_hudActiveBar.store(0, std::memory_order_relaxed);
  g_hudTtcMs.store(-1, std::memory_order_relaxed);
  g_hudDtMs.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_previewMutex);
    const std::wstring status = g_preview.status;
    const std::wstring lastLog = g_preview.lastLog;
    g_preview = PreviewState{};
    g_preview.status = status;
    g_preview.lastLog = lastLog;
    g_preview.running = g_running.load(std::memory_order_relaxed);
  }
  if (g_cursorWnd) ShowWindow(g_cursorWnd, SW_HIDE);
  if (g_marksWnd) ShowWindow(g_marksWnd, SW_HIDE);
  RequestOverlayRepaint();
}
void HideTransientOverlays() { ClearOverlayState(); }
bool DetectInGame() { CaptureFrame frame; if (!CaptureScreenRegion(frame, nullptr)) return false; return AnalyzeFrame(frame).inMinigame; }
void RunSession() { WorkerLoop(); ClearOverlayState(); }
RECT InitialHudRect() { return InitialHudScreenRect(); }
int HudWidth() { return kHudWidth; }
int HudHeight() { return CurrentHudHeight(); }
int CursorSize() { return kCursorSize; }
int HotkeyId() { return kHotkeyToggleId; }
bool IsListeningHotkey() { return g_listeningHotkey.load(std::memory_order_relaxed); }
void UpdatePreviewRunning(bool running) { std::lock_guard<std::mutex> lock(g_previewMutex); g_preview.running = running; }
void SetHudLogText(const std::wstring& text) { std::lock_guard<std::mutex> lock(g_previewMutex); g_preview.lastLog = text; g_preview.running = g_running.load(std::memory_order_relaxed); }
void SetHudStatusText(const std::wstring& text) { std::lock_guard<std::mutex> lock(g_previewMutex); g_preview.status = text; g_preview.running = g_running.load(std::memory_order_relaxed); }
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return OverlayProc(hwnd, msg, wParam, lParam); }
LRESULT CALLBACK CursorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return CursorProc(hwnd, msg, wParam, lParam); }
LRESULT CALLBACK MarksWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return MarksProc(hwnd, msg, wParam, lParam); }

}  // namespace gta5::games::slider
