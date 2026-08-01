#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "games.h"
#include "../capture/game_window.h"
#include "../input/key_input.h"
#ifdef CLI_TEST
#include <olectl.h>
#include <gdiplus.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace gta5::games::choose_fingerprint {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Region {
    Rect rect;
    int pixels = 0;
    double aspect = 1.0;
    double cx = 0.0, cy = 0.0;
};

struct Frame {
    int x = 0, y = 0, w = 0, h = 0;
    int screenW = 0, screenH = 0;
    double toScreenX = 1.0, toScreenY = 1.0;
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> gray;
};

struct TitleBars {
    Rect timer, target, components, signals;
    bool hasTimer = false, hasTarget = false, hasComponents = false, hasSignals = false;
};

struct RoiInfo {
    bool isMinigame = false;
    Rect panel;
    TitleBars bars;
};

struct BlockInfo {
    int index = 0;
    Rect rect;
    double score = 0.0;
    bool correct = false;
    bool selected = false;
    bool cursor = false;
    int cursorScore = 0;
};

struct OverlayState {
    bool visible = false;
    uint64_t targetHash = 0;
    Rect target;
    std::vector<BlockInfo> blocks;
};

struct SolverCache {
    bool valid = false;
    uint64_t targetHash = 0;
    Rect target;
    std::vector<BlockInfo> baseBlocks;
};

struct AutomationState {
    bool submitted = false;
    uint64_t plannedHash = 0;
    gta5::input::Job inputJob;
};

struct FrameTiming {
    double captureMs = 0.0;
    double gateMs = 0.0;
    double successMs = 0.0;
    double roiMs = 0.0;
    double hashMs = 0.0;
    double answerMs = 0.0;
    double stateMs = 0.0;
    double analyzeMs = 0.0;
    double autoMs = 0.0;
    double publishMs = 0.0;
    double totalMs = 0.0;
    bool cacheHit = false;
    bool minigame = false;
    bool success = false;
};

static HWND gMainWnd = nullptr;
static HWND gOverlayWnd = nullptr;
static HWND gLogList = nullptr;
static HWND gStatusText = nullptr;
static std::atomic<bool> gRunning{false};
static std::atomic<bool> gStopping{false};
static std::thread gWorker;
static CRITICAL_SECTION gStateLock;
static OverlayState gState;
static int gVirtualX = 0, gVirtualY = 0, gVirtualW = 0, gVirtualH = 0;
static DWORD gUiThreadId = 0;
static constexpr UINT WM_APP_LOG = WM_APP + 1;
static constexpr UINT WM_APP_WORKER_STOPPED = WM_APP + 2;
static constexpr DWORD kFrameDelayMs = 10;

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static std::string timingText(const FrameTiming& t) {
    char buf[256];
    std::snprintf(
        buf,
        sizeof(buf),
        "dt=%.1fms cap=%.1f ana=%.1f gate=%.1f suc=%.1f roi=%.1f hash=%.1f ans=%.1f state=%.1f auto=%.1f pub=%.1f",
        t.totalMs,
        t.captureMs,
        t.analyzeMs,
        t.gateMs,
        t.successMs,
        t.roiMs,
        t.hashMs,
        t.answerMs,
        t.stateMs,
        t.autoMs,
        t.publishMs
    );
    return buf;
}

static std::string correctList(const OverlayState& s) {
    std::string correct;
    for (const auto& b : s.blocks) {
        if (b.correct) {
            if (!correct.empty()) correct += ",";
            correct += std::to_string(b.index);
        }
    }
    return correct;
}

static int oddKernel(int h, int w, double ratio, int minimum = 3) {
    int s = std::max(minimum, (int)std::lround(std::min(h, w) * ratio));
    return (s % 2) ? s : s + 1;
}

static Rect clampRect(Rect r, int w, int h) {
    r.x = std::max(0, r.x);
    r.y = std::max(0, r.y);
    r.w = std::max(0, std::min(r.w, w - r.x));
    r.h = std::max(0, std::min(r.h, h - r.y));
    return r;
}

static Rect scaleRectToScreen(const Frame& f, Rect r) {
    int x1 = f.x + (int)std::lround(r.x * f.toScreenX);
    int y1 = f.y + (int)std::lround(r.y * f.toScreenY);
    int x2 = f.x + (int)std::lround((r.x + r.w) * f.toScreenX);
    int y2 = f.y + (int)std::lround((r.y + r.h) * f.toScreenY);
    x1 = std::clamp(x1, f.x, f.x + f.screenW);
    y1 = std::clamp(y1, f.y, f.y + f.screenH);
    x2 = std::clamp(x2, f.x, f.x + f.screenW);
    y2 = std::clamp(y2, f.y, f.y + f.screenH);
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

static OverlayState scaleOverlayStateToScreen(const Frame& f, OverlayState s) {
    s.target = scaleRectToScreen(f, s.target);
    for (auto& b : s.blocks) {
        b.rect = scaleRectToScreen(f, b.rect);
    }
    return s;
}

static Rect padRect(Rect r, int w, int h, double ratio) {
    int pad = (int)std::lround(std::max(r.w, r.h) * ratio);
    return clampRect({r.x - pad, r.y - pad, r.w + pad * 2, r.h + pad * 2}, w, h);
}

static int scaledPx(int frameW, int frameH, int px1080) {
    (void)frameW;
    return std::max(1, (int)std::lround(px1080 * (frameH / 1080.0)));
}

static int scaledPx(const Frame& f, int px1080) {
    return scaledPx(f.w, f.h, px1080);
}

static int scaledArea(const Frame& f, int area1080) {
    const double scale = f.h / 1080.0;
    return std::max(1, (int)std::lround(area1080 * scale * scale));
}

static std::wstring widenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static void postLog(const std::string& s) {
    if (!gMainWnd) return;
    PostMessageW(gMainWnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(widenAscii(s)));
}

static std::string rectText(Rect r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%d,%d %dx%d)", r.x, r.y, r.w, r.h);
    return buf;
}

static std::vector<uint8_t> cropGray(const Frame& f, Rect r) {
    r = clampRect(r, f.w, f.h);
    std::vector<uint8_t> out(r.w * r.h);
    for (int y = 0; y < r.h; ++y) {
        memcpy(out.data() + y * r.w, f.gray.data() + (r.y + y) * f.w + r.x, r.w);
    }
    return out;
}

static std::vector<uint8_t> fingerprintMask(const Frame& f, Rect r) {
    r = clampRect(r, f.w, f.h);
    std::vector<uint8_t> raw(r.w * r.h, 0);
    if (f.bgra.size() != static_cast<size_t>(f.w) * f.h * 4) return raw;

    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const size_t p = (static_cast<size_t>(r.y + y) * f.w + r.x + x) * 4;
            const int b = f.bgra[p + 0];
            const int g = f.bgra[p + 1];
            const int red = f.bgra[p + 2];
            const int hi = std::max({red, g, b});
            const int lo = std::min({red, g, b});
            raw[y * r.w + x] = hi >= 28 && hi - lo <= 28;
        }
    }

    // Selected components render their ridges in white, while unselected
    // components and the target use gray. Keep both and reject the isolated
    // background pixels by their lack of local support.
    std::vector<uint8_t> mask(raw.size(), 0);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            if (!raw[y * r.w + x]) continue;
            int neighbors = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = y + dy;
                if (ny < 0 || ny >= r.h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx;
                    if (nx >= 0 && nx < r.w) neighbors += raw[ny * r.w + nx] ? 1 : 0;
                }
            }
            mask[y * r.w + x] = neighbors >= 3;
        }
    }
    return mask;
}

