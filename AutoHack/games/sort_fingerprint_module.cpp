#include "../capture/game_window.h"
#include "games.h"
#include "../input/key_input.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace gta5::games::sort_fingerprint {

namespace {

struct Rect {
    int x{}, y{}, w{}, h{};
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    double cx() const { return x + w / 2.0; }
    double cy() const { return y + h / 2.0; }
};

struct Image {
    int x{}, y{}, w{}, h{};
    std::uint64_t windowGeneration{};
    std::vector<std::uint32_t> pixels;
    const std::uint32_t* view{};
    std::uint32_t at(int px, int py) const {
        const auto* data = view ? view : pixels.data();
        return data[static_cast<size_t>(py) * w + px];
    }
};

struct Binary {
    int w{}, h{};
    std::vector<std::uint8_t> data;
    std::uint8_t at(int x, int y) const { return data[static_cast<size_t>(y) * w + x]; }
    std::uint8_t& at(int x, int y) { return data[static_cast<size_t>(y) * w + x]; }
};

struct Anchors {
    Rect top;
    Rect left;
    Rect right;
};

struct RoiGeometry {
    Anchors anchors;
    std::array<Rect, 8> frames;
};

std::optional<RoiGeometry> gDetectedGeometry;
std::uint64_t gDetectedGeneration = 0;

struct RoiResult {
    Anchors anchors;
    std::array<Rect, 8> frames;
    Rect targetRect;
    Binary targetMask;
    int selected{};
    std::array<int, 8> labels{};
    std::array<double, 8> confidence{};
    std::uint64_t targetHash{};
};

struct Plan {
    std::vector<WORD> keys;
    std::array<int, 8> labels{};
    int selected{};
};

int blue(std::uint32_t p) { return static_cast<int>(p & 0xff); }
int green(std::uint32_t p) { return static_cast<int>((p >> 8) & 0xff); }
int red(std::uint32_t p) { return static_cast<int>((p >> 16) & 0xff); }

bool isGreen(std::uint32_t p, int minimum = 65) {
    const int b = blue(p), g = green(p), r = red(p);
    return g >= minimum && g * 100 > r * 128 && g * 100 > b * 120;
}

bool isWhite(std::uint32_t p) {
    const int b = blue(p), g = green(p), r = red(p);
    const int maximum = std::max({r, g, b});
    const int minimum = std::min({r, g, b});
    return minimum >= 165 && maximum - minimum <= 65;
}

int gray(std::uint32_t p) {
    return (29 * blue(p) + 150 * green(p) + 77 * red(p)) >> 8;
}

class ScreenCapture {
public:
    Image capture() {
        if (!gta5::capture::CaptureGameFrame(frame_)) throw std::runtime_error("screen capture failed");
        Image image;
        image.x = 0;
        image.y = 0;
        image.w = frame_.width;
        image.h = frame_.height;
        image.windowGeneration = frame_.windowGeneration;
        image.view = frame_.bgra.data();
        return image;
    }

private:
    gta5::capture::GameFrame frame_;
};

std::vector<Rect> dedupeRects(std::vector<Rect> input, int tolerance) {
    std::sort(input.begin(), input.end(), [](const Rect& a, const Rect& b) {
        return a.w * a.h > b.w * b.h;
    });
    std::vector<Rect> result;
    for (const Rect& rect : input) {
        bool duplicate = false;
        for (const Rect& kept : result) {
            if (std::abs(rect.cx() - kept.cx()) <= tolerance &&
                std::abs(rect.cy() - kept.cy()) <= tolerance &&
                std::abs(rect.w - kept.w) <= std::max(tolerance, static_cast<int>(kept.w * .08)) &&
                std::abs(rect.h - kept.h) <= std::max(tolerance, static_cast<int>(kept.h * .08))) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) result.push_back(rect);
    }
    std::sort(result.begin(), result.end(), [](const Rect& a, const Rect& b) {
        return a.y == b.y ? a.x < b.x : a.y < b.y;
    });
    return result;
}

std::vector<Rect> findHeaderBars(const Image& image) {
    const int bandH = std::max(8, static_cast<int>(std::lround(image.h * .026)));
    const int yStep = std::max(1, image.h / 1080);
    const int allowedGap = std::max(3, static_cast<int>(image.w * .0045));
    std::vector<int> columns(image.w, 0);
    for (int y = 0; y < std::min(bandH, image.h); ++y) {
        for (int x = 0; x < image.w; ++x) columns[x] += isGreen(image.at(x, y)) ? 1 : 0;
    }

    std::vector<Rect> raw;
    for (int y = 0; y + bandH <= image.h; ++y) {
        if (y > 0) {
            for (int x = 0; x < image.w; ++x) {
                columns[x] -= isGreen(image.at(x, y - 1)) ? 1 : 0;
                columns[x] += isGreen(image.at(x, y + bandH - 1)) ? 1 : 0;
            }
        }
        if (y % yStep != 0) continue;
        int start = -1, lastActive = -1;
        long long densitySum = 0;
        auto finish = [&](int endX) {
            if (start < 0) return;
            const int width = endX - start + 1;
            if (width >= image.w * .18 && width <= image.w * .48) {
                const double fill = static_cast<double>(densitySum) / (width * bandH);
                if (fill >= .65) raw.push_back({start, y, width, bandH});
            }
            start = lastActive = -1;
            densitySum = 0;
        };
        for (int x = 0; x < image.w; ++x) {
            const bool active = columns[x] >= std::max(2, static_cast<int>(bandH * .16));
            if (active) {
                if (start < 0) start = x;
                lastActive = x;
            }
            if (start >= 0) densitySum += columns[x];
            if (start >= 0 && x - lastActive > allowedGap) finish(lastActive);
        }
        if (start >= 0) finish(lastActive);
    }
    return dedupeRects(std::move(raw), std::max(7, image.h / 80));
}

std::optional<Anchors> selectAnchors(const std::vector<Rect>& headers, int sw, int sh) {
    double bestScore = -1e30;
    std::optional<Anchors> best;
    for (size_t i = 0; i < headers.size(); ++i) {
        for (size_t j = i + 1; j < headers.size(); ++j) {
            Rect left = headers[i], right = headers[j];
            if (left.x > right.x) std::swap(left, right);
            const double sameRow = std::abs(left.cy() - right.cy());
            if (sameRow > std::max({left.h * 1.0, right.h * 1.0, sh * .018})) continue;
            if (right.cx() - left.cx() < sw * .17) continue;
            if (std::abs(left.w - right.w) > std::max(left.w, right.w) * .35) continue;
            for (const Rect& top : headers) {
                if (top.cy() >= right.cy() - right.h * 1.5) continue;
                if (std::abs(top.cx() - right.cx()) >= right.w * .25) continue;
                if (std::abs(top.w - right.w) >= right.w * .35) continue;
                const double score = left.y * 2.0 - sameRow - std::abs(left.w - right.w) * .2;
                if (score > bestScore) {
                    bestScore = score;
                    best = Anchors{top, left, right};
                }
            }
        }
    }
    return best;
}

double greenFillRatio(const Image& image, const Rect& rect) {
    const int x1 = std::clamp(rect.x, 0, image.w);
    const int y1 = std::clamp(rect.y, 0, image.h);
    const int x2 = std::clamp(rect.right(), 0, image.w);
    const int y2 = std::clamp(rect.bottom(), 0, image.h);
    int greenPixels = 0;
    for (int y = y1; y < y2; ++y)
        for (int x = x1; x < x2; ++x)
            greenPixels += isGreen(image.at(x, y)) ? 1 : 0;
    return static_cast<double>(greenPixels) / std::max(1, (x2 - x1) * (y2 - y1));
}

bool anchorsPresent(const Image& image, const Anchors& anchors) {
    return greenFillRatio(image, anchors.top) >= .48 &&
           greenFillRatio(image, anchors.left) >= .48 &&
           greenFillRatio(image, anchors.right) >= .48;
}

bool isSortFingerprintAnchorLayout(const Anchors& anchors, int screenH) {
    const double scale = screenH / 1080.0;
    const int alignTolerance = std::max(6, static_cast<int>(std::lround(16 * scale)));
    const int rowTolerance = std::max(6, static_cast<int>(std::lround(18 * scale)));
    const int heightTolerance = std::max(4, static_cast<int>(std::lround(7 * scale)));
    const int columnGap = anchors.right.x - anchors.left.right();
    const int rowGap = anchors.right.y - anchors.top.y;

    if (std::abs(anchors.top.x - anchors.right.x) > alignTolerance) return false;
    if (std::abs(anchors.top.right() - anchors.right.right()) > alignTolerance) return false;
    if (std::abs(anchors.left.y - anchors.right.y) > rowTolerance) return false;
    if (std::abs(anchors.top.h - anchors.right.h) > heightTolerance) return false;
    if (std::abs(anchors.left.h - anchors.right.h) > heightTolerance) return false;
    if (columnGap < static_cast<int>(4 * scale) || columnGap > static_cast<int>(44 * scale)) return false;
    if (rowGap < static_cast<int>(160 * scale) || rowGap > static_cast<int>(235 * scale)) return false;
    if (anchors.left.w < screenH * .42 || anchors.left.w > screenH * .56) return false;
    if (anchors.right.w < screenH * .55 || anchors.right.w > screenH * .70) return false;

    return true;
}

struct LineGroup { int y1{}, y2{}, x1{}, x2{}, count{}; };

std::vector<Rect> findFragmentFrames(const Image& image, const Rect& header) {
    const int x1 = std::max(0, header.x - static_cast<int>(header.w * .03));
    const int x2 = std::min(image.w, header.right() + static_cast<int>(header.w * .03));
    const int y1 = std::min(image.h - 1, header.bottom() + std::max(4, static_cast<int>(image.h * .010)));
    const int threshold = std::max(30, static_cast<int>(header.w * .55));
    const int allowedGap = std::max(3, static_cast<int>(header.w * .010));
    std::vector<LineGroup> lines;
    for (int y = y1; y < image.h; ++y) {
        int count = 0, first = x2, last = x1;
        int runStart = -1, runLast = -1, runCount = 0;
        auto finishRun = [&]() {
            if (runStart >= 0 && runLast - runStart > last - first) {
                first = runStart; last = runLast; count = runCount;
            }
            runStart = runLast = -1; runCount = 0;
        };
        for (int x = x1; x < x2; ++x) {
            const auto p = image.at(x, y);
            if (isGreen(p) || isWhite(p)) {
                if (runStart < 0) runStart = x;
                runLast = x;
                ++runCount;
            }
            if (runStart >= 0 && x - runLast > allowedGap) finishRun();
        }
        finishRun();
        if (count < threshold) continue;
        if (!lines.empty() && y <= lines.back().y2 + 2) {
            LineGroup& line = lines.back();
            line.y2 = y;
            if (count > line.count) {
                line.count = count;
                line.x1 = first;
                line.x2 = last;
            }
        } else {
            lines.push_back({y, y, first, last, count});
        }
    }

    std::vector<Rect> candidates;
    for (size_t i = 0; i < lines.size(); ++i) {
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const int boxH = lines[j].y2 - lines[i].y1 + 1;
            if (boxH > image.h * .10) break;
            if (boxH < image.h * .03) continue;
            const int bx1 = std::min(lines[i].x1, lines[j].x1);
            const int bx2 = std::max(lines[i].x2, lines[j].x2);
            const int boxW = bx2 - bx1 + 1;
            if (boxW < header.w * .64 || boxW > header.w * 1.04) continue;
            if (std::abs((bx1 + bx2) / 2.0 - header.cx()) > header.w * .18) continue;
            candidates.push_back({bx1, lines[i].y1, boxW, boxH});
        }
    }
    candidates = dedupeRects(std::move(candidates), std::max(2, image.h / 500));

