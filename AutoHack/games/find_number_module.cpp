#include "games.h"

#include "../capture/game_window.h"
#include "../input/key_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace gta5::games::find_number {
namespace {

using Frame = gta5::capture::GameFrame;
using Clock = std::chrono::steady_clock;

struct Color { double r = 0, g = 0, b = 0; };
struct Rect { int left = 0, top = 0, right = 0, bottom = 0; };
struct Box : Rect { int pixels = 0; };
struct Bar : Rect { bool found = false; };
using Glyph = std::array<float, 20 * 32>;

// All cached coordinates are full-frame analysis pixels, never screen pixels.
struct Geometry {
  Bar bar;
  Rect targetRoi;
  Rect gridRoi;
  std::uint64_t windowGeneration = 0;
  int frameWidth = 0;
  int frameHeight = 0;
};

struct Analysis {
  bool readable = false;
  bool match = false;
  int currentStart = -1;
  int targetStart = -1;
};

std::optional<Geometry> g_detectedGeometry;

Color PixelColor(std::uint32_t pixel) {
  return {static_cast<double>((pixel >> 16) & 255),
          static_cast<double>((pixel >> 8) & 255),
          static_cast<double>(pixel & 255)};
}

double ColorDistance(const Color& a, const Color& b) {
  const double dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return std::sqrt(dr * dr + dg * dg + db * db) / 441.673;
}

bool CoolChromatic(std::uint32_t pixel) {
  const Color c = PixelColor(pixel);
  const double maximum = std::max({c.r, c.g, c.b});
  const double minimum = std::min({c.r, c.g, c.b});
  const double chroma = maximum - minimum;
  return maximum >= 48 && chroma >= 30 && chroma / maximum >= .32 &&
         c.b - c.r >= std::max(22.0, chroma * .42) && c.b >= c.g - chroma * .18;
}

bool Red(std::uint32_t pixel) {
  const Color c = PixelColor(pixel);
  const double maximum = std::max({c.r, c.g, c.b});
  const double minimum = std::min({c.r, c.g, c.b});
  const double chroma = maximum - minimum;
  return c.r >= 105 && chroma >= 45 && c.r == maximum &&
         c.r - std::max(c.g, c.b) >= chroma * .55;
}

bool Ink(std::uint32_t pixel) {
  if (Red(pixel)) return true;
  const Color c = PixelColor(pixel);
  const double maximum = std::max({c.r, c.g, c.b});
  const double minimum = std::min({c.r, c.g, c.b});
  return maximum >= 125 && minimum >= 92 && maximum - minimum <= maximum * .28;
}

Color MeanColor(const Frame& frame, int left, int top, int right, int bottom, int step) {
  left = std::clamp(left, 0, frame.width);
  right = std::clamp(right, 0, frame.width);
  top = std::clamp(top, 0, frame.height);
  bottom = std::clamp(bottom, 0, frame.height);
  Color result{};
  int count = 0;
  for (int y = top; y < bottom; y += step) {
    for (int x = left; x < right; x += step) {
      const Color c = PixelColor(frame.bgra[static_cast<std::size_t>(y) * frame.width + x]);
      result.r += c.r; result.g += c.g; result.b += c.b; ++count;
    }
  }
  if (count) { result.r /= count; result.g /= count; result.b /= count; }
  return result;
}

struct RowRun { int left = 0, right = 0, y = 0; double purity = 0; };

RowRun LongestCoolRun(const Frame& frame, int y, int allowedGap) {
  RowRun best{};
  int start = -1, good = 0, misses = 0;
  auto consider = [&](int right) {
    const int length = right - start;
    const double purity = length > 0 ? good / static_cast<double>(length) : 0;
    if (length > best.right - best.left && purity >= .94) best = {start, right, y, purity};
  };
  for (int x = 0; x < frame.width; ++x) {
    if (CoolChromatic(frame.bgra[static_cast<std::size_t>(y) * frame.width + x])) {
      if (start < 0) start = x;
      ++good; misses = 0; continue;
    }
    if (start < 0) continue;
    if (++misses <= allowedGap) continue;
    consider(x - misses + 1);
    start = -1; good = 0; misses = 0;
  }
  if (start >= 0) consider(frame.width - misses);
  return best;
}

bool MatchingBarRow(const Frame& frame, int y, int left, int right,
                    const Color& reference) {
  const int inset = std::max(3, static_cast<int>(std::lround(frame.height * .035)));
  const int step = std::max(1, frame.height / 540);
  int cool = 0, close = 0, count = 0;
  for (int x = left + inset; x < right - inset; x += step) {
    const auto pixel = frame.bgra[static_cast<std::size_t>(y) * frame.width + x];
    cool += CoolChromatic(pixel);
    close += ColorDistance(PixelColor(pixel), reference) <= .12;
    ++count;
  }
  return count > 0 && cool / static_cast<double>(count) >= .95 &&
         close / static_cast<double>(count) >= .93;
}

bool ValidateBar(const Frame& frame, const Bar& bar) {
  if (!bar.found || frame.height < 360 || bar.left < 0 || bar.top < 0 ||
      bar.right > frame.width || bar.bottom > frame.height) return false;
  const int length = bar.right - bar.left, thickness = bar.bottom - bar.top;
  if (length < frame.height * .86 || length > frame.height * 1.45 ||
      thickness < frame.height * .035 || thickness > frame.height * .09) return false;
  const int step = std::max(1, frame.height / 540);
  const int inset = std::max(3, static_cast<int>(std::lround(frame.height * .04)));
  const Color reference = MeanColor(frame, bar.left + inset, bar.top,
                                    bar.right - inset, bar.bottom, step);
  int valid = 0, count = 0;
  for (int y = bar.top; y < bar.bottom; y += step) {
    for (int x = bar.left + inset; x < bar.right - inset; x += step) {
      const auto pixel = frame.bgra[static_cast<std::size_t>(y) * frame.width + x];
      valid += CoolChromatic(pixel) && ColorDistance(PixelColor(pixel), reference) <= .12;
      ++count;
    }
  }
  if (!count || valid / static_cast<double>(count) < .955) return false;
  const int edgeWidth = std::max(3, static_cast<int>(std::lround(frame.height * .018)));
  const int edgeInset = std::max(2, static_cast<int>(std::lround(frame.height * .008)));
  const int verticalInset = std::max(1, thickness / 4);
  const Color insideLeft = MeanColor(frame, bar.left + edgeInset, bar.top + verticalInset,
      bar.left + edgeInset + edgeWidth, bar.bottom - verticalInset, step);
  const Color outsideLeft = MeanColor(frame, bar.left - edgeInset - edgeWidth,
      bar.top + verticalInset, bar.left - edgeInset, bar.bottom - verticalInset, step);
  const Color insideRight = MeanColor(frame, bar.right - edgeInset - edgeWidth,
      bar.top + verticalInset, bar.right - edgeInset, bar.bottom - verticalInset, step);
  const Color outsideRight = MeanColor(frame, bar.right + edgeInset, bar.top + verticalInset,
      bar.right + edgeInset + edgeWidth, bar.bottom - verticalInset, step);
  return ColorDistance(insideLeft, outsideLeft) >= .18 &&
         ColorDistance(insideRight, outsideRight) >= .18;
}

Bar FindBar(const Frame& frame) {
  if (frame.height < 360 || frame.width <= 0 ||
      frame.bgra.size() != static_cast<std::size_t>(frame.width) * frame.height) return {};
  const int scanTop = std::max(0, static_cast<int>(std::lround(frame.height * .035)));
  const int scanBottom = std::min(frame.height, static_cast<int>(std::lround(frame.height * .24)));
  const int step = std::max(1, frame.height / 540);
  RowRun best{};
  for (int y = scanTop; y < scanBottom; y += step) {
    const RowRun run = LongestCoolRun(frame, y, step);
    const int length = run.right - run.left;
    if (length >= frame.height * .86 && length <= frame.height * 1.45 &&
        length > best.right - best.left) best = run;
  }
  if (best.right <= best.left) return {};
  const int inset = std::max(3, static_cast<int>(std::lround(frame.height * .04)));
  const Color reference = MeanColor(frame, best.left + inset, best.y,
                                    best.right - inset, best.y + 1, step);
  int top = best.y, bottom = best.y + 1;
  while (top > 0 && MatchingBarRow(frame, top - 1, best.left, best.right, reference)) --top;
  while (bottom < frame.height &&
         MatchingBarRow(frame, bottom, best.left, best.right, reference)) ++bottom;
  Bar bar{{best.left, top, best.right, bottom}, true};
  return ValidateBar(frame, bar) ? bar : Bar{};
}

enum class PixelKind { RedOnly, AllInk };
bool Matches(std::uint32_t pixel, PixelKind kind) {
  return kind == PixelKind::RedOnly ? Red(pixel) : Ink(pixel);
}

std::vector<Box> Components(const Frame& frame, Rect roi, int minimumHeight,
                            int minimumPixels, PixelKind kind) {
  roi.left = std::clamp(roi.left, 0, frame.width); roi.right = std::clamp(roi.right, 0, frame.width);
  roi.top = std::clamp(roi.top, 0, frame.height); roi.bottom = std::clamp(roi.bottom, 0, frame.height);
  const int width = roi.right - roi.left, height = roi.bottom - roi.top;
  if (width <= 0 || height <= 0) return {};
  std::vector<unsigned char> seen(static_cast<std::size_t>(width) * height);
  std::vector<int> pending;
  std::vector<Box> result;
  pending.reserve(2048);
  for (int sy = 0; sy < height; ++sy) for (int sx = 0; sx < width; ++sx) {
    const int start = sy * width + sx;
    if (seen[start] || !Matches(frame.bgra[static_cast<std::size_t>(roi.top + sy) *
                                       frame.width + roi.left + sx], kind)) continue;
    seen[start] = 1; pending.clear(); pending.push_back(start);
    Box box{{roi.left + sx, roi.top + sy, roi.left + sx + 1, roi.top + sy + 1}, 0};
    for (std::size_t q = 0; q < pending.size(); ++q) {
      const int index = pending[q], px = index % width, py = index / width;
      ++box.pixels;
      box.left = std::min(box.left, roi.left + px); box.top = std::min(box.top, roi.top + py);
      box.right = std::max(box.right, roi.left + px + 1); box.bottom = std::max(box.bottom, roi.top + py + 1);
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const int nx = px + dx, ny = py + dy;
        if ((!dx && !dy) || nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
        const int next = ny * width + nx;
        if (!seen[next] && Matches(frame.bgra[static_cast<std::size_t>(roi.top + ny) *
                                            frame.width + roi.left + nx], kind)) {
          seen[next] = 1; pending.push_back(next);
        }
      }
    }
    if (box.bottom - box.top >= minimumHeight && box.pixels >= minimumPixels)
      result.push_back(box);
  }
  return result;
}

double CenterY(const std::vector<Box>& row) {
  double sum = 0;
  for (const Box& box : row) sum += (box.top + box.bottom) * .5;
  return row.empty() ? 0 : sum / row.size();
}

std::vector<std::vector<Box>> GroupRows(std::vector<Box> boxes, int tolerance) {
  std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
    return a.top + a.bottom != b.top + b.bottom ? a.top + a.bottom < b.top + b.bottom
                                                : a.left < b.left;
  });
  std::vector<std::vector<Box>> rows;
  std::vector<double> centers;
  for (const Box& box : boxes) {
    const double center = (box.top + box.bottom) * .5;
    if (rows.empty() || std::abs(center - centers.back()) > tolerance) {
      rows.push_back({box}); centers.push_back(center);
    } else {
      auto& row = rows.back();
      centers.back() = (centers.back() * row.size() + center) / (row.size() + 1);
      row.push_back(box);
    }
  }
  for (auto& row : rows)
    std::sort(row.begin(), row.end(), [](const Box& a, const Box& b) { return a.left < b.left; });
  return rows;
}