static std::vector<uint8_t> thresholdBright(const std::vector<uint8_t>& gray, int w, int h) {
    std::vector<uint8_t> mask(w * h);
    for (int i = 0; i < w * h; ++i) mask[i] = gray[i] >= 180 ? 1 : 0;
    return mask;
}

static std::vector<uint8_t> dilate(const std::vector<uint8_t>& src, int w, int h, int k) {
    int r = k / 2;
    std::vector<uint8_t> tmp(w * h), dst(w * h);
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        for (int x = -r; x <= r; ++x) if (0 <= x && x < w) sum += src[y * w + x];
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = sum > 0;
            int oldx = x - r;
            int newx = x + r + 1;
            if (0 <= oldx && oldx < w) sum -= src[y * w + oldx];
            if (0 <= newx && newx < w) sum += src[y * w + newx];
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -r; y <= r; ++y) if (0 <= y && y < h) sum += tmp[y * w + x];
        for (int y = 0; y < h; ++y) {
            dst[y * w + x] = sum > 0;
            int oldy = y - r;
            int newy = y + r + 1;
            if (0 <= oldy && oldy < h) sum -= tmp[oldy * w + x];
            if (0 <= newy && newy < h) sum += tmp[newy * w + x];
        }
    }
    return dst;
}

static std::vector<uint8_t> erode(const std::vector<uint8_t>& src, int w, int h, int k) {
    int r = k / 2;
    std::vector<uint8_t> tmp(w * h), dst(w * h);
    for (int y = 0; y < h; ++y) {
        int sum = 0, cnt = 0;
        for (int x = -r; x <= r; ++x) if (0 <= x && x < w) { sum += src[y * w + x]; cnt++; }
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = sum == cnt;
            int oldx = x - r;
            int newx = x + r + 1;
            if (0 <= oldx && oldx < w) { sum -= src[y * w + oldx]; cnt--; }
            if (0 <= newx && newx < w) { sum += src[y * w + newx]; cnt++; }
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0, cnt = 0;
        for (int y = -r; y <= r; ++y) if (0 <= y && y < h) { sum += tmp[y * w + x]; cnt++; }
        for (int y = 0; y < h; ++y) {
            dst[y * w + x] = sum == cnt;
            int oldy = y - r;
            int newy = y + r + 1;
            if (0 <= oldy && oldy < h) { sum -= tmp[oldy * w + x]; cnt--; }
            if (0 <= newy && newy < h) { sum += tmp[newy * w + x]; cnt++; }
        }
    }
    return dst;
}

static std::vector<uint8_t> closeMask(std::vector<uint8_t> m, int w, int h, int k, int iters = 1) {
    for (int i = 0; i < iters; ++i) {
        m = dilate(m, w, h, k);
        m = erode(m, w, h, k);
    }
    return m;
}

static std::vector<uint8_t> openMask(std::vector<uint8_t> m, int w, int h, int k, int iters = 1) {
    for (int i = 0; i < iters; ++i) {
        m = erode(m, w, h, k);
        m = dilate(m, w, h, k);
    }
    return m;
}

static std::vector<Region> connectedRegions(const std::vector<uint8_t>& mask, int w, int h, int minPixels) {
    std::vector<uint8_t> work = mask;
    std::vector<Region> regions;
    std::vector<int> q;
    q.reserve(4096);

    for (int start = 0; start < w * h; ++start) {
        if (!work[start]) continue;
        int sx = start % w, sy = start / w;
        int minx = sx, maxx = sx, miny = sy, maxy = sy, pixels = 0;
        q.clear();
        q.push_back(start);
        work[start] = 0;

        for (size_t qi = 0; qi < q.size(); ++qi) {
            int p = q[qi], x = p % w, y = p / w;
            pixels++;
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);

            for (int dy = -1; dy <= 1; ++dy) {
                int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx;
                    if ((dx == 0 && dy == 0) || nx < 0 || nx >= w) continue;
                    int np = ny * w + nx;
                    if (work[np]) {
                        work[np] = 0;
                        q.push_back(np);
                    }
                }
            }
        }

        if (pixels >= minPixels) {
            Rect r{minx, miny, maxx - minx + 1, maxy - miny + 1};
            regions.push_back({r, pixels, (double)r.w / std::max(1, r.h), r.x + r.w / 2.0, r.y + r.h / 2.0});
        }
    }
    return regions;
}

