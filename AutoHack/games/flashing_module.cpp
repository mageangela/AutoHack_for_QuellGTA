#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#include "games.h"
#include "../capture/game_window.h"
#include "../input/key_input.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <sstream>
#include <string>
#include <vector>
#include <functional>

namespace gta5::games::flashing {

static const int GRID_ROWS = 5;
static const int GRID_COLS = 6;
static const int GRID_CELLS = GRID_ROWS * GRID_COLS;
static const UINT TIMER_ID = 1001;
static const UINT TIMER_MS = 16;
static const int kSelectedStableFramesBeforeInput = 2;
static const int kLevelFocusHideAfterMissFrames = 5;
struct Pattern {
    std::array<int, GRID_CELLS> on{};

    bool any() const {
        for (int v : on) {
            if (v) return true;
        }
        return false;
    }

    bool equals(const Pattern& other) const {
        return on == other.on;
    }
};

struct PatternTracker {
    Pattern lastFlash{};
    Pattern target{};
    bool hasLastFlash = false;
    bool targetReady = false;
    int repeatCount = 0;
    int flashCount = 0;
};

struct ScreenShot {
    int w = 0;
    int h = 0;
    int screenW = 0;
    int screenH = 0;
    int screenX = 0;
    int screenY = 0;
    double toScreenX = 1.0;
    double toScreenY = 1.0;
    std::uint64_t windowGeneration = 0;
    std::vector<uint8_t> pixels;
};

struct WhiteBar {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int score = 0;
};

struct MinigameGeometry {
    bool valid = false;
    std::uint64_t windowGeneration = 0;
    int frameW = 0;
    int frameH = 0;
    WhiteBar signal;
    WhiteBar rightTitle;
    WhiteBar topTitle;
};

struct Circle {
    int x = 0;
    int y = 0;
    int r = 0;
    int score = 0;
};

struct UiRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct HLine {
    int x1 = 0;
    int y = 0;
    int x2 = 0;
};

static int ToScreenX(const ScreenShot& shot, int x) {
    return shot.screenX + static_cast<int>(std::lround(x * shot.toScreenX));
}

static int ToScreenY(const ScreenShot& shot, int y) {
    return shot.screenY + static_cast<int>(std::lround(y * shot.toScreenY));
}

static int ToShotX(const ScreenShot& shot, int x) {
    return static_cast<int>(std::lround((x - shot.screenX) / std::max(0.0001, shot.toScreenX)));
}

static int ToShotY(const ScreenShot& shot, int y) {
    return static_cast<int>(std::lround((y - shot.screenY) / std::max(0.0001, shot.toScreenY)));
}

static std::vector<WhiteBar> FindWhiteBars(const ScreenShot& shot);
static bool FindSignalRepeaterBar(const ScreenShot& shot, const std::vector<WhiteBar>& bars, WhiteBar& signal);

struct AppState {
    HWND mainWnd = nullptr;
    HWND overlayWnd = nullptr;
    HWND btnStart = nullptr;
    HWND editThresh = nullptr;
    HWND status = nullptr;
    HWND roiLabel = nullptr;

    int roiX = 430;
    int roiY = 285;
    int roiW = 680;
    int roiH = 540;
    int searchX = 372;
    int searchY = 215;
    int searchSize = 792;
    std::array<Circle, GRID_CELLS> circles{};
    bool circlesReady = false;
    int thresholdPct = 8;
    bool running = false;
    int frameCount = 0;
    int detectedCount = 0;
    int blankFrames = 0;
    int selectedIndex = -1;
    int selectedScore = 0;
    int lastSelectedIndex = -1;
    int selectedStableFrames = 0;
    int levelSlot = 0;
    int observedLevelSlot = 0;
    int observedLevelStableFrames = 0;
    bool levelFocusVisible = false;
    bool levelFocusDetectedThisFrame = false;
    int levelFocusMissFrames = 0;
    UiRect levelFocus{};
    DWORD64 nextInputAt = 0;
    gta5::input::Job inputJob;
    bool autoInputEnabled = false;
    int pendingVerifyCol = -1;
    DWORD64 finalSubmitAt = 0;
    bool minigameVisible = false;
    MinigameGeometry minigameGeometry;
    std::wstring autoMessage = L"Auto idle";

    Pattern current{};
    Pattern answerPath{};
    Pattern flashUnion{};
    PatternTracker tracker{};
    std::wstring message = L"Idle";
};

static AppState g;

static std::wstring PatternToText(const Pattern& p) {
    std::wstringstream ss;
    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            ss << (p.on[r * GRID_COLS + c] ? L'#' : L'.');
        }
        if (r != GRID_ROWS - 1) ss << L"\r\n";
    }
    return ss.str();
}

static int GetEditInt(HWND h, int fallback) {
    wchar_t buf[64]{};
    GetWindowTextW(h, buf, 63);
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    return static_cast<int>(v);
}

static bool CaptureScreen(ScreenShot& shot) {
    gta5::capture::GameFrame captured;
    if (!gta5::capture::CaptureGameFrame(captured)) return false;
    shot.screenX = captured.screenX;
    shot.screenY = captured.screenY;
    shot.screenW = captured.screenW;
    shot.screenH = captured.screenH;
    shot.w = captured.width;
    shot.h = captured.height;
    shot.toScreenX = captured.toScreenX;
    shot.toScreenY = captured.toScreenY;
    shot.windowGeneration = captured.windowGeneration;
    shot.pixels.resize(captured.bgra.size() * sizeof(uint32_t));
    memcpy(shot.pixels.data(), captured.bgra.data(), shot.pixels.size());
    return true;
}

static bool IsWhiteUiPixel(uint8_t r, uint8_t gch, uint8_t b) {
    return r > 175 && gch > 175 && b > 175 && std::abs(r - gch) < 55 && std::abs(r - b) < 55;
}

static bool IsLevelLinePixel(uint8_t r, uint8_t gch, uint8_t b) {
    int gray = (static_cast<int>(r) * 30 + static_cast<int>(gch) * 59 + static_cast<int>(b) * 11) / 100;
    bool cyan = gch > 120 && b > 120 && r < 110 && (gch - r) > 25 && (b - r) > 25;
    return gray >= 140 && !cyan;
}

static bool IsBluePixel(uint8_t r, uint8_t gch, uint8_t b);

static const uint8_t* PixelAt(const ScreenShot& shot, int x, int y) {
    if (x < 0 || y < 0 || x >= shot.w || y >= shot.h) return nullptr;
    return shot.pixels.data() + (static_cast<size_t>(y) * shot.w + x) * 4;
}

static int ScalePx(int px, int screenH) {
    return std::max(1, static_cast<int>(std::round(px * (static_cast<double>(screenH) / 1080.0))));
}

static int GrayAt(const uint8_t* p) {
    return (static_cast<int>(p[2]) * 30 + static_cast<int>(p[1]) * 59 + static_cast<int>(p[0]) * 11) / 100;
}

static int CircleEdgePixelScore(const ScreenShot& shot, int x, int y) {
    const uint8_t* p = PixelAt(shot, x, y);
    if (!p) return 0;

    int r = p[2];
    int gch = p[1];
    int b = p[0];
    int lum = (r + gch + b) / 3;
    int hi = std::max(r, std::max(gch, b));
    int lo = std::min(r, std::min(gch, b));
    int sat = hi - lo;

    if (r > 125 && gch > 125 && b > 125 && sat < 80) return 3;
    if (r > 70 && r > gch + 15 && r > b + 15) return 2;
    if (lum > 72 && sat < 70) return 1;
    return 0;
}