std::optional<std::array<std::vector<Box>, 8>> FindGridRows(const Frame& frame,
                                                            const Rect& roi) {
  const int minimumHeight = std::max(12, static_cast<int>(std::lround(frame.height * .024)));
  auto boxes = Components(frame, roi, minimumHeight, minimumHeight * 2, PixelKind::AllInk);
  const int maximumHeight = static_cast<int>(std::lround(frame.height * .052));
  const int maximumWidth = static_cast<int>(std::lround(frame.height * .040));
  boxes.erase(std::remove_if(boxes.begin(), boxes.end(), [&](const Box& box) {
    return box.bottom - box.top > maximumHeight || box.right - box.left > maximumWidth;
  }), boxes.end());
  auto grouped = GroupRows(std::move(boxes),
                           std::max(3, static_cast<int>(std::lround(frame.height * .009))));
  std::vector<std::vector<Box>> candidates;
  for (auto& row : grouped) if (row.size() == 20) candidates.push_back(std::move(row));
  int bestStart = -1;
  double bestError = std::numeric_limits<double>::max();
  for (int start = 0; start + 7 < static_cast<int>(candidates.size()); ++start) {
    std::array<double, 7> spacing{};
    double average = 0; bool valid = true;
    for (int i = 0; i < 7; ++i) {
      spacing[i] = CenterY(candidates[start + i + 1]) - CenterY(candidates[start + i]);
      average += spacing[i];
      valid = valid && spacing[i] >= frame.height * .040 && spacing[i] <= frame.height * .075;
    }
    if (!valid) continue;
    average /= spacing.size();
    double error = 0; for (double value : spacing) error += std::abs(value - average);
    if (error < bestError) { bestError = error; bestStart = start; }
  }
  if (bestStart < 0) return std::nullopt;
  std::array<std::vector<Box>, 8> rows;
  for (int i = 0; i < 8; ++i) rows[i] = std::move(candidates[bestStart + i]);
  return rows;
}