    // Only the first two complete frames are measured. Their top coordinates
    // define the arithmetic progression used for all remaining rows.
    double bestPairScore = 1e30;
    std::optional<std::pair<Rect, Rect>> firstPair;
    for (const Rect& first : candidates) {
        for (const Rect& second : candidates) {
            if (second.y <= first.y) continue;
            const double averageH = (first.h + second.h) / 2.0;
            const int pitch = second.y - first.y;
            const double gap = pitch - averageH;
            if (std::abs(first.h - second.h) > averageH * .12) continue;
            if (std::abs(first.w - second.w) > std::max(first.w, second.w) * .08) continue;
            if (std::abs(first.cx() - second.cx()) > std::max(first.w, second.w) * .05) continue;
            if (gap < averageH * .03 || gap > averageH * .32) continue;
            const double score = first.y * 1000.0 + std::abs(first.h - second.h) * 20.0 +
                                 std::abs(first.w - second.w) + gap;
            if (score < bestPairScore) {
                bestPairScore = score;
                firstPair = std::make_pair(first, second);
            }
        }
    }
    if (!firstPair) return {};

    const Rect& first = firstPair->first;
    const Rect& second = firstPair->second;
    const int pitch = second.y - first.y;
    const int x = static_cast<int>(std::lround((first.x + second.x) / 2.0));
    const int w = static_cast<int>(std::lround((first.w + second.w) / 2.0));
    const int h = static_cast<int>(std::lround((first.h + second.h) / 2.0));
    std::vector<Rect> inferred;
    inferred.reserve(8);
    for (int index = 0; index < 8; ++index)
        inferred.push_back({x, first.y + index * pitch, w, h});
    return inferred;
}

