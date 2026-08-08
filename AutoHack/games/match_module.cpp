#include "games.h"

#include "../capture/game_window.h"
#include "../input/key_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gta5::games::match {
namespace {

using Frame = gta5::capture::GameFrame;
using Clock = std::chrono::steady_clock;

struct RectI {
  int left = 0, top = 0, right = 0, bottom = 0;
  int width() const { return right - left; }
  int height() const { return bottom - top; }
  int centerX() const { return (left + right) / 2; }
  int centerY() const { return (top + bottom) / 2; }
};

// Coordinates are full-frame analysis pixels. Layout contains every solver ROI.
struct Geometry {
  double scale = 0;
  std::array<RectI, 3> targetDigits{};
  std::array<RectI, 3> leftDigits{};
  std::array<RectI, 3> multiplierIcons{};
  std::uint64_t windowGeneration = 0;
  int frameWidth = 0;
  int frameHeight = 0;
};

struct Rgb { int r = 0, g = 0, b = 0; };
struct VisualState {
  int leftCurrent = -1;
  int rightCurrent = -1;
  int completedMask = 0;
  int usedMask = 0;
  bool operator==(const VisualState& other) const {
    return leftCurrent == other.leftCurrent && rightCurrent == other.rightCurrent &&
           completedMask == other.completedMask && usedMask == other.usedMask;
  }
};

enum class InputPhase { None, EnteringRight, ConfirmingChoice };

std::optional<Geometry> g_detectedGeometry;

struct OverlayText {
  int screenX = 0;
  int screenCenterY = 0;
  int fontHeight = 0;
  RECT screenBounds{};
  std::wstring text;
};

struct OverlaySnapshot {
  bool visible = false;
  std::array<OverlayText, 3> multipliers{};
  OverlayText equation;
};

HWND g_overlayWindow = nullptr;
std::mutex g_overlayMutex;
OverlaySnapshot g_overlaySnapshot;

void ClearOverlayState() {
  {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    g_overlaySnapshot = {};
  }
  if (g_overlayWindow) InvalidateRect(g_overlayWindow, nullptr, FALSE);
}

int AnalysisToScreenX(const Frame& frame, int x) {
  return frame.screenX + static_cast<int>(std::lround(x * frame.toScreenX));
}

int AnalysisToScreenY(const Frame& frame, int y) {
  return frame.screenY + static_cast<int>(std::lround(y * frame.toScreenY));
}

void PublishOverlay(const Frame& frame, const Geometry& geometry,
                    const std::array<int, 3>& values,
                    const std::array<int, 3>& multipliers,
                    const std::array<int, 3>& solution, int target) {
  OverlaySnapshot next;
  next.visible = true;
  const int targetScreenHeight = std::max(
      1, AnalysisToScreenY(frame, geometry.targetDigits[0].bottom) -
             AnalysisToScreenY(frame, geometry.targetDigits[0].top));
  const int labelFont = std::clamp(
      static_cast<int>(std::lround(targetScreenHeight * .27)), 15, 44);
  const int equationFont = std::clamp(
      static_cast<int>(std::lround(targetScreenHeight * .25)), 15, 40);
  const int labelGap = std::max(8, static_cast<int>(std::lround(frame.screenH * .012)));
  const int equationGap = std::max(10, static_cast<int>(std::lround(frame.screenH * .022)));
  const RECT screenBounds{frame.screenX, frame.screenY,
                          frame.screenX + frame.screenW,
                          frame.screenY + frame.screenH};

  for (int row = 0; row < 3; ++row) {
    const RectI& icon = geometry.multiplierIcons[row];
    const int radius = static_cast<int>(std::lround(icon.width() / (2 * .78)));
    next.multipliers[row] = {
        AnalysisToScreenX(frame, icon.centerX() + radius) + labelGap,
        AnalysisToScreenY(frame, icon.centerY()), labelFont, screenBounds,
        L"x" + std::to_wstring(multipliers[row])};
  }

  std::wstring equation;
  for (int row = 0; row < 3; ++row) {
    if (row) equation += L" + ";
    equation += std::to_wstring(values[row]);
    equation += L"x";
    equation += std::to_wstring(multipliers[solution[row]]);
  }
  equation += L" = ";
  equation += std::to_wstring(target);
  const RectI& lastTarget = geometry.targetDigits[2];
  next.equation = {
      AnalysisToScreenX(frame, lastTarget.right) + equationGap,
      AnalysisToScreenY(frame, (lastTarget.top + lastTarget.bottom) / 2),
      equationFont, screenBounds, std::move(equation)};

  {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    g_overlaySnapshot = std::move(next);
  }
  if (g_overlayWindow) InvalidateRect(g_overlayWindow, nullptr, FALSE);
}

void SyncOverlayWindow(bool enabled) {
  if (!g_overlayWindow) return;
  bool visible = false;
  {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    visible = g_overlaySnapshot.visible;
  }
  if (!enabled || !visible) {
    ShowWindow(g_overlayWindow, SW_HIDE);
    return;
  }
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  SetWindowPos(g_overlayWindow, HWND_TOPMOST, x, y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void DrawOverlay(HDC dc, HWND hwnd) {
  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  OverlaySnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    snapshot = g_overlaySnapshot;
  }
  if (!snapshot.visible) return;

  const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  SetBkMode(dc, TRANSPARENT);

  auto drawTag = [&](const OverlayText& item, bool equation) {
    const int fontHeight = std::max(12, item.fontHeight);
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, equation ? FW_SEMIBOLD : FW_BOLD,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT measured{0, 0, 0, 0};
    DrawTextW(dc, item.text.c_str(), -1, &measured,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int padX = std::max(7, fontHeight / 2);
    const int padY = std::max(4, fontHeight / 4);
    RECT box{item.screenX - virtualX,
             item.screenCenterY - virtualY - (measured.bottom + padY * 2) / 2,
             0, 0};
    box.right = box.left + measured.right + padX * 2;
    box.bottom = box.top + measured.bottom + padY * 2;

    // Keep tags inside the captured GTA client. Normally they remain to the
    // right of their anchors; only constrained/windowed layouts shift them.
    const int safeMargin = std::max(4, fontHeight / 4);
    const int minLeft = item.screenBounds.left - virtualX + safeMargin;
    const int maxRight = item.screenBounds.right - virtualX - safeMargin;
    const int minTop = item.screenBounds.top - virtualY + safeMargin;
    const int maxBottom = item.screenBounds.bottom - virtualY - safeMargin;
    if (box.right > maxRight) OffsetRect(&box, maxRight - box.right, 0);
    if (box.left < minLeft) OffsetRect(&box, minLeft - box.left, 0);
    if (box.bottom > maxBottom) OffsetRect(&box, 0, maxBottom - box.bottom);
    if (box.top < minTop) OffsetRect(&box, 0, minTop - box.top);

    HBRUSH background = CreateSolidBrush(equation ? RGB(12, 17, 23) : RGB(8, 22, 22));
    HPEN glow = CreatePen(PS_SOLID, std::max(3, fontHeight / 5),
                          equation ? RGB(63, 18, 30) : RGB(0, 55, 52));
    HPEN border = CreatePen(PS_SOLID, std::max(1, fontHeight / 14),
                            equation ? RGB(255, 74, 102) : RGB(58, 238, 207));
    HGDIOBJ oldBrush = SelectObject(dc, background);
    HGDIOBJ oldPen = SelectObject(dc, glow);
    const int radius = std::clamp(fontHeight / 3, 4, 8);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);
    SelectObject(dc, border);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);

    RECT textRect{box.left + padX, box.top + padY,
                  box.right - padX, box.bottom - padY};
    RECT shadow = textRect;
    OffsetRect(&shadow, std::max(1, fontHeight / 12), std::max(1, fontHeight / 12));
    SetTextColor(dc, equation ? RGB(90, 18, 31) : RGB(0, 78, 72));
    DrawTextW(dc, item.text.c_str(), -1, &shadow,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, equation ? RGB(255, 238, 241) : RGB(116, 255, 226));
    DrawTextW(dc, item.text.c_str(), -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldFont);
    DeleteObject(background);
    DeleteObject(glow);
    DeleteObject(border);
    DeleteObject(font);
  };

  for (const OverlayText& item : snapshot.multipliers) drawTag(item, false);
  drawTag(snapshot.equation, true);
}

std::uint32_t Pixel(const Frame& frame, int x, int y) {
  if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) return 0;
  return frame.bgra[static_cast<std::size_t>(y) * frame.width + x];
}