std::optional<Geometry> LocateGeometry(const Frame& frame) {
  const Bar bar = FindBar(frame);
  if (!bar.found) return std::nullopt;
  const int inset = std::max(4, static_cast<int>(std::lround(frame.height * .025)));
  Rect grid{bar.left + inset, bar.bottom + static_cast<int>(std::lround(frame.height * .17)),
            bar.right - inset,
            std::min(frame.height, bar.bottom + static_cast<int>(std::lround(frame.height * .70)))};
  const auto rows = FindGridRows(frame, grid);
  if (!rows) return std::nullopt;
  Rect target{bar.left + inset, bar.bottom + static_cast<int>(std::lround(frame.height * .025)),
              bar.right - inset,
              static_cast<int>(std::lround(CenterY((*rows)[0]) - frame.height * .055))};
  if (target.bottom <= target.top) return std::nullopt;
  return Geometry{bar, target, grid, frame.windowGeneration, frame.width, frame.height};
}

Glyph Normalize(const Frame& frame, const Box& box, PixelKind kind) {
  Glyph glyph{};
  const int sourceWidth = box.right - box.left, sourceHeight = box.bottom - box.top;
  for (int oy = 0; oy < 32; ++oy) for (int ox = 0; ox < 20; ++ox) {
    int hits = 0;
    for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
      const double u = (ox + (sx + .5) / 4) / 20.0;
      const double v = (oy + (sy + .5) / 4) / 32.0;
      const int x = std::clamp(box.left + static_cast<int>(u * sourceWidth), box.left, box.right - 1);
      const int y = std::clamp(box.top + static_cast<int>(v * sourceHeight), box.top, box.bottom - 1);
      hits += Matches(frame.bgra[static_cast<std::size_t>(y) * frame.width + x], kind);
    }
    glyph[oy * 20 + ox] = hits / 16.0f;
  }
  return glyph;
}