static bool captureScreen(Frame& out) {
    gta5::capture::GameFrame captured;
    if (!gta5::capture::CaptureGameFrame(captured)) return false;
    gVirtualX = captured.screenX;
    gVirtualY = captured.screenY;
    gVirtualW = captured.screenW;
    gVirtualH = captured.screenH;
    out.x = captured.screenX; out.y = captured.screenY;
    out.w = captured.width; out.h = captured.height;
    out.screenW = captured.screenW; out.screenH = captured.screenH;
    out.toScreenX = captured.toScreenX;
    out.toScreenY = captured.toScreenY;
    const uint8_t* px = reinterpret_cast<const uint8_t*>(captured.bgra.data());
    out.bgra.assign(px, px + static_cast<size_t>(out.w) * out.h * 4);
    out.gray.resize(static_cast<size_t>(out.w) * out.h);
    for (int i = 0; i < out.w * out.h; ++i) {
        uint8_t b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
        out.gray[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    }
    return true;
}

static bool barMatches(const Region& b, Rect panel, double xmin, double xmax, double ymin, double ymax, double wmin, double wmax) {
    double rx = (b.rect.x - panel.x) / (double)panel.w;
    double ry = (b.rect.y - panel.y) / (double)panel.h;
    double rw = b.rect.w / (double)panel.w;
    return xmin <= rx && rx <= xmax && ymin <= ry && ry <= ymax && wmin <= rw && rw <= wmax;
}

static bool isTitleBarCandidate(const Frame& f, const Region& rg) {
    Rect r = rg.rect;
    double asp = r.w / (double)std::max(1, r.h);
    return r.w > scaledPx(f, 269)
        && r.h >= scaledPx(f, 7)
        && r.h <= scaledPx(f, 48)
        && asp > 5.0
        && rg.cy > f.h * 0.04
        && rg.cy < f.h * 0.86;
}

static bool extractTitleStripFromTallRegion(const Frame& f, const std::vector<uint8_t>& bright, const Region& rg, Region& out) {
    Rect r = rg.rect;
    if (!(r.w > scaledPx(f, 269) && r.h > scaledPx(f, 48) && r.h < scaledPx(f, 140))) return false;
    if (!(rg.cy > f.h * 0.04 && rg.cy < f.h * 0.86)) return false;

    const int rowOn = std::max(4, (int)std::lround(r.w * 0.012));
    const int maxGap = scaledPx(f, 4);
    const int maxSearchH = std::min(r.h, scaledPx(f, 62));
    int start = -1, end = -1, gap = 0;

    for (int yy = 0; yy < maxSearchH; ++yy) {
        int y = r.y + yy;
        int count = 0;
        const uint8_t* row = bright.data() + y * f.w + r.x;
        for (int x = 0; x < r.w; ++x) count += row[x] ? 1 : 0;

        if (count >= rowOn) {
            if (start < 0) start = yy;
            end = yy;
            gap = 0;
        } else if (start >= 0 && ++gap > maxGap) {
            break;
        }
    }

    if (start < 0 || end < start) return false;
    int y0 = r.y + start;
    int y1 = r.y + end;
    int minx = r.x + r.w, maxx = r.x - 1, pixels = 0;
    for (int y = y0; y <= y1; ++y) {
        const uint8_t* row = bright.data() + y * f.w;
        for (int x = r.x; x < r.x + r.w; ++x) {
            if (!row[x]) continue;
            minx = std::min(minx, x);
            maxx = std::max(maxx, x);
            ++pixels;
        }
    }
    if (maxx < minx) return false;

    Rect strip{minx, y0, maxx - minx + 1, y1 - y0 + 1};
    Region candidate{strip, pixels, strip.w / (double)std::max(1, strip.h), strip.x + strip.w / 2.0, strip.y + strip.h / 2.0};
    if (!isTitleBarCandidate(f, candidate)) return false;
    out = candidate;
    return true;
}

static RoiInfo detectMinigame(const Frame& f, std::string* diag = nullptr) {
    RoiInfo info;
    auto bright = thresholdBright(f.gray, f.w, f.h);
    int k = oddKernel(f.h, f.w, 0.0028);
    bright = closeMask(std::move(bright), f.w, f.h, k);
    auto regs = connectedRegions(bright, f.w, f.h, scaledArea(f, 1659));

    std::vector<Region> bars;
    for (const auto& rg : regs) {
        if (isTitleBarCandidate(f, rg)) {
            bars.push_back(rg);
            continue;
        }
        Region strip;
        if (extractTitleStripFromTallRegion(f, bright, rg, strip)) {
            bars.push_back(strip);
        }
    }

    auto right = [](Rect r) { return r.x + r.w; };
    auto bottom = [](Rect r) { return r.y + r.h; };
    auto closeEnough = [](double a, double b, double limit) { return std::abs(a - b) <= limit; };

    int bestScore = -1000000000;
    TitleBars bestBars;
    Rect bestPanel{};
    for (const auto& target : bars) {
        if (!(target.rect.w > scaledPx(f, 461) && target.rect.w < scaledPx(f, 1056))) continue;
        if (!(target.cy > f.h * 0.05 && target.cy < f.h * 0.20)) continue;

        for (const auto& components : bars) {
            if (&components == &target) continue;
            if (!(components.cx < target.cx - scaledPx(f, 384) &&
                  components.rect.w > scaledPx(f, 307) && components.rect.w < scaledPx(f, 730))) continue;
            if (!(components.cy > target.cy + f.h * 0.07 && components.cy < target.cy + f.h * 0.23)) continue;

            for (const auto& signals : bars) {
                if (&signals == &target || &signals == &components) continue;
                if (!(signals.cy > components.cy + f.h * 0.35 && signals.cy < f.h * 0.84)) continue;
                if (!closeEnough(signals.cx, target.cx, scaledPx(f, 230))) continue;
                if (!closeEnough(signals.rect.w, target.rect.w, scaledPx(f, 307))) continue;

                int left = components.rect.x - scaledPx(f, 36);
                int top = target.rect.y - scaledPx(f, 14);
                int panelRight = std::max(right(target.rect), right(signals.rect)) + scaledPx(f, 36);
                int panelBottom = bottom(signals.rect) + scaledPx(f, 170);
                Rect panel = clampRect({left, top, panelRight - left, panelBottom - top}, f.w, f.h);
                double panelAsp = panel.w / (double)std::max(1, panel.h);
                if (!(panel.w > scaledPx(f, 864) && panel.h > f.h * 0.45 &&
                      panelAsp > 1.0 && panelAsp < 2.0)) continue;

                int score = target.pixels + components.pixels + signals.pixels;
                score -= (int)std::lround(std::abs(signals.cx - target.cx) * 2.0);
                score -= (int)std::lround(
                    std::abs((target.cx - components.cx) - scaledPx(f, 595)) * 1.5);
                if (score > bestScore) {
                    bestScore = score;
                    bestBars.target = target.rect;
                    bestBars.components = components.rect;
                    bestBars.signals = signals.rect;
                    bestBars.hasTarget = bestBars.hasComponents = bestBars.hasSignals = true;
                    bestPanel = panel;
                }
            }
        }
    }

    if (bestScore > 0) {
        info.bars = bestBars;
        info.panel = bestPanel;
    } else if (diag) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "title layout failed bars=%zu", bars.size());
        *diag = buf;
        return info;
    };

    info.isMinigame = info.bars.hasTarget && info.bars.hasComponents && info.bars.hasSignals;
    if (diag) {
        char buf[256];
        std::snprintf(
            buf,
            sizeof(buf),
            "panel=%s title_bars=%zu timer=%d target=%d components=%d signals=%d",
            rectText(info.panel).c_str(),
            bars.size(),
            info.bars.hasTimer ? 1 : 0,
            info.bars.hasTarget ? 1 : 0,
            info.bars.hasComponents ? 1 : 0,
            info.bars.hasSignals ? 1 : 0
        );
        *diag = buf;
    }
    return info;
}

static std::vector<int> edgeLinePeaks(const std::vector<int>& projection, int base, int minDist, double thresholdRatio) {
    if (projection.empty()) return {};

    std::vector<int> smooth(projection.size(), 0);
    for (int i = 0; i < (int)projection.size(); ++i) {
        int sum = 0, count = 0;
        for (int d = -2; d <= 2; ++d) {
            int j = i + d;
            if (0 <= j && j < (int)projection.size()) {
                sum += projection[j];
                ++count;
            }
        }
        smooth[i] = sum / std::max(1, count);
    }

    int maxVal = *std::max_element(smooth.begin(), smooth.end());
    int threshold = (int)std::lround(maxVal * thresholdRatio);
    std::vector<std::pair<int, int>> candidates;
    for (int i = 1; i + 1 < (int)smooth.size(); ++i) {
        if (smooth[i] < threshold) continue;
        if (smooth[i] >= smooth[i - 1] && smooth[i] >= smooth[i + 1]) {
            candidates.push_back({smooth[i], i});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](auto a, auto b){ return a.first > b.first; });

    std::vector<int> chosen;
    for (auto [score, idx] : candidates) {
        bool isNear = false;
        for (int c : chosen) {
            if (std::abs(c - idx) < minDist) {
                isNear = true;
                break;
            }
        }
        if (!isNear) chosen.push_back(idx);
    }
    std::sort(chosen.begin(), chosen.end());
    for (int& v : chosen) v += base;
    return chosen;
}

static std::vector<std::pair<int, int>> selectGridBorderPairs(
        const std::vector<int>& peaks,
        int pairCount,
        int minSide,
        int maxSide,
        double expectedSide,
        double expectedGap) {
    std::vector<std::pair<int, int>> best;
    std::vector<std::pair<int, int>> current;
    double bestCost = 1e100;
    const int minGap = std::max(1, (int)std::lround(expectedGap * 0.25));
    const int maxGap = std::max(minGap, (int)std::lround(expectedGap * 2.5));

    std::function<void(int, double)> search = [&](int firstIndex, double cost) {
        if ((int)current.size() == pairCount) {
            if (cost < bestCost) {
                bestCost = cost;
                best = current;
            }
            return;
        }

        const int pairsLeft = pairCount - (int)current.size();
        if ((int)peaks.size() - firstIndex < pairsLeft * 2) return;

        for (int leftIndex = firstIndex; leftIndex + 1 < (int)peaks.size(); ++leftIndex) {
            if (!current.empty()) {
                const int gap = peaks[leftIndex] - current.back().second;
                if (gap < minGap) continue;
                if (gap > maxGap) break;
            }
            for (int rightIndex = leftIndex + 1; rightIndex < (int)peaks.size(); ++rightIndex) {
                const int side = peaks[rightIndex] - peaks[leftIndex];
                if (side < minSide) continue;
                if (side > maxSide) break;

                double nextCost = cost + std::pow((side - expectedSide) / expectedSide, 2.0);
                if (!current.empty()) {
                    const int gap = peaks[leftIndex] - current.back().second;
                    nextCost += std::pow((gap - expectedGap) / expectedGap, 2.0);
                }
                if (nextCost >= bestCost) continue;

                current.push_back({peaks[leftIndex], peaks[rightIndex]});
                search(rightIndex + 1, nextCost);
                current.pop_back();
            }
        }
    };

    search(0, 0.0);
    return best;
}