Rgb Color(std::uint32_t pixel) {
  return {static_cast<int>((pixel >> 16) & 255), static_cast<int>((pixel >> 8) & 255),
          static_cast<int>(pixel & 255)};
}

double NeutralBrightness(const Frame& frame, int x, int y) {
  const Rgb c = Color(Pixel(frame, x, y));
  return std::min({c.r, c.g, c.b});
}

double MeanNeutralHorizontal(const Frame& frame, int y, int left, int right) {
  left = std::clamp(left, 0, frame.width);
  right = std::clamp(right, 0, frame.width);
  if (y < 0 || y >= frame.height || right <= left) return 0;
  double sum = 0;
  for (int x = left; x < right; ++x) sum += NeutralBrightness(frame, x, y);
  return sum / (right - left);
}

double MeanNeutralVertical(const Frame& frame, int x, int top, int bottom) {
  top = std::clamp(top, 0, frame.height);
  bottom = std::clamp(bottom, 0, frame.height);
  if (x < 0 || x >= frame.width || bottom <= top) return 0;
  double sum = 0;
  for (int y = top; y < bottom; ++y) sum += NeutralBrightness(frame, x, y);
  return sum / (bottom - top);
}

bool AnchorsPresent(const Frame& frame, const Geometry& geometry) {
  if (geometry.targetDigits[0].height() <= 0) return false;
  const int delta = std::max(2, static_cast<int>(std::lround(frame.height * .004)));
  const int top = geometry.targetDigits[0].top;
  const int bottom = geometry.targetDigits[0].bottom;
  const int left = geometry.targetDigits[0].left - 1;
  const int right = geometry.targetDigits[2].right + 1;
  const int inset = std::max(2, delta);
  const double topContrast = MeanNeutralHorizontal(frame, top, left + inset, right - inset) -
      (MeanNeutralHorizontal(frame, top - delta, left + inset, right - inset) +
       MeanNeutralHorizontal(frame, top + delta, left + inset, right - inset)) * .5;
  const double bottomContrast = MeanNeutralHorizontal(frame, bottom, left + inset, right - inset) -
      (MeanNeutralHorizontal(frame, bottom - delta, left + inset, right - inset) +
       MeanNeutralHorizontal(frame, bottom + delta, left + inset, right - inset)) * .5;
  if (topContrast + bottomContrast < 30) return false;
  double verticalContrast = 0;
  for (int i = 0; i < 4; ++i) {
    const int x = i == 0 ? left : geometry.targetDigits[i - 1].right + 1;
    verticalContrast += MeanNeutralVertical(frame, x, top + inset, bottom - inset) -
        (MeanNeutralVertical(frame, x - delta, top + inset, bottom - inset) +
         MeanNeutralVertical(frame, x + delta, top + inset, bottom - inset)) * .5;
  }
  return verticalContrast >= 45;
}

