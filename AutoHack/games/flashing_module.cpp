#define NOMINMAX
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
static const int kMinigameCheckIntervalFrames = 10;
static const int kSelectedStableFramesBeforeInput = 2;
static const int kSuccessHitFramesBeforeLevelComplete = 3;
static const DWORD64 kSuccessHitWindowMs = 450;
static const DWORD64 kSuccessGoneMsBeforeNextLevel = 180;
static const DWORD64 kFinalSubmitEmptyCheckDelayMs = 900;
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
    double toScreenX = 1.0;
    double toScreenY = 1.0;
    std::vector<uint8_t> pixels;
};

struct WhiteBar {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int score = 0;
};

struct Circle {
    int x = 0;
    int y = 0;
    int r = 0;
    int score = 0;
};

static int CaptureHeightForScreen(int screenH) {
    return screenH > 1080 ? 1080 : screenH;
}

static int CaptureWidthForScreen(int screenW, int screenH, int captureH) {
    return screenH > 0 ? std::max(1, static_cast<int>(std::lround(screenW * (captureH / static_cast<double>(screenH))))) : screenW;
}

static int ToScreenX(const ScreenShot& shot, int x) {
    return static_cast<int>(std::lround(x * shot.toScreenX));
}

static int ToScreenY(const ScreenShot& shot, int y) {
    return static_cast<int>(std::lround(y * shot.toScreenY));
}

static int ToShotX(const ScreenShot& shot, int x) {
    return static_cast<int>(std::lround(x / std::max(0.0001, shot.toScreenX)));
}

static int ToShotY(const ScreenShot& shot, int y) {
    return static_cast<int>(std::lround(y / std::max(0.0001, shot.toScreenY)));
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
    DWORD64 nextInputAt = 0;
    bool autoInputEnabled = false;
    int pendingVerifyCol = -1;
    DWORD64 finalSubmitAt = 0;
    bool minigameVisible = false;
    bool successVisible = false;
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
    shot.screenW = GetSystemMetrics(SM_CXSCREEN);
    shot.screenH = GetSystemMetrics(SM_CYSCREEN);
    if (shot.screenW <= 0 || shot.screenH <= 0) return false;
    shot.h = CaptureHeightForScreen(shot.screenH);
    shot.w = CaptureWidthForScreen(shot.screenW, shot.screenH, shot.h);
    shot.toScreenX = shot.screenW / static_cast<double>(std::max(1, shot.w));
    shot.toScreenY = shot.screenH / static_cast<double>(std::max(1, shot.h));

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = shot.w;
    bmi.bmiHeader.biHeight = -shot.h;
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
    SetStretchBltMode(mem, COLORONCOLOR);
    StretchBlt(mem, 0, 0, shot.w, shot.h, screen, 0, 0, shot.screenW, shot.screenH, SRCCOPY);
    shot.pixels.resize(static_cast<size_t>(shot.w) * shot.h * 4);
    memcpy(shot.pixels.data(), bits, shot.pixels.size());

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return true;
}