std::optional<std::array<Rect, 8>> selectEightFrames(const std::vector<Rect>& candidates, const Rect& header) {
    std::vector<Rect> useful;
    for (const Rect& r : candidates) if (r.y > header.bottom()) useful.push_back(r);
    std::sort(useful.begin(), useful.end(), [](const Rect& a, const Rect& b) { return a.cy() < b.cy(); });
    if (useful.size() < 8) return std::nullopt;
    if (useful.size() > 18) useful.resize(18);

    double bestScore = 1e30;
    std::optional<std::array<Rect, 8>> best;
    std::array<int, 8> pick{};
    auto evaluate = [&]() {
        std::array<Rect, 8> group{};
        std::array<double, 7> gaps{};
        double avgH = 0, avgW = 0, avgX = 0;
        for (int i = 0; i < 8; ++i) {
            group[i] = useful[pick[i]];
            avgH += group[i].h;
            avgW += group[i].w;
            avgX += group[i].cx();
            if (i) gaps[i - 1] = group[i].cy() - group[i - 1].cy();
        }
        avgH /= 8; avgW /= 8; avgX /= 8;
        auto sortedGaps = gaps;
        std::sort(sortedGaps.begin(), sortedGaps.end());
        const double pitch = sortedGaps[3];
        if (pitch < avgH * .90 || pitch > avgH * 1.80) return;
        for (double gap : gaps) if (gap <= avgH * .65) return;
        double gapErr = 0, widthErr = 0, xErr = 0, heightErr = 0;
        for (double gap : gaps) gapErr += std::abs(gap - pitch) / pitch;
        for (const Rect& r : group) {
            widthErr += std::abs(r.w - avgW) / avgW;
            xErr += std::abs(r.cx() - avgX) / avgW;
            heightErr += std::abs(r.h - avgH) / avgH;
        }
        const double score = gapErr * 5 + widthErr * 3 + xErr * 4 + heightErr * 2;
        if (score < bestScore) { bestScore = score; best = group; }
    };
    std::function<void(int, int)> choose = [&](int depth, int start) {
        if (depth == 8) { evaluate(); return; }
        for (int i = start; i <= static_cast<int>(useful.size()) - (8 - depth); ++i) {
            pick[depth] = i;
            choose(depth + 1, i + 1);
        }
    };
    choose(0, 0);
    return best;
}

int findSelected(const Image& image, const std::array<Rect, 8>& frames) {
    int selected = -1;
    double bestRatio = 0;
    int bestWhite = 0;
    for (int index = 0; index < 8; ++index) {
        const Rect& r = frames[index];
        const int bandY = std::max(2, static_cast<int>(r.h * .13));
        const int bandX = std::max(2, static_cast<int>(r.w * .018));
        const int upper = std::max(bandY + 1, static_cast<int>(r.h * .58));
        int whiteCount = 0, greenCount = 0;
        for (int y = 0; y < upper; ++y) {
            for (int x = 0; x < r.w; ++x) {
                if (!(y < bandY || x < bandX || x >= r.w - bandX)) continue;
                const auto p = image.at(r.x + x, r.y + y);
                whiteCount += isWhite(p) ? 1 : 0;
                greenCount += isGreen(p) ? 1 : 0;
            }
        }
        const double ratio = static_cast<double>(whiteCount) / std::max(1, whiteCount + greenCount);
        if (ratio > bestRatio) { bestRatio = ratio; bestWhite = whiteCount; selected = index; }
    }
    if (selected < 0 || bestRatio < .52 || bestWhite < std::max(20, static_cast<int>(frames[selected].w * .12))) {
        throw std::runtime_error("white selected frame was not found");
    }
    return selected;
}

std::vector<int> largestCluster(const std::vector<int>& indices, int maxGap) {
    if (indices.empty()) return {};
    size_t bestStart = 0, bestEnd = 1, start = 0;
    for (size_t i = 1; i <= indices.size(); ++i) {
        if (i == indices.size() || indices[i] - indices[i - 1] > maxGap) {
            if (indices[i - 1] - indices[start] > indices[bestEnd - 1] - indices[bestStart]) {
                bestStart = start; bestEnd = i;
            }
            start = i;
        }
    }
    return std::vector<int>(indices.begin() + static_cast<long long>(bestStart), indices.begin() + static_cast<long long>(bestEnd));
}