std::optional<Geometry> LocateGeometry(const Frame& frame) {
  if (frame.width < 640 || frame.height < 360 ||
      frame.bgra.size() != static_cast<std::size_t>(frame.width) * frame.height) return std::nullopt;
  const int centerX = frame.width / 2;
  const int delta = std::max(2, static_cast<int>(std::lround(frame.height * .004)));
  const int hx1 = std::max(0, static_cast<int>(std::lround(centerX - frame.height * .16)));
  const int hx2 = std::min(frame.width, static_cast<int>(std::lround(centerX + frame.height * .16)));
  const int hy1 = std::max(delta, static_cast<int>(std::lround(frame.height * .05)));
  const int hy2 = std::min(frame.height - delta - 1,
                           static_cast<int>(std::lround(frame.height * .30)));
  std::vector<double> horizontal(frame.height), contrast(frame.height);
  for (int y = hy1 - delta; y <= hy2 + delta; ++y)
    horizontal[y] = MeanNeutralHorizontal(frame, y, hx1, hx2);
  for (int y = hy1; y <= hy2; ++y)
    contrast[y] = horizontal[y] - (horizontal[y - delta] + horizontal[y + delta]) * .5;

  int top = -1, bottom = -1;
  double bestHorizontal = 0;
  for (int y1 = hy1 + 1; y1 < hy2; ++y1) {
    if (contrast[y1] < contrast[y1 - 1] || contrast[y1] <= contrast[y1 + 1]) continue;
    for (int y2 = y1 + 1; y2 < hy2; ++y2) {
      const double separation = (y2 - y1) / static_cast<double>(frame.height);
      if (separation < .085 || separation > .13 ||
          contrast[y2] < contrast[y2 - 1] || contrast[y2] <= contrast[y2 + 1]) continue;
      const double score = contrast[y1] + contrast[y2];
      if (score > bestHorizontal) { bestHorizontal = score; top = y1; bottom = y2; }
    }
  }
  if (top < 0 || bestHorizontal < 35) return std::nullopt;

  const int vx1 = std::max(delta, static_cast<int>(std::lround(centerX - frame.height * .22)));
  const int vx2 = std::min(frame.width - delta - 1,
                           static_cast<int>(std::lround(centerX + frame.height * .22)));
  std::vector<double> vertical(frame.width), vContrast(frame.width);
  for (int x = vx1 - delta; x <= vx2 + delta; ++x)
    vertical[x] = MeanNeutralVertical(frame, x, top, bottom + 1);
  std::vector<int> peaks;
  for (int x = vx1; x <= vx2; ++x)
    vContrast[x] = vertical[x] - (vertical[x - delta] + vertical[x + delta]) * .5;
  for (int x = vx1 + 1; x < vx2; ++x) {
    if (vContrast[x] <= 15 || vContrast[x] < vContrast[x - 1] ||
        vContrast[x] <= vContrast[x + 1]) continue;
    if (!peaks.empty() && x - peaks.back() <= 2) {
      if (vContrast[x] > vContrast[peaks.back()]) peaks.back() = x;
    } else peaks.push_back(x);
  }

  std::array<int, 4> edges{};
  double bestVertical = 0;
  for (int first : peaks) for (int last : peaks) {
    if (last <= first) continue;
    const double pitch = (last - first) / 3.0;
    if (pitch / frame.height < .055 || pitch / frame.height > .105) continue;
    std::array<int, 4> candidate{};
    double error = 0;
    for (int i = 0; i < 4; ++i) {
      const double expected = first + pitch * i;
      candidate[i] = *std::min_element(peaks.begin(), peaks.end(), [=](int a, int b) {
        return std::abs(a - expected) < std::abs(b - expected);
      });
      error += std::abs(candidate[i] - expected) / pitch;
    }
    if (error > .15 || candidate[0] == candidate[1] || candidate[1] == candidate[2] ||
        candidate[2] == candidate[3]) continue;
    double score = -20 * error;
    for (int edge : candidate) score += vContrast[edge];
    score -= std::abs((first + last) * .5 - centerX) * .05;
    if (score > bestVertical) { bestVertical = score; edges = candidate; }
  }
  if (bestVertical < 60) return std::nullopt;

  Geometry geometry;
  geometry.scale = ((bottom - top) / 116.0 +
                    (edges[3] - edges[0]) / (3.0 * 89.0)) * .5;
  if (geometry.scale <= .25) return std::nullopt;
  for (int i = 0; i < 3; ++i)
    geometry.targetDigits[i] = {edges[i] + 1, top, edges[i + 1] - 1, bottom};

  const int frameH = bottom - top;
  const int frameW = static_cast<int>(std::lround((edges[3] - edges[0]) / 3.0));
  constexpr double kTargetCellAspect = 89.0 / 116.0;
  const double targetCellAspect = frameW / static_cast<double>(frameH);
  if (targetCellAspect < kTargetCellAspect * .90 ||
      targetCellAspect > kTargetCellAspect * 1.10) return std::nullopt;
  const int connectionInset = std::max(delta, static_cast<int>(std::lround(frameH * .06)));
  const int connectionSpan = std::max(connectionInset + 1, frameH / 3);
  auto verticalSegmentContrast = [&](int x, int y1, int y2) {
    return MeanNeutralVertical(frame, x, y1, y2) -
        (MeanNeutralVertical(frame, x - delta, y1, y2) +
         MeanNeutralVertical(frame, x + delta, y1, y2)) * .5;
  };
  for (int edge : edges) {
    const double upper = verticalSegmentContrast(
        edge, top + connectionInset, top + connectionSpan);
    const double lower = verticalSegmentContrast(
        edge, bottom - connectionSpan, bottom - connectionInset);
    if (upper < 15 || lower < 15) return std::nullopt;
  }
  const int edgeOffset = std::max(3, static_cast<int>(std::lround(frameH * .05)));
  auto maximum = [&](int x, int y) { const Rgb c = Color(Pixel(frame, x, y)); return std::max({c.r,c.g,c.b}); };
  auto verticalLine = [&](int x, int y, int height) {
    if (x < 1 || x + 1 >= frame.width || y < 0 || y + height > frame.height) return 0.0;
    double sum = 0;
    for (int py = y; py < y + height; ++py)
      sum += std::max({maximum(x - 1, py), maximum(x, py), maximum(x + 1, py)});
    return sum / height;
  };
  auto horizontalLine = [&](int x, int y, int width) {
    if (y < 1 || y + 1 >= frame.height || x < 0 || x + width > frame.width) return 0.0;
    double sum = 0;
    for (int px = x; px < x + width; ++px)
      sum += std::max({maximum(px, y - 1), maximum(px, y), maximum(px, y + 1)});
    return sum / width;
  };
  auto rectangleScore = [&](int x, int y) {
    if (x - edgeOffset < 1 || x + frameW + edgeOffset >= frame.width ||
        y - edgeOffset < 1 || y + frameH + edgeOffset >= frame.height) return -1.0;
    const double edge = verticalLine(x, y, frameH) + verticalLine(x + frameW, y, frameH) +
                        horizontalLine(x, y, frameW) + horizontalLine(x, y + frameH, frameW);
    const double nearby = (verticalLine(x - edgeOffset, y, frameH) +
        verticalLine(x + edgeOffset, y, frameH) + verticalLine(x + frameW - edgeOffset, y, frameH) +
        verticalLine(x + frameW + edgeOffset, y, frameH) + horizontalLine(x, y - edgeOffset, frameW) +
        horizontalLine(x, y + edgeOffset, frameW) + horizontalLine(x, y + frameH - edgeOffset, frameW) +
        horizontalLine(x, y + frameH + edgeOffset, frameW)) * .5;
    return edge - nearby;
  };

  for (int row = 0; row < 3; ++row) {
    const int expectedX = static_cast<int>(std::lround(edges[0] - frameH * 3.04));
    const int expectedY = static_cast<int>(std::lround(top + frameH * (1.27 + row * 2.01)));
    const int radiusX = static_cast<int>(std::lround(frameH * .45));
    const int radiusY = static_cast<int>(std::lround(frameH * .25));
    const int coarse = std::max(1, static_cast<int>(std::lround(geometry.scale * 2)));
    double bestScore = -1; int bestX = 0, bestY = 0;
    for (int y = expectedY - radiusY; y <= expectedY + radiusY; y += coarse)
      for (int x = expectedX - radiusX; x <= expectedX + radiusX; x += coarse) {
        const double score = rectangleScore(x, y);
        if (score > bestScore) { bestScore = score; bestX = x; bestY = y; }
      }
    for (int y = bestY - coarse; y <= bestY + coarse; ++y)
      for (int x = bestX - coarse; x <= bestX + coarse; ++x) {
        const double score = rectangleScore(x, y);
        if (score > bestScore) { bestScore = score; bestX = x; bestY = y; }
      }
    if (bestScore < 80) return std::nullopt;
    geometry.leftDigits[row] = {bestX, bestY, bestX + frameW, bestY + frameH};
  }

  constexpr double kPi = 3.14159265358979323846;
  auto ringScore = [&](int cx, int cy, int radius) {
    double edge = 0, inside = 0, outside = 0;
    const int radialOffset = std::max(3, static_cast<int>(std::lround(frameH * .05)));
    for (int i = 0; i < 32; ++i) {
      const double angle = 2 * kPi * i / 32, cs = std::cos(angle), sn = std::sin(angle);
      edge += maximum(static_cast<int>(std::lround(cx + radius * cs)), static_cast<int>(std::lround(cy + radius * sn)));
      inside += maximum(static_cast<int>(std::lround(cx + (radius - radialOffset) * cs)),
                        static_cast<int>(std::lround(cy + (radius - radialOffset) * sn)));
      outside += maximum(static_cast<int>(std::lround(cx + (radius + radialOffset) * cs)),
                         static_cast<int>(std::lround(cy + (radius + radialOffset) * sn)));
    }
    return (edge - (inside + outside) * .5) / 32;
  };
  for (int row = 0; row < 3; ++row) {
    const int expectedX = static_cast<int>(std::lround(edges[3] + frameH * 2.55));
    const int expectedY = geometry.leftDigits[row].centerY();
    const int searchX = static_cast<int>(std::lround(frameH * .70));
    const int searchY = static_cast<int>(std::lround(frameH * .18));
    const int minRadius = static_cast<int>(std::lround(frameH * .43));
    const int maxRadius = static_cast<int>(std::lround(frameH * .58));
    const int step = std::max(1, static_cast<int>(std::lround(geometry.scale * 2)));
    double bestScore = -1; int bestX = 0, bestY = 0, bestRadius = 0;
    for (int cy = expectedY - searchY; cy <= expectedY + searchY; cy += step)
      for (int cx = expectedX - searchX; cx <= expectedX + searchX; cx += step)
        for (int radius = minRadius; radius <= maxRadius; radius += step) {
          const double score = ringScore(cx, cy, radius);
          if (score > bestScore) { bestScore = score; bestX = cx; bestY = cy; bestRadius = radius; }
        }
    if (bestScore < 12) return std::nullopt;
    const int halfW = static_cast<int>(std::lround(bestRadius * .78));
    const int halfH = static_cast<int>(std::lround(bestRadius * .82));
    geometry.multiplierIcons[row] = {bestX - halfW, bestY - halfH, bestX + halfW, bestY + halfH};
  }
  geometry.windowGeneration = frame.windowGeneration;
  geometry.frameWidth = frame.width;
  geometry.frameHeight = frame.height;
  return geometry;
}

