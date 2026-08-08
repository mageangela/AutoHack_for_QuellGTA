#include <windows.h>
#include <windowsx.h>
#include "../capture/game_window.h"
#include "../app/app_ui.h"
#include "../app/app_runtime.h"
#include "../input/key_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <functional>
#include <iomanip>
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
constexpr double kBaselineScreenHeightPx = 1080.0;
constexpr double kAnalysisIntervalSeconds = 1.0 / 60.0;
constexpr double kDefaultEndToEndLatencySeconds = 0.060;
constexpr double kMaximumCalibratedLatencySeconds = 0.500;
constexpr double kEdgeTriggerZonePx = 80.0;
constexpr double kMinUsableVelocityPxPerSec = 25.0;
constexpr double kInvalidPredictionSeconds = 999.0;
constexpr double kYellowCenterStripHalfWidthPx = 8.0;
constexpr double kYellowHalfOverlapPx = 72.0;
constexpr int kOverlayMargin = 18;
constexpr int kCursorSize = 64;
constexpr int kCursorArrowTopOffset = 14;
constexpr int kCursorArrowBottomOffset = 14;
constexpr int kCursorArrowRightOffset = 14;
constexpr COLORREF kOverlayGreen = RGB(70, 255, 120);
constexpr COLORREF kOverlayBrightGreen = RGB(80, 255, 140);
constexpr COLORREF kOverlayTextGreen = RGB(145, 255, 175);
constexpr COLORREF kOverlayWarningOrange = RGB(255, 174, 72);
constexpr COLORREF kOverlayDeepGreen = RGB(18, 66, 42);
constexpr COLORREF kOverlayBlack = RGB(10, 14, 20);

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
  double toScreenX = 1.0;
  double toScreenY = 1.0;
  double analysisGeometryScale = 1.0;
  double screenGeometryScale = 1.0;
  std::uint64_t windowGeneration = 0;
  std::vector<uint32_t> bgra;
};

struct TrackSample {
  double center = 0.0;
  std::chrono::steady_clock::time_point time;
  int topBottomY = 0;
  int bottomTopY = 0;
};

struct TrackSlot {
  bool valid = false;
  double lastCenter = 0.0;
  double velocity = 0.0;
  std::vector<TrackSample> history;
};

struct PostPressCalibration {
  bool active = false;
  int barIndex = -1;
  gta5::input::Job inputJob;
  std::chrono::steady_clock::time_point scheduledAt{};
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
  double velocity = 0.0;
  double scale = 1.0;
  int screenX = 0;
  int screenY = 0;
  int screenW = 0;
  int screenH = 0;
};

HWND g_mainWnd = nullptr;
HWND g_cursorWnd = nullptr;
HWND g_marksWnd = nullptr;

std::atomic<bool> g_marksRepaintPending{false};
std::mutex g_previewMutex;
PreviewState g_preview;
std::atomic<int> g_hudActiveBar{0};
std::atomic<int> g_hudEdgePx{0};
std::atomic<int> g_hudTtcMs{-1};
std::atomic<int> g_hudAnalysisHz10{0};
std::atomic<int> g_hudEndToEndMs{-1};
std::atomic<int> g_hudVelocityPx{0};
std::atomic<int> g_hudScale100{100};
std::atomic<bool> g_cursorVisible{false};
std::atomic<int> g_cursorX{0};
std::atomic<int> g_cursorY{0};
std::atomic<int> g_cursorTargetY{0};
std::atomic<bool> g_cursorInZone{false};
std::atomic<int> g_cursorBar{0};
thread_local std::vector<int> g_rowCountScratch;

struct InGameCache {
  bool valid = false;
  std::uint64_t windowGeneration = 0;
  RedBar red;
  std::vector<BarMeasure> bars;
  SearchCells cells;
  double geometryScale = 1.0;
};

InGameCache g_inGameCache;

// All tunable pixel constants are authored for a 1080p game image.
// Runtime geometry follows the GTA client height, including windowed mode.
double GeometryScaleFromScreenPoint(int screenX, int screenY) {
  (void)screenX;
  (void)screenY;
  RECT game{};
  const int h = gta5::capture::GetGameClientRect(game) ? game.bottom - game.top : GetSystemMetrics(SM_CYSCREEN);
  return h > 0 ? std::clamp(h / kBaselineScreenHeightPx, 0.45, 2.25) : 1.0;
}

double GeometryScaleFromRedScreen(const RedBar& red) {
  if (!red.ok) return GeometryScaleFromScreenPoint(0, 0);
  return GeometryScaleFromScreenPoint((red.x1 + red.x2) / 2, red.centerY);
}

RECT VirtualDesktopRect() {
  return RECT{
      GetSystemMetrics(SM_XVIRTUALSCREEN),
      GetSystemMetrics(SM_YVIRTUALSCREEN),
      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
  };
}

RECT ClampOverlayScreenRect(RECT panel) {
  RECT vd = VirtualDesktopRect();
  const int panelW = panel.right - panel.left;
  const int panelH = panel.bottom - panel.top;
  const int minLeft = static_cast<int>(vd.left) + kOverlayMargin;
  const int minTop = static_cast<int>(vd.top) + kOverlayMargin;
  const int maxLeft = std::max(minLeft, static_cast<int>(vd.right) - panelW - kOverlayMargin);
  const int maxTop = std::max(minTop, static_cast<int>(vd.bottom) - panelH - kOverlayMargin);
  const int left = std::clamp(static_cast<int>(panel.left), minLeft, maxLeft);
  const int top = std::clamp(static_cast<int>(panel.top), minTop, maxTop);
  panel.left = left;
  panel.top = top;
  panel.right = left + panelW;
  panel.bottom = top + panelH;
  return panel;
}

PreviewState SnapshotPreviewState() {
  static PreviewState cached;
  std::unique_lock<std::mutex> lock(g_previewMutex, std::try_to_lock);
  if (lock.owns_lock()) cached = g_preview;
  return cached;
}