std::pair<Rect, Binary> findTarget(const Image& image, const Rect& header, const std::array<Rect, 8>& frames) {
    const int x1 = std::max(0, header.x + static_cast<int>(header.w * .05));
    const int x2 = std::min(image.w, header.right() - static_cast<int>(header.w * .05));
    const int y1 = std::min(image.h - 1, header.bottom() + std::max(8, static_cast<int>(image.h * .012)));
    const int y2 = std::min(image.h, std::max(frames[7].bottom() + static_cast<int>(image.h * .025), y1 + static_cast<int>(image.h * .35)));
    const int rw = x2 - x1, rh = y2 - y1;
    Binary mask{rw, rh, std::vector<std::uint8_t>(static_cast<size_t>(rw) * rh)};
    std::vector<int> rowCounts(rh), colCounts(rw);
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            if (isGreen(image.at(x1 + x, y1 + y), 105)) {
                mask.at(x, y) = 1;
                ++rowCounts[y]; ++colCounts[x];
            }
        }
    }
    std::vector<int> rows, cols;
    const int rowMin = std::max(4, static_cast<int>(header.w * .012));
    const int colMin = std::max(4, static_cast<int>(rh * .012));
    for (int y = 0; y < rh; ++y) if (rowCounts[y] >= rowMin && rowCounts[y] < rw * .62) rows.push_back(y);
    for (int x = 0; x < rw; ++x) if (colCounts[x] >= colMin && colCounts[x] < rh * .62) cols.push_back(x);
    rows = largestCluster(rows, std::max(7, static_cast<int>(rh * .018)));
    cols = largestCluster(cols, std::max(7, static_cast<int>(rw * .018)));
    if (rows.size() < 20 || cols.size() < 20) throw std::runtime_error("target fingerprint was not found");
    Rect target{x1 + cols.front(), y1 + rows.front(), cols.back() - cols.front() + 1, rows.back() - rows.front() + 1};
    Binary cropped{target.w, target.h, std::vector<std::uint8_t>(static_cast<size_t>(target.w) * target.h)};
    for (int y = 0; y < target.h; ++y)
        for (int x = 0; x < target.w; ++x)
            cropped.at(x, y) = mask.at(cols.front() + x, rows.front() + y);
    return {target, std::move(cropped)};
}

int otsuThreshold(const std::array<int, 256>& histogram, int total) {
    long long sum = 0;
    for (int i = 0; i < 256; ++i) sum += static_cast<long long>(i) * histogram[i];
    long long backgroundSum = 0;
    int backgroundWeight = 0;
    double best = -1;
    int threshold = 0;
    for (int t = 0; t < 256; ++t) {
        backgroundWeight += histogram[t];
        if (!backgroundWeight) continue;
        const int foregroundWeight = total - backgroundWeight;
        if (!foregroundWeight) break;
        backgroundSum += static_cast<long long>(t) * histogram[t];
        const double meanB = static_cast<double>(backgroundSum) / backgroundWeight;
        const double meanF = static_cast<double>(sum - backgroundSum) / foregroundWeight;
        const double between = static_cast<double>(backgroundWeight) * foregroundWeight * (meanB - meanF) * (meanB - meanF);
        if (between > best) { best = between; threshold = t; }
    }
    return threshold;
}

Binary extractPiece(const Image& image, const Rect& r, bool selected) {
    const int x1 = r.x + static_cast<int>(r.w * .11);
    const int x2 = r.x + static_cast<int>(r.w * .89);
    const int y1 = r.y + std::max(2, static_cast<int>(r.h * .10));
    const int y2 = r.y + std::min(r.h - 2, static_cast<int>(r.h * .90));
    Binary result{x2 - x1, y2 - y1, std::vector<std::uint8_t>(static_cast<size_t>(x2 - x1) * (y2 - y1))};
    std::array<int, 256> histogram{};
    for (int y = y1; y < y2; ++y) for (int x = x1; x < x2; ++x) ++histogram[gray(image.at(x, y))];
    const int threshold = otsuThreshold(histogram, result.w * result.h);
    for (int y = 0; y < result.h; ++y) {
        for (int x = 0; x < result.w; ++x) {
            const auto pixel = image.at(x1 + x, y1 + y);
            const bool grayscaleForeground = gray(pixel) > threshold;
            const bool expectedColor = selected ? isWhite(pixel) : isGreen(pixel);
            result.at(x, y) = grayscaleForeground && expectedColor ? 1 : 0;
        }
    }
    return result;
}

Binary normalize(const Binary& source, int outW = 256, int outH = 56) {
    int minX = source.w, minY = source.h, maxX = -1, maxY = -1, count = 0;
    for (int y = 0; y < source.h; ++y) for (int x = 0; x < source.w; ++x) if (source.at(x, y)) {
        minX = std::min(minX, x); minY = std::min(minY, y); maxX = std::max(maxX, x); maxY = std::max(maxY, y); ++count;
    }
    Binary output{outW, outH, std::vector<std::uint8_t>(static_cast<size_t>(outW) * outH)};
    if (count < 12) return output;
    const int sw = maxX - minX + 1, sh = maxY - minY + 1;
    const double scale = std::min((outW - 8.0) / sw, (outH - 4.0) / sh);
    const int nw = std::max(1, static_cast<int>(sw * scale));
    const int nh = std::max(1, static_cast<int>(sh * scale));
    const int ox = (outW - nw) / 2, oy = (outH - nh) / 2;
    for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) {
        const int sx = minX + std::min(sw - 1, static_cast<int>(x / scale));
        const int sy = minY + std::min(sh - 1, static_cast<int>(y / scale));
        output.at(ox + x, oy + y) = source.at(sx, sy);
    }
    return output;
}

Binary resizeFull(const Binary& source, int outW = 256, int outH = 56) {
    Binary output{outW, outH, std::vector<std::uint8_t>(static_cast<size_t>(outW) * outH)};
    for (int y = 0; y < outH; ++y) {
        const int sy = std::min(source.h - 1, y * source.h / outH);
        for (int x = 0; x < outW; ++x) {
            const int sx = std::min(source.w - 1, x * source.w / outW);
            output.at(x, y) = source.at(sx, sy);
        }
    }
    return output;
}

constexpr int kCompareW = 120;
constexpr int kCompareH = 24;
constexpr int kPackedWordsPerRow = (kCompareW + 63) / 64;
constexpr int kShiftStep = 4;

struct PackedMask {
    std::array<std::uint64_t, kCompareH * kPackedWordsPerRow> words{};
};