bool IsBright(Rgb c) { return std::max({c.r, c.g, c.b}) >= 145 && c.r + c.g + c.b >= 300; }

double BrightRatio(const Frame& frame, RectI roi) {
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  int bright = 0, total = 0;
  for (int y = roi.top; y < roi.bottom; ++y) for (int x = roi.left; x < roi.right; ++x) {
    bright += IsBright(Color(Pixel(frame, x, y))); ++total;
  }
  return total ? bright / static_cast<double>(total) : 0;
}

int ReadSevenSegment(const Frame& frame, const RectI& box) {
  constexpr std::array<std::array<double, 4>, 7> strips{{
      {{.36,.17,.64,.24}}, {{.66,.27,.77,.43}}, {{.66,.58,.77,.74}},
      {{.36,.77,.64,.84}}, {{.25,.58,.36,.74}}, {{.25,.27,.36,.43}},
      {{.36,.47,.64,.55}}}};
  int mask = 0;
  for (int i = 0; i < 7; ++i) {
    const auto& s = strips[i];
    const RectI roi{static_cast<int>(std::lround(box.left + s[0] * box.width())),
                    static_cast<int>(std::lround(box.top + s[1] * box.height())),
                    static_cast<int>(std::lround(box.left + s[2] * box.width())),
                    static_cast<int>(std::lround(box.top + s[3] * box.height()))};
    if (BrightRatio(frame, roi) > .12) mask |= 1 << i;
  }
  constexpr std::array<int, 10> masks{0b0111111,0b0000110,0b1011011,0b1001111,0b1100110,
                                      0b1101101,0b1111101,0b0000111,0b1111111,0b1101111};
  int best = -1, bestDistance = 8;
  for (int digit = 0; digit < 10; ++digit) {
    unsigned difference = static_cast<unsigned>(mask ^ masks[digit]); int distance = 0;
    while (difference) { distance += difference & 1U; difference >>= 1U; }
    if (distance < bestDistance) { bestDistance = distance; best = digit; }
  }
  return bestDistance <= 1 ? best : -1;
}