double Difference(const Glyph& a, const Glyph& b) {
  double difference = 0, mass = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference += std::abs(a[i] - b[i]); mass += std::max(a[i], b[i]);
  }
  return mass > 0 ? difference / mass : 1.0;
}

int RedPixels(const Frame& frame, const Box& box) {
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y)
    for (int x = box.left; x < box.right; ++x)
      count += Red(frame.bgra[static_cast<std::size_t>(y) * frame.width + x]);
  return count;
}

Analysis Analyze(const Frame& frame, const Geometry& geometry) {
  Analysis out;
  const auto rows = FindGridRows(frame, geometry.gridRoi);
  if (!rows) return out;
  const int minimumHeight = std::max(15, static_cast<int>(std::lround(frame.height * .028)));
  auto target = Components(frame, geometry.targetRoi, minimumHeight,
                           minimumHeight * 2, PixelKind::RedOnly);
  const int maximumHeight = static_cast<int>(std::lround(frame.height * .068));
  target.erase(std::remove_if(target.begin(), target.end(), [&](const Box& box) {
    return box.bottom - box.top > maximumHeight;
  }), target.end());
  std::sort(target.begin(), target.end(), [](const Box& a, const Box& b) { return a.left < b.left; });
  if (target.size() != 8) return out;

  std::array<Glyph, 8> targetGlyphs{};
  for (int i = 0; i < 8; ++i) targetGlyphs[i] = Normalize(frame, target[i], PixelKind::RedOnly);
  std::array<std::array<Glyph, 2>, 80> grid{};
  std::vector<int> active;
  for (int row = 0; row < 8; ++row) for (int column = 0; column < 10; ++column) {
    const int index = row * 10 + column;
    const Box& first = (*rows)[row][column * 2];
    const Box& second = (*rows)[row][column * 2 + 1];
    grid[index][0] = Normalize(frame, first, PixelKind::AllInk);
    grid[index][1] = Normalize(frame, second, PixelKind::AllInk);
    const int inkPixels = first.pixels + second.pixels;
    const int redPixels = RedPixels(frame, first) + RedPixels(frame, second);
    if (redPixels >= std::max(10, static_cast<int>(inkPixels * .22))) active.push_back(index);
  }
  bool consecutive = active.size() == 4;
  for (std::size_t i = 1; i < active.size(); ++i)
    consecutive = consecutive && active[i] == active.front() + static_cast<int>(i);
  if (!consecutive) return out;
  out.currentStart = active.front();

  double bestMean = 1, bestWorst = 1, secondMean = 1;
  for (int start = 0; start <= 76; ++start) {
    double sum = 0, worst = 0;
    for (int offset = 0; offset < 4; ++offset) for (int digit = 0; digit < 2; ++digit) {
      const double difference = Difference(targetGlyphs[offset * 2 + digit], grid[start + offset][digit]);
      sum += difference; worst = std::max(worst, difference);
    }
    const double mean = sum / 8;
    if (mean < bestMean) {
      secondMean = bestMean; bestMean = mean; bestWorst = worst; out.targetStart = start;
    } else if (mean < secondMean) secondMean = mean;
  }
  out.readable = out.targetStart >= 0 && bestMean <= .30 && bestWorst <= .55 &&
                 secondMean - bestMean >= .07;
  out.match = out.readable && out.currentStart == out.targetStart;
  return out;
}