static bool detectComponentBoxesByBorder(const Frame& f, const RoiInfo& roi, std::vector<Rect>& components, std::string* diag = nullptr) {
    Rect cb = roi.bars.components;
    Rect p = roi.panel;
    int left = cb.x + (int)std::lround(cb.w * 0.08);
    int right = cb.x + (int)std::lround(cb.w * 0.86);
    int top = cb.y + cb.h + (int)std::lround(cb.h * 0.25);
    int bottom = p.y + (int)std::lround(p.h * 0.84);
    Rect search = clampRect({left, top, right - left, bottom - top}, f.w, f.h);
    if (search.w <= 0 || search.h <= 0) {
        if (diag) *diag = "border search empty";
        return false;
    }

    std::vector<int> vertical(search.w, 0), horizontal(search.h, 0);
    for (int y = search.y; y < search.y + search.h; ++y) {
        int row = y * f.w;
        for (int x = search.x + 1; x < search.x + search.w; ++x) {
            vertical[x - search.x] += std::abs((int)f.gray[row + x] - (int)f.gray[row + x - 1]);
        }
    }
    for (int y = search.y + 1; y < search.y + search.h; ++y) {
        int row = y * f.w;
        int prev = (y - 1) * f.w;
        for (int x = search.x; x < search.x + search.w; ++x) {
            horizontal[y - search.y] += std::abs((int)f.gray[row + x] - (int)f.gray[prev + x]);
        }
    }

    int minDist = scaledPx(f, 10);
    auto xs = edgeLinePeaks(vertical, search.x, minDist, 0.45);
    auto ys = edgeLinePeaks(horizontal, search.y, minDist, 0.45);

    int minSide = std::max(scaledPx(f, 45), (int)std::lround(cb.w * 0.16));
    int maxSide = std::max(minSide + 1, (int)std::lround(cb.w * 0.36));
    auto colPairs = selectGridBorderPairs(xs, 2, minSide, maxSide, cb.w * 0.255, cb.w * 0.05);

    if (colPairs.size() != 2) {
        if (diag) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "border columns failed xs=%zu pairs=%zu search=%s", xs.size(), colPairs.size(), rectText(search).c_str());
            *diag = buf;
        }
        return false;
    }

    double avgW = 0.0;
    for (auto [a, b] : colPairs) avgW += b - a;
    avgW /= colPairs.size();

    auto rowPairs = selectGridBorderPairs(
        ys,
        4,
        (int)std::lround(avgW * 0.72),
        (int)std::lround(avgW * 1.35),
        avgW,
        avgW * 0.20);

    if (rowPairs.size() != 4) {
        if (diag) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "border rows failed ys=%zu pairs=%zu search=%s", ys.size(), rowPairs.size(), rectText(search).c_str());
            *diag = buf;
        }
        return false;
    }

    components.clear();
    for (auto [y0, y1] : rowPairs) {
        for (auto [x0, x1] : colPairs) {
            components.push_back(clampRect({x0, y0, x1 - x0 + 1, y1 - y0 + 1}, f.w, f.h));
        }
    }

    if (diag) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "border boxes=8 search=%s side=%.1f", rectText(search).c_str(), avgW);
        *diag = buf;
    }
    return components.size() == 8;
}

static bool detectRois(const Frame& f, const RoiInfo& roi, Rect& target, std::vector<Rect>& components, std::string* diag = nullptr) {
    Rect cb = roi.bars.components, tb = roi.bars.target, sb = roi.bars.signals, p = roi.panel;

    int targetTop = tb.y + tb.h + (int)std::lround(tb.h * 0.35);
    int targetBottom = sb.y - (int)std::lround(tb.h * 0.55);
    int targetLeft = tb.x + (int)std::lround(tb.w * 0.12);
    int targetRight = tb.x + (int)std::lround(tb.w * 0.72);
    target = clampRect({targetLeft, targetTop, targetRight - targetLeft, targetBottom - targetTop}, f.w, f.h);

    std::string compDiag;
    if (!detectComponentBoxesByBorder(f, roi, components, &compDiag)) {
        if (diag) *diag = "component ROI failed: " + compDiag;
        return false;
    }
    if (target.w <= 0 || target.h <= 0 || components.size() != 8) {
        if (diag) *diag = "component ROI failed: " + compDiag;
        return false;
    }

    if (diag) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "target=%s components=8 source=border %s",
                      rectText(target).c_str(),
                      compDiag.c_str());
        *diag = buf;
    }
    return true;
}

static bool isWhiteUiGray(uint8_t gray) {
    return gray >= 175;
}

static bool isSuccessPanelVisible(const Frame& f, const RoiInfo& roi, std::string* diag = nullptr) {
    if (roi.panel.w <= 0 || roi.panel.h <= 0) {
        if (diag) *diag = "success check skipped: no panel";
        return false;
    }

    // Same idea as GTAscript2: sample the expected SIGNAL MATCH popup area and
    // classify it by white UI pixels. This tool anchors that area from the
    // detected main panel because the popup spans both left and right panes.
    Rect p = roi.panel;
    int left = p.x + (int)(p.w * 0.27);
    int top = p.y + (int)(p.h * 0.40);
    int right = p.x + (int)(p.w * 0.75);
    int bottom = p.y + (int)(p.h * 0.60);
    Rect r = clampRect({left, top, right - left, bottom - top}, f.w, f.h);

    int white = 0;
    int samples = 0;
    int step = std::max(1, (int)std::lround(roi.panel.w / 314.0));
    for (int y = r.y; y <= r.y + r.h; y += step) {
        for (int x = r.x; x <= r.x + r.w; x += step) {
            if (isWhiteUiGray(f.gray[y * f.w + x])) ++white;
            ++samples;
        }
    }

    int pct = samples > 0 ? white * 100 / samples : 0;
    if (diag) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "success pct=%d rect=%s step=%d", pct, rectText(r).c_str(), step);
        *diag = buf;
    }
    return pct >= 4;
}

static std::vector<uint8_t> resizeMaskNearest(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst(dw * dh);
    for (int y = 0; y < dh; ++y) {
        int sy = std::min(sh - 1, (int)((int64_t)y * sh / dh));
        for (int x = 0; x < dw; ++x) {
            int sx = std::min(sw - 1, (int)((int64_t)x * sw / dw));
            dst[y * dw + x] = src[sy * sw + sx];
        }
    }
    return dst;
}

static void trimMask(std::vector<uint8_t>& m, int& w, int& h) {
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (m[y * w + x]) {
        minx = std::min(minx, x); maxx = std::max(maxx, x);
        miny = std::min(miny, y); maxy = std::max(maxy, y);
    }
    if (maxx < minx) return;
    int nw = maxx - minx + 1, nh = maxy - miny + 1;
    std::vector<uint8_t> out(nw * nh);
    for (int y = 0; y < nh; ++y) memcpy(out.data() + y * nw, m.data() + (miny + y) * w + minx, nw);
    m.swap(out); w = nw; h = nh;
}

struct IntegralMask {
    int w = 0, h = 0;
    std::vector<int> sum;

    int rectSum(int x, int y, int rw, int rh) const {
        int stride = w + 1;
        int x1 = x + rw, y1 = y + rh;
        return sum[y1 * stride + x1] - sum[y * stride + x1] - sum[y1 * stride + x] + sum[y * stride + x];
    }
};