int ReadMultiplier(const Frame& frame, const Geometry& geometry, int row) {
  RectI roi = geometry.multiplierIcons[row];
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  int minX = roi.right, minY = roi.bottom, maxX = -1, maxY = -1, count = 0;
  for (int y = roi.top; y < roi.bottom; ++y) for (int x = roi.left; x < roi.right; ++x) {
    const Rgb c = Color(Pixel(frame, x, y));
    if (std::min({c.r,c.g,c.b}) < 155 || c.r + c.g + c.b < 570) continue;
    minX = std::min(minX, x); minY = std::min(minY, y);
    maxX = std::max(maxX, x); maxY = std::max(maxY, y); ++count;
  }
  if (count < std::max(12, static_cast<int>(180 * geometry.scale * geometry.scale))) return -1;
  const double width = (maxX - minX + 1) / geometry.scale;
  const double height = (maxY - minY + 1) / geometry.scale;
  if (height < 52) return 2;
  if (width < 57) return 1;
  return 10;
}

bool ReadPuzzle(const Frame& frame, const Geometry& geometry, std::array<int, 3>& values,
                std::array<int, 3>& multipliers, int& target) {
  for (int row = 0; row < 3; ++row) {
    values[row] = ReadSevenSegment(frame, geometry.leftDigits[row]);
    multipliers[row] = ReadMultiplier(frame, geometry, row);
    if (values[row] < 0 || multipliers[row] < 0) return false;
  }
  const int d0 = ReadSevenSegment(frame, geometry.targetDigits[0]);
  const int d1 = ReadSevenSegment(frame, geometry.targetDigits[1]);
  const int d2 = ReadSevenSegment(frame, geometry.targetDigits[2]);
  if (d0 < 0 || d1 < 0 || d2 < 0) return false;
  target = d0 * 100 + d1 * 10 + d2;
  return true;
}