std::vector<WORD> PlanDirections(int start, int target) {
  constexpr int kLastStart = 76;
  if (start < 0 || start > kLastStart || target < 0 || target > kLastStart) return {};
  std::array<int, kLastStart + 1> previous{};
  std::array<WORD, kLastStart + 1> previousKey{};
  previous.fill(-2); previous[start] = -1;
  std::queue<int> pending; pending.push(start);
  constexpr std::array<std::pair<int, WORD>, 4> moves{{
      {-10, VK_UP}, {10, VK_DOWN}, {-1, VK_LEFT}, {1, VK_RIGHT}}};
  while (!pending.empty() && previous[target] == -2) {
    const int current = pending.front(); pending.pop();
    for (const auto& move : moves) {
      const int next = current + move.first;
      if (next < 0 || next > kLastStart || previous[next] != -2) continue;
      previous[next] = current; previousKey[next] = move.second; pending.push(next);
    }
  }
  if (previous[target] == -2) return {};
  std::vector<WORD> keys;
  for (int at = target; at != start; at = previous[at]) keys.push_back(previousKey[at]);
  std::reverse(keys.begin(), keys.end());
  return keys;
}

bool GeometryMatchesFrame(const Geometry& geometry, const Frame& frame) {
  return geometry.windowGeneration == frame.windowGeneration &&
         geometry.frameWidth == frame.width && geometry.frameHeight == frame.height;
}