static int CircleEdgeScore(const ScreenShot& shot, int cx, int cy, int radius) {
    static const double PI = 3.14159265358979323846;
    static const int samples = 128;
    struct UnitPoint { double x = 0.0, y = 0.0; };
    static const std::array<UnitPoint, samples> unit = [] {
        std::array<UnitPoint, samples> out{};
        for (int i = 0; i < samples; ++i) {
            double a = 2.0 * PI * i / samples;
            out[i] = {std::cos(a), std::sin(a)};
        }
        return out;
    }();
    int hits = 0;
    int quadrants[4]{};

    for (int i = 0; i < samples; ++i) {
        int best = 0;
        for (int rr = radius - 1; rr <= radius + 1; ++rr) {
            int x = cx + static_cast<int>(std::round(rr * unit[i].x));
            int y = cy + static_cast<int>(std::round(rr * unit[i].y));
            best = std::max(best, CircleEdgePixelScore(shot, x, y));
        }
        if (best > 0) {
            hits += best;
            ++quadrants[i * 4 / samples];
        }
    }

    int minQuadrant = std::min(std::min(quadrants[0], quadrants[1]), std::min(quadrants[2], quadrants[3]));
    return hits + minQuadrant * 8;
}

static Circle RefineCircleByEdge(const ScreenShot& shot, int seedX, int seedY, int seedR, int maxDx, int maxDy, int minR, int maxR) {
    Circle best{ seedX, seedY, seedR, -1 };
    minR = std::max(ScalePx(26, shot.h), minR);
    maxR = std::min(ScalePx(70, shot.h), maxR);
    if (minR > maxR) std::swap(minR, maxR);

    for (int y = seedY - maxDy; y <= seedY + maxDy; y += 2) {
        for (int x = seedX - maxDx; x <= seedX + maxDx; x += 2) {
            for (int r = minR; r <= maxR; r += 2) {
                int score = CircleEdgeScore(shot, x, y, r);
                if (score > best.score) {
                    best = { x, y, r, score };
                }
            }
        }
    }
    return best;
}

static bool AcceptCircleTriplet(const Circle& topLeft, const Circle& rightNeighbor, const Circle& down,
                                int seedR, double cellW, double cellH, int minScore) {
    if (topLeft.score < minScore || rightNeighbor.score < minScore || down.score < minScore) return false;

    int dx = rightNeighbor.x - topLeft.x;
    int dy = down.y - topLeft.y;
    if (dx < seedR || dy < seedR) return false;
    if (dx < cellW * 0.72 || dx > cellW * 1.28) return false;
    if (dy < cellH * 0.72 || dy > cellH * 1.28) return false;
    if (std::abs(rightNeighbor.y - topLeft.y) > cellH * 0.18) return false;
    if (std::abs(down.x - topLeft.x) > cellW * 0.18) return false;
    return true;
}

static void CommitCircleGrid(const ScreenShot& shot, const Circle& topLeft, const Circle& rightNeighbor, const Circle& down) {
    int dx = rightNeighbor.x - topLeft.x;
    int dy = down.y - topLeft.y;
    std::vector<int> radii{ topLeft.r, rightNeighbor.r, down.r };
    std::sort(radii.begin(), radii.end());
    int radius = radii[1];

    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            g.circles[r * GRID_COLS + c] = {
                topLeft.x + c * dx,
                topLeft.y + r * dy,
                radius,
                999
            };
        }
    }

    int left = shot.w, top = shot.h, right = 0, bottom = 0;
    int pad = ScalePx(24, shot.h);
    for (const Circle& circle : g.circles) {
        left = std::min(left, circle.x - circle.r - pad);
        top = std::min(top, circle.y - circle.r - pad);
        right = std::max(right, circle.x + circle.r + pad);
        bottom = std::max(bottom, circle.y + circle.r + pad);
    }
    g.roiX = std::max(0, left);
    g.roiY = std::max(0, top);
    g.roiW = std::min(shot.w - 1, right) - g.roiX;
    g.roiH = std::min(shot.h - 1, bottom) - g.roiY;
    g.circlesReady = true;
}

static void ScaleLockedGeometryToScreen(const ScreenShot& shot) {
    int roiRight = ToScreenX(shot, g.roiX + g.roiW);
    int roiBottom = ToScreenY(shot, g.roiY + g.roiH);
    g.roiX = ToScreenX(shot, g.roiX);
    g.roiY = ToScreenY(shot, g.roiY);
    g.roiW = std::max(1, roiRight - g.roiX);
    g.roiH = std::max(1, roiBottom - g.roiY);

    g.searchX = ToScreenX(shot, g.searchX);
    g.searchY = ToScreenY(shot, g.searchY);
    g.searchSize = std::max(1, static_cast<int>(std::lround(g.searchSize * shot.toScreenX)));

    for (Circle& circle : g.circles) {
        circle.x = ToScreenX(shot, circle.x);
        circle.y = ToScreenY(shot, circle.y);
        circle.r = std::max(1, static_cast<int>(std::lround(circle.r * (shot.toScreenX + shot.toScreenY) * 0.5)));
    }
}

static bool DetectCircles(const ScreenShot& shot, int seedRoiX, int seedRoiY, int seedRoiW, int seedRoiH) {
    double cellW = static_cast<double>(seedRoiW) / GRID_COLS;
    double cellH = static_cast<double>(seedRoiH) / GRID_ROWS;
    int seedR = static_cast<int>(std::round(std::min(cellW, cellH) * 0.38));
    int maxDx = std::max(ScalePx(16, shot.h), static_cast<int>(std::round(cellW * 0.20)));
    int maxDy = std::max(ScalePx(16, shot.h), static_cast<int>(std::round(cellH * 0.20)));
    int radiusSlack = ScalePx(16, shot.h);

    int x0Seed = seedRoiX + static_cast<int>(std::round(0.5 * cellW));
    int y0Seed = seedRoiY + static_cast<int>(std::round(0.5 * cellH));
    Circle topLeft = RefineCircleByEdge(shot, x0Seed, y0Seed, seedR, maxDx, maxDy, seedR - radiusSlack, seedR + radiusSlack);
    int edgeMinR = topLeft.r - std::max(ScalePx(3, shot.h), radiusSlack / 2);
    int edgeMaxR = topLeft.r + std::max(ScalePx(3, shot.h), radiusSlack / 2);
    int edgeRightSeedX = topLeft.x + static_cast<int>(std::round(cellW));
    int edgeDownSeedY = topLeft.y + static_cast<int>(std::round(cellH));
    Circle rightNeighbor = RefineCircleByEdge(shot, edgeRightSeedX, topLeft.y, topLeft.r, maxDx, std::max(ScalePx(4, shot.h), maxDy / 2), edgeMinR, edgeMaxR);
    Circle down = RefineCircleByEdge(shot, topLeft.x, edgeDownSeedY, topLeft.r, std::max(ScalePx(4, shot.h), maxDx / 2), maxDy, edgeMinR, edgeMaxR);

    if (!AcceptCircleTriplet(topLeft, rightNeighbor, down, seedR, cellW, cellH, 70)) {
        g.circlesReady = false;
        return false;
    }

    CommitCircleGrid(shot, topLeft, rightNeighbor, down);
    return true;
}