double MosaicChromaScore(const Frame& frame, RectI roi, int block) {
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  block = std::max(1, block);
  std::vector<double> scores;
  for (int by = roi.top; by + block <= roi.bottom; by += block)
    for (int bx = roi.left; bx + block <= roi.right; bx += block) {
      double r = 0, g = 0, b = 0;
      for (int y = by; y < by + block; ++y) for (int x = bx; x < bx + block; ++x) {
        const Rgb c = Color(Pixel(frame, x, y)); r += c.r; g += c.g; b += c.b;
      }
      const double count = block * block;
      r /= count; g /= count; b /= count;
      const double high = std::max({r,g,b}), low = std::min({r,g,b});
      if (high > 25) scores.push_back((high - low) / (high + 20));
    }
  if (scores.empty()) return 0;
  std::sort(scores.begin(), scores.end(), std::greater<double>());
  const std::size_t take = std::max<std::size_t>(1, scores.size() / 5);
  return std::accumulate(scores.begin(), scores.begin() + take, 0.0) / take;
}

int UniqueColoredRow(const std::array<double, 3>& scores) {
  int best = 0;
  for (int row = 1; row < 3; ++row) if (scores[row] > scores[best]) best = row;
  double second = 0;
  for (int row = 0; row < 3; ++row) if (row != best) second = std::max(second, scores[row]);
  return scores[best] > .18 && scores[best] - second > .12 ? best : -1;
}

VisualState ReadVisualState(const Frame& frame, const Geometry& geometry) {
  std::array<double, 3> leftScores{};
  std::array<double, 3> rightScores{};
  VisualState state;
  const int block = std::max(1, static_cast<int>(std::lround(geometry.scale * 4)));
  const int wireBlock = std::max(1, static_cast<int>(std::lround(geometry.scale * 2)));
  for (int row = 0; row < 3; ++row) {
    const RectI& number = geometry.leftDigits[row];
    const int h = number.height();
    const int margin = std::max(2, static_cast<int>(std::lround(h * .06)));
    leftScores[row] = MosaicChromaScore(frame,
        {number.left-margin,number.top-margin,number.right+margin,number.bottom+margin}, block);
    const int halfWire = std::max(2, static_cast<int>(std::lround(h * .06)));
    const double leftWire = MosaicChromaScore(frame,
        {number.right + static_cast<int>(std::lround(h * .37)), number.centerY()-halfWire,
         number.right + static_cast<int>(std::lround(h * .67)), number.centerY()+halfWire}, wireBlock);
    const RectI& icon = geometry.multiplierIcons[row];
    const int circleRadius = static_cast<int>(std::lround(icon.width() / (2 * .78)));
    const int circleLeft = icon.centerX() - circleRadius;
    const int circleMargin = std::max(2, static_cast<int>(std::lround(circleRadius * .08)));
    rightScores[row] = MosaicChromaScore(
        frame,
        {icon.centerX() - circleRadius - circleMargin,
         icon.centerY() - circleRadius - circleMargin,
         icon.centerX() + circleRadius + circleMargin,
         icon.centerY() + circleRadius + circleMargin}, block);
    const double rightWire = MosaicChromaScore(frame,
        {circleLeft - static_cast<int>(std::lround(h * .69)), icon.centerY()-halfWire,
         circleLeft - static_cast<int>(std::lround(h * .34)), icon.centerY()+halfWire}, wireBlock);
    if (leftWire > .18) state.completedMask |= 1 << row;
    if (rightWire > .18) state.usedMask |= 1 << row;
  }
  state.leftCurrent = UniqueColoredRow(leftScores);
  state.rightCurrent = UniqueColoredRow(rightScores);
  return state;
}

int BitCount3(int value) { return (value & 1) + ((value >> 1) & 1) + ((value >> 2) & 1); }

int NextAvailableRow(int row, int usedMask) {
  for (int step = 1; step <= 3; ++step) {
    const int candidate = (row + step) % 3;
    if (!(usedMask & (1 << candidate))) return candidate;
  }
  return -1;
}

std::optional<std::array<int, 3>> MakeSolution(const std::array<int, 3>& values,
                                               const std::array<int, 3>& multipliers,
                                               int target) {
  std::array<int, 3> rows{0,1,2};
  do {
    int sum = 0;
    for (int i = 0; i < 3; ++i) sum += values[i] * multipliers[rows[i]];
    if (sum == target) return rows;
  } while (std::next_permutation(rows.begin(), rows.end()));
  return std::nullopt;
}

bool SameLayout(const Geometry& a, const Geometry& b) {
  const RectI& ar = a.targetDigits[0]; const RectI& br = b.targetDigits[0];
  return std::abs(ar.left-br.left) <= 2 && std::abs(ar.top-br.top) <= 2 &&
         std::abs(a.targetDigits[2].right-b.targetDigits[2].right) <= 2 &&
         std::abs(ar.bottom-br.bottom) <= 2;
}

bool MatchesFrame(const Geometry& geometry, const Frame& frame) {
  return geometry.windowGeneration == frame.windowGeneration &&
         geometry.frameWidth == frame.width && geometry.frameHeight == frame.height;
}

HWND ForegroundGameWindow(const Frame& frame) {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return nullptr;
  RECT client{}; POINT origin{};
  if (!GetClientRect(foreground, &client) || !ClientToScreen(foreground, &origin)) return nullptr;
  return origin.x == frame.screenX && origin.y == frame.screenY &&
         client.right-client.left == frame.screenW && client.bottom-client.top == frame.screenH
      ? foreground : nullptr;
}