static bool IsWhiteUiPixel(uint8_t r, uint8_t gch, uint8_t b) {
    return r > 175 && gch > 175 && b > 175 && std::abs(r - gch) < 55 && std::abs(r - b) < 55;
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
    if (std::abs(shot.toScreenX - 1.0) < 1e-6 && std::abs(shot.toScreenY - 1.0) < 1e-6) return;

    int roiRight = ToScreenX(shot, g.roiX + g.roiW);
    int roiBottom = ToScreenY(shot, g.roiY + g.roiH);
    g.roiX = ToScreenX(shot, g.roiX);
    g.roiY = ToScreenY(shot, g.roiY);
    g.roiW = std::max(1, roiRight - g.roiX);
    g.roiH = std::max(1, roiBottom - g.roiY);

    g.searchX = ToScreenX(shot, g.searchX);
    g.searchY = ToScreenY(shot, g.searchY);
    g.searchSize = std::max(1, ToScreenX(shot, g.searchSize));

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

static bool AutoLocateRoi() {
    ScreenShot shot;
    if (!CaptureScreen(shot)) return false;

    const int screenW = shot.w;
    const int screenH = shot.h;
    std::vector<WhiteBar> bars = FindWhiteBars(shot);
    WhiteBar best{};
    if (!FindSignalRepeaterBar(shot, bars, best)) {
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
    const int minRun = std::max(ScalePx(260, screenH), screenW / 7);
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

        for (int x = screenW / 10; x < screenW * 85 / 100; x += 2) {
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
    const int screenW = shot.w;
    const int screenH = shot.h;
    int bestRank = -1000000;

    for (const WhiteBar& b : bars) {
        int centerX = b.x + b.w / 2;
        bool leftPanel = centerX < screenW * 63 / 100;
        bool belowTopBoxes = b.y > screenH * 16 / 100;
        bool notTooLow = b.y < screenH * 34 / 100;
        bool wideEnough = b.w > screenW * 28 / 100;
        if (!leftPanel || !belowTopBoxes || !notTooLow || !wideEnough || b.score < 28) continue;

        int rank = b.w * 3 + b.y * 6 - std::abs(centerX - screenW * 2 / 5);
        if (rank > bestRank) {
            bestRank = rank;
            signal = b;
        }
    }

    return bestRank != -1000000;
}

static bool AnalyzeMinigamePage(const ScreenShot& shot, WhiteBar* signalOut = nullptr) {
    std::vector<WhiteBar> bars = FindWhiteBars(shot);
    WhiteBar signal{};
    if (!FindSignalRepeaterBar(shot, bars, signal)) return false;

    int titleYSlack = ScalePx(35, shot.h);
    bool hasRightTitle = false;
    int topTitleCount = 0;
    for (const WhiteBar& b : bars) {
        int centerX = b.x + b.w / 2;
        int centerY = b.y + b.h / 2;

        if (std::abs(centerY - (signal.y + signal.h / 2)) < titleYSlack &&
            b.x > signal.x + signal.w * 3 / 4 &&
            centerX > shot.w * 55 / 100 &&
            b.w > shot.w * 14 / 100 &&
            b.score >= 28) {
            hasRightTitle = true;
        }

        if (centerY < signal.y - titleYSlack &&
            centerY > shot.h * 8 / 100 &&
            centerY < shot.h * 24 / 100 &&
            b.w > shot.w * 18 / 100 &&
            b.score >= 28) {
            ++topTitleCount;
        }
    }

    if (!hasRightTitle || topTitleCount < 2) return false;
    if (signalOut) *signalOut = signal;
    return true;
}

static bool IsMinigamePageVisible() {
    ScreenShot shot;
    if (!CaptureScreen(shot)) return false;
    return AnalyzeMinigamePage(shot, nullptr);
}

static bool IsSuccessPopupVisible() {
    ScreenShot shot;
    if (!CaptureScreen(shot)) return false;

    int screenW = shot.w;
    int screenH = shot.h;

    int regionX = screenW * 28 / 100;
    int regionY = screenH * 38 / 100;
    int regionW = screenW * 44 / 100;
    int regionH = screenH * 28 / 100;

    int whitePixels = 0;
    int totalPixels = 0;
    int step = std::max(2, screenW / 200);

    for (int y = regionY; y < regionY + regionH; y += step) {
        for (int x = regionX; x < regionX + regionW; x += step) {
            if (x >= screenW || y >= screenH) continue;
            const uint8_t* p = shot.pixels.data() + (static_cast<size_t>(y) * screenW + x) * 4;
            int gray = (p[2] * 30 + p[1] * 59 + p[0] * 11) / 100;
            if (gray >= 175) whitePixels++;
            totalPixels++;
        }
    }

    int whitePct = totalPixels > 0 ? whitePixels * 100 / totalPixels : 0;

    return whitePct >= 15;
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

    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const double downScale = screenH > 1080 ? screenH / 1080.0 : 1.0;
    const int captureW = std::max(1, static_cast<int>(std::lround(g.roiW / downScale)));
    const int captureH = std::max(1, static_cast<int>(std::lround(g.roiH / downScale)));

    static HDC mem = nullptr;
    static HBITMAP dib = nullptr;
    static HGDIOBJ old = nullptr;
    static void* bits = nullptr;
    static int bufW = 0;
    static int bufH = 0;

    HDC screen = GetDC(nullptr);
    if (!screen) return result;
    if (!mem) {
        mem = CreateCompatibleDC(screen);
        if (!mem) {
            ReleaseDC(nullptr, screen);
            return result;
        }
    }

    if (!dib || bufW != captureW || bufH != captureH) {
        if (dib) {
            SelectObject(mem, old);
            DeleteObject(dib);
            dib = nullptr;
            old = nullptr;
            bits = nullptr;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = captureW;
        bmi.bmiHeader.biHeight = -captureH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            dib = nullptr;
            bits = nullptr;
            bufW = bufH = 0;
            ReleaseDC(nullptr, screen);
            return result;
        }
        old = SelectObject(mem, dib);
        bufW = captureW;
        bufH = captureH;
    }

    SetStretchBltMode(mem, COLORONCOLOR);
    if (!StretchBlt(mem, 0, 0, captureW, captureH, screen, g.roiX, g.roiY, g.roiW, g.roiH, SRCCOPY)) {
        ReleaseDC(nullptr, screen);
        return result;
    }
    ReleaseDC(nullptr, screen);

    const uint8_t* px = static_cast<const uint8_t*>(bits);
    const int stride = captureW * 4;
    if (!px) return result;
    if (detectBlue) {
        for (int r = 0; r < GRID_ROWS; ++r) {
            for (int c = 0; c < GRID_COLS; ++c) {
                int idx = r * GRID_COLS + c;
                Circle circle = g.circles[idx];
                double cx = static_cast<double>(circle.x - g.roiX) / downScale;
                double cy = static_cast<double>(circle.y - g.roiY) / downScale;
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
        double cx = static_cast<double>(circle.x - g.roiX) / downScale;
        double cy = static_cast<double>(circle.y - g.roiY) / downScale;
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

static bool WaitingColumnVerify() {
    return g.tracker.targetReady
        && g.pendingVerifyCol >= 0
        && GetTickCount64() < g.nextInputAt;
}

static bool AwaitingFinalSubmitResult() {
    return g.tracker.targetReady && g.finalSubmitAt != 0;
}

static bool FinalSubmitEmptyCheckDue() {
    return AwaitingFinalSubmitResult()
        && GetTickCount64() - g.finalSubmitAt >= kFinalSubmitEmptyCheckDelayMs;
}

static bool UpdateFinalSubmitEmptyCheck(const CaptureResult& capture) {
    if (!FinalSubmitEmptyCheckDue()) return false;
    return !capture.pattern.any();
}

static void PressScanCode(WORD scanCode, bool extended = false) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &input, sizeof(INPUT));
    Sleep((DWORD)gta5::games::slider::TapHoldMs());

    input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &input, sizeof(INPUT));
}

static void PressGameKey(WORD vk) {
    switch (vk) {
    case VK_UP:
        PressScanCode(0x48, true);
        break;
    case VK_DOWN:
        PressScanCode(0x50, true);
        break;
    case VK_LEFT:
        PressScanCode(0x4B, true);
        break;
    case VK_RIGHT:
        PressScanCode(0x4D, true);
        break;
    case VK_RETURN:
        PressScanCode(0x1C, false);
        break;
    default:
        PressScanCode(static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)), false);
        break;
    }
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

    if (g.tracker.repeatCount >= 2 && !g.tracker.targetReady) {
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

    if (g.pendingVerifyCol >= 0) {
        int pendingCol = g.pendingVerifyCol;
        if (selectedCol > pendingCol) {
            g.pendingVerifyCol = -1;
            g.autoMessage = L"Column verified";
            if (pendingCol >= GRID_COLS - 1) {
                g.autoInputEnabled = false;
                g.autoMessage = L"Waiting success";
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
    for (WORD key : keys) {
        PressGameKey(key);
        Sleep((DWORD)gta5::games::slider::TapGapMs());
    }
    DWORD settleMs = static_cast<DWORD>(keys.size()) *
        ((DWORD)gta5::games::slider::TapHoldMs() + (DWORD)gta5::games::slider::TapGapMs());
    g.nextInputAt = GetTickCount64() + settleMs + 120;

    g.pendingVerifyCol = selectedCol;
    if (selectedCol >= GRID_COLS - 1) {
        g.finalSubmitAt = GetTickCount64();
    }
    g.autoMessage = selectedCol >= GRID_COLS - 1 ? L"Waiting success" : L"Verifying column";
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
    g.autoInputEnabled = false;
    g.pendingVerifyCol = -1;
    g.finalSubmitAt = 0;
    g.successVisible = false;
    g.autoMessage = reason;
    g.current = Pattern{};
    g.answerPath = Pattern{};
    g.flashUnion = Pattern{};
    g.tracker = PatternTracker{};
    g.message = reason;
}

static void ClearRuntimeState(const wchar_t* reason) {
    g.circlesReady = false;
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
    KillTimer(g.mainWnd, TIMER_ID);
    SetWindowTextW(g.btnStart, L"Start");
    InvalidateRect(g.overlayWnd, nullptr, TRUE);
    UpdateStatus();
}

static void PositionOverlay() {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g.overlayWnd, HWND_TOPMOST, 0, 0, screenW, screenH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        if (!g.running || !g.minigameVisible || !g.circlesReady) {
            EndPaint(hwnd, &ps);
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
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OnTimer() {
    if (!g.running) return;
    bool detectBlue = !g.tracker.targetReady;
    bool checkSuccess = g.circlesReady;
    g.successVisible = checkSuccess ? IsSuccessPopupVisible() : false;
    if (g.successVisible) {
        ++g.frameCount;
        ClearLevelState(L"Success");
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }
    bool checkMinigame = !g.circlesReady || (!detectBlue && (g.frameCount % kMinigameCheckIntervalFrames) == 0);
    if (checkMinigame) {
        g.minigameVisible = IsMinigamePageVisible();
    } else {
        g.minigameVisible = true;
    }
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
        g.autoMessage = g.pendingVerifyCol >= GRID_COLS - 1 ? L"Waiting success" : L"Verifying column";
        g.message = g.autoMessage;
        PositionOverlay();
        InvalidateRect(g.overlayWnd, nullptr, TRUE);
        UpdateStatus();
        return;
    }

    if (AwaitingFinalSubmitResult()) {
        ++g.frameCount;
        if (FinalSubmitEmptyCheckDue()) {
            CaptureResult emptyCheck = CaptureFrame(true);
            g.current = emptyCheck.pattern;
            g.selectedIndex = emptyCheck.selectedIndex;
            g.selectedScore = emptyCheck.selectedScore;
            if (UpdateFinalSubmitEmptyCheck(emptyCheck)) {
                ClearLevelState(L"Next level");
                g.message = L"Next level";
            } else {
                g.autoMessage = L"Checking empty panel";
                g.message = g.autoMessage;
            }
        } else {
            g.selectedIndex = -1;
            g.selectedScore = 0;
            g.autoMessage = L"Waiting success";
            g.message = g.autoMessage;
        }
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
                                x, y, w, h, parent, reinterpret_cast<HMENU>(id),
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
    g.nextInputAt = 0;
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


bool DetectInGame() { return IsMinigamePageVisible(); }
HWND OverlayWindow() { return g.overlayWnd; }
void SetOverlayWindow(HWND hwnd) { g.overlayWnd = hwnd; }
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return OverlayProc(hwnd, msg, wp, lp); }
void HideOverlay() { if (g.overlayWnd) ShowWindow(g.overlayWnd, SW_HIDE); }
void ResetForSession() { g.thresholdPct = 8; g.running = true; g.frameCount = 0; g.detectedCount = 0; ClearRuntimeState(L"Locating"); g.minigameVisible = true; }
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
      SetWindowPos(g.overlayWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      ShowWindow(g.overlayWnd, SW_HIDE);
    }
  };
  ResetForSession();
  AutoLocateRoi();
  syncOverlay();
  int lostFrames = 0;
  bool successLatched = false;
  DWORD64 successHitWindowStart = 0;
  int successHitFrames = 0;
  DWORD64 successGoneAt = 0;
  bool completedAnyLevel = false;
  setStatus(L"flashing: locating");
  while (!stopRequested()) {
    auto loopStart = Clock::now();
    CaptureResult capture;
    bool detectBlue = !g.tracker.targetReady;
    syncOverlay();
    bool checkSuccess = g.circlesReady || successLatched;
    if (checkSuccess) {
      g.successVisible = IsSuccessPopupVisible();
    } else {
      g.successVisible = false;
    }
    DWORD64 nowTick = GetTickCount64();
    if (g.successVisible) {
      if (successHitWindowStart == 0 || nowTick - successHitWindowStart > kSuccessHitWindowMs) {
        successHitWindowStart = nowTick;
        successHitFrames = 0;
      }
      ++successHitFrames;
      successGoneAt = 0;
    } else if (!successLatched && successHitWindowStart != 0 && nowTick - successHitWindowStart > kSuccessHitWindowMs) {
      successHitWindowStart = 0;
      successHitFrames = 0;
    }
    bool successStable = g.successVisible && successHitFrames >= kSuccessHitFramesBeforeLevelComplete;
    if (g.successVisible && !successLatched && !successStable) {
      setStatus(L"flashing: confirming success");
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(0);
      continue;
    }
    if (successStable || successLatched) {
      completedAnyLevel = true;
      if (g.successVisible) {
        if (!successLatched) {
          ClearLevelState(L"Success");
          g.successVisible = true;
        }
        successLatched = true;
        successGoneAt = 0;
        setStatus(L"flashing: level complete");
      } else {
        if (successGoneAt == 0) successGoneAt = nowTick;
        if (nowTick - successGoneAt < kSuccessGoneMsBeforeNextLevel) {
          setStatus(L"flashing: level complete");
        } else {
          successLatched = false;
          successHitWindowStart = 0;
          successHitFrames = 0;
          successGoneAt = 0;
          setStatus(L"flashing: next level");
          ClearLevelState(L"Next level");
        }
      }
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(g.successVisible ? TIMER_MS : 0);
      continue;
    }
    bool checkMinigame = !g.circlesReady || (g.frameCount % kMinigameCheckIntervalFrames) == 0;
    if (checkMinigame) {
      g.minigameVisible = IsMinigamePageVisible();
    } else {
      g.minigameVisible = true;
    }
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
      g.autoMessage = g.pendingVerifyCol >= GRID_COLS - 1 ? L"Waiting success" : L"Verifying column";
      g.message = g.autoMessage;
      setStatus(g.pendingVerifyCol >= GRID_COLS - 1 ? L"flashing: waiting success" : L"flashing: verifying column");
      if (g.overlayWnd && overlayEnabled()) InvalidateRect(g.overlayWnd, nullptr, TRUE);
      Sleep(0);
      continue;
    }
    if (AwaitingFinalSubmitResult()) {
      if (FinalSubmitEmptyCheckDue()) {
        CaptureResult emptyCheck = CaptureFrame(true);
        ++g.frameCount;
        g.current = emptyCheck.pattern;
        g.selectedIndex = emptyCheck.selectedIndex;
        g.selectedScore = emptyCheck.selectedScore;
        if (UpdateFinalSubmitEmptyCheck(emptyCheck)) {
          completedAnyLevel = true;
          setStatus(L"flashing: next level");
          ClearLevelState(L"Next level");
        } else {
          g.autoMessage = L"Checking empty panel";
          g.message = g.autoMessage;
          setStatus(L"flashing: checking empty panel");
        }
      } else {
        g.selectedIndex = -1;
        g.selectedScore = 0;
        g.autoMessage = L"Waiting success";
        g.message = g.autoMessage;
        setStatus(L"flashing: waiting success");
      }
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
  return completedAnyLevel;
}

}  // namespace gta5::games::flashing