struct TargetCandidate {
    int label{};
    std::array<PackedMask, 5> shifted;
};

PackedMask packMask(const Binary& source, int shiftX = 0) {
    PackedMask packed;
    for (int y = 0; y < kCompareH; ++y) {
        for (int x = 0; x < kCompareW; ++x) {
            if (!source.at(x, y)) continue;
            const int destinationX = x + shiftX;
            if (destinationX < 0 || destinationX >= kCompareW) continue;
            packed.words[static_cast<size_t>(y) * kPackedWordsPerRow + destinationX / 64] |=
                1ULL << (destinationX % 64);
        }
    }
    return packed;
}

std::vector<TargetCandidate> buildTargetCandidates(const Binary& target) {
    std::vector<TargetCandidate> candidates;
    const int minWindow = std::max(8, static_cast<int>(target.h * .090));
    const int maxWindow = std::max(minWindow, static_cast<int>(target.h * .132));
    const int windowStep = std::max(2, target.h / 60);
    const int yStep = std::max(1, target.h / 180);
    for (int windowH = minWindow; windowH <= maxWindow; windowH += windowStep) {
        const double segmentPitch = (target.h - windowH) / 7.0;
        if (segmentPitch <= 0) continue;
        for (int y = 0; y + windowH <= target.h; y += yStep) {
            Binary resized{kCompareW, kCompareH,
                           std::vector<std::uint8_t>(static_cast<size_t>(kCompareW) * kCompareH)};
            for (int outY = 0; outY < kCompareH; ++outY) {
                const int sourceY = y + std::min(windowH - 1, outY * windowH / kCompareH);
                for (int outX = 0; outX < kCompareW; ++outX) {
                    const int sourceX = std::min(target.w - 1, outX * target.w / kCompareW);
                    resized.at(outX, outY) = target.at(sourceX, sourceY);
                }
            }
            TargetCandidate candidate;
            candidate.label = std::clamp(static_cast<int>(std::lround(y / segmentPitch)), 0, 7);
            for (int shiftIndex = 0; shiftIndex < 5; ++shiftIndex)
                candidate.shifted[shiftIndex] = packMask(resized, (shiftIndex - 2) * kShiftStep);
            candidates.push_back(std::move(candidate));
        }
    }
    return candidates;
}

int popcount64(std::uint64_t value) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt64(value));
#else
    return __builtin_popcountll(value);
#endif
}

std::array<double, 8> slidingScores(const Binary& piece,
                                    const std::vector<TargetCandidate>& candidates,
                                    double validFraction) {
    const Binary normalizedPiece = resizeFull(piece, kCompareW, kCompareH);
    const PackedMask packedPiece = packMask(normalizedPiece);
    const int validHeight = std::max(1, static_cast<int>(kCompareH * validFraction));
    int pieceCount = 0;
    for (int y = 0; y < validHeight; ++y)
        for (int word = 0; word < kPackedWordsPerRow; ++word)
            pieceCount += popcount64(packedPiece.words[static_cast<size_t>(y) * kPackedWordsPerRow + word]);

    std::array<double, 8> scores{};
    for (const TargetCandidate& candidate : candidates) {
        double best = 0;
        for (const PackedMask& shifted : candidate.shifted) {
            int candidateCount = 0, intersection = 0;
            for (int y = 0; y < validHeight; ++y) {
                for (int word = 0; word < kPackedWordsPerRow; ++word) {
                    const size_t index = static_cast<size_t>(y) * kPackedWordsPerRow + word;
                    candidateCount += popcount64(shifted.words[index]);
                    intersection += popcount64(packedPiece.words[index] & shifted.words[index]);
                }
            }
            best = std::max(best, 2.0 * intersection / std::max(1, pieceCount + candidateCount));
        }
        scores[candidate.label] = std::max(scores[candidate.label], best);
    }
    return scores;
}

std::uint64_t fingerprintHash(const Binary& target) {
    const Binary fixed = normalize(target, 64, 96);
    std::uint64_t hash = 0;
    constexpr int blockW = 8;
    constexpr int blockH = 12;
    int bitIndex = 0;
    for (int by = 0; by < fixed.h; by += blockH) {
        for (int bx = 0; bx < fixed.w; bx += blockW) {
            int occupied = 0;
            for (int y = by; y < std::min(by + blockH, fixed.h); ++y)
                for (int x = bx; x < std::min(bx + blockW, fixed.w); ++x)
                    occupied += fixed.at(x, y);
            if (occupied >= 11) hash |= (1ULL << bitIndex);
            ++bitIndex;
        }
    }
    return hash;
}

int hammingDistance(std::uint64_t a, std::uint64_t b) {
    std::uint64_t value = a ^ b;
    int count = 0;
    while (value) {
        value &= value - 1;
        ++count;
    }
    return count;
}

void analyzePieces(const Image& image, RoiResult& result) {
    const auto targetCandidates = buildTargetCandidates(result.targetMask);
    std::array<std::array<double, 8>, 8> scores{};
    for (int row = 0; row < 8; ++row) {
        const int tolerance = std::max(3, static_cast<int>(result.frames[row].h * .06)) + row;
        std::vector<std::pair<int, Binary>> variants;
        variants.reserve(tolerance * 2 + 1);
        for (int offset = -tolerance; offset <= tolerance; ++offset) {
            Rect shifted = result.frames[row];
            shifted.y = std::clamp(shifted.y + offset, 0, image.h - shifted.h);
            variants.emplace_back(offset, extractPiece(image, shifted, row == result.selected));
        }
        for (const auto& variant : variants) {
            const double validFraction = (row == 7 && row == result.selected) ? .50 : 1.0;
            const auto variantScores = slidingScores(variant.second, targetCandidates, validFraction);
            for (int targetIndex = 0; targetIndex < 8; ++targetIndex) {
                const double adjusted = variantScores[targetIndex] - std::abs(variant.first) * .005;
                scores[row][targetIndex] = std::max(scores[row][targetIndex], adjusted);
            }
        }
    }
    for (int row = 0; row < 8; ++row) {
        result.labels[row] = static_cast<int>(
            std::max_element(scores[row].begin(), scores[row].end()) - scores[row].begin()
        );
        std::array<double, 8> sorted = scores[row];
        std::sort(sorted.rbegin(), sorted.rend());
        const double margin = sorted[0] - sorted[1];
        result.confidence[row] = std::clamp(scores[row][result.labels[row]] * .75 + margin * 1.5, 0.0, 1.0);
    }
}