static bool AutoLocateRoiFromShot(const ScreenShot& shot) {
    const int screenW = shot.w;
    const int screenH = shot.h;
    WhiteBar best{};
    if (g.minigameGeometry.valid &&
        g.minigameGeometry.windowGeneration == shot.windowGeneration &&
        g.minigameGeometry.frameW == shot.w && g.minigameGeometry.frameH == shot.h) {
        best = g.minigameGeometry.signal;
    } else {
        std::vector<WhiteBar> bars = FindWhiteBars(shot);
        if (!FindSignalRepeaterBar(shot, bars, best)) {
            g.message = L"Auto ROI failed";
            return false;
        }
    }
    if (best.w <= 0 || best.h <= 0) {
        g.message = L"Auto ROI failed";
        return false;
    }

    int searchPad = ScalePx(80, screenH);
    int searchLeft = std::max(0, best.x - searchPad);
    int searchRight = std::min(screenW - 1, best.x + best.w + searchPad);
    std::vector<int> active(searchRight - searchLeft + 1, 0);
    int activeHeightThreshold = std::max(2, best.h / 6);
    for (int xx = searchLeft; xx <= searchRight; ++xx) {
        int count = 0;
        for (int yy = best.y; yy < std::min(best.y + best.h, screenH); ++yy) {
            const uint8_t* p = shot.pixels.data() + (static_cast<size_t>(yy) * screenW + xx) * 4;
            if (IsWhiteUiPixel(p[2], p[1], p[0])) ++count;
        }
        active[xx - searchLeft] = count >= activeHeightThreshold ? 1 : 0;
    }

    int runStart = searchLeft;
    int runEnd = searchRight;
    int curStart = -1;
    int gap = 0;
    int bestLen = 0;
    for (int i = 0; i < static_cast<int>(active.size()); ++i) {
        int x = searchLeft + i;
        if (active[i]) {
            if (curStart < 0) curStart = x;
            gap = 0;
        } else if (curStart >= 0) {
            ++gap;
            if (gap > ScalePx(24, screenH)) {
                int end = x - gap;
                int len = end - curStart;
                if (len > bestLen) {
                    bestLen = len;
                    runStart = curStart;
                    runEnd = end;
                }
                curStart = -1;
                gap = 0;
            }
        }
    }
    if (curStart >= 0 && searchRight - curStart > bestLen) {
        runStart = curStart;
        runEnd = searchRight;
        bestLen = runEnd - runStart;
    }

    int refinedTitleW = std::max(ScalePx(620, screenH), std::min(ScalePx(920, screenH), bestLen));
    int titleX = std::max(0, runStart);
    int titleY = best.y;
    g.searchX = titleX;
    g.searchY = titleY;
    g.searchSize = std::min(refinedTitleW, std::min(screenW - g.searchX, screenH - g.searchY));

    int roiW = static_cast<int>(refinedTitleW * 0.86);
    roiW = std::max(ScalePx(520, screenH), std::min(ScalePx(760, screenH), roiW));
    int roiH = static_cast<int>(roiW * 0.795);
    int roiX = titleX + static_cast<int>(refinedTitleW * 0.075);
    int roiY = titleY + static_cast<int>(roiW * 0.102);

    if (roiX + roiW > screenW) roiW = screenW - roiX;
    if (roiY + roiH > screenH) roiH = screenH - roiY;
    if (roiW < ScalePx(300, screenH) || roiH < ScalePx(250, screenH)) return false;

    g.circlesReady = false;
    if (!DetectCircles(shot, roiX, roiY, roiW, roiH)) {
        g.roiX = roiX;
        g.roiY = roiY;
        g.roiW = roiW;
        g.roiH = roiH;
        ScaleLockedGeometryToScreen(shot);
        g.message = L"Circle detect failed";
        return false;
    }

    ScaleLockedGeometryToScreen(shot);
    g.message = L"Circles locked";
    return true;
}

static bool AutoLocateRoi() {
    ScreenShot shot;
    return CaptureScreen(shot) && AutoLocateRoiFromShot(shot);
}

static void SetEditInt(HWND h, int value) {
    wchar_t buf[32]{};
    wsprintfW(buf, L"%d", value);
    SetWindowTextW(h, buf);
}

static bool IsBluePixel(uint8_t r, uint8_t gch, uint8_t b) {
    bool cyan = gch > 95 && b > 105 && r < 115 && (gch - r) > 28 && (b - r) > 28;
    bool brightCyan = gch > 145 && b > 145 && r < 150 && (gch + b) > (r * 2 + 80);
    return cyan || brightCyan;
}

static std::vector<WhiteBar> FindWhiteBars(const ScreenShot& shot) {
    const int screenW = shot.w;
    const int screenH = shot.h;
    const int yMin = std::max(ScalePx(80, screenH), screenH / 10);
    const int yMax = std::min(screenH / 2, screenH - ScalePx(260, screenH));
    const int minRun = ScalePx(274, screenH);
    const int maxGap = ScalePx(18, screenH);
    const int barPadTop = ScalePx(5, screenH);
    const int barPadBottom = ScalePx(18, screenH);
    const int barHeight = ScalePx(24, screenH);
    const int densityStep = ScalePx(6, screenH);
    std::vector<WhiteBar> bars;

    for (int y = yMin; y < yMax; y += 2) {
        bool inRun = false;
        int runStart = 0;
        int gap = 0;

        for (int x = 0; x < screenW; x += 2) {
            const uint8_t* p = shot.pixels.data() + (static_cast<size_t>(y) * screenW + x) * 4;
            bool white = IsWhiteUiPixel(p[2], p[1], p[0]);
            if (white) {
                if (!inRun) {
                    inRun = true;
                    runStart = x;
                }
                gap = 0;
            } else if (inRun) {
                gap += 2;
                if (gap > maxGap) {
                    int runEnd = x - gap;
                    int runW = runEnd - runStart;
                    if (runW >= minRun) {
                        int density = 0;
                        int samples = 0;
                        for (int yy = std::max(y - barPadTop, 0); yy <= std::min(y + barPadBottom, screenH - 1); yy += 2) {
                            const uint8_t* row = shot.pixels.data() + static_cast<size_t>(yy) * screenW * 4;
                            for (int xx = runStart; xx <= runEnd; xx += densityStep) {
                                const uint8_t* q = row + xx * 4;
                                if (IsWhiteUiPixel(q[2], q[1], q[0])) ++density;
                                ++samples;
                            }
                        }
                        bars.push_back({ runStart, y - barPadTop, runW, barHeight, samples ? density * 100 / samples : 0 });
                    }
                    inRun = false;
                    gap = 0;
                }
            }
        }
    }

    return bars;
}

static bool FindSignalRepeaterBar(const ScreenShot& shot, const std::vector<WhiteBar>& bars, WhiteBar& signal) {
    const int screenH = shot.h;
    int bestRank = -1000000;

    for (const WhiteBar& b : bars) {
        bool belowTopBoxes = b.y > screenH * 16 / 100;
        bool notTooLow = b.y < screenH * 34 / 100;
        bool wideEnough = b.w > ScalePx(537, screenH);
        if (!belowTopBoxes || !notTooLow || !wideEnough || b.score < 25) continue;

        int rank = b.w * 3 + b.y * 6;
        if (rank > bestRank) {
            bestRank = rank;
            signal = b;
        }
    }

    return bestRank != -1000000;
}

static bool AnalyzeMinigamePage(const ScreenShot& shot, MinigameGeometry* geometryOut = nullptr) {
    std::vector<WhiteBar> bars = FindWhiteBars(shot);
    WhiteBar signal{};
    if (!FindSignalRepeaterBar(shot, bars, signal)) return false;

    int titleYSlack = ScalePx(35, shot.h);
    bool hasRightTitle = false;
    int topTitleCount = 0;
    WhiteBar rightTitle{};
    WhiteBar topTitle{};
    for (const WhiteBar& b : bars) {
        int centerY = b.y + b.h / 2;

        if (std::abs(centerY - (signal.y + signal.h / 2)) < titleYSlack &&
            b.x > signal.x + signal.w * 3 / 4 &&
            b.w > ScalePx(268, shot.h) &&
            b.score >= 28) {
            hasRightTitle = true;
            if (b.score > rightTitle.score) rightTitle = b;
        }

        if (centerY < signal.y - titleYSlack &&
            centerY > shot.h * 8 / 100 &&
            centerY < shot.h * 24 / 100 &&
            b.w > ScalePx(345, shot.h) &&
            b.score >= 28) {
            ++topTitleCount;
            if (b.score > topTitle.score) topTitle = b;
        }
    }

    if (!hasRightTitle || topTitleCount < 1) return false;
    if (geometryOut) {
        geometryOut->valid = true;
        geometryOut->windowGeneration = shot.windowGeneration;
        geometryOut->frameW = shot.w;
        geometryOut->frameH = shot.h;
        geometryOut->signal = signal;
        geometryOut->rightTitle = rightTitle;
        geometryOut->topTitle = topTitle;
    }
    return true;
}