HWND ForegroundGameWindow(const Frame& frame) {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return nullptr;
  RECT client{};
  if (!GetClientRect(foreground, &client)) return nullptr;
  POINT origin{client.left, client.top};
  if (!ClientToScreen(foreground, &origin)) return nullptr;
  const int width = client.right - client.left, height = client.bottom - client.top;
  return origin.x == frame.screenX && origin.y == frame.screenY &&
         width == frame.screenW && height == frame.screenH ? foreground : nullptr;
}

void WaitForFrame(const std::function<bool()>& stopRequested, Clock::time_point started) {
  const auto deadline = started + std::chrono::milliseconds(16);
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
    return false;
  }
  g_detectedGeometry = LocateGeometry(frame);
  return g_detectedGeometry.has_value();
}

void ResetInGameCache() { g_detectedGeometry.reset(); }

bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status) {
  std::optional<Geometry> geometry = g_detectedGeometry;
  gta5::input::Job inputJob;
  bool submitting = false;
  int absentFrames = 0;
  auto analyzeAfter = Clock::time_point{};
  std::wstring lastStatus;
  auto setStatus = [&](const wchar_t* value) {
    if (lastStatus == value) return;
    lastStatus = value;
    status(value);
  };
  auto cleanup = [&] {
    gta5::input::CancelAll();
    inputJob = {};
    geometry.reset();
    ResetInGameCache();
  };

  setStatus(L"find_number: locating");
  while (!stopRequested()) {
    const auto frameStarted = Clock::now();
    Frame frame;
    if (!gta5::capture::CaptureGameFrame(frame)) {
      geometry.reset();
      g_detectedGeometry.reset();
      if (++absentFrames >= 3) break;
      WaitForFrame(stopRequested, frameStarted);
      continue;
    }

    if (geometry && (!GeometryMatchesFrame(*geometry, frame) ||
                     !ValidateBar(frame, geometry->bar))) {
      geometry.reset();
      g_detectedGeometry.reset();
    }
    if (!geometry) {
      geometry = LocateGeometry(frame);  // Immediate full-frame fallback on this capture.
      if (!geometry) {
        if (++absentFrames >= 3) {
          setStatus(L"find_number: minigame exited");
          break;
        }
        WaitForFrame(stopRequested, frameStarted);
        continue;
      }
      g_detectedGeometry = geometry;
    }
    absentFrames = 0;

    if (inputJob) {
      if (inputJob.Pending()) {
        WaitForFrame(stopRequested, frameStarted);
        continue;
      }
      if (!inputJob.Succeeded()) {
        inputJob = {};
        submitting = false;
        analyzeAfter = Clock::now() + std::chrono::milliseconds(80);
        setStatus(L"find_number: analyzing");
        WaitForFrame(stopRequested, frameStarted);
        continue;
      }
      inputJob = {};
      if (submitting) {
        setStatus(L"find_number: completed");
        cleanup();
        return true;
      }
      analyzeAfter = Clock::now() + std::chrono::milliseconds(100);
      setStatus(L"find_number: verifying position");
      WaitForFrame(stopRequested, frameStarted);
      continue;
    }

    if (Clock::now() < analyzeAfter) {
      WaitForFrame(stopRequested, frameStarted);
      continue;
    }
    setStatus(L"find_number: analyzing");
    const Analysis result = Analyze(frame, *geometry);
    if (!result.readable) {
      WaitForFrame(stopRequested, frameStarted);
      continue;
    }
    const HWND foreground = ForegroundGameWindow(frame);
    if (!foreground) {
      WaitForFrame(stopRequested, frameStarted);
      continue;
    }
    std::vector<gta5::input::Key> keys;
    if (result.match) {
      keys.push_back(gta5::input::Key::FromVirtualKey(VK_RETURN));
      submitting = true;
      setStatus(L"find_number: submitting");
    } else {
      for (WORD key : PlanDirections(result.currentStart, result.targetStart))
        keys.push_back(gta5::input::Key::FromVirtualKey(key));
      if (keys.empty()) {
        WaitForFrame(stopRequested, frameStarted);
        continue;
      }
      submitting = false;
      setStatus(L"find_number: moving");
    }
    inputJob = gta5::input::QueueSequence(keys, foreground);
    WaitForFrame(stopRequested, frameStarted);
  }

  cleanup();
  return false;
}

}  // namespace gta5::games::find_number