void RequestMarksRepaint() {
  bool expected = false;
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

inline bool IsRed(uint32_t px) {
  const double r = static_cast<double>((px >> 16) & 0xff);
  const double g = static_cast<double>((px >> 8) & 0xff);
  const double b = static_cast<double>(px & 0xff);
  const double maximum = std::max({r, g, b});
  const double minimum = std::min({r, g, b});
  const double chroma = maximum - minimum;
  return maximum >= 40 && chroma >= 20 && chroma / maximum >= .25 &&
         r == maximum && r - std::max(g, b) >= std::max(10.0, chroma * .35);
}

inline bool IsWhite(uint32_t px) {
  const int r = static_cast<int>((px >> 16) & 0xff);
  const int g = static_cast<int>((px >> 8) & 0xff);
  const int b = static_cast<int>(px & 0xff);
  return r >= 210 && g >= 210 && b >= 210 && std::abs(r - g) <= 38 && std::abs(r - b) <= 38;
}

inline bool IsYellow(uint32_t px) {
  const int r = static_cast<int>((px >> 16) & 0xff);
  const int g = static_cast<int>((px >> 8) & 0xff);
  const int b = static_cast<int>(px & 0xff);
  return r >= 185 && g >= 145 && b <= 95 && r >= b + 90 && g >= b + 55 && std::abs(r - g) <= 95;
}

bool CaptureScreenRegion(CaptureFrame& frame, const RectI* region, const RECT* sessionClient = nullptr) {
  RECT requested{};
  const RECT* requestedPtr = nullptr;
  if (region) {
    requested = {region->x, region->y, region->x + region->w, region->y + region->h};
    requestedPtr = &requested;
  }
  gta5::capture::GameFrame captured;
  captured.bgra = std::move(frame.bgra);
  const bool capturedOk = sessionClient
                              ? gta5::capture::CaptureGameFrameFromClientRect(captured, *sessionClient, requestedPtr)
                              : gta5::capture::CaptureGameFrame(captured, requestedPtr);
  if (!capturedOk) {
    frame.bgra = std::move(captured.bgra);
    return false;
  }
  frame.x = captured.screenX;
  frame.y = captured.screenY;
  frame.w = captured.width;
  frame.h = captured.height;
  frame.toScreenX = captured.toScreenX;
  frame.toScreenY = captured.toScreenY;
  frame.windowGeneration = captured.windowGeneration;
  const int gameHeight = captured.clientHeight;
  frame.analysisGeometryScale = std::clamp(std::min(gameHeight, 1080) / kBaselineScreenHeightPx, 0.45, 2.25);
  frame.screenGeometryScale = std::clamp(gameHeight / kBaselineScreenHeightPx, 0.45, 2.25);
  frame.bgra = std::move(captured.bgra);
  return true;
}

uint32_t Pixel(const CaptureFrame& f, int x, int y) {
  return f.bgra[static_cast<size_t>(y) * f.w + x];
}

int ToScreenX(const CaptureFrame& f, int x) {
  return f.x + static_cast<int>(std::lround(x * f.toScreenX));
}

int ToScreenY(const CaptureFrame& f, int y) {
  return f.y + static_cast<int>(std::lround(y * f.toScreenY));
}

int ToFrameX(const CaptureFrame& f, int x) {
  return static_cast<int>(std::lround((x - f.x) / std::max(0.0001, f.toScreenX)));
}

int ToFrameY(const CaptureFrame& f, int y) {
  return static_cast<int>(std::lround((y - f.y) / std::max(0.0001, f.toScreenY)));
}

std::vector<std::pair<int, int>> GroupRuns(const std::vector<int>& values, int begin, int end, int threshold, int minWidth);

RedBar LocateRedBar(const CaptureFrame& f) {
  RedBar red;
  const int cx1 = std::max(0, f.w / 32);
  const int cx2 = std::min(f.w, f.w - f.w / 32);
  const int cy1 = std::max(0, f.h / 12);
  const int cy2 = std::min(f.h, f.h - f.h / 12);

  auto& rowCount = g_rowCountScratch;
  rowCount.assign(f.h, 0);
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

  // Moving white/yellow bars can split the coarse red component. Sample away
  // from the vertical frame edges and keep the longest vertical red segment
  // that intersects the coarse band.
  const int sampleInset = std::max(3, static_cast<int>(std::lround(f.h * .025)));
  const int sampleStep = std::max(3, static_cast<int>(std::lround(f.h * .025)));
  const int searchMargin = std::max(4, static_cast<int>(std::lround(f.h * .06)));
  const int sampleLeft = x1 + sampleInset;
  const int sampleRight = x2 - sampleInset;
  const int searchTop = std::max(cy1, y1 - searchMargin);
  const int searchBottom = std::min(cy2 - 1, y2 + searchMargin);
  int refinedY1 = y1;
  int refinedY2 = y2;
  int longestSpan = 0;
  for (int x = sampleLeft; x <= sampleRight; x += sampleStep) {
    int runStart = -1;
    for (int y = searchTop; y <= searchBottom + 1; ++y) {
      const bool redPixel = y <= searchBottom && IsRed(Pixel(f, x, y));
      if (redPixel) {
        if (runStart < 0) runStart = y;
        continue;
      }
      if (runStart < 0) continue;
      const int runEnd = y - 1;
      if (runStart <= y2 && runEnd >= y1) {
        const int span = runEnd - runStart + 1;
        if (span > longestSpan) {
          longestSpan = span;
          refinedY1 = runStart;
          refinedY2 = runEnd;
        }
      }
      runStart = -1;
    }
  }
  if (longestSpan > 0) {
    y1 = refinedY1;
    y2 = refinedY2;
  }

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

    auto& rowCount = g_rowCountScratch;
    rowCount.assign(f.h, 0);
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

  auto& rowCount = g_rowCountScratch;
  rowCount.assign(f.h, 0);
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

bool MeasureYellowGapForBar(const CaptureFrame& f, const RedBar& red, int index,
                            int cellLeftScreen, int cellRightScreen, double scale,
                            int verticalHalf, YellowMeasure& out) {
  const double localScale = scale / std::max(0.0001, (f.toScreenX + f.toScreenY) * 0.5);
  const int cellLeft = ToFrameX(f, cellLeftScreen);
  const int cellRight = ToFrameX(f, cellRightScreen);
  const int centerY = ToFrameY(f, red.centerY);
  int y1 = std::max(0, centerY - ScaledPx(320, localScale));
  int y2 = std::min(f.h - 1, centerY + ScaledPx(320, localScale));
  const int halfOverlap = ScaledPx(kYellowHalfOverlapPx, localScale);
  if (verticalHalf < 0) y2 = std::min(y2, centerY + halfOverlap);
  if (verticalHalf > 0) y1 = std::max(y1, centerY - halfOverlap);
  if (cellRight < 0 || cellLeft >= f.w || y2 <= y1) return false;

  auto& rowCount = g_rowCountScratch;
  rowCount.assign(f.h, 0);
  const int cellCenter = (cellLeft + cellRight) / 2;
  const int stripHalfWidth = ScaledPx(kYellowCenterStripHalfWidthPx, localScale);
  const int scanX1 = std::clamp(cellCenter - stripHalfWidth, 0, f.w - 1);
  const int scanX2 = std::clamp(cellCenter + stripHalfWidth, 0, f.w - 1);
  for (int y = y1; y <= y2; ++y) {
    int count = 0;
    for (int x = scanX1; x <= scanX2; ++x) {
      if (IsYellow(Pixel(f, x, y))) ++count;
    }
    rowCount[y] = count;
  }

  auto runs = GroupRuns(rowCount, y1, y2 + 1,
                        ScaledPx(2, localScale), ScaledPx(1, localScale));
  if (runs.size() < 2) return false;

  std::pair<int, int> topRun{};
  std::pair<int, int> bottomRun{};
  int bestScore = -1000000;
  bool foundPair = false;
  for (size_t i = 0; i + 1 < runs.size(); ++i) {
    for (size_t j = i + 1; j < runs.size(); ++j) {
      const int verticalGap = runs[j].first - runs[i].second - 1;
      const int topHeight = runs[i].second - runs[i].first + 1;
      const int bottomHeight = runs[j].second - runs[j].first + 1;
      if (verticalGap < ScaledPx(8, localScale) || verticalGap > ScaledPx(95, localScale)) continue;

      int yellowPixels = 0;
      for (int y = runs[i].first; y <= runs[i].second; ++y) yellowPixels += rowCount[y];
      for (int y = runs[j].first; y <= runs[j].second; ++y) yellowPixels += rowCount[y];
      const int balancePenalty = std::abs(topHeight - bottomHeight) * 2;
      const int gapPenalty = std::abs(verticalGap - ScaledPx(24, localScale));
      const int score = yellowPixels + topHeight + bottomHeight - balancePenalty - gapPenalty;
      if (score > bestScore) {
        bestScore = score;
        topRun = runs[i];
        bottomRun = runs[j];
        foundPair = true;
      }
    }
  }
  if (!foundPair) return false;

  out.ok = true;
  out.index = index;
  out.topBottomY = ToScreenY(f, topRun.second);
  out.bottomTopY = ToScreenY(f, bottomRun.first);
  out.gapCenterY = (out.topBottomY + out.bottomTopY) / 2.0;
  int score = 0;
  for (int y = topRun.first; y <= topRun.second; ++y) score += rowCount[y];
  for (int y = bottomRun.first; y <= bottomRun.second; ++y) score += rowCount[y];
  out.score = score;
  return true;
}

YellowMeasure FindActiveYellowMeasure(const CaptureFrame& f, const RedBar& red, const SearchCells& cells, double scale,
                                      int preferredVerticalHalf,
                                      const std::array<bool, 8>* skip = nullptr) {
  YellowMeasure best;
  if (!red.ok || !cells.ok || cells.xRanges.size() != 8) return best;
  const double localScale = scale / std::max(0.0001, (f.toScreenX + f.toScreenY) * 0.5);
  const int minimumScore = ScaledPx(20, localScale);
  const int passCount = preferredVerticalHalf == 0 ? 1 : 2;
  for (int pass = 0; pass < passCount && !best.ok; ++pass) {
    const int verticalHalf = pass == 0 ? preferredVerticalHalf : -preferredVerticalHalf;
    for (int i = 0; i < 8; ++i) {
      if (skip && (*skip)[i]) continue;
      YellowMeasure m;
      if (MeasureYellowGapForBar(f, red, i, cells.xRanges[i].first,
                                 cells.xRanges[i].second, scale, verticalHalf, m)) {
        if (m.score >= minimumScore && (!best.ok || m.score > best.score)) best = m;
      }
    }
  }
  return best;
}

YellowMeasure FindActiveYellowMeasureCandidates(const CaptureFrame& f, const RedBar& red, const SearchCells& cells,
                                                const std::array<int, 3>& candidates, double scale,
                                                int preferredVerticalHalf,
                                                std::array<bool, 8>& scanned) {
  YellowMeasure best;
  scanned.fill(false);
  if (!red.ok || !cells.ok || cells.xRanges.size() != 8) return best;
  for (int rawIndex : candidates) scanned[std::clamp(rawIndex, 0, 7)] = true;
  const double localScale = scale / std::max(0.0001, (f.toScreenX + f.toScreenY) * 0.5);
  const int minimumScore = ScaledPx(20, localScale);
  const int passCount = preferredVerticalHalf == 0 ? 1 : 2;
  for (int pass = 0; pass < passCount && !best.ok; ++pass) {
    const int verticalHalf = pass == 0 ? preferredVerticalHalf : -preferredVerticalHalf;
    for (int i = 0; i < 8; ++i) {
      if (!scanned[i]) continue;
      YellowMeasure m;
      if (MeasureYellowGapForBar(f, red, i, cells.xRanges[i].first,
                                 cells.xRanges[i].second, scale, verticalHalf, m)) {
        if (m.score >= minimumScore && (!best.ok || m.score > best.score)) best = m;
      }
    }
  }
  return best;
}

std::array<int, 3> YellowCandidateIndices(int lastYellowIndex, int expectedIndex) {
  if (lastYellowIndex >= 0) {
    return {lastYellowIndex, lastYellowIndex - 1, lastYellowIndex + 1};
  }
  return {expectedIndex, expectedIndex + 1, expectedIndex - 1};
}

RectI BuildYellowCaptureRegion(const RedBar& red, const SearchCells& cells,
                               const std::array<int, 3>& candidates,
                               double scale, int preferredVerticalHalf) {
  int left = cells.xRanges[std::clamp(candidates[0], 0, 7)].first;
  int right = cells.xRanges[std::clamp(candidates[0], 0, 7)].second;
  for (int rawIndex : candidates) {
    const int index = std::clamp(rawIndex, 0, 7);
    left = std::min(left, cells.xRanges[index].first);
    right = std::max(right, cells.xRanges[index].second);
  }

  const int horizontalMargin = ScaledPx(6, scale);
  const int fullExtent = ScaledPx(320, scale);
  const int halfOverlap = ScaledPx(kYellowHalfOverlapPx, scale);
  int top = red.centerY - fullExtent;
  int bottom = red.centerY + fullExtent;
  if (preferredVerticalHalf < 0) bottom = red.centerY + halfOverlap;
  if (preferredVerticalHalf > 0) top = red.centerY - halfOverlap;
  return {left - horizontalMargin, top,
          right - left + 1 + horizontalMargin * 2, bottom - top + 1};
}

RectI BuildFullYellowCaptureRegion(const RedBar& red, const SearchCells& cells,
                                   double scale) {
  const int horizontalMargin = ScaledPx(6, scale);
  const int verticalExtent = ScaledPx(320, scale);
  const int left = cells.xRanges.front().first - horizontalMargin;
  const int right = cells.xRanges.back().second + horizontalMargin;
  const int top = red.centerY - verticalExtent;
  const int bottom = red.centerY + verticalExtent;
  return {left, top, right - left + 1, bottom - top + 1};
}

FrameAnalysis AnalyzeFrame(const CaptureFrame& f, const std::vector<std::pair<int, int>>* knownXRuns = nullptr) {
  FrameAnalysis analysis;
  analysis.red = LocateRedBar(f);
  if (!analysis.red.ok) {
    analysis.minigameStatus = L"searching minigame";
    analysis.minigameLog = L"not in minigame: red line not found";
    return analysis;
  }
  const double scale = f.analysisGeometryScale;
  if (knownXRuns && knownXRuns->size() == 8) {
    analysis.bars = LocateWhiteBarsAtKnownX(f, analysis.red, *knownXRuns, scale);
  }
  if (analysis.bars.size() < 8) {
    analysis.bars = LocateWhiteBars(f, analysis.red, scale);
  }
  analysis.ok = analysis.bars.size() >= 8;
  analysis.inMinigame = analysis.ok;

  analysis.red.x1 = ToScreenX(f, analysis.red.x1);
  analysis.red.x2 = ToScreenX(f, analysis.red.x2);
  analysis.red.y1 = ToScreenY(f, analysis.red.y1);
  analysis.red.y2 = ToScreenY(f, analysis.red.y2);
  analysis.red.centerY = ToScreenY(f, analysis.red.centerY);
  for (auto& bar : analysis.bars) {
    bar.x1 = ToScreenX(f, bar.x1);
    bar.x2 = ToScreenX(f, bar.x2);
    bar.topY1 = ToScreenY(f, bar.topY1);
    bar.topY2 = ToScreenY(f, bar.topY2);
    bar.bottomY1 = ToScreenY(f, bar.bottomY1);
    bar.bottomY2 = ToScreenY(f, bar.bottomY2);
    bar.gapCenterY = f.y + bar.gapCenterY * f.toScreenY;
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
  for (size_t i = begin; i < slot.history.size(); ++i) {
    const double t = std::chrono::duration<double>(slot.history[i].time - tLast).count();
    const double recency = std::clamp(1.0 + t / 0.10, 0.0, 1.0);
    const double w = 0.50 + recency * recency * 2.50;
    sumW += w;
    sumT += w * t;
    sumE += w * slot.history[i].center;
  }
  if (sumW <= 0.0) return 0.0;
  const double meanT = sumT / sumW;
  const double meanE = sumE / sumW;
  double num = 0.0;
  double den = 0.0;
  for (size_t i = begin; i < slot.history.size(); ++i) {
    const double t = std::chrono::duration<double>(slot.history[i].time - tLast).count();
    const double recency = std::clamp(1.0 + t / 0.10, 0.0, 1.0);
    const double w = 0.50 + recency * recency * 2.50;
    num += w * (t - meanT) * (slot.history[i].center - meanE);
    den += w * (t - meanT) * (t - meanT);
  }
  return den > 1e-6 ? num / den : 0.0;
}

bool AddTrackSample(TrackSlot& slot, double observedError,
                    int topBottomY, int bottomTopY,
                    std::chrono::steady_clock::time_point sampleTime) {
  slot.lastCenter = observedError;
  if (!slot.history.empty()) {
    const TrackSample& previous = slot.history.back();
    const bool sameMarker = topBottomY == previous.topBottomY &&
                            bottomTopY == previous.bottomTopY;
    const bool sameQuantizedCenter = std::abs(observedError - previous.center) < 0.25;
    if (sameMarker || sameQuantizedCenter) return false;
  }
  slot.history.push_back({observedError, sampleTime, topBottomY, bottomTopY});
  while (slot.history.size() > 32) slot.history.erase(slot.history.begin());
  slot.velocity = EstimateVelocity(slot);
  return true;
}

bool IsUsableTiming(double seconds) {
  return std::isfinite(seconds) && seconds > 0.0 && seconds < 1.0;
}

double EstimateEdgeTriggerTime(double edgeError, double velocity, double scale,
                               double lookaheadSeconds) {
  const double speed = std::abs(velocity);
  const double forecastZone = ScaledPx(kEdgeTriggerZonePx, scale) +
                              speed * std::max(0.0, lookaheadSeconds);
  if (std::abs(edgeError) > forecastZone ||
      speed < kMinUsableVelocityPxPerSec || edgeError * velocity >= 0.0) {
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
                   double velocity = 0.0,
                   double scale = 1.0) {
  PreviewState next;
  next.hasFrame = true;
  next.hasRed = analysis.red.ok;
  next.hasYellow = yellow && yellow->ok;
  next.running = gta5::app::runtime::Running();
  next.red = analysis.red;
  next.yellow = yellow ? *yellow : YellowMeasure{};
  next.status = status;
  next.edgeError = edgeError;
  next.triggerTimeSec = triggerTimeSec;
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
    lock.unlock();
    RequestMarksRepaint();
  }
}

void WorkerLoop(const std::function<bool()>& stopRequested) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  PostLog(L"Start: keep the game visible. Using vision to detect the red line and white bars.");
  RECT sessionClient{};
  if (!gta5::capture::GetGameClientRect(sessionClient)) {
    PostStatus(L"capture failed");
    PostLog(L"Error: cached game window geometry unavailable; stopping.");
    return;
  }
  std::vector<TrackSlot> tracks(8);
  for (auto& track : tracks) track.history.reserve(32);
  auto lastEnter = std::chrono::steady_clock::now() - std::chrono::seconds(2);
  int frameNo = 0;
  int lastLoggedActive = -2;
  int missFrames = 0;
  auto lastFrameTime = std::chrono::steady_clock::now();
  int expectedIndex = 0;
  int lastYellowIndex = -1;
  int trackedActive = -1;
  int yellowMissFrames = 0;
  int preferredYellowHalf = 0;
  bool hasTrackingRegion = false;
  RectI trackingRegion{};
  RedBar lockedRedScreen{};
  std::vector<std::pair<int, int>> knownBarXRunsScreen;
  std::vector<BarMeasure> lastBarsScreen;
  SearchCells searchCellsScreen;
  bool cellsLocked = false;
  double geometryScale = 1.0;
  std::uint64_t cachedWindowGeneration = 0;
  if (g_inGameCache.valid) {
    lockedRedScreen = g_inGameCache.red;
    lastBarsScreen = g_inGameCache.bars;
    searchCellsScreen = g_inGameCache.cells;
    cellsLocked = searchCellsScreen.ok;
    hasTrackingRegion = lockedRedScreen.ok && lastBarsScreen.size() >= 8 && cellsLocked;
    cachedWindowGeneration = g_inGameCache.windowGeneration;
    geometryScale = g_inGameCache.geometryScale;
  }
  auto lastUiUpdate = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  const auto activeSearchStart = std::chrono::steady_clock::now();
  bool activeFoundOnce = false;
  bool finishPendingAfter8 = false;
  auto finishConfirmStart = std::chrono::steady_clock::now();
  PostPressCalibration postPress;
  std::vector<double> calibratedLatencySamples;
  calibratedLatencySamples.reserve(2);
  calibratedLatencySamples.push_back(kDefaultEndToEndLatencySeconds);
  double calibratedLatencySeconds = kDefaultEndToEndLatencySeconds;
  g_hudEndToEndMs.store(
      static_cast<int>(std::lround(calibratedLatencySeconds * 1000.0)),
      std::memory_order_relaxed);
  using AnalysisClock = std::chrono::steady_clock;
  const auto analysisInterval = std::chrono::duration_cast<AnalysisClock::duration>(
      std::chrono::duration<double>(kAnalysisIntervalSeconds));
  auto nextAnalysisTime = AnalysisClock::now();
  std::array<double, 30> analysisIntervalSamples{};
  size_t analysisIntervalSampleCount = 0;
  size_t analysisIntervalSampleIndex = 0;
  double analysisIntervalSampleSum = 0.0;
  double actualAnalysisIntervalSeconds = kAnalysisIntervalSeconds;
  CaptureFrame frame;
  while (!stopRequested()) {
    auto frameTime = AnalysisClock::now();
    if (frameTime < nextAnalysisTime) {
      std::this_thread::sleep_until(nextAnalysisTime);
      frameTime = AnalysisClock::now();
    }
    nextAnalysisTime += analysisInterval;
    if (nextAnalysisTime <= frameTime) {
      nextAnalysisTime = frameTime + analysisInterval;
    }
    const double frameIntervalSeconds =
        std::chrono::duration<double>(frameTime - lastFrameTime).count();
    const int frameDtMs = static_cast<int>(std::round(frameIntervalSeconds * 1000.0));
    lastFrameTime = frameTime;
    if (frameIntervalSeconds >= 0.001 && frameIntervalSeconds < 1.0) {
      if (analysisIntervalSampleCount < analysisIntervalSamples.size()) {
        ++analysisIntervalSampleCount;
      } else {
        analysisIntervalSampleSum -= analysisIntervalSamples[analysisIntervalSampleIndex];
      }
      analysisIntervalSamples[analysisIntervalSampleIndex] = frameIntervalSeconds;
      analysisIntervalSampleSum += frameIntervalSeconds;
      analysisIntervalSampleIndex =
          (analysisIntervalSampleIndex + 1) % analysisIntervalSamples.size();
      actualAnalysisIntervalSeconds =
          analysisIntervalSampleSum / analysisIntervalSampleCount;
      const double actualHz = 1.0 / actualAnalysisIntervalSeconds;
      g_hudAnalysisHz10.store(static_cast<int>(std::lround(actualHz * 10.0)),
                              std::memory_order_relaxed);
    }
    const std::array<int, 3> yellowCandidates = postPress.active
        ? std::array<int, 3>{postPress.barIndex,
                             std::min(7, postPress.barIndex + 1),
                             std::max(0, postPress.barIndex - 1)}
        : YellowCandidateIndices(lastYellowIndex, expectedIndex);
    const int yellowSearchHalf = postPress.active ? 0 : preferredYellowHalf;
    const bool focusedCapture = hasTrackingRegion && lockedRedScreen.ok &&
                                searchCellsScreen.ok &&
                                searchCellsScreen.xRanges.size() == 8;
    bool allYellowCellsCaptured = !focusedCapture;
    if (focusedCapture) {
      trackingRegion = BuildYellowCaptureRegion(lockedRedScreen, searchCellsScreen,
                                                yellowCandidates, geometryScale,
                                                yellowSearchHalf);
    }
    if (!CaptureScreenRegion(frame, focusedCapture ? &trackingRegion : nullptr, nullptr)) {
      if (focusedCapture) {
        g_inGameCache = {};
        hasTrackingRegion = false;
        lockedRedScreen = {};
        knownBarXRunsScreen.clear();
        searchCellsScreen = {};
        cellsLocked = false;
        lastBarsScreen.clear();
        cachedWindowGeneration = 0;
        if (CaptureScreenRegion(frame, nullptr, nullptr)) {
          allYellowCellsCaptured = true;
          PostLog(L"cached minigame region failed; retrying full frame");
        } else {
          PostStatus(L"capture failed");
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
      } else {
        g_inGameCache = {};
        PostStatus(L"capture failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
    }
    if (cachedWindowGeneration != 0 && frame.windowGeneration != cachedWindowGeneration) {
      g_inGameCache = {};
      hasTrackingRegion = false;
      lockedRedScreen = {};
      knownBarXRunsScreen.clear();
      searchCellsScreen = {};
      cellsLocked = false;
      lastBarsScreen.clear();
      cachedWindowGeneration = 0;
      if (focusedCapture && !CaptureScreenRegion(frame, nullptr, nullptr)) {
      PostStatus(L"capture failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
      }
      allYellowCellsCaptured = true;
    }
    auto sampleTime = std::chrono::steady_clock::now();

    const bool canUseLockedGeometry = hasTrackingRegion && lockedRedScreen.ok && lastBarsScreen.size() >= 8 && searchCellsScreen.ok;
    FrameAnalysis a;
    if (canUseLockedGeometry) {
      a.ok = true;
      a.inMinigame = true;
      a.red = lockedRedScreen;
      a.bars = lastBarsScreen;
      a.minigameStatus = L"in minigame";
    } else {
      if (focusedCapture && !CaptureScreenRegion(frame, nullptr, nullptr)) {
        PostStatus(L"capture failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      allYellowCellsCaptured = true;
      std::vector<std::pair<int, int>> knownBarXRunsLocal;
      if (knownBarXRunsScreen.size() == 8) {
        for (auto [x1, x2] : knownBarXRunsScreen) {
          knownBarXRunsLocal.push_back({ToFrameX(frame, x1), ToFrameX(frame, x2)});
        }
      }
      a = AnalyzeFrame(frame, knownBarXRunsLocal.size() == 8 ? &knownBarXRunsLocal : nullptr);
    }
    if (!a.inMinigame) {
      if (++missFrames % 15 == 1) {
        PostLog(a.minigameLog.empty() ? L"not in minigame" : a.minigameLog);
      }
      if (finishPendingAfter8 && missFrames >= 3 &&
          postPress.inputJob.Succeeded()) {
        PostLog(L"Completed: bar 8 input succeeded and minigame exited; stopping.");
        PostStatus(L"completed; stopped");
        gta5::app::runtime::RequestStop();
        break;
      }
      const bool finalInputPending =
          finishPendingAfter8 && postPress.inputJob.Pending();
      if (activeFoundOnce && missFrames >= 10 && !finalInputPending) {
        PostLog(L"Slider minigame exited; stopping session.");
        PostStatus(L"stopped");
        break;
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
    cachedWindowGeneration = frame.windowGeneration;
    lockedRedScreen = a.red;
    lastBarsScreen = a.bars;
    geometryScale = frame.screenGeometryScale;
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
    std::array<bool, 8> scannedYellowCells{};
    YellowMeasure yellow = FindActiveYellowMeasureCandidates(frame, a.red, searchCellsScreen, yellowCandidates,
                                                             geometryScale, yellowSearchHalf,
                                                             scannedYellowCells);
    if (!yellow.ok && focusedCapture) {
      const RectI fallbackRegion =
          BuildFullYellowCaptureRegion(a.red, searchCellsScreen, geometryScale);
      if (CaptureScreenRegion(frame, &fallbackRegion, nullptr)) {
        allYellowCellsCaptured = true;
        sampleTime = std::chrono::steady_clock::now();
        yellow = FindActiveYellowMeasureCandidates(
            frame, a.red, searchCellsScreen, yellowCandidates, geometryScale,
            yellowSearchHalf, scannedYellowCells);
      }
    }
    if (postPress.active && postPress.barIndex < 7) {
      const int nextIndex = postPress.barIndex + 1;
      const std::array<int, 3> nextOnly{nextIndex, nextIndex, nextIndex};
      std::array<bool, 8> nextScanned{};
      YellowMeasure nextYellow = FindActiveYellowMeasureCandidates(
          frame, a.red, searchCellsScreen, nextOnly, geometryScale, 0, nextScanned);
      if (nextYellow.ok) yellow = nextYellow;
    }
    if (!yellow.ok && allYellowCellsCaptured) {
      yellow = FindActiveYellowMeasure(frame, a.red, searchCellsScreen, geometryScale,
                                       preferredYellowHalf);
    }
    if (!yellow.ok && allYellowCellsCaptured) {
      if (!CaptureScreenRegion(frame, nullptr, nullptr)) {
        g_inGameCache = {};
        PostStatus(L"capture failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      sampleTime = std::chrono::steady_clock::now();
      FrameAnalysis presence = AnalyzeFrame(frame);
      if (!presence.inMinigame) {
        g_inGameCache = {};
        if (finishPendingAfter8 && postPress.inputJob.Succeeded()) {
          PostLog(L"Completed: bar 8 input succeeded and minigame exited; stopping.");
          PostStatus(L"completed; stopped");
        } else {
          PostLog(L"Slider minigame exited; stopping session.");
          PostStatus(L"stopped");
        }
        break;
      }

      a = std::move(presence);
      cachedWindowGeneration = frame.windowGeneration;
      lockedRedScreen = a.red;
      lastBarsScreen = a.bars;
      geometryScale = frame.screenGeometryScale;
      knownBarXRunsScreen.clear();
      for (int i = 0; i < 8; ++i) {
        knownBarXRunsScreen.push_back({a.bars[i].x1, a.bars[i].x2});
      }
      searchCellsScreen = BuildSearchCellsFromWhiteBars(a.bars, geometryScale);
      cellsLocked = searchCellsScreen.ok;
      hasTrackingRegion = a.bars.size() >= 8 && cellsLocked;
      g_inGameCache.valid = hasTrackingRegion;
      g_inGameCache.windowGeneration = frame.windowGeneration;
      g_inGameCache.red = a.red;
      g_inGameCache.bars = a.bars;
      g_inGameCache.cells = searchCellsScreen;
      g_inGameCache.geometryScale = geometryScale;
      yellow = FindActiveYellowMeasure(frame, a.red, searchCellsScreen,
                                       geometryScale, preferredYellowHalf);
    }
    int active = yellow.ok ? yellow.index : -1;
    if (active >= 0 && active != trackedActive) {
      ResetTrack(tracks[active]);
      trackedActive = active;
    }
    if (yellow.ok) {
      yellowMissFrames = 0;
      lastYellowIndex = active;
      if (yellow.gapCenterY < a.red.centerY) preferredYellowHalf = -1;
      else if (yellow.gapCenterY > a.red.centerY) preferredYellowHalf = 1;
      tracks[active].valid = true;
      const double observedError = yellow.gapCenterY - a.red.centerY;
      AddTrackSample(tracks[active], observedError,
                     yellow.topBottomY, yellow.bottomTopY, sampleTime);
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

    if (postPress.active) {
      const auto inputStartedAt = postPress.inputJob.StartedAt();
      if (postPress.barIndex < 7 && active == postPress.barIndex + 1 &&
          inputStartedAt != AnalysisClock::time_point{} &&
          sampleTime >= inputStartedAt) {
        const double measuredLatencySeconds =
            std::chrono::duration<double>(sampleTime - inputStartedAt).count();
        const int completedBar = postPress.barIndex;
        if (measuredLatencySeconds > 0.0 &&
            measuredLatencySeconds <= kMaximumCalibratedLatencySeconds) {
          if (calibratedLatencySamples.size() == 2) {
            calibratedLatencySamples.erase(calibratedLatencySamples.begin());
          }
          calibratedLatencySamples.push_back(measuredLatencySeconds);
          double latencySum = 0.0;
          for (double sample : calibratedLatencySamples) latencySum += sample;
          calibratedLatencySeconds = latencySum / calibratedLatencySamples.size();
          g_hudEndToEndMs.store(
              static_cast<int>(std::lround(calibratedLatencySeconds * 1000.0)),
              std::memory_order_relaxed);

          std::wstringstream calibrationLog;
          calibrationLog << L"E2E calibrated: bar=" << (completedBar + 1)
                         << L" next=" << (active + 1)
                         << L" sample="
                         << static_cast<int>(std::lround(measuredLatencySeconds * 1000.0))
                         << L"ms average="
                         << static_cast<int>(std::lround(calibratedLatencySeconds * 1000.0))
                         << L"ms";
          PostLog(calibrationLog.str());
        }

        postPress = {};
        ResetTrack(tracks[completedBar]);
        trackedActive = -1;
        expectedIndex = completedBar + 1;
        lastYellowIndex = -1;
        preferredYellowHalf = 0;
        ++frameNo;
        continue;
      }

      const auto timeoutStart = inputStartedAt != AnalysisClock::time_point{}
                                    ? inputStartedAt
                                    : postPress.scheduledAt;
      if (AnalysisClock::now() - timeoutStart > std::chrono::milliseconds(650)) {
        const int timedOutBar = postPress.barIndex;
        std::wstringstream timeoutLog;
        timeoutLog << L"E2E calibration timeout: bar=" << (timedOutBar + 1)
                   << L"; resuming tracking";
        PostLog(timeoutLog.str());
        postPress = {};
        ResetTrack(tracks[timedOutBar]);
        trackedActive = -1;
        expectedIndex = timedOutBar;
        lastYellowIndex = -1;
        preferredYellowHalf = 0;
        ++frameNo;
        continue;
      }
    }

    if (active == -1) {
      if (!activeFoundOnce && std::chrono::steady_clock::now() - activeSearchStart > std::chrono::seconds(10)) {
        PostLog(L"Error: active bar not found within 10s; stopping.");
        PostStatus(L"active bar timeout; stopped");
        gta5::app::runtime::RequestStop();
        break;
      }
      g_cursorVisible.store(false, std::memory_order_relaxed);
      g_cursorInZone.store(false, std::memory_order_relaxed);
      if (finishPendingAfter8 && std::chrono::steady_clock::now() - finishConfirmStart > std::chrono::milliseconds(650)) {
        PostLog(L"Completed: bar 8 confirmed without rollback; stopping.");
        PostStatus(L"completed; stopped");
        gta5::app::runtime::RequestStop();
        break;
      }
      if (++yellowMissFrames >= 3) lastYellowIndex = -1;
      if (yellowMissFrames >= 3) trackedActive = -1;
      PostStatus(L"waiting yellow outline");
      UpdatePreview(frame, a, tracks, -1, L"waiting yellow outline", nullptr,
                    0.0, kInvalidPredictionSeconds, 0.0, geometryScale);
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
        gta5::app::runtime::RequestStop();
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
    const double analysisHorizonSec = actualAnalysisIntervalSeconds;
    const double appliedLeadSec = calibratedLatencySeconds;
    const double forecastEdgeError = edgeError + velocity * analysisHorizonSec;
    const int cursorTargetY = velocity >= 0.0 ? a.red.y1 : a.red.y2;
    g_cursorTargetY.store(cursorTargetY, std::memory_order_relaxed);
    g_cursorInZone.store(
        std::abs(forecastEdgeError) <= ScaledPx(kEdgeTriggerZonePx * 2.0, geometryScale) ||
            edgeError * forecastEdgeError <= 0.0,
        std::memory_order_relaxed);
    const double triggerTimeSec =
        EstimateEdgeTriggerTime(edgeError, velocity, geometryScale, analysisHorizonSec);
    const auto now = std::chrono::steady_clock::now();
    const bool cooledDown = now - lastEnter > std::chrono::milliseconds(170);
    const bool modelReady = tracks[active].history.size() >= 2;
    const bool triggerReady = modelReady && IsUsableTiming(triggerTimeSec);
    const double scheduledDelaySec = triggerTimeSec - appliedLeadSec;
    const int triggerMs = IsUsableTiming(triggerTimeSec) ? static_cast<int>(std::round(triggerTimeSec * 1000.0)) : -1;
    if (now - lastUiUpdate >= std::chrono::milliseconds(50)) {
      g_hudActiveBar.store(active + 1, std::memory_order_relaxed);
      g_hudEdgePx.store(static_cast<int>(std::round(edgeError)), std::memory_order_relaxed);
      g_hudTtcMs.store(triggerMs, std::memory_order_relaxed);
      g_hudVelocityPx.store(static_cast<int>(std::round(velocity)), std::memory_order_relaxed);
      g_hudScale100.store(static_cast<int>(std::round(geometryScale * 100.0)), std::memory_order_relaxed);
      UpdatePreview(frame, a, tracks, active, a.minigameStatus.empty() ? L"in minigame" : a.minigameStatus,
                    &yellow, edgeError, triggerTimeSec, velocity, geometryScale);
      lastUiUpdate = now;
    }
    const bool pressReady = triggerReady && cooledDown;
    if (pressReady) {
      const auto predictedAt = sampleTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                            std::chrono::duration<double>(scheduledDelaySec));
      postPress.active = true;
      postPress.barIndex = active;
      postPress.scheduledAt = predictedAt;
      postPress.inputJob = gta5::input::QueueImmediate({0x1C, false}, predictedAt);
      lastEnter = predictedAt;
      std::wstringstream ss;
      ss << L"Press Enter: bar=" << (active + 1) << L" schedErr=" << static_cast<int>(std::round(error)) << L"px";
      PostLog(ss.str());
      std::wstringstream timingLog;
      timingLog << std::fixed << std::setprecision(1);
      timingLog << L"Timing: bar=" << (active + 1)
                << L" edge=" << static_cast<int>(std::round(edgeError))
                << L"px copy=" << triggerMs
                << L" lead=" << static_cast<int>(std::lround(appliedLeadSec * 1000.0))
                << L"ms(e2e)"
                << L" vel=" << static_cast<int>(std::round(velocity))
                << L"px/s dt=" << frameDtMs
                << L"ms";
      PostLog(timingLog.str());
      if (active == 7) {
        finishPendingAfter8 = true;
        finishConfirmStart = predictedAt;
      }
      ++frameNo;
      continue;
    }

    ++frameNo;
  }

  g_cursorVisible.store(false, std::memory_order_relaxed);
  g_cursorInZone.store(false, std::memory_order_relaxed);
  if (g_marksWnd) ShowWindowAsync(g_marksWnd, SW_HIDE);
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

LRESULT CALLBACK MarksProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      SetTimer(hwnd, 1, 50, nullptr);
      return 0;
    case WM_TIMER: {
      if (wParam != 1) return 0;
      PreviewState state = SnapshotPreviewState();
      if (!gta5::app::ui::OverlayEnabled() ||
          !gta5::app::runtime::Running() || state.bars.empty()) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }

      const int infoW = ScaledPx(104, state.scale);
      const int infoH = ScaledPx(62, state.scale);
      const int gap = ScaledPx(12, state.scale);
      const int pad = ScaledPx(12, state.scale);
      const int infoRight = state.red.x1 - gap;
      const int markerScreenY = state.red.centerY;
      RECT desired{infoRight - infoW - pad, markerScreenY - infoH / 2 - pad,
                   infoRight + pad, markerScreenY + infoH / 2 + pad};
      RECT clamped = ClampOverlayScreenRect(desired);
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
      if (!gta5::app::ui::OverlayEnabled() ||
          !gta5::app::runtime::Running() || state.bars.empty()) {
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
      HPEN activePen = CreatePen(PS_SOLID, 2, kOverlayGreen);
      HBRUSH infoBrush = CreateSolidBrush(kOverlayBlack);
      HGDIOBJ oldPen = SelectObject(hdc, activePen);
      HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

      const int infoW = ScaledPx(104, state.scale);
      const int infoH = ScaledPx(62, state.scale);
      const int gap = ScaledPx(12, state.scale);
      const int infoRight = state.red.x1 - gap - wr.left;
      const int infoCenterY = state.red.centerY - wr.top;
      const int analysisHz10 = g_hudAnalysisHz10.load(std::memory_order_relaxed);
      const int ttcMs = g_hudTtcMs.load(std::memory_order_relaxed);
      const int endToEndMs = g_hudEndToEndMs.load(std::memory_order_relaxed);
      std::wstring hzText = L"hz " + std::to_wstring(analysisHz10 / 10) + L"." +
                            std::to_wstring(std::abs(analysisHz10 % 10));
      std::wstring ttcText = L"ttc " + std::to_wstring(ttcMs) + L"ms";
      std::wstring endToEndText = endToEndMs >= 0
                                      ? L"e2e " + std::to_wstring(endToEndMs) + L"ms"
                                      : L"e2e --";
      RECT info{infoRight - infoW, infoCenterY - infoH / 2, infoRight, infoCenterY + infoH / 2};
      SelectObject(hdc, infoBrush);
      SelectObject(hdc, activePen);
      RoundRect(hdc, info.left, info.top, info.right, info.bottom, 8, 8);
      const int textX = info.left + ScaledPx(8, state.scale);
      SetTextColor(hdc, analysisHz10 > 0 && analysisHz10 < 300
                            ? kOverlayWarningOrange : kOverlayTextGreen);
      TextOutW(hdc, textX, info.top + ScaledPx(5, state.scale), hzText.c_str(), static_cast<int>(hzText.size()));
      SetTextColor(hdc, kOverlayTextGreen);
      TextOutW(hdc, textX, info.top + ScaledPx(23, state.scale), ttcText.c_str(), static_cast<int>(ttcText.size()));
      TextOutW(hdc, textX, info.top + ScaledPx(41, state.scale), endToEndText.c_str(),
               static_cast<int>(endToEndText.size()));

      SelectObject(hdc, oldBrush);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldFont);
      DeleteObject(font);
      DeleteObject(activePen);
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
      if (!gta5::app::ui::OverlayEnabled() ||
          !g_cursorVisible.load(std::memory_order_relaxed) ||
          !gta5::app::runtime::Running()) {
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
      RECT clamped = ClampOverlayScreenRect(desired);
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
      HPEN glowPen = CreatePen(PS_SOLID, 8, kOverlayBlack);
      HPEN arrowPen = CreatePen(PS_SOLID, 5, kOverlayGreen);
      HPEN linkGlowPen = CreatePen(PS_SOLID, 5, kOverlayBlack);
      HPEN linkPen = CreatePen(PS_SOLID, 2, kOverlayGreen);
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

}  // namespace

void SetHostWindow(HWND hwnd) { g_mainWnd = hwnd; }
HWND CursorWindow() { return g_cursorWnd; }
HWND MarksWindow() { return g_marksWnd; }
void SetCursorWindow(HWND hwnd) { g_cursorWnd = hwnd; }
void SetMarksWindow(HWND hwnd) { g_marksWnd = hwnd; }
void ClearOverlayState() {
  g_cursorVisible.store(false, std::memory_order_relaxed);
  g_cursorInZone.store(false, std::memory_order_relaxed);
  g_hudActiveBar.store(0, std::memory_order_relaxed);
  g_hudTtcMs.store(-1, std::memory_order_relaxed);
  g_hudAnalysisHz10.store(0, std::memory_order_relaxed);
  g_hudEndToEndMs.store(-1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_previewMutex);
    const std::wstring status = g_preview.status;
    const std::wstring lastLog = g_preview.lastLog;
    g_preview = PreviewState{};
    g_preview.status = status;
    g_preview.lastLog = lastLog;
    g_preview.running = gta5::app::runtime::Running();
  }
  // This may run on the session worker while the UI thread is joining it.
  // Synchronous cross-thread ShowWindow calls can deadlock that shutdown path.
  if (g_cursorWnd) ShowWindowAsync(g_cursorWnd, SW_HIDE);
  if (g_marksWnd) ShowWindowAsync(g_marksWnd, SW_HIDE);
  RequestMarksRepaint();
}
void HideTransientOverlays() { ClearOverlayState(); }
bool DetectInGame() {
  CaptureFrame frame;
  if (!CaptureScreenRegion(frame, nullptr)) {
    g_inGameCache = {};
    return false;
  }
  FrameAnalysis analysis = AnalyzeFrame(frame);
  if (!analysis.inMinigame) {
    g_inGameCache = {};
    return false;
  }
  g_inGameCache.valid = true;
  g_inGameCache.windowGeneration = frame.windowGeneration;
  g_inGameCache.red = analysis.red;
  g_inGameCache.bars = analysis.bars;
  g_inGameCache.cells = BuildSearchCellsFromWhiteBars(analysis.bars, frame.screenGeometryScale);
  g_inGameCache.geometryScale = frame.screenGeometryScale;
  return true;
}
void ResetInGameCache() { g_inGameCache = {}; }
void RunSession(const std::function<bool()>& stopRequested) {
  WorkerLoop(stopRequested);
  ClearOverlayState();
  ResetInGameCache();
}
int CursorSize() { return kCursorSize; }
LRESULT CALLBACK CursorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return CursorProc(hwnd, msg, wParam, lParam); }
LRESULT CALLBACK MarksWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return MarksProc(hwnd, msg, wParam, lParam); }

}  // namespace gta5::games::slider