void WaitFrame(const std::function<bool()>& stopRequested, Clock::time_point started) {
  const auto deadline = started + std::chrono::milliseconds(50);
  while (!stopRequested() && Clock::now() < deadline) {
    if (deadline - Clock::now() > std::chrono::milliseconds(2)) Sleep(1);
    else std::this_thread::yield();
  }
}

}  // namespace

bool DetectInGame() {
  Frame frame;
  if (!gta5::capture::CaptureGameFrame(frame)) {
    g_detectedGeometry.reset();
    ClearOverlayState();
    return false;
  }
  g_detectedGeometry = LocateGeometry(frame);
  if (!g_detectedGeometry) ClearOverlayState();
  return g_detectedGeometry.has_value();
}

void ResetInGameCache() {
  g_detectedGeometry.reset();
  ClearOverlayState();
}

bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status) {
  std::optional<Geometry> geometry = g_detectedGeometry;
  std::optional<std::array<int, 3>> solution;
  gta5::input::Job inputJob;
  InputPhase inputPhase = InputPhase::None;
  VisualState state{}, previous{};
  int absentFrames = 0, stableFrames = 0;
  bool havePrevious = false, awaitingRightSelection = false;
  bool awaitingTransition = false, stateReady = false;
  int oldCompleted = 0, oldUsed = 0, connectingLeft = -1, connectingRight = -1;
  int enterAttempts = 0, rightStableFrames = 0, previousRight = -1;
  auto rightSelectionDeadline = Clock::time_point{};
  auto transitionDeadline = Clock::time_point{};
  std::wstring lastStatus;
  auto setStatus = [&](const wchar_t* value) {
    if (lastStatus != value) { lastStatus = value; status(value); }
  };
  auto resetPlan = [&] {
    solution.reset(); inputPhase = InputPhase::None;
    awaitingRightSelection = false; awaitingTransition = false; stateReady = false;
    enterAttempts = 0; rightStableFrames = 0; previousRight = -1;
    stableFrames = 0; havePrevious = false;
    ClearOverlayState();
  };
  auto cleanup = [&] {
    gta5::input::CancelAll(); inputJob = {}; geometry.reset();
    ClearOverlay();
    ResetInGameCache();
  };

  ClearOverlayState();
  setStatus(L"match: locating");
  while (!stopRequested()) {
    const auto started = Clock::now();
    SyncOverlayWindow(overlayEnabled());
    Frame frame;
    if (!gta5::capture::CaptureGameFrame(frame)) {
      geometry.reset(); g_detectedGeometry.reset(); resetPlan();
      if (++absentFrames >= 3) break;
      WaitFrame(stopRequested, started); continue;
    }

    bool needsRelocation = !geometry || !MatchesFrame(*geometry, frame) || !AnchorsPresent(frame, *geometry);
    if (needsRelocation) {
      const auto relocated = LocateGeometry(frame);  // Same-frame full-search fallback.
      if (!relocated) {
        geometry.reset(); g_detectedGeometry.reset();
        if (++absentFrames >= 3) {
          if (inputJob && inputPhase == InputPhase::ConfirmingChoice &&
              BitCount3(oldCompleted) == 2) {
            if (inputJob.Pending()) { WaitFrame(stopRequested, started); continue; }
            if (inputJob.Succeeded()) {
              setStatus(L"match: completed"); cleanup(); return true;
            }
          }
          if (awaitingTransition && BitCount3(oldCompleted) == 2) {
            setStatus(L"match: completed"); cleanup(); return true;
          }
          setStatus(L"match: minigame exited"); break;
        }
        WaitFrame(stopRequested, started); continue;
      }
      if (!geometry || !SameLayout(*geometry, *relocated) || !MatchesFrame(*geometry, frame)) {
        gta5::input::CancelAll(); inputJob = {}; resetPlan();
      }
      geometry = relocated; g_detectedGeometry = relocated;
    }
    absentFrames = 0;

    if (inputJob) {
      if (inputJob.Pending()) { WaitFrame(stopRequested, started); continue; }
      if (!inputJob.Succeeded()) {
        inputJob = {}; resetPlan(); setStatus(L"match: analyzing");
        WaitFrame(stopRequested, started); continue;
      }
      inputJob = {};
      if (inputPhase == InputPhase::EnteringRight) {
        inputPhase = InputPhase::None;
        awaitingRightSelection = true;
        rightSelectionDeadline = Clock::now() + std::chrono::milliseconds(200);
        rightStableFrames = 0;
        previousRight = -1;
      } else if (inputPhase == InputPhase::ConfirmingChoice) {
        inputPhase = InputPhase::None;
        awaitingTransition = true;
        transitionDeadline = Clock::now() + std::chrono::seconds(6);
        stableFrames = 0; havePrevious = false;
        setStatus(L"match: verifying connection");
      } else {
        resetPlan();
      }
    }

    state = ReadVisualState(frame, *geometry);
    if (awaitingRightSelection) {
      if (state.rightCurrent >= 0 && state.rightCurrent == previousRight) ++rightStableFrames;
      else rightStableFrames = state.rightCurrent >= 0 ? 1 : 0;
      previousRight = state.rightCurrent;

      if (rightStableFrames >= 2) {
        int selectedRight = (oldUsed & (1 << connectingLeft))
            ? NextAvailableRow(connectingLeft, oldUsed) : connectingLeft;
        std::vector<gta5::input::Key> keys;
        for (int moves = 0; selectedRight != connectingRight && moves < 3; ++moves) {
          keys.push_back(gta5::input::Key::FromVirtualKey(VK_DOWN));
          selectedRight = NextAvailableRow(selectedRight, oldUsed);
        }
        if (selectedRight != connectingRight) {
          resetPlan(); setStatus(L"match: analyzing");
          WaitFrame(stopRequested, started); continue;
        }
        keys.push_back(gta5::input::Key::FromVirtualKey(VK_RETURN));
        const HWND foreground = ForegroundGameWindow(frame);
        if (!foreground) { WaitFrame(stopRequested, started); continue; }
        awaitingRightSelection = false;
        inputPhase = InputPhase::ConfirmingChoice;
        inputJob = gta5::input::QueueSequence(keys, foreground);
        WaitFrame(stopRequested, started); continue;
      }

      if (Clock::now() >= rightSelectionDeadline) {
        if (state.leftCurrent == connectingLeft && enterAttempts < 3) {
          const HWND foreground = ForegroundGameWindow(frame);
          if (!foreground) { WaitFrame(stopRequested, started); continue; }
          awaitingRightSelection = false;
          ++enterAttempts;
          inputPhase = InputPhase::EnteringRight;
          inputJob = gta5::input::QueueSequence(
              std::vector<gta5::input::Key>{
                  gta5::input::Key::FromVirtualKey(VK_RETURN)}, foreground);
        } else {
          // Never send navigation until a visible right-side cursor proves
          // that GTA accepted the first Enter.
          cleanup();
          return false;
        }
      }
      WaitFrame(stopRequested, started); continue;
    }

    if (awaitingTransition) {
      const bool finalPair = BitCount3(oldCompleted) == 2;
      const bool connected = (state.completedMask & (1 << connectingLeft)) &&
                             (state.usedMask & (1 << connectingRight)) &&
                             state.completedMask != oldCompleted && state.usedMask != oldUsed;
      const bool advanced = finalPair || (state.leftCurrent >= 0 && state.leftCurrent != connectingLeft);
      if (connected && advanced && havePrevious && state == previous) ++stableFrames;
      else stableFrames = connected && advanced ? 1 : 0;
      previous = state; havePrevious = true;
      if (stableFrames >= 3) {
        awaitingTransition = false; stableFrames = 0; havePrevious = false;
        stateReady = true;
        if (BitCount3(state.completedMask) == 3) {
          setStatus(L"match: completed"); cleanup(); return true;
        }
        setStatus(L"match: connecting");
      } else if (Clock::now() >= transitionDeadline) {
        resetPlan(); setStatus(L"match: analyzing");
      } else {
        WaitFrame(stopRequested, started); continue;
      }
    }

    if (!solution) {
      setStatus(L"match: reading puzzle");
      std::array<int, 3> values{}, multipliers{}; int target = 0;
      if (!ReadPuzzle(frame, *geometry, values, multipliers, target)) {
        WaitFrame(stopRequested, started); continue;
      }
      solution = MakeSolution(values, multipliers, target);
      if (!solution) {
        setStatus(L"match: no solution"); cleanup(); return false;
      }
      PublishOverlay(frame, *geometry, values, multipliers, *solution, target);
      SyncOverlayWindow(overlayEnabled());
      setStatus(L"match: connecting");
    }

    if (!stateReady) {
      if (havePrevious && state == previous) ++stableFrames;
      else stableFrames = 1;
      previous = state; havePrevious = true;
      if (stableFrames < 3) { WaitFrame(stopRequested, started); continue; }
      stateReady = true; stableFrames = 0; havePrevious = false;
    }

    if (BitCount3(state.completedMask) == 3) {
      setStatus(L"match: completed"); cleanup(); return true;
    }
    int leftRow = state.leftCurrent;
    if (leftRow < 0)
      for (int row = 0; row < 3; ++row) if (!(state.completedMask & (1 << row))) { leftRow = row; break; }
    if (leftRow < 0 || (state.completedMask & (1 << leftRow))) {
      WaitFrame(stopRequested, started); continue;
    }
    const int targetRight = (*solution)[leftRow];
    const HWND foreground = ForegroundGameWindow(frame);
    if (!foreground) { WaitFrame(stopRequested, started); continue; }
    oldCompleted = state.completedMask; oldUsed = state.usedMask;
    connectingLeft = leftRow; connectingRight = targetRight;
    enterAttempts = 1;
    inputPhase = InputPhase::EnteringRight;
    // Entering the right selector and navigating it are separate verified
    // phases. Otherwise a dropped Enter turns the following Down into a
    // left-side move and desynchronizes the solver.
    inputJob = gta5::input::QueueSequence(
        std::vector<gta5::input::Key>{gta5::input::Key::FromVirtualKey(VK_RETURN)},
        foreground);
    stateReady = false;
    setStatus(L"match: connecting");
    WaitFrame(stopRequested, started);
  }

  cleanup();
  return false;
}

void SetOverlayWindow(HWND hwnd) { g_overlayWindow = hwnd; }

void ClearOverlay() {
  ClearOverlayState();
  if (g_overlayWindow) ShowWindow(g_overlayWindow, SW_HIDE);
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(hwnd, &paint);
      DrawOverlay(dc, hwnd);
      EndPaint(hwnd, &paint);
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

}  // namespace gta5::games::match