static bool ValidateMinigameGeometry(const ScreenShot& shot, const MinigameGeometry& geometry) {
    if (!geometry.valid || geometry.windowGeneration != shot.windowGeneration ||
        geometry.frameW != shot.w || geometry.frameH != shot.h) return false;
    auto barPresent = [&](const WhiteBar& bar) {
        const int left = std::clamp(bar.x, 0, shot.w);
        const int top = std::clamp(bar.y, 0, shot.h);
        const int right = std::clamp(bar.x + bar.w, 0, shot.w);
        const int bottom = std::clamp(bar.y + bar.h, 0, shot.h);
        if (right <= left || bottom <= top) return false;
        const int step = std::max(1, std::min(right - left, bottom - top) / 10);
        int white = 0;
        int samples = 0;
        for (int y = top; y < bottom; y += step) {
            for (int x = left; x < right; x += step) {
                const uint8_t* p = shot.pixels.data() + (static_cast<size_t>(y) * shot.w + x) * 4;
                white += IsWhiteUiPixel(p[2], p[1], p[0]) ? 1 : 0;
                ++samples;
            }
        }
        return samples > 0 && white * 100 >= samples * 2;
    };
    return barPresent(geometry.signal) && barPresent(geometry.rightTitle) &&
           barPresent(geometry.topTitle);
}

static bool CheckMinigamePage(const ScreenShot* shot) {
    if (!shot) {
        g.minigameGeometry = {};
        g.circlesReady = false;
        return false;
    }
    if (ValidateMinigameGeometry(*shot, g.minigameGeometry)) return true;
    MinigameGeometry relocated;
    if (!AnalyzeMinigamePage(*shot, &relocated)) {
        g.minigameGeometry = {};
        g.circlesReady = false;
        return false;
    }
    g.minigameGeometry = relocated;
    g.circlesReady = AutoLocateRoiFromShot(*shot);
    return true;
}

static bool IsMinigamePageVisible() {
    ScreenShot shot;
    return CaptureScreen(shot) && CheckMinigamePage(&shot);
}

static bool FindDecodedDigitsBar(const ScreenShot& shot, const std::vector<WhiteBar>& bars,
                                 const WhiteBar& signal, WhiteBar& decoded) {
    int titleYSlack = ScalePx(35, shot.h);
    int signalCenterY = signal.y + signal.h / 2;
    int bestRank = -1000000;

    for (const WhiteBar& b : bars) {
        int centerX = b.x + b.w / 2;
        int centerY = b.y + b.h / 2;
        if (std::abs(centerY - signalCenterY) > titleYSlack) continue;
        if (b.x <= signal.x + signal.w * 3 / 4) continue;
        if (b.w < signal.w * 25 / 100 || b.w > signal.w * 70 / 100) continue;
        if (b.score < 28) continue;

        int rank = b.w * 4 - std::abs(centerY - signalCenterY) * 16 + centerX / 8;
        if (rank > bestRank) {
            bestRank = rank;
            decoded = b;
        }
    }

    return bestRank != -1000000;
}

static std::vector<HLine> FindHorizontalRuns(const ScreenShot& shot, const UiRect& region,
                                             int minLen, int maxLen, int maxGap) {
    std::vector<HLine> lines;
    int left = std::max(0, std::min(region.left, shot.w - 1));
    int top = std::max(0, std::min(region.top, shot.h - 1));
    int right = std::max(left, std::min(region.right, shot.w - 1));
    int bottom = std::max(top, std::min(region.bottom, shot.h - 1));
    maxGap = std::max(1, maxGap);

    for (int y = top; y <= bottom; ++y) {
        bool inRun = false;
        int runStart = left;
        int lastWhite = left;
        int gap = 0;
        for (int x = left; x <= right; ++x) {
            const uint8_t* p = PixelAt(shot, x, y);
            bool white = p && IsLevelLinePixel(p[2], p[1], p[0]);
            if (white) {
                if (!inRun) {
                    inRun = true;
                    runStart = x;
                }
                lastWhite = x;
                gap = 0;
            } else if (inRun) {
                ++gap;
                if (gap > maxGap) {
                    int len = lastWhite - runStart + 1;
                    if (len >= minLen && len <= maxLen) {
                        lines.push_back({ runStart, y, lastWhite });
                    }
                    inRun = false;
                    gap = 0;
                }
            }
        }
        if (inRun) {
            int len = lastWhite - runStart + 1;
            if (len >= minLen && len <= maxLen) {
                lines.push_back({ runStart, y, lastWhite });
            }
        }
    }
    return lines;
}

static bool FindLevelBlock(const ScreenShot& shot, UiRect& block) {
    const int scaleH = shot.h;
    if (!g.circlesReady) return false;

    std::vector<WhiteBar> bars = FindWhiteBars(shot);
    WhiteBar signal{};
    WhiteBar decoded{};
    if (!FindSignalRepeaterBar(shot, bars, signal)) return false;
    if (!FindDecodedDigitsBar(shot, bars, signal, decoded)) return false;

    int bottom = 0;
    std::vector<int> centers;
    std::vector<int> radii;
    for (int row = 0; row < GRID_ROWS; ++row) {
        const Circle& circle = g.circles[row * GRID_COLS + (GRID_COLS - 1)];
        int cy = ToShotY(shot, circle.y);
        int r = std::max(1, static_cast<int>(std::lround(circle.r / std::max(0.0001, shot.toScreenY))));
        centers.push_back(cy);
        radii.push_back(r);
        bottom = std::max(bottom, cy + r);
    }
    std::sort(centers.begin(), centers.end());
    std::sort(radii.begin(), radii.end());
    int radius = radii[radii.size() / 2];
    UiRect search{
        std::max(0, decoded.x - ScalePx(8, scaleH)),
        std::max(0, centers.back() - radius),
        std::min(shot.w - 1, decoded.x + decoded.w + ScalePx(8, scaleH)),
        std::min(shot.h - 1, bottom + ScalePx(150, scaleH))
    };
    if (search.right <= search.left || search.bottom <= search.top) return false;

    int minBlockW = std::max(ScalePx(240, scaleH), decoded.w * 70 / 100);
    int maxBlockW = decoded.w + ScalePx(36, scaleH);
    std::vector<HLine> lines = FindHorizontalRuns(
        shot,
        search,
        minBlockW,
        maxBlockW,
        ScalePx(20, scaleH));

    int bestScore = -1;
    UiRect best{};
    for (const HLine& top : lines) {
        for (const HLine& bottom : lines) {
            if (bottom.y <= top.y) continue;
            int h = bottom.y - top.y;
            if (h < ScalePx(90, scaleH) || h > ScalePx(150, scaleH)) continue;

            int left = std::min(top.x1, bottom.x1);
            int right = std::max(top.x2, bottom.x2);
            int w = right - left + 1;
            if (w < minBlockW || w > maxBlockW) continue;
            if (std::abs(top.x1 - bottom.x1) > ScalePx(28, scaleH)) continue;
            if (std::abs(top.x2 - bottom.x2) > ScalePx(28, scaleH)) continue;

            int decodedCenterX = decoded.x + decoded.w / 2;
            int score = w * h - std::abs((left + right) / 2 - decodedCenterX);
            if (score > bestScore) {
                bestScore = score;
                best = { left, top.y, right, bottom.y };
            }
        }
    }

    if (bestScore < 0) return false;
    block = best;
    return true;
}

static bool FindLevelFocusFrame(const ScreenShot& shot, const UiRect& block, UiRect& focus) {
    const int scaleH = shot.h;
    UiRect inner{
        block.left + ScalePx(8, scaleH),
        block.top + ScalePx(8, scaleH),
        block.right - ScalePx(8, scaleH),
        block.bottom - ScalePx(18, scaleH)
    };
    std::vector<HLine> lines = FindHorizontalRuns(
        shot,
        inner,
        ScalePx(54, scaleH),
        ScalePx(90, scaleH),
        ScalePx(6, scaleH));

    int bestScore = -1;
    UiRect best{};
    for (const HLine& top : lines) {
        for (const HLine& bottom : lines) {
            if (bottom.y <= top.y) continue;
            int h = bottom.y - top.y + 1;
            if (h < ScalePx(50, scaleH) || h > ScalePx(86, scaleH)) continue;
            int wTop = top.x2 - top.x1 + 1;
            int wBottom = bottom.x2 - bottom.x1 + 1;
            if (std::abs(top.x1 - bottom.x1) > ScalePx(12, scaleH)) continue;
            if (std::abs(top.x2 - bottom.x2) > ScalePx(12, scaleH)) continue;

            int left = std::min(top.x1, bottom.x1);
            int right = std::max(top.x2, bottom.x2);
            int w = right - left + 1;
            if (std::abs(w - h) > ScalePx(18, scaleH)) continue;

            int score = w * h + std::min(wTop, wBottom) * 8;
            if (score > bestScore) {
                bestScore = score;
                best = { left, top.y, right, bottom.y };
            }
        }
    }

    if (bestScore < 0) return false;
    focus = best;
    return true;
}