RoiResult analyzeStoredRoi(const Image& image, const RoiGeometry& geometry, bool includePieceAnalysis = true) {
    RoiResult result;
    result.anchors = geometry.anchors;
    result.frames = geometry.frames;
    result.selected = findSelected(image, result.frames);
    auto target = findTarget(image, geometry.anchors.right, result.frames);
    result.targetRect = target.first;
    result.targetMask = std::move(target.second);
    result.targetHash = fingerprintHash(result.targetMask);
    if (includePieceAnalysis) analyzePieces(image, result);
    return result;
}

RoiResult analyzeRoi(const Image& image, const Anchors& anchors, bool includePieceAnalysis = true) {
    const auto candidates = findFragmentFrames(image, anchors.left);
    const auto frames = selectEightFrames(candidates, anchors.left);
    if (!frames) throw std::runtime_error("eight colored fragment frames were not found");
    return analyzeStoredRoi(image, RoiGeometry{anchors, *frames}, includePieceAnalysis);
}

Plan makePlan(const RoiResult& result) {
    Plan plan;
    plan.labels = result.labels;
    plan.selected = result.selected;
    int lastRequiredStep = -1;
    for (int step = 0; step < 8; ++step) {
        const int row = (result.selected + step) % 8;
        if (result.labels[row] != row) lastRequiredStep = step;
    }
    for (int step = 0; step <= lastRequiredStep; ++step) {
        const int row = (result.selected + step) % 8;
        const int current = result.labels[row];
        const int target = row;
        const int rightSteps = (target - current + 8) % 8;
        const int leftSteps = (current - target + 8) % 8;
        const WORD key = rightSteps <= leftSteps ? 'D' : 'A';
        const int count = std::min(rightSteps, leftSteps);
        for (int i = 0; i < count; ++i) plan.keys.push_back(key);
        if (step != lastRequiredStep) plan.keys.push_back('S');
    }
    return plan;
}

std::wstring keyPlanText(const Plan& plan) {
    std::wostringstream out;
    for (WORD key : plan.keys) out << static_cast<wchar_t>(key) << L' ';
    return out.str();
}

constexpr int kVerificationDelayMs = 350;

struct OverlayRow {
    RECT rect{};
    int clicks = 0;
};

struct OverlayState {
    bool visible = false;
    std::array<OverlayRow, 8> rows{};
};

HWND gOverlayWindow = nullptr;
std::mutex gOverlayMutex;
OverlayState gOverlayState;

int ScaleUi(int value) {
    RECT game{};
    const int height = gta5::capture::GetGameClientRect(game) ? game.bottom - game.top : 1080;
    const double scale = std::clamp(height / 1080.0, 0.75, 2.0);
    return std::max(1, static_cast<int>(std::lround(value * scale)));
}

RECT ToScreenRect(const Image& image, const Rect& rect) {
    RECT game{};
    if (!gta5::capture::GetGameClientRect(game)) return {};
    const int gameW = game.right - game.left;
    const int gameH = game.bottom - game.top;
    const double scaleX = static_cast<double>(gameW) / std::max(1, image.w);
    const double scaleY = static_cast<double>(gameH) / std::max(1, image.h);
    return RECT{
        game.left + static_cast<int>(std::lround(rect.x * scaleX)),
        game.top + static_cast<int>(std::lround(rect.y * scaleY)),
        game.left + static_cast<int>(std::lround(rect.right() * scaleX)),
        game.top + static_cast<int>(std::lround(rect.bottom() * scaleY))};
}

void PublishOverlay(const Image& image, const RoiResult& result, bool includePlan) {
    OverlayState next;
    next.visible = true;
    for (int row = 0; row < 8; ++row) {
        next.rows[row].rect = ToScreenRect(image, result.frames[row]);
        if (includePlan) {
            const int rightClicks = (row - result.labels[row] + 8) % 8;
            const int leftClicks = (result.labels[row] - row + 8) % 8;
            next.rows[row].clicks = std::min(rightClicks, leftClicks);
        }
    }
    {
        std::lock_guard<std::mutex> lock(gOverlayMutex);
        gOverlayState = next;
    }
    if (gOverlayWindow) InvalidateRect(gOverlayWindow, nullptr, TRUE);
}

void ClearOverlayState() {
    {
        std::lock_guard<std::mutex> lock(gOverlayMutex);
        gOverlayState = {};
    }
    if (gOverlayWindow) InvalidateRect(gOverlayWindow, nullptr, TRUE);
}