static IntegralMask makeIntegralMask(const std::vector<uint8_t>& mask, int w, int h) {
    IntegralMask out;
    out.w = w;
    out.h = h;
    out.sum.assign((w + 1) * (h + 1), 0);
    int stride = w + 1;
    for (int y = 0; y < h; ++y) {
        int rowSum = 0;
        for (int x = 0; x < w; ++x) {
            rowSum += mask[y * w + x] ? 1 : 0;
            out.sum[(y + 1) * stride + x + 1] = out.sum[y * stride + x + 1] + rowSum;
        }
    }
    return out;
}

static double matchScoreCoarse(const std::vector<uint8_t>& target, const IntegralMask& targetIntegral, int tw, int th, const std::vector<uint8_t>& comp, int cw, int ch) {
    if (cw >= tw || ch >= th) return -1.0;

    std::vector<int> compOnes;
    compOnes.reserve(comp.size() / 4);
    for (int i = 0; i < (int)comp.size(); ++i) {
        if (comp[i]) compOnes.push_back(i);
    }

    int n = cw * ch;
    double csum = (double)compOnes.size();
    double cvar = csum - (csum * csum / n);
    if (cvar < 1e-6) return -1.0;

    double best = -1.0;
    int step = tw <= 100 ? 1 : 2;
    for (int y0 = 0; y0 <= th - ch; y0 += step) {
        for (int x0 = 0; x0 <= tw - cw; x0 += step) {
            double tsum = (double)targetIntegral.rectSum(x0, y0, cw, ch);
            double tvar = tsum - (tsum * tsum / n);
            if (tvar <= 1e-6) continue;

            int tcross = 0;
            for (int off : compOnes) {
                int y = off / cw;
                int x = off - y * cw;
                tcross += target[(y0 + y) * tw + x0 + x] ? 1 : 0;
            }
            double dot = tcross - (tsum * csum / n);
            best = std::max(best, dot / std::sqrt(tvar * cvar));
        }
    }
    return best;
}

static std::vector<double> answerScores(const Frame& f, Rect target, const std::vector<Rect>& components) {
    int targetInset = std::max(1, (int)std::lround(std::min(target.w, target.h) * 0.02));
    target = clampRect(
        {target.x + targetInset, target.y + targetInset,
         target.w - targetInset * 2, target.h - targetInset * 2},
        f.w, f.h);
    auto tm = fingerprintMask(f, target);
    int down = 3;
    int dtw = std::max(1, target.w / down), dth = std::max(1, target.h / down);
    tm = resizeMaskNearest(tm, target.w, target.h, dtw, dth);
    auto targetIntegral = makeIntegralMask(tm, dtw, dth);

    struct ComponentMask {
        std::vector<uint8_t> pixels;
        int w = 0;
        int h = 0;
    };

    std::vector<ComponentMask> masks;
    masks.reserve(components.size());
    for (Rect cr : components) {
        int inset = std::max(1, (int)std::lround(std::min(cr.w, cr.h) * 0.05));
        cr = clampRect({cr.x + inset, cr.y + inset, cr.w - inset * 2, cr.h - inset * 2}, f.w, f.h);
        auto cm = fingerprintMask(f, cr);
        int cw = cr.w, ch = cr.h;
        trimMask(cm, cw, ch);
        int baseW = std::max(1, cw / down), baseH = std::max(1, ch / down);
        cm = resizeMaskNearest(cm, cw, ch, baseW, baseH);
        trimMask(cm, baseW, baseH);
        masks.push_back({std::move(cm), baseW, baseH});
    }

    // All eight candidates are rendered from the same source sheet, so the
    // four matching fragments must share one scale. Allowing every candidate
    // to choose its own scale lets a decoy imitate unrelated target ridges.
    const double scales[] = {1.0, 1.15, 1.3, 1.45};
    std::vector<double> scores(masks.size(), -1.0);
    double bestScaleScore = -1e100;
    for (double scale : scales) {
        std::vector<double> scaleScores;
        scaleScores.reserve(masks.size());
        for (const auto& mask : masks) {
            int sw = std::max(scaledPx(f, 8), (int)std::lround(mask.w * scale));
            int sh = std::max(scaledPx(f, 8), (int)std::lround(mask.h * scale));
            auto sm = resizeMaskNearest(mask.pixels, mask.w, mask.h, sw, sh);
            scaleScores.push_back(matchScoreCoarse(tm, targetIntegral, dtw, dth, sm, sw, sh));
        }

        auto ranked = scaleScores;
        std::sort(ranked.begin(), ranked.end(), std::greater<double>());
        double scaleScore = std::accumulate(
            ranked.begin(), ranked.begin() + std::min<size_t>(4, ranked.size()), 0.0);
        if (scaleScore > bestScaleScore) {
            bestScaleScore = scaleScore;
            scores = std::move(scaleScores);
        }
    }
    return scores;
}

static uint64_t targetFingerprintHash(const Frame& f, Rect target) {
    int inset = std::max(1, (int)std::lround(std::min(target.w, target.h) * 0.02));
    target = clampRect(
        {target.x + inset, target.y + inset,
         target.w - inset * 2, target.h - inset * 2},
        f.w, f.h);
    uint64_t hash = 1469598103934665603ull;
    int stepX = std::max(1, target.w / 48);
    int stepY = std::max(1, target.h / 64);

    for (int y = target.y; y < target.y + target.h; y += stepY) {
        for (int x = target.x; x < target.x + target.w; x += stepX) {
            uint8_t g = f.gray[y * f.w + x];
            uint8_t bit = ((g >= 25 && g <= 180) || g >= 185) ? 1 : 0;
            hash ^= bit;
            hash *= 1099511628211ull;
        }
    }

    hash ^= (uint64_t)target.w * 1000003ull;
    hash ^= (uint64_t)target.h * 9176ull;
    return hash;
}

static void markStates(const Frame& f, std::vector<BlockInfo>& blocks) {
    int bestCursor = -1, bestScore = -1;
    double bestCursorRatio = 0.0;
    std::vector<int> cursorScores;
    for (auto& b : blocks) {
        Rect selectedRect = padRect(b.rect, f.w, f.h, 0.03);
        auto selectedGray = cropGray(f, selectedRect);
        int bright = 0;
        for (uint8_t v : selectedGray) if (v >= 185) bright++;
        b.selected = (bright / (double)std::max(1, selectedRect.w * selectedRect.h)) >= 0.12;

        Rect cursorRect = padRect(b.rect, f.w, f.h, 0.12);
        auto g = cropGray(f, cursorRect);
        int len = std::max(scaledPx(f, 10), (int)std::lround(std::min(cursorRect.w, cursorRect.h) * 0.22));
        int band = std::max(scaledPx(f, 3), (int)std::lround(std::min(cursorRect.w, cursorRect.h) * 0.045));
        auto at = [&](int x, int y){ return g[y * cursorRect.w + x] >= 185 ? 1 : 0; };
        int score = 0;
        for (int y = 0; y < band; ++y) for (int x = 0; x < len; ++x) score += at(x, y) + at(cursorRect.w - 1 - x, y);
        for (int y = 0; y < len; ++y) for (int x = 0; x < band; ++x) score += at(x, y) + at(cursorRect.w - 1 - x, y);
        for (int y = 0; y < band; ++y) for (int x = 0; x < len; ++x) score += at(x, cursorRect.h - 1 - y) + at(cursorRect.w - 1 - x, cursorRect.h - 1 - y);
        for (int y = 0; y < len; ++y) for (int x = 0; x < band; ++x) score += at(x, cursorRect.h - 1 - y) + at(cursorRect.w - 1 - x, cursorRect.h - 1 - y);
        cursorScores.push_back(score);
        b.cursorScore = score;
        double maxScore = std::max(1, 8 * len * band);
        double ratio = score / maxScore;
        if (score > bestScore) { bestScore = score; bestCursor = b.index; bestCursorRatio = ratio; }
    }
    if (bestCursor > 0 && bestCursorRatio >= 0.04) {
        for (auto& b : blocks) b.cursor = b.index == bestCursor;
    }
}