static int DetectLevelSlotFromShot(const ScreenShot& shot, UiRect* focusScreen = nullptr) {
    UiRect block{};
    UiRect focus{};
    if (!FindLevelBlock(shot, block)) return 0;
    if (!FindLevelFocusFrame(shot, block, focus)) return 0;

    if (focusScreen) {
        *focusScreen = {
            ToScreenX(shot, focus.left),
            ToScreenY(shot, focus.top),
            ToScreenX(shot, focus.right),
            ToScreenY(shot, focus.bottom)
        };
    }

    double blockW = std::max(1, block.right - block.left + 1);
    double focusCenterX = (focus.left + focus.right) * 0.5;
    double ratio = (focusCenterX - block.left) / blockW;
    int slot = static_cast<int>(std::floor(ratio * 4.0)) + 1;
    return std::max(1, std::min(4, slot));
}

static int DetectLevelSlotFromCaptured(const ScreenShot* shot) {
    if (!shot) {
        g.levelFocusDetectedThisFrame = false;
        if (++g.levelFocusMissFrames >= kLevelFocusHideAfterMissFrames) {
            g.levelFocusVisible = false;
        }
        return 0;
    }

    UiRect focus{};
    int slot = DetectLevelSlotFromShot(*shot, &focus);
    if (slot > 0) {
        g.levelFocus = focus;
        g.levelFocusVisible = true;
        g.levelFocusDetectedThisFrame = true;
        g.levelFocusMissFrames = 0;
    } else {
        g.levelFocusDetectedThisFrame = false;
        if (++g.levelFocusMissFrames >= kLevelFocusHideAfterMissFrames) {
            g.levelFocusVisible = false;
        }
    }
    return slot;
}

static int DetectLevelSlot() {
    ScreenShot shot;
    return DetectLevelSlotFromCaptured(CaptureScreen(shot) ? &shot : nullptr);
}

static bool UpdateObservedLevelSlot(int detectedSlot, bool* changed = nullptr, bool initialIsChange = false) {
    if (changed) *changed = false;
    if (detectedSlot <= 0) {
        if (g.levelFocusMissFrames < kLevelFocusHideAfterMissFrames) {
            return g.observedLevelStableFrames >= 2;
        }
        g.observedLevelSlot = 0;
        g.observedLevelStableFrames = 0;
        return false;
    }

    if (detectedSlot == g.observedLevelSlot) {
        ++g.observedLevelStableFrames;
    } else {
        g.observedLevelSlot = detectedSlot;
        g.observedLevelStableFrames = 1;
    }

    if (g.observedLevelStableFrames < 2) return false;
    if (g.levelSlot == 0) {
        g.levelSlot = detectedSlot;
        if (initialIsChange && changed) *changed = true;
        return true;
    }
    if (detectedSlot != g.levelSlot) {
        g.levelSlot = detectedSlot;
        if (changed) *changed = true;
        return true;
    }
    return true;
}

struct CaptureResult {
    Pattern pattern{};
    int bluePixelsTotal = 0;
    int selectedIndex = -1;
    int selectedScore = 0;
};

static double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static CaptureResult CaptureFrame(bool detectBlue) {
    CaptureResult result{};
    RECT roi{g.roiX, g.roiY, g.roiX + g.roiW, g.roiY + g.roiH};
    gta5::capture::GameFrame captured;
    if (!gta5::capture::CaptureGameFrame(captured, &roi)) return result;
    const double downScale = (captured.toScreenX + captured.toScreenY) * 0.5;
    const int captureW = captured.width;
    const int captureH = captured.height;
    const uint8_t* px = reinterpret_cast<const uint8_t*>(captured.bgra.data());
    const int stride = captureW * 4;
    if (!px) return result;
    if (detectBlue) {
        for (int r = 0; r < GRID_ROWS; ++r) {
            for (int c = 0; c < GRID_COLS; ++c) {
                int idx = r * GRID_COLS + c;
                Circle circle = g.circles[idx];
                double cx = static_cast<double>(circle.x - captured.screenX) / captured.toScreenX;
                double cy = static_cast<double>(circle.y - captured.screenY) / captured.toScreenY;
                double radius = std::max(6.0, circle.r * 0.36 / downScale);
                double radiusSq = radius * radius;
                int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
                int x1 = std::min(captureW - 1, static_cast<int>(std::ceil(cx + radius)));
                int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
                int y1 = std::min(captureH - 1, static_cast<int>(std::ceil(cy + radius)));
                int blue = 0;
                int samples = 0;

                for (int y = y0; y < y1; y += 2) {
                    const uint8_t* row = px + y * stride;
                    for (int x = x0; x < x1; x += 2) {
                        double dx = x - cx;
                        double dy = y - cy;
                        if (dx * dx + dy * dy > radiusSq) continue;
                        const uint8_t* p = row + x * 4;
                        uint8_t b = p[0], gch = p[1], rr = p[2];
                        if (IsBluePixel(rr, gch, b)) ++blue;
                        ++samples;
                    }
                }

                result.bluePixelsTotal += blue;
                int pct = samples > 0 ? (blue * 100 / samples) : 0;
                result.pattern.on[idx] = pct >= g.thresholdPct ? 1 : 0;
            }
        }
    }

    int bestIdx = -1;
    int bestScore = 0;
    for (int idx = 0; idx < GRID_CELLS; ++idx) {
        Circle circle = g.circles[idx];
        double cx = static_cast<double>(circle.x - captured.screenX) / captured.toScreenX;
        double cy = static_cast<double>(circle.y - captured.screenY) / captured.toScreenY;
        double rLocal = circle.r / downScale;
        double inner = rLocal + std::max(3.0, rLocal * 0.10);
        double outer = rLocal + std::max(10.0, rLocal * 0.38);
        double innerSq = inner * inner;
        double outerSq = outer * outer;
        int x0 = std::max(0, static_cast<int>(std::floor(cx - outer)));
        int x1 = std::min(captureW - 1, static_cast<int>(std::ceil(cx + outer)));
        int y0 = std::max(0, static_cast<int>(std::floor(cy - outer)));
        int y1 = std::min(captureH - 1, static_cast<int>(std::ceil(cy + outer)));
        int white = 0;
        int samples = 0;

        for (int y = y0; y < y1; y += 2) {
            const uint8_t* row = px + y * stride;
            for (int x = x0; x < x1; x += 2) {
                double dx = x - cx;
                double dy = y - cy;
                double d2 = dx * dx + dy * dy;
                if (d2 < innerSq || d2 > outerSq) continue;
                const uint8_t* p = row + x * 4;
                if (IsWhiteUiPixel(p[2], p[1], p[0])) ++white;
                ++samples;
            }
        }

        int score = samples > 0 ? (white * 100 / samples) : 0;
        if (score > bestScore) {
            bestScore = score;
            bestIdx = idx;
        }
    }

    if (bestScore >= 10) {
        result.selectedIndex = bestIdx;
        result.selectedScore = bestScore;
    }

    return result;
}

static void UpdateStatus() {
    std::wstringstream ss;
    ss << (g.running ? L"Running" : L"Stopped")
       << L" | Frames: " << g.frameCount
       << L" | Detected flashes: " << g.detectedCount
       << L" | Repeat: " << g.tracker.repeatCount
       << L" | Target: " << (g.tracker.targetReady ? L"yes" : L"no")
       << L" | Level: " << (g.levelSlot > 0 ? g.levelSlot : g.observedLevelSlot)
       << L" | Selected: ";
    if (g.selectedIndex >= 0) {
        ss << (g.selectedIndex + 1) << L" score " << g.selectedScore;
    } else {
        ss << L"-";
    }
    ss
       << L" | ROI: " << g.roiX << L"," << g.roiY << L" "
       << g.roiW << L"x" << g.roiH;
    SetWindowTextW(g.status, ss.str().c_str());

    if (g.roiLabel) {
        std::wstringstream roi;
        roi << L"Auto ROI: X=" << g.roiX << L" Y=" << g.roiY
            << L" W=" << g.roiW << L" H=" << g.roiH;
        SetWindowTextW(g.roiLabel, roi.str().c_str());
    }
}