void SyncOverlayWindow(bool enabled) {
    if (!gOverlayWindow) return;
    if (!enabled) {
        ShowWindow(gOverlayWindow, SW_HIDE);
        return;
    }
    RECT game{};
    if (!gta5::capture::GetGameClientRect(game)) return;
    SetWindowPos(gOverlayWindow, HWND_TOPMOST, game.left, game.top,
                 game.right - game.left, game.bottom - game.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void DrawOverlay(HDC hdc, HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    OverlayState state;
    {
        std::lock_guard<std::mutex> lock(gOverlayMutex);
        state = gOverlayState;
    }
    if (!state.visible) return;

    RECT game{};
    gta5::capture::GetGameClientRect(game);
    const int originX = game.left;
    const int originY = game.top;
    const int bracketGap = ScaleUi(10);
    const int bracketWidth = ScaleUi(12);
    const int penWidth = ScaleUi(3);
    const int tagW = ScaleUi(82);
    const int tagH = ScaleUi(27);
    const int tagGap = ScaleUi(13);

    HPEN blueGlow = CreatePen(PS_SOLID, penWidth + ScaleUi(4), RGB(0, 34, 62));
    HPEN blue = CreatePen(PS_SOLID, penWidth, RGB(24, 174, 255));
    HPEN redGlow = CreatePen(PS_SOLID, penWidth + ScaleUi(4), RGB(74, 8, 12));
    HPEN red = CreatePen(PS_SOLID, penWidth, RGB(255, 55, 65));
    HPEN tagBorder = CreatePen(PS_SOLID, ScaleUi(1), RGB(255, 84, 92));
    HBRUSH tagBrush = CreateSolidBrush(RGB(17, 22, 31));
    HGDIOBJ oldPen = SelectObject(hdc, blue);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

    auto drawBracket = [&](const RECT& r, HPEN pen) {
        SelectObject(hdc, pen);
        const int x = r.left - bracketGap;
        MoveToEx(hdc, x + bracketWidth, r.top, nullptr);
        LineTo(hdc, x, r.top);
        LineTo(hdc, x, r.bottom);
        LineTo(hdc, x + bracketWidth, r.bottom);
    };

    for (const auto& row : state.rows) {
        RECT r{row.rect.left - originX, row.rect.top - originY,
               row.rect.right - originX, row.rect.bottom - originY};
        if (row.clicks > 0) {
            drawBracket(r, redGlow);
            drawBracket(r, red);
        } else {
            drawBracket(r, blueGlow);
            drawBracket(r, blue);
        }
        if (row.clicks <= 0) continue;

        RECT tag{std::max<LONG>(ScaleUi(4), r.left - bracketGap - bracketWidth - tagGap - tagW),
                 (r.top + r.bottom - tagH) / 2, 0, 0};
        tag.right = tag.left + tagW;
        tag.bottom = tag.top + tagH;
        SelectObject(hdc, tagBorder);
        SelectObject(hdc, tagBrush);
        RoundRect(hdc, tag.left, tag.top, tag.right, tag.bottom, ScaleUi(6), ScaleUi(6));

        const std::wstring text = std::to_wstring(row.clicks) + L" clicks";
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(245, 248, 252));
        HFONT font = CreateFontW(-ScaleUi(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, font);
        DrawTextW(hdc, text.c_str(), -1, &tag, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(font);

        SelectObject(hdc, red);
        const int cy = (r.top + r.bottom) / 2;
        MoveToEx(hdc, tag.right + ScaleUi(3), cy, nullptr);
        LineTo(hdc, r.left - bracketGap - ScaleUi(3), cy);
        SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(blueGlow);
    DeleteObject(blue);
    DeleteObject(redGlow);
    DeleteObject(red);
    DeleteObject(tagBorder);
    DeleteObject(tagBrush);
}

}  // namespace

bool DetectInGame() {
    try {
        ScreenCapture capture;
        const Image screen = capture.capture();
        const auto headers = findHeaderBars(screen);
        const auto anchors = selectAnchors(headers, screen.w, screen.h);
        if (!anchors || !anchorsPresent(screen, *anchors) ||
            !isSortFingerprintAnchorLayout(*anchors, screen.h)) {
            gDetectedGeometry.reset();
            gDetectedGeneration = 0;
            return false;
        }
        RoiResult located = analyzeRoi(screen, *anchors, false);
        gDetectedGeometry = RoiGeometry{located.anchors, located.frames};
        gDetectedGeneration = screen.windowGeneration;
        return true;
    } catch (...) {
        gDetectedGeometry.reset();
        gDetectedGeneration = 0;
        return false;
    }
}

void ResetInGameCache() {
    gDetectedGeometry.reset();
    gDetectedGeneration = 0;
}

bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status,
                const std::function<void(const std::wstring&)>& log) {
    using Clock = std::chrono::steady_clock;
    constexpr auto frameInterval = std::chrono::microseconds(16667);

    ScreenCapture screenCapture;
    std::optional<RoiGeometry> storedGeometry = gDetectedGeometry;
    std::uint64_t storedGeneration = gDetectedGeneration;
    std::optional<std::uint64_t> handledHash;
    bool completedAnyRound = false;
    bool waitingForNextRound = false;
    bool verificationPending = false;
    gta5::input::Job inputJob;
    std::uint64_t inputTargetHash = 0;
    int correctionAttempt = 0;
    auto verificationDue = Clock::time_point{};
    auto nextRoundDeadline = Clock::time_point{};
    std::string lastError;
    auto lastErrorTime = Clock::time_point{};
    std::wstring lastStatus;

    auto setStatus = [&](const std::wstring& text) {
        if (text == lastStatus) return;
        lastStatus = text;
        status(text);
    };
    auto waitForNextFrame = [&](Clock::time_point frameStarted) {
        const auto deadline = frameStarted + frameInterval;
        while (!stopRequested() && Clock::now() < deadline) {
            const auto remaining = deadline - Clock::now();
            if (remaining > std::chrono::milliseconds(2)) Sleep(1);
            else std::this_thread::yield();
        }
    };
    auto beginNextRoundWait = [&](Clock::time_point now, const wchar_t* message) {
        if (waitingForNextRound) return;
        verificationPending = false;
        ClearOverlayState();
        waitingForNextRound = true;
        nextRoundDeadline = now + std::chrono::seconds(5);
        setStatus(L"sort_fingerprint: waiting next round");
        log(message);
    };
    auto resumeNextRound = [&] {
        waitingForNextRound = false;
        verificationPending = false;
        correctionAttempt = 0;
        handledHash.reset();
        setStatus(L"sort_fingerprint: analyzing next round");
        log(L"sort_fingerprint: detected next round; reusing cached ROI geometry");
    };

    setStatus(L"sort_fingerprint: locating");
    while (!stopRequested()) {
        const auto frameStarted = Clock::now();
        if (waitingForNextRound && frameStarted >= nextRoundDeadline) {
            log(L"sort_fingerprint: no next round within 5 seconds; completed");
            break;
        }
        SyncOverlayWindow(overlayEnabled());
        try {
            const auto now = Clock::now();
            const HWND foreground = GetForegroundWindow();
            const Image screen = screenCapture.capture();

            if (storedGeometry && storedGeneration != screen.windowGeneration) {
                storedGeometry.reset();
                storedGeneration = 0;
                gDetectedGeometry.reset();
                gDetectedGeneration = 0;
            }

            if (!storedGeometry) {
                const auto headers = findHeaderBars(screen);
                const auto anchors = selectAnchors(headers, screen.w, screen.h);
                if (!anchors) {
                    if (completedAnyRound) {
                        beginNextRoundWait(now,
                            L"sort_fingerprint: geometry lost after a completed round; waiting up to 5 seconds for next round");
                    }
                    waitForNextFrame(frameStarted);
                    continue;
                }
                RoiResult located = analyzeRoi(screen, *anchors, false);
                storedGeometry = RoiGeometry{located.anchors, located.frames};
                storedGeneration = screen.windowGeneration;
                PublishOverlay(screen, located, false);
                if (waitingForNextRound) {
                    resumeNextRound();
                } else {
                    setStatus(L"sort_fingerprint: analyzing");
                    log(L"sort_fingerprint: detected first round; cached ROI geometry");
                }
            } else if (!anchorsPresent(screen, storedGeometry->anchors)) {
                if (!waitingForNextRound) {
                    const auto headers = findHeaderBars(screen);
                    const auto relocatedAnchors = selectAnchors(headers, screen.w, screen.h);
                    if (relocatedAnchors && anchorsPresent(screen, *relocatedAnchors) &&
                        isSortFingerprintAnchorLayout(*relocatedAnchors, screen.h)) {
                        RoiResult relocated = analyzeRoi(screen, *relocatedAnchors, false);
                        storedGeometry = RoiGeometry{relocated.anchors, relocated.frames};
                        storedGeneration = screen.windowGeneration;
                        PublishOverlay(screen, relocated, false);
                        log(L"sort_fingerprint: cached anchors failed; relocated in full frame");
                    } else {
                        beginNextRoundWait(now,
                            L"sort_fingerprint: minigame left; waiting up to 5 seconds for next round");
                    }
                }

                if (waitingForNextRound) {
                    if (inputJob && !inputJob.Pending()) {
                        if (inputJob.Succeeded()) {
                            handledHash = inputTargetHash;
                            completedAnyRound = true;
                        } else {
                            handledHash.reset();
                            correctionAttempt = 0;
                            log(L"sort_fingerprint: foreground changed or input failed while waiting");
                        }
                        inputJob = {};
                    }
                    if (now >= nextRoundDeadline) {
                        log(L"sort_fingerprint: no next round within 5 seconds; completed");
                        break;
                    }
                    waitForNextFrame(frameStarted);
                    continue;
                }
            } else {
                if (waitingForNextRound) {
                    resumeNextRound();
                }
            }

            if (inputJob) {
                if (inputJob.Pending()) {
                    waitForNextFrame(frameStarted);
                    continue;
                }
                if (inputJob.Succeeded()) {
                    handledHash = inputTargetHash;
                    completedAnyRound = true;
                    verificationPending = true;
                    verificationDue = Clock::now() + std::chrono::milliseconds(kVerificationDelayMs);
                    setStatus(L"sort_fingerprint: verifying input");
                } else {
                    handledHash.reset();
                    verificationPending = false;
                    correctionAttempt = 0;
                    log(L"sort_fingerprint: foreground changed or input failed; retrying analysis");
                }
                inputJob = {};
                waitForNextFrame(frameStarted);
                continue;
            }

            if (handledHash && (!verificationPending || Clock::now() < verificationDue)) {
                waitForNextFrame(frameStarted);
                continue;
            }

            RoiResult result = analyzeStoredRoi(screen, *storedGeometry, false);
            const bool sameHash = handledHash && hammingDistance(*handledHash, result.targetHash) <= 3;
            bool isVerification = false;
            if (sameHash) {
                if (!verificationPending || Clock::now() < verificationDue) {
                    waitForNextFrame(frameStarted);
                    continue;
                }
                verificationPending = false;
                isVerification = true;
                ++correctionAttempt;
            } else if (handledHash) {
                verificationPending = false;
                correctionAttempt = 0;
                handledHash.reset();
                log(L"sort_fingerprint: fingerprint changed without a visible gap; treating it as a new round");
            }

            analyzePieces(screen, result);
            const Plan plan = makePlan(result);
            PublishOverlay(screen, result, true);
            std::wostringstream planLine;
            planLine << L"sort_fingerprint: " << (isVerification ? L"correction" : L"initial")
                     << L" plan, selected row " << result.selected + 1 << L", keys: " << keyPlanText(plan);
            log(planLine.str());

            if (plan.keys.empty()) {
                handledHash = result.targetHash;
                completedAnyRound = true;
                verificationPending = false;
                setStatus(L"sort_fingerprint: round complete");
            } else {
                std::vector<gta5::input::Key> keys;
                keys.reserve(plan.keys.size());
                for (WORD key : plan.keys) keys.push_back(gta5::input::Key::FromVirtualKey(key));
                inputTargetHash = result.targetHash;
                inputJob = gta5::input::QueueSequence(keys, foreground);
                setStatus(L"sort_fingerprint: verifying input");
            }
        } catch (const std::exception& error) {
            storedGeometry.reset();
            storedGeneration = 0;
            gDetectedGeometry.reset();
            gDetectedGeneration = 0;
            const std::string what = error.what();
            const auto now = Clock::now();
            if (completedAnyRound) {
                beginNextRoundWait(now,
                    L"sort_fingerprint: analysis lost after a completed round; waiting up to 5 seconds for next round");
            }
            if (what != lastError || now - lastErrorTime >= std::chrono::seconds(2)) {
                std::wstring message = L"sort_fingerprint: ROI not stable: ";
                message.append(what.begin(), what.end());
                log(message);
                lastError = what;
                lastErrorTime = now;
            }
        }
        waitForNextFrame(frameStarted);
    }

    gta5::input::CancelAll();
    ClearOverlay();
    ResetInGameCache();
    return completedAnyRound;
}

void SetOverlayWindow(HWND hwnd) {
    gOverlayWindow = hwnd;
}

void ClearOverlay() {
    ClearOverlayState();
    if (gOverlayWindow) ShowWindow(gOverlayWindow, SW_HIDE);
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawOverlay(hdc, hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace gta5::games::sort_fingerprint