static void resetAutomation(AutomationState& aut) {
    gta5::input::CancelAll();
    aut = {};
}

static bool allBlocksCorrectlySelected(const OverlayState& state) {
    if (!state.visible || state.blocks.size() != 8) return false;
    for (const auto& b : state.blocks) {
        if (b.selected != b.correct) return false;
    }
    return true;
}

static int cursorIndex(const OverlayState& state) {
    for (const auto& b : state.blocks) {
        if (b.cursor) return b.index;
    }
    return -1;
}

static void appendCursorMoves(int& cur, int target, std::vector<gta5::input::Key>& keys) {
    int curRow = (cur - 1) / 2;
    int curCol = (cur - 1) % 2;
    int targetRow = (target - 1) / 2;
    int targetCol = (target - 1) % 2;

    while (curCol < targetCol) {
        keys.push_back({0x4D, true});
        ++curCol;
    }
    while (curCol > targetCol) {
        keys.push_back({0x4B, true});
        --curCol;
    }
    while (curRow < targetRow) {
        keys.push_back({0x50, true});
        ++curRow;
    }
    while (curRow > targetRow) {
        keys.push_back({0x48, true});
        --curRow;
    }
    cur = target;
}

static void planAndRunAutomation(const OverlayState& state, AutomationState& aut, std::string* diag = nullptr) {
    if (!state.visible) {
        resetAutomation(aut);
        if (diag) *diag = "auto reset";
        return;
    }

    if (aut.inputJob) {
        if (aut.inputJob.Pending()) {
            if (diag) *diag = "auto input queued";
            return;
        }
        if (!aut.inputJob.Succeeded()) {
            aut = {};
            if (diag) *diag = "auto input canceled or failed";
            return;
        }
        aut.inputJob = {};
        aut.submitted = true;
        if (diag) *diag = "auto plan executed";
        return;
    }

    if (aut.submitted) {
        if (aut.plannedHash != state.targetHash) {
            aut.submitted = false;
            aut.plannedHash = 0;
        } else {
            aut.submitted = false;
            if (diag) *diag = "auto retry after submit without success";
        }
    }

    int cur = cursorIndex(state);
    if (cur < 1 || state.blocks.size() != 8) {
        if (diag) *diag = "auto waiting cursor";
        return;
    }

    std::vector<bool> selected(9, false);
    std::vector<bool> correct(9, false);
    for (const auto& b : state.blocks) {
        if (b.index >= 1 && b.index <= 8) {
            selected[b.index] = b.selected;
            correct[b.index] = b.correct;
        }
    }

    int toggles = 0;
    std::vector<gta5::input::Key> keys;
    for (int target = 1; target <= 8; ++target) {
        if (selected[target] == correct[target]) continue;
        appendCursorMoves(cur, target, keys);
        keys.push_back({0x1C, false});
        selected[target] = !selected[target];
        ++toggles;
    }

    std::vector<gta5::input::Command> commands;
    commands.reserve(keys.size() + 1);
    for (const auto& key : keys) commands.push_back({key, gta5::input::Action::Tap});
    commands.push_back({{0x0F, false}, gta5::input::Action::LongPress});
    aut.inputJob = gta5::input::QueueSequence(commands);
    aut.plannedHash = state.targetHash;

    if (diag) {
        *diag = "auto plan queued toggles=" + std::to_string(toggles) + " tab=2s";
    }
}

static OverlayState analyzeFrame(const Frame& f, SolverCache& cache, std::string* diag = nullptr, FrameTiming* timing = nullptr) {
    auto analyzeStart = Clock::now();
    OverlayState os;
    std::string minigameDiag;
    auto phaseStart = Clock::now();
    RoiInfo roi = detectMinigame(f, &minigameDiag);
    if (timing) {
        timing->gateMs = msSince(phaseStart);
        timing->minigame = roi.isMinigame;
    }
    if (!roi.isMinigame) {
        cache = {};
        if (diag) *diag = "not in minigame: " + minigameDiag;
        if (timing) timing->analyzeMs = msSince(analyzeStart);
        return os;
    }

    std::string successDiag;
    phaseStart = Clock::now();
    if (isSuccessPanelVisible(f, roi, &successDiag)) {
        cache = {};
        if (diag) *diag = "success panel visible: " + successDiag;
        if (timing) {
            timing->successMs = msSince(phaseStart);
            timing->success = true;
            timing->analyzeMs = msSince(analyzeStart);
        }
        return os;
    }
    if (timing) timing->successMs = msSince(phaseStart);

    Rect target;
    std::vector<Rect> comps;
    std::string roiDiag;
    phaseStart = Clock::now();
    if (!detectRois(f, roi, target, comps, &roiDiag)) {
        cache = {};
        if (diag) *diag = "minigame found, ROI failed: " + roiDiag;
        if (timing) {
            timing->roiMs = msSince(phaseStart);
            timing->analyzeMs = msSince(analyzeStart);
        }
        return os;
    }
    if (timing) timing->roiMs = msSince(phaseStart);

    phaseStart = Clock::now();
    uint64_t targetHash = targetFingerprintHash(f, target);
    if (timing) timing->hashMs = msSince(phaseStart);
    bool cacheHit = cache.valid
        && cache.targetHash == targetHash
        && cache.baseBlocks.size() == comps.size()
        && cache.target.x == target.x
        && cache.target.y == target.y
        && cache.target.w == target.w
        && cache.target.h == target.h;
    if (timing) timing->cacheHit = cacheHit;

    if (!cacheHit) {
        phaseStart = Clock::now();
        auto scores = answerScores(f, target, comps);
        if (timing) timing->answerMs = msSince(phaseStart);
        std::vector<int> order(scores.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b){ return scores[a] > scores[b]; });

        cache = {};
        cache.valid = true;
        cache.targetHash = targetHash;
        cache.target = target;
        for (int i = 0; i < (int)comps.size(); ++i) {
            BlockInfo b;
            b.index = i + 1;
            b.rect = comps[i];
            b.score = scores[i];
            b.correct = std::find(order.begin(), order.begin() + std::min(4, (int)order.size()), i) != order.begin() + std::min(4, (int)order.size());
            cache.baseBlocks.push_back(b);
        }
    }

    os.visible = true;
    os.targetHash = targetHash;
    os.target = target;
    os.blocks = cache.baseBlocks;
    phaseStart = Clock::now();
    markStates(f, os.blocks);
    if (timing) timing->stateMs = msSince(phaseStart);
    if (diag) {
        std::string correct;
        for (const auto& b : os.blocks) {
            if (b.correct) {
                if (!correct.empty()) correct += ",";
                correct += std::to_string(b.index);
            }
        }
        *diag = std::string("overlay updated: ") + (cacheHit ? "cache-hit " : "cache-miss ")
            + roiDiag + " correct=" + correct;
    }
    if (timing) timing->analyzeMs = msSince(analyzeStart);
    return os;
}