static void OrPattern(Pattern& target, const Pattern& source) {
    for (int i = 0; i < GRID_CELLS; ++i) {
        target.on[i] = target.on[i] || source.on[i];
    }
}

static int TargetRowForColumn(const Pattern& pattern, int col) {
    if (col < 0 || col >= GRID_COLS) return -1;
    for (int row = 0; row < GRID_ROWS; ++row) {
        if (pattern.on[row * GRID_COLS + col]) return row;
    }
    return -1;
}

static int LastTargetColumn(const Pattern& pattern) {
    for (int col = GRID_COLS - 1; col >= 0; --col) {
        if (TargetRowForColumn(pattern, col) >= 0) return col;
    }
    return -1;
}

static bool WaitingColumnVerify() {
    return g.tracker.targetReady
        && g.pendingVerifyCol >= 0
        && GetTickCount64() < g.nextInputAt;
}

static bool AwaitingFinalSubmitResult() {
    return g.tracker.targetReady && g.finalSubmitAt != 0;
}

static void RecordCompletedFlash(const Pattern& p) {
    if (!p.any()) return;
    ++g.tracker.flashCount;
    if (g.tracker.hasLastFlash && p.equals(g.tracker.lastFlash)) {
        ++g.tracker.repeatCount;
    } else {
        g.tracker.lastFlash = p;
        g.tracker.hasLastFlash = true;
        g.tracker.repeatCount = 1;
    }

    if (g.tracker.repeatCount >= 3 && !g.tracker.targetReady) {
        g.tracker.target = p;
        g.tracker.targetReady = true;
        g.autoInputEnabled = true;
        g.pendingVerifyCol = -1;
        g.finalSubmitAt = 0;
        g.answerPath = p;
        g.autoMessage = L"Target locked";
    }
}

static void TickAutoInput() {
    if (g.selectedIndex == g.lastSelectedIndex && g.selectedIndex >= 0) {
        ++g.selectedStableFrames;
    } else {
        g.lastSelectedIndex = g.selectedIndex;
        g.selectedStableFrames = g.selectedIndex >= 0 ? 1 : 0;
    }

    if (!g.autoInputEnabled || !g.tracker.targetReady) {
        return;
    }
    if (g.inputJob) {
        if (g.inputJob.Pending()) {
            g.autoMessage = L"Sending column";
            return;
        }
        if (g.inputJob.Succeeded()) {
            g.nextInputAt = GetTickCount64() + 120;
            g.autoMessage = L"Waiting input settle";
        } else {
            g.pendingVerifyCol = -1;
            g.autoMessage = L"Input failed; retrying";
        }
        g.inputJob = {};
        return;
    }
    if (GetTickCount64() < g.nextInputAt) {
        g.autoMessage = L"Waiting input settle";
        return;
    }
    if (g.selectedIndex < 0) {
        g.autoMessage = L"Waiting for selected ring";
        return;
    }
    if (g.selectedStableFrames < kSelectedStableFramesBeforeInput) {
        g.autoMessage = L"Confirming selected ring";
        return;
    }

    int selectedRow = g.selectedIndex / GRID_COLS;
    int selectedCol = g.selectedIndex % GRID_COLS;
    if (selectedCol < 0 || selectedCol >= GRID_COLS) return;
    int lastTargetCol = LastTargetColumn(g.tracker.target);
    if (lastTargetCol < 0) {
        g.autoMessage = L"Plan failed: empty target";
        return;
    }

    if (g.pendingVerifyCol >= 0) {
        int pendingCol = g.pendingVerifyCol;
        if (selectedCol > pendingCol) {
            g.pendingVerifyCol = -1;
            g.autoMessage = L"Column verified";
            if (pendingCol >= lastTargetCol) {
                g.autoInputEnabled = false;
                g.autoMessage = L"Waiting next level";
                return;
            }
        } else {
            g.pendingVerifyCol = -1;
            g.autoMessage = selectedCol == pendingCol ? L"Retrying column" : L"Resync column";
        }
    }

    std::vector<WORD> keys;
    int targetRow = TargetRowForColumn(g.tracker.target, selectedCol);
    if (targetRow < 0) {
        g.autoMessage = L"Plan failed: missing column";
        return;
    }

    InvalidateRect(g.overlayWnd, nullptr, TRUE);
    UpdateWindow(g.overlayWnd);

    int currentRow = selectedRow;
    while (currentRow < targetRow) {
        keys.push_back(VK_DOWN);
        ++currentRow;
    }
    while (currentRow > targetRow) {
        keys.push_back(VK_UP);
        --currentRow;
    }
    keys.push_back(VK_RETURN);

    g.autoMessage = L"Sending column";
    std::vector<gta5::input::Key> inputKeys;
    inputKeys.reserve(keys.size());
    for (WORD key : keys) inputKeys.push_back(gta5::input::Key::FromVirtualKey(key));
    g.inputJob = gta5::input::QueueSequence(inputKeys);
    g.nextInputAt = 0;

    g.pendingVerifyCol = selectedCol;
    if (selectedCol >= lastTargetCol) {
        g.finalSubmitAt = GetTickCount64();
    }
    g.autoMessage = selectedCol >= lastTargetCol ? L"Waiting next level" : L"Verifying column";
}

static void PositionOverlay();

static void ClearLevelState(const wchar_t* reason) {
    g.detectedCount = 0;
    g.blankFrames = 0;
    g.selectedIndex = -1;
    g.selectedScore = 0;
    g.lastSelectedIndex = -1;
    g.selectedStableFrames = 0;
    g.nextInputAt = 0;
    g.inputJob = {};
    g.autoInputEnabled = false;
    g.pendingVerifyCol = -1;
    g.finalSubmitAt = 0;
    g.autoMessage = reason;
    g.current = Pattern{};
    g.answerPath = Pattern{};
    g.flashUnion = Pattern{};
    g.tracker = PatternTracker{};
    g.message = reason;
}

static void ClearRuntimeState(const wchar_t* reason) {
    g.circlesReady = false;
    g.levelSlot = 0;
    g.observedLevelSlot = 0;
    g.observedLevelStableFrames = 0;
    g.levelFocusVisible = false;
    g.levelFocusDetectedThisFrame = false;
    g.levelFocusMissFrames = 0;
    g.levelFocus = UiRect{};
    if (wcscmp(reason, L"Not in minigame") == 0) {
        g.minigameVisible = false;
    }
    ClearLevelState(reason);
}

static void StartCapture() {
    g.thresholdPct = std::min(80, std::max(1, GetEditInt(g.editThresh, g.thresholdPct)));
    g.running = true;
    ClearRuntimeState(L"Locating");
    g.minigameVisible = IsMinigamePageVisible();
    if (g.minigameVisible && AutoLocateRoi()) {
        g.autoMessage = L"Auto idle";
        g.message = L"Watching";
    } else {
        g.circlesReady = false;
        g.autoMessage = L"Locating";
        g.message = L"Locating";
    }
    SetWindowTextW(g.btnStart, L"Stop");
    SetTimer(g.mainWnd, TIMER_ID, TIMER_MS, nullptr);
    ShowWindow(g.overlayWnd, SW_SHOWNOACTIVATE);
    PositionOverlay();
    UpdateStatus();
}

static void StopCapture() {
    g.running = false;
    g.minigameVisible = false;
    g.message = L"Stopped";
    g.minigameGeometry = {};
    g.circlesReady = false;
    KillTimer(g.mainWnd, TIMER_ID);
    SetWindowTextW(g.btnStart, L"Start");
    InvalidateRect(g.overlayWnd, nullptr, TRUE);
    UpdateStatus();
}