static void publishState(const OverlayState& s) {
    EnterCriticalSection(&gStateLock);
    gState = s;
    LeaveCriticalSection(&gStateLock);
    if (gOverlayWnd) {
        if (GetCurrentThreadId() == gUiThreadId) {
            RedrawWindow(gOverlayWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        } else {
            InvalidateRect(gOverlayWnd, nullptr, FALSE);
        }
    }
}

#ifdef CLI_TEST
static bool loadPngFrame(const wchar_t* path, Frame& out) {
    Gdiplus::Bitmap bitmap(path);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;

    int w = (int)bitmap.GetWidth();
    int h = (int)bitmap.GetHeight();
    Gdiplus::Rect lockRect(0, 0, w, h);
    Gdiplus::BitmapData data{};
    Gdiplus::Status st = bitmap.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data);
    if (st != Gdiplus::Ok) return false;

    out.x = 0;
    out.y = 0;
    out.w = w;
    out.h = h;
    out.bgra.assign(w * h * 4, 0);
    out.gray.assign(w * h, 0);

    for (int y = 0; y < h; ++y) {
        uint8_t* row = (uint8_t*)data.Scan0 + y * data.Stride;
        for (int x = 0; x < w; ++x) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            int p = y * w + x;
            out.bgra[p * 4 + 0] = b;
            out.bgra[p * 4 + 1] = g;
            out.bgra[p * 4 + 2] = r;
            out.bgra[p * 4 + 3] = 255;
            out.gray[p] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    bitmap.UnlockBits(&data);
    return true;
}

static int runCli(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: gta_fingerprint_cli.exe image.png\n");
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        fprintf(stderr, "GDI+ startup failed\n");
        return 1;
    }

    Frame f;
    if (!loadPngFrame(argv[1], f)) {
        fwprintf(stderr, L"failed to load image: %ls\n", argv[1]);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    printf("image: %dx%d\n", f.w, f.h);

    bool runTwice = argc >= 3 && std::wstring(argv[2]) == L"--twice";
    if (runTwice) {
        SolverCache cache;
        std::string d1, d2;
        analyzeFrame(f, cache, &d1);
        analyzeFrame(f, cache, &d2);
        printf("cache_test_1: %s\n", d1.c_str());
        printf("cache_test_2: %s\n", d2.c_str());
    }

    std::string gateDiag;
    RoiInfo roi = detectMinigame(f, &gateDiag);
    printf("gate: %s\n", roi.isMinigame ? "true" : "false");
    printf("gate_diag: %s\n", gateDiag.c_str());
    if (!roi.isMinigame) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 2;
    }

    std::string successDiag;
    bool successVisible = isSuccessPanelVisible(f, roi, &successDiag);
    printf("success: %s\n", successVisible ? "true" : "false");
    printf("success_diag: %s\n", successDiag.c_str());
    if (successVisible) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 0;
    }

    Rect target;
    std::vector<Rect> comps;
    std::string roiDiag;
    bool roiOk = detectRois(f, roi, target, comps, &roiDiag);
    printf("roi: %s\n", roiOk ? "true" : "false");
    printf("roi_diag: %s\n", roiDiag.c_str());
    if (!roiOk) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 3;
    }

    auto scores = answerScores(f, target, comps);
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b){ return scores[a] > scores[b]; });

    std::vector<BlockInfo> blocks;
    for (int i = 0; i < (int)comps.size(); ++i) {
        BlockInfo b;
        b.index = i + 1;
        b.rect = comps[i];
        b.score = scores[i];
        b.correct = std::find(order.begin(), order.begin() + std::min(4, (int)order.size()), i) != order.begin() + std::min(4, (int)order.size());
        blocks.push_back(b);
    }
    markStates(f, blocks);

    printf("target: %s\n", rectText(target).c_str());
    printf("blocks:\n");
    for (const auto& b : blocks) {
        printf(
            "  #%d rect=%s score=%.3f correct=%d cursor=%d selected=%d cursor_score=%d\n",
            b.index,
            rectText(b.rect).c_str(),
            b.score,
            b.correct ? 1 : 0,
            b.cursor ? 1 : 0,
            b.selected ? 1 : 0,
            b.cursorScore
        );
    }

    printf("correct:");
    for (const auto& b : blocks) if (b.correct) printf(" %d", b.index);
    printf("\n");
    printf("cursor:");
    bool anyCursor = false;
    for (const auto& b : blocks) if (b.cursor) { printf(" %d", b.index); anyCursor = true; }
    if (!anyCursor) printf(" none");
    printf("\nselected:");
    bool anySelected = false;
    for (const auto& b : blocks) if (b.selected) { printf(" %d", b.index); anySelected = true; }
    if (!anySelected) printf(" none");
    printf("\n");

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
#endif

static void workerLoop() {
    postLog("worker started");
    int frameNo = 0;
    bool lastMinigame = false;
    bool lastSuccess = false;
    bool captureFailed = false;
    SolverCache cache;
    AutomationState automation;
    while (gRunning.load()) {
        auto loopStart = Clock::now();
        FrameTiming timing;
        Frame f;
        auto phaseStart = Clock::now();
        if (captureScreen(f)) {
            timing.captureMs = msSince(phaseStart);
            if (frameNo == 0) {
                postLog("capture " + std::to_string(f.w) + "x" + std::to_string(f.h) + " origin=(0,0)");
            }
            if (captureFailed) {
                postLog("capture recovered");
                captureFailed = false;
            }
            OverlayState state = analyzeFrame(f, cache, nullptr, &timing);
            std::string autoDiag;
            phaseStart = Clock::now();
            publishState(scaleOverlayStateToScreen(f, state));
            timing.publishMs = msSince(phaseStart);
            phaseStart = Clock::now();
            planAndRunAutomation(state, automation, &autoDiag);
            timing.autoMs = msSince(phaseStart);
            timing.totalMs = msSince(loopStart);

            if (timing.minigame && !lastMinigame) {
                postLog("minigame detected");
            } else if (!timing.minigame && lastMinigame) {
                postLog("minigame lost");
            }

            if (timing.success && !lastSuccess) {
                postLog("success panel detected");
            }

            if (state.visible && !timing.cacheHit) {
                postLog("answer ready: correct=" + correctList(state)
                    + " target=" + rectText(state.target)
                    + " | " + timingText(timing));
            }

            if (autoDiag.find("executed") != std::string::npos) {
                postLog(autoDiag);
            }
            lastMinigame = timing.minigame;
            lastSuccess = timing.success;
        } else {
            if (!captureFailed) {
                postLog("screen capture failed");
                captureFailed = true;
            }
        }
        frameNo++;
        Sleep(kFrameDelayMs);
    }
    resetAutomation(automation);
    publishState({});
    postLog("worker stopped");
}

static void drawOverlay(HDC hdc) {
    OverlayState s;
    EnterCriticalSection(&gStateLock);
    s = gState;
    LeaveCriticalSection(&gStateLock);

    RECT rc{0, 0, gVirtualW, gVirtualH};
    HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, clearBrush);
    DeleteObject(clearBrush);

    if (!s.visible) return;
    int refBlock = s.blocks.empty() ? std::max(1, s.target.w / 3) : std::max(1, s.blocks.front().rect.w);
    int blockPen = std::max(1, (int)std::lround(refBlock / 60.0));
    int targetPen = blockPen;
    int dot = std::max(4, (int)std::lround(refBlock * 0.10));
    int dotMargin = std::max(2, (int)std::lround(refBlock * 0.05));
    HPEN green = CreatePen(PS_SOLID, blockPen, RGB(0, 255, 0));
    HPEN red = CreatePen(PS_SOLID, blockPen, RGB(255, 50, 50));
    HPEN magenta = CreatePen(PS_SOLID, targetPen, RGB(255, 0, 255));
    HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    HBRUSH yellow = CreateSolidBrush(RGB(255, 230, 0));
    HBRUSH pink = CreateSolidBrush(RGB(255, 70, 190));
    SelectObject(hdc, hollow);
    SetBkMode(hdc, TRANSPARENT);

    SelectObject(hdc, magenta);
    Rectangle(
        hdc,
        s.target.x - gVirtualX,
        s.target.y - gVirtualY,
        s.target.x + s.target.w - gVirtualX,
        s.target.y + s.target.h - gVirtualY);

    for (const auto& b : s.blocks) {
        int x = b.rect.x - gVirtualX, y = b.rect.y - gVirtualY;
        SelectObject(hdc, b.correct ? green : red);
        Rectangle(hdc, x, y, x + b.rect.w, y + b.rect.h);
        if (b.cursor) {
            SelectObject(hdc, pink);
            int markerTop = y + (b.rect.h - dot) / 2;
            int markerGap = std::max(dot, dotMargin * 2);
            if (b.selected) markerGap += dot + dotMargin;
            bool leftColumn = b.index % 2 == 1;
            int markerLeft = leftColumn
                ? x - markerGap - dot
                : x + b.rect.w + markerGap;
            Ellipse(hdc, markerLeft, markerTop, markerLeft + dot, markerTop + dot);
            SelectObject(hdc, hollow);
        }
        if (b.selected) {
            SelectObject(hdc, yellow);
            int markerTop = y + (b.rect.h - dot) / 2;
            int markerGap = std::max(dot, dotMargin * 2);
            bool leftColumn = b.index % 2 == 1;
            int markerLeft = leftColumn
                ? x - markerGap - dot
                : x + b.rect.w + markerGap;
            Ellipse(hdc, markerLeft, markerTop, markerLeft + dot, markerTop + dot);
            SelectObject(hdc, hollow);
        }
    }

    DeleteObject(green); DeleteObject(red); DeleteObject(magenta); DeleteObject(yellow); DeleteObject(pink);
}

static LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            drawOverlay(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void updateButton(HWND hwnd) {
    SetDlgItemTextW(hwnd, 1001, gStopping.load() ? L"Stopping" : (gRunning.load() ? L"Stop" : L"Start"));
    HWND btn = GetDlgItem(hwnd, 1001);
    if (btn) EnableWindow(btn, !gStopping.load());
}

static void startWorker(HWND hwnd) {
    if (gStopping.load()) return;
    if (gRunning.exchange(true)) return;
    postLog("start requested");
    if (gOverlayWnd) {
        SetWindowPos(gOverlayWnd, HWND_TOPMOST, gVirtualX, gVirtualY, gVirtualW, gVirtualH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        publishState({});
    }
    gWorker = std::thread(workerLoop);
    updateButton(hwnd);
}

static void stopWorker(HWND hwnd) {
    if (!gRunning.exchange(false)) return;
    gStopping.store(true);
    postLog("stop requested");
    publishState({});
    updateButton(hwnd);
    if (gWorker.joinable()) {
        std::thread oldWorker = std::move(gWorker);
        std::thread joiner([worker = std::move(oldWorker)]() mutable {
            if (worker.joinable()) worker.join();
            gStopping.store(false);
            if (gMainWnd) PostMessageW(gMainWnd, WM_APP_WORKER_STOPPED, 0, 0);
        });
        joiner.detach();
    } else {
        gStopping.store(false);
        updateButton(hwnd);
    }
}

static LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            CreateWindowW(L"BUTTON", L"Start", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 16, 16, 120, 36, hwnd, (HMENU)1001, GetModuleHandle(nullptr), nullptr);
            gStatusText = CreateWindowW(L"STATIC", L"Idle", WS_VISIBLE | WS_CHILD, 150, 24, 430, 24, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
            gLogList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                       16, 64, 560, 260, hwnd, (HMENU)1002, GetModuleHandle(nullptr), nullptr);
            postLog("app ready");
            return 0;
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> text((std::wstring*)lp);
            if (gLogList) {
                int index = (int)SendMessageW(gLogList, LB_ADDSTRING, 0, (LPARAM)text->c_str());
                SendMessageW(gLogList, LB_SETTOPINDEX, std::max(0, index - 14), 0);
                int count = (int)SendMessageW(gLogList, LB_GETCOUNT, 0, 0);
                while (count > 200) {
                    SendMessageW(gLogList, LB_DELETESTRING, 0, 0);
                    count--;
                }
            }
            if (gStatusText) SetWindowTextW(gStatusText, text->c_str());
            std::ofstream log("debug.log", std::ios::app);
            std::string narrow;
            narrow.reserve(text->size());
            for (wchar_t ch : *text) narrow.push_back(static_cast<char>(ch));
            log << narrow << "\n";
            return 0;
        }
        case WM_APP_WORKER_STOPPED:
            publishState({});
            updateButton(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == 1001) {
                if (gRunning.load()) stopWorker(hwnd); else startWorker(hwnd);
            }
            return 0;
        case WM_DESTROY:
            gRunning.store(false);
            if (gWorker.joinable()) gWorker.join();
            while (gStopping.load()) Sleep(10);
            publishState({});
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}


bool DetectInGame() { Frame f; if (!captureScreen(f)) return false; std::string diag; return detectMinigame(f, &diag).isMinigame; }
HWND OverlayWindow() { return gOverlayWnd; }
void SetOverlayWindow(HWND hwnd) { gOverlayWnd = hwnd; }
void SetUiThread() { gUiThreadId = GetCurrentThreadId(); }
void InitStateLock() { InitializeCriticalSection(&gStateLock); }
void DeleteStateLock() { DeleteCriticalSection(&gStateLock); }
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return overlayProc(hwnd, msg, wp, lp); }
void ClearOverlay() { publishState({}); if (gOverlayWnd) ShowWindow(gOverlayWnd, SW_HIDE); }
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
    if (!gOverlayWnd) return;
    if (overlayEnabled()) {
      SetWindowPos(gOverlayWnd, HWND_TOPMOST, gVirtualX, gVirtualY, gVirtualW, gVirtualH,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      publishState({});
      ShowWindow(gOverlayWnd, SW_HIDE);
    }
  };
  SolverCache cache;
  AutomationState automation;
  gRunning.store(true);
  RECT game{};
  if (!gta5::capture::GetGameClientRect(game)) {
    gRunning.store(false);
    return false;
  }
  gVirtualX = game.left; gVirtualY = game.top;
  gVirtualW = game.right - game.left; gVirtualH = game.bottom - game.top;
  syncOverlay();
  bool completedAnyLevel = false;
  int lostFrames = 0;
  setStatus(L"fingerprint: locating");
  while (!stopRequested()) {
    syncOverlay();
    Frame frame; FrameTiming timing;
    if (!captureScreen(frame)) { Sleep(30); continue; }
    OverlayState state = analyzeFrame(frame, cache, nullptr, &timing);
    if (!timing.minigame) {
      if (++lostFrames >= 15) {
        setStatus(L"fingerprint: exited");
        break;
      }
      setStatus(L"fingerprint: confirming exit");
      resetAutomation(automation);
      publishState({});
      Sleep(50);
      continue;
    }
    lostFrames = 0;
    if (timing.success) {
      completedAnyLevel = true;
      setStatus(L"fingerprint: level complete");
      resetAutomation(automation);
      cache = {};
      publishState({});
      Sleep(120);
      continue;
    }
    publishState(overlayEnabled() ? scaleOverlayStateToScreen(frame, state) : OverlayState{});
    std::string autoDiag;
    setStatus(state.visible ? L"fingerprint: auto input" : L"fingerprint: locating");
    planAndRunAutomation(state, automation, &autoDiag);
    if (automation.submitted) {
      completedAnyLevel = true;
      setStatus(L"fingerprint: waiting next level");
    }
    Sleep(kFrameDelayMs);
  }
  gRunning.store(false);
  resetAutomation(automation);
  ClearOverlay();
  return completedAnyLevel;
}

}  // namespace gta5::games::choose_fingerprint