static void PositionOverlay() {
    RECT game{};
    if (!gta5::capture::GetGameClientRect(game)) return;
    SetWindowPos(g.overlayWnd, HWND_TOPMOST, game.left, game.top,
                 game.right - game.left, game.bottom - game.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC paintDc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdc = CreateCompatibleDC(paintDc);
        HBITMAP buffer = CreateCompatibleBitmap(paintDc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ oldBitmap = SelectObject(hdc, buffer);
        auto present = [&] {
            SetViewportOrgEx(hdc, 0, 0, nullptr);
            BitBlt(paintDc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, oldBitmap);
            DeleteObject(buffer);
            DeleteDC(hdc);
            EndPaint(hwnd, &ps);
        };
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        RECT game{};
        if (gta5::capture::GetGameClientRect(game)) SetViewportOrgEx(hdc, -game.left, -game.top, nullptr);

        SetBkMode(hdc, TRANSPARENT);
        if (!g.running || !g.minigameVisible || !g.circlesReady) {
            present();
            return 0;
        }

        for (int idx = 0; idx < GRID_CELLS; ++idx) {
            Circle circle = g.circles[idx];
            if (circle.r <= 0) continue;
            COLORREF color = g.current.on[idx] ? RGB(255, 40, 40) : RGB(0, 230, 80);
            if (g.answerPath.on[idx]) {
                color = RGB(40, 170, 255);
            }

            int offset = std::max(6, static_cast<int>(std::round(circle.r * 0.21)));
            int len = std::max(10, static_cast<int>(std::round(circle.r * 0.38)));
            int glowWidth = std::max(5, static_cast<int>(std::round(circle.r * 0.18)));
            int penWidth = std::max(3, static_cast<int>(std::round(circle.r * 0.10)));
            int x = circle.x - circle.r - offset;
            int y = circle.y + circle.r + offset;
            HPEN glowPen = CreatePen(PS_SOLID, glowWidth, RGB(12, 12, 12));
            HGDIOBJ oldPen = SelectObject(hdc, glowPen);
            MoveToEx(hdc, x, y, nullptr);
            LineTo(hdc, x + len, y);
            MoveToEx(hdc, x, y, nullptr);
            LineTo(hdc, x, y - len);
            SelectObject(hdc, oldPen);
            DeleteObject(glowPen);

            HPEN pen = CreatePen(PS_SOLID, penWidth, color);
            oldPen = SelectObject(hdc, pen);
            MoveToEx(hdc, x, y, nullptr);
            LineTo(hdc, x + len, y);
            MoveToEx(hdc, x, y, nullptr);
            LineTo(hdc, x, y - len);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        if (g.selectedIndex >= 0 && g.selectedIndex < GRID_CELLS) {
            Circle circle = g.circles[g.selectedIndex];
            int penWidth = std::max(3, static_cast<int>(std::round(circle.r * 0.09)));
            int sideOffset = std::max(22, static_cast<int>(std::round(circle.r * 0.80)));
            int halfHeight = std::max(16, static_cast<int>(std::round(circle.r * 0.52)));
            int railGap = std::max(7, static_cast<int>(std::round(circle.r * 0.23)));
            int capLen = std::max(12, static_cast<int>(std::round(circle.r * 0.38)));
            HPEN pen = CreatePen(PS_SOLID, penWidth, RGB(255, 240, 40));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            int x = circle.x + circle.r + sideOffset;
            int y = circle.y;
            MoveToEx(hdc, x, y - halfHeight, nullptr);
            LineTo(hdc, x, y + halfHeight);
            MoveToEx(hdc, x + railGap, y - halfHeight, nullptr);
            LineTo(hdc, x + railGap, y + halfHeight);
            MoveToEx(hdc, x, y - halfHeight, nullptr);
            LineTo(hdc, x + capLen, y - halfHeight);
            MoveToEx(hdc, x, y + halfHeight, nullptr);
            LineTo(hdc, x + capLen, y + halfHeight);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        if (g.levelFocusVisible && g.observedLevelStableFrames >= 2) {
            int screenH = std::max(1, GetSystemMetrics(SM_CYSCREEN));
            int cx = (g.levelFocus.left + g.levelFocus.right) / 2;
            int top = g.levelFocus.bottom + ScalePx(8, screenH);
            int halfW = ScalePx(11, screenH);
            int triH = ScalePx(10, screenH);
            POINT shadow[3]{
                { cx, top + triH + ScalePx(2, screenH) },
                { cx - halfW - ScalePx(2, screenH), top - ScalePx(2, screenH) },
                { cx + halfW + ScalePx(2, screenH), top - ScalePx(2, screenH) }
            };
            HBRUSH shadowBrush = CreateSolidBrush(RGB(8, 12, 10));
            HGDIOBJ oldBrush = SelectObject(hdc, shadowBrush);
            HPEN shadowPen = CreatePen(PS_SOLID, ScalePx(1, screenH), RGB(8, 12, 10));
            HGDIOBJ oldPen = SelectObject(hdc, shadowPen);
            Polygon(hdc, shadow, 3);
            SelectObject(hdc, oldPen);
            DeleteObject(shadowPen);

            POINT tri[3]{
                { cx, top + triH },
                { cx - halfW, top },
                { cx + halfW, top }
            };
            const COLORREF focusColor = g.levelFocusDetectedThisFrame ? RGB(0, 245, 95) : RGB(255, 190, 45);
            HBRUSH brush = CreateSolidBrush(focusColor);
            HPEN pen = CreatePen(PS_SOLID, ScalePx(1, screenH), focusColor);
            SelectObject(hdc, brush);
            SelectObject(hdc, pen);
            Polygon(hdc, tri, 3);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(pen);
            DeleteObject(brush);
            DeleteObject(shadowBrush);
        }
        present();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OnTimer() {
    if (!g.running) return;
    bool detectBlue = !g.tracker.targetReady;
    ScreenShot ingameShot;
    const bool capturedIngame = CaptureScreen(ingameShot);
    bool levelChanged = false;
    UpdateObservedLevelSlot(DetectLevelSlotFromCaptured(capturedIngame ? &ingameShot : nullptr),
                            &levelChanged, AwaitingFinalSubmitResult());
    if (levelChanged) {
        ++g.frameCount;
        ClearLevelState(L"Next level");
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }
    g.minigameVisible = CheckMinigamePage(capturedIngame ? &ingameShot : nullptr);
    if (!g.minigameVisible) {
        ++g.frameCount;
        ClearRuntimeState(L"Not in minigame");
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }

    if (!g.circlesReady) {
        ++g.frameCount;
        if (AutoLocateRoi()) {
            g.autoMessage = L"Auto idle";
            g.message = L"Watching";
        } else {
            g.circlesReady = false;
            g.autoMessage = L"Locating";
            g.message = L"Locating";
        }
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }

    if (WaitingColumnVerify()) {
        ++g.frameCount;
        g.selectedIndex = -1;
        g.selectedScore = 0;
        int lastTargetCol = LastTargetColumn(g.tracker.target);
        g.autoMessage = g.pendingVerifyCol >= lastTargetCol ? L"Waiting next level" : L"Verifying column";
        g.message = g.autoMessage;
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }

    if (AwaitingFinalSubmitResult()) {
        ++g.frameCount;
        g.selectedIndex = -1;
        g.selectedScore = 0;
        g.autoMessage = L"Waiting next level";
        g.message = g.autoMessage;
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }

    CaptureResult capture = CaptureFrame(detectBlue);
    Pattern framePattern = capture.pattern;
    ++g.frameCount;

    g.current = framePattern;
    g.selectedIndex = capture.selectedIndex;
    g.selectedScore = capture.selectedScore;

    if (!detectBlue) {
        g.message = L"Answering";
    } else if (framePattern.any()) {
        g.blankFrames = 0;
        OrPattern(g.flashUnion, framePattern);
        g.message = L"Blue flash merging";
    } else {
        ++g.blankFrames;
        if (g.flashUnion.any() && g.blankFrames >= 2) {
            ++g.detectedCount;
            RecordCompletedFlash(g.flashUnion);
            g.flashUnion = Pattern{};
        }
        g.message = L"Blank / waiting";
    }

    TickAutoInput();

    PositionOverlay();
    InvalidateRect(g.overlayWnd, nullptr, TRUE);
    UpdateStatus();
}

static HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h,
                         parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

static HWND MakeEdit(HWND parent, int id, int x, int y, int w, int h, int value) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    SetEditInt(edit, value);
    return edit;
}

static void ResetHistory() {
    g.frameCount = 0;
    g.detectedCount = 0;
    g.blankFrames = 0;
    g.selectedIndex = -1;
    g.selectedScore = 0;
    g.lastSelectedIndex = -1;
    g.selectedStableFrames = 0;
    g.levelSlot = 0;
    g.observedLevelSlot = 0;
    g.observedLevelStableFrames = 0;
    g.levelFocusVisible = false;
    g.levelFocusDetectedThisFrame = false;
    g.levelFocusMissFrames = 0;
    g.levelFocus = UiRect{};
    g.nextInputAt = 0;
    g.inputJob = {};
    g.autoInputEnabled = false;
    g.pendingVerifyCol = -1;
    g.finalSubmitAt = 0;
    g.autoMessage = L"Auto idle";
    g.current = Pattern{};
    g.answerPath = Pattern{};
    g.flashUnion = Pattern{};
    g.tracker = PatternTracker{};
    g.message = L"History reset";
    InvalidateRect(g.overlayWnd, nullptr, TRUE);
    UpdateStatus();
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.mainWnd = hwnd;
        MakeLabel(hwnd, L"Blue threshold %", 16, 18, 120, 22);
        g.editThresh = MakeEdit(hwnd, 205, 136, 14, 70, 24, g.thresholdPct);

        g.btnStart = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   16, 54, 120, 34, hwnd, reinterpret_cast<HMENU>(101),
                                   GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      148, 54, 120, 34, hwnd, reinterpret_cast<HMENU>(102),
                      GetModuleHandleW(nullptr), nullptr);

        g.roiLabel = CreateWindowW(L"STATIC", L"Auto ROI: not locked yet", WS_CHILD | WS_VISIBLE,
                                   16, 104, 520, 24, hwnd, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        g.status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                 16, 136, 520, 46, hwnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        UpdateStatus();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 101) {
            if (g.running) StopCapture(); else StartCapture();
        } else if (id == 102) {
            ResetHistory();
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ID) OnTimer();
        return 0;
    case WM_DESTROY:
        StopCapture();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


bool DetectInGame() {
  ScreenShot shot;
  if (!CaptureScreen(shot)) {
    ResetInGameCache();
    return false;
  }
  MinigameGeometry geometry;
  if (!AnalyzeMinigamePage(shot, &geometry)) {
    ResetInGameCache();
    return false;
  }
  g.minigameGeometry = geometry;
  if (!AutoLocateRoiFromShot(shot)) {
    ResetInGameCache();
    return false;
  }
  return true;
}
void ResetInGameCache() {
  g.minigameGeometry = {};
  g.circlesReady = false;
}
HWND OverlayWindow() { return g.overlayWnd; }
void SetOverlayWindow(HWND hwnd) { g.overlayWnd = hwnd; }
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return OverlayProc(hwnd, msg, wp, lp); }
void HideOverlay() { if (g.overlayWnd) ShowWindow(g.overlayWnd, SW_HIDE); }
void ResetForSession() {
  const bool keepLocation = g.minigameGeometry.valid && g.circlesReady;
  const MinigameGeometry geometry = g.minigameGeometry;
  const auto circles = g.circles;
  const int roiX = g.roiX, roiY = g.roiY, roiW = g.roiW, roiH = g.roiH;
  const int searchX = g.searchX, searchY = g.searchY, searchSize = g.searchSize;
  g.thresholdPct = 8; g.running = true; g.frameCount = 0; g.detectedCount = 0;
  ClearRuntimeState(L"Locating");
  if (keepLocation) {
    g.minigameGeometry = geometry;
    g.circles = circles;
    g.roiX = roiX; g.roiY = roiY; g.roiW = roiW; g.roiH = roiH;
    g.searchX = searchX; g.searchY = searchY; g.searchSize = searchSize;
    g.circlesReady = true;
  }
  g.minigameVisible = true;
}
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status) {
  using Clock = std::chrono::steady_clock;
  std::wstring lastStatus;
  auto setStatus = [&](const std::wstring& text) {
    if (text != lastStatus) {
      lastStatus = text;
      status(text);
    }
  };
  auto syncOverlay = [&] {
    if (!g.overlayWnd) return;
    if (overlayEnabled()) {
      PositionOverlay();
    } else {
      ShowWindow(g.overlayWnd, SW_HIDE);
    }
  };
  ResetForSession();
  if (!g.circlesReady) AutoLocateRoi();
  syncOverlay();
  int lostFrames = 0;
  bool completedAnyLevel = false;
  setStatus(L"flashing: locating");
  while (!stopRequested()) {
    auto loopStart = Clock::now();
    CaptureResult capture;
    bool detectBlue = !g.tracker.targetReady;
    syncOverlay();
    ScreenShot ingameShot;
    const bool capturedIngame = CaptureScreen(ingameShot);
    bool levelChanged = false;
    UpdateObservedLevelSlot(DetectLevelSlotFromCaptured(capturedIngame ? &ingameShot : nullptr),
                            &levelChanged, AwaitingFinalSubmitResult());
    if (levelChanged) {
      completedAnyLevel = true;
      setStatus(L"flashing: next level");
      ClearLevelState(L"Next level");
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(0);
      continue;
    }
    g.minigameVisible = CheckMinigamePage(capturedIngame ? &ingameShot : nullptr);
    if (!g.minigameVisible) {
      if (++lostFrames >= 15) {
        setStatus(L"flashing: exited");
        ClearRuntimeState(L"Not in minigame");
        break;
      }
      setStatus(L"flashing: confirming exit");
      Sleep(50);
      continue;
    }
    lostFrames = 0;
    if (WaitingColumnVerify()) {
      g.selectedIndex = -1;
      g.selectedScore = 0;
      int lastTargetCol = LastTargetColumn(g.tracker.target);
      g.autoMessage = g.pendingVerifyCol >= lastTargetCol ? L"Waiting next level" : L"Verifying column";
      g.message = g.autoMessage;
      setStatus(g.pendingVerifyCol >= lastTargetCol ? L"flashing: waiting next level" : L"flashing: verifying column");
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(0);
      continue;
    }
    if (AwaitingFinalSubmitResult()) {
      ++g.frameCount;
      g.selectedIndex = -1;
      g.selectedScore = 0;
      g.autoMessage = L"Waiting next level";
      g.message = g.autoMessage;
      setStatus(L"flashing: waiting next level");
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(0);
      continue;
    }
    if (!g.circlesReady) {
      setStatus(L"flashing: locating");
      AutoLocateRoi();
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(30);
      continue;
    }
    setStatus(detectBlue ? L"flashing: reading pattern" : L"flashing: auto input");
    capture = CaptureFrame(detectBlue);
    Pattern framePattern = capture.pattern;
    ++g.frameCount;
    g.current = framePattern;
    g.selectedIndex = capture.selectedIndex;
    g.selectedScore = capture.selectedScore;
    if (!detectBlue) { g.message = L"Answering"; }
    else if (framePattern.any()) { g.blankFrames = 0; OrPattern(g.flashUnion, framePattern); g.message = L"Blue flash merging"; }
    else { ++g.blankFrames; if (g.flashUnion.any() && g.blankFrames >= 2) { ++g.detectedCount; RecordCompletedFlash(g.flashUnion); g.flashUnion = Pattern{}; } g.message = L"Blank / waiting"; }
    TickAutoInput();
    if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
    double elapsedMs = MsSince(loopStart);
    int sleepMs = TIMER_MS - static_cast<int>(std::round(elapsedMs));
    if (sleepMs > 1) {
      Sleep(static_cast<DWORD>(sleepMs));
    } else {
      Sleep(0);
    }
  }
  g.running = false;
  HideOverlay();
  g.minigameGeometry = {};
  g.circlesReady = false;
  return completedAnyLevel;
}

}  // namespace gta5::games::flashing
