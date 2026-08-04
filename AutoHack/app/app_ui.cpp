#define NOMINMAX
#include "app_ui.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <fstream>
#include <unordered_map>  // 添加这个头文件

namespace gta5::app::ui {
    namespace {

        // 直接嵌入英文文本
        std::wstring T(const char* key) {
            static const std::unordered_map<std::string, std::wstring> map = {
                {"state.on", L"ON"},
                {"state.off", L"OFF"},
                {"status.idle", L"Idle"},
                {"status.starting", L"Starting..."},
                {"status.stopping", L"Stopping..."},
                {"status.stopped", L"Stopped"},
                {"status.search_game", L"Searching for GTA5..."},
                {"status.search_minigame", L"Searching minigame..."},
                {"status.running", L"Running"},
                {"status.detect_timeout", L"No minigame detected"},
                {"status.game_timeout", L"GTA5 window not found"},
                {"status.completed", L"Completed"},
                {"status.slider_latency", L"Analysis latency too high"},
                {"status.capture_failed", L"Capture failed"},
                {"status.slider_timeout", L"Active bar timeout"},
                {"status.waiting_yellow", L"Waiting for yellow outline"},
                {"status.in_minigame", L"In minigame"},
                {"status.fingerprint_locating", L"Fingerprint: locating"},
                {"status.fingerprint_exited", L"Fingerprint: exited"},
                {"status.fingerprint_confirm_exit", L"Fingerprint: confirming exit"},
                {"status.fingerprint_complete", L"Fingerprint: level complete"},
                {"status.fingerprint_input", L"Fingerprint: auto input"},
                {"status.fingerprint_next", L"Fingerprint: waiting next level"},
                {"status.sort_locating", L"Sort: locating"},
                {"status.sort_analyzing", L"Sort: analyzing"},
                {"status.sort_next", L"Sort: waiting next round"},
                {"status.sort_analyzing_next", L"Sort: analyzing next round"},
                {"status.sort_complete", L"Sort: round complete"},
                {"status.sort_verifying", L"Sort: verifying input"},
                {"status.flashing_locating", L"Flashing: locating"},
                {"status.flashing_next_level", L"Flashing: next level"},
                {"status.flashing_exited", L"Flashing: exited"},
                {"status.flashing_confirm_exit", L"Flashing: confirming exit"},
                {"status.flashing_wait_next", L"Flashing: waiting next level"},
                {"status.flashing_verify", L"Flashing: verifying column"},
                {"status.flashing_read", L"Flashing: reading pattern"},
                {"status.flashing_input", L"Flashing: auto input"},
                {"status.fleeca_planning", L"Fleeca: planning route"},
                {"status.fleeca_starting", L"Fleeca: starting signal"},
                {"status.fleeca_navigating", L"Fleeca: navigating"},
                {"status.fleeca_wait_result", L"Fleeca: waiting for result"},
                {"status.fleeca_detect_result", L"Fleeca: detecting result"},
                {"status.fleeca_result_delay", L"Fleeca: result detected"},
                {"status.fleeca_advancing", L"Fleeca: advancing"},
                {"status.fleeca_wait_next", L"Fleeca: waiting next round"},
                {"status.fleeca_next", L"Fleeca: next round"},
                {"status.fleeca_completed", L"Fleeca: completed"},
                {"status.fleeca_route_failed", L"Fleeca: route unavailable"},
                {"status.fleeca_signal_failed", L"Fleeca: signal not detected"},
                {"status.fleeca_direction_failed", L"Fleeca: direction not confirmed"},
                {"status.fleeca_result_failed", L"Fleeca: result not detected"},
                {"game.slider", L"Slider"},
                {"game.flashing", L"Flashing"},
                {"game.choose_fingerprint", L"Fingerprint"},
                {"game.sort_fingerprint", L"Sort Fingerprint"},
                {"game.fleeca", L"Fleeca"},
                {"game.none", L"None"},
                {"log.start", L"Started"},
                {"log.found_game", L"GTA5 window found"},
                {"log.ready", L"Ready"},
                {"toast.on", L"AUTO ON"},
                {"toast.off", L"AUTO OFF"},
            };
            auto it = map.find(key);
            if (it != map.end()) return it->second;
            return L"";
        }

        // 窗口尺寸
        constexpr int kHudWidth = 260;
        constexpr int kHudMiniWidth = 260;
        constexpr int kHudMiniHeight = 44;

        HWND g_hostWnd = nullptr;
        HWND g_hudWnd = nullptr;
        std::atomic<bool> g_repaintPending{ false };
        std::atomic<bool> g_running{ false };
        std::atomic<int> g_tapHoldMs{ 20 };
        std::atomic<int> g_tapGapMs{ 20 };
        std::atomic<bool> g_overlayEnabled{ true };
        std::mutex g_textMutex;
        std::wstring g_status = L"Idle";
        std::wstring g_lastLog;
        POINT g_hudPos{ 0, 0 };

        int CurrentHeight() { return kHudMiniHeight; }
        int CurrentWidth() { return kHudMiniWidth; }

        std::wstring GetSettingsPath() {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) *(slash + 1) = L'\0';
            return std::wstring(path) + L"QuellGTA.ini";
        }

        int ClampTapMs(int value) { return std::clamp(value, 1, 250); }

        void LoadSettingsInternal() {
            std::wstring iniPath = GetSettingsPath();

            int overlay = GetPrivateProfileIntW(L"AutoHack", L"overlay_cursor", 1, iniPath.c_str());
            int tapHold = GetPrivateProfileIntW(L"AutoHack", L"tap_hold_ms", 20, iniPath.c_str());
            int tapGap = GetPrivateProfileIntW(L"AutoHack", L"tap_gap_ms", 20, iniPath.c_str());

            g_overlayEnabled.store(overlay != 0, std::memory_order_relaxed);
            g_tapHoldMs.store(ClampTapMs(tapHold), std::memory_order_relaxed);
            g_tapGapMs.store(ClampTapMs(tapGap), std::memory_order_relaxed);
            g_status = T("status.idle");
        }

        RECT VirtualDesktopRect() {
            return RECT{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        }

        RECT ClampRect(RECT panel) {
            const int width = panel.right - panel.left;
            const int height = panel.bottom - panel.top;
            MONITORINFO info{ sizeof(info) };
            const HMONITOR monitor = MonitorFromRect(&panel, MONITOR_DEFAULTTONEAREST);
            if (!GetMonitorInfoW(monitor, &info)) info.rcMonitor = VirtualDesktopRect();
            const RECT& desktop = info.rcMonitor;
            const int minLeft = desktop.left;
            const int minTop = desktop.top;
            const int maxLeft = std::max(minLeft, static_cast<int>(desktop.right) - width);
            const int maxTop = std::max(minTop, static_cast<int>(desktop.bottom) - height);
            panel.left = std::clamp(static_cast<int>(panel.left), minLeft, maxLeft);
            panel.top = std::clamp(static_cast<int>(panel.top), minTop, maxTop);
            panel.right = panel.left + width;
            panel.bottom = panel.top + height;
            return panel;
        }

        RECT PanelRect() { return RECT{ 0, 0, CurrentWidth(), CurrentHeight() }; }

        bool ProcessExeNameEquals(DWORD processId, const wchar_t* exeName) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (!process) return false;
            wchar_t imagePath[MAX_PATH * 4]{};
            DWORD size = static_cast<DWORD>(std::size(imagePath));
            bool matches = QueryFullProcessImageNameW(process, 0, imagePath, &size) &&
                wcsstr(imagePath, exeName) != nullptr;
            CloseHandle(process);
            return matches;
        }

        struct WindowSearch {
            const wchar_t* exeName = nullptr;
            HWND window = nullptr;
            RECT rect{};
        };

        BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM parameter) {
            auto* search = reinterpret_cast<WindowSearch*>(parameter);
            if (!IsWindowVisible(hwnd) || hwnd == g_hostWnd || hwnd == g_hudWnd || GetWindow(hwnd, GW_OWNER)) return TRUE;
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect) || rect.right - rect.left < 100 || rect.bottom - rect.top < 100) return TRUE;
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            if (processId && ProcessExeNameEquals(processId, search->exeName)) {
                search->window = hwnd;
                search->rect = rect;
                return FALSE;
            }
            return TRUE;
        }

        bool FindWindowRectByExe(const wchar_t* exeName, RECT& rect) {
            WindowSearch search{ exeName };
            EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&search));
            if (!search.window) return false;
            rect = search.rect;
            return true;
        }

        HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
            return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }

        void DrawPanel(HDC hdc, const RECT& panel) {
            const bool running = g_running.load(std::memory_order_relaxed);
            std::wstring status;
            {
                std::lock_guard<std::mutex> lock(g_textMutex);
                status = g_status;
            }

            HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(67, 205, 132));
            HBRUSH panelBrush = CreateSolidBrush(RGB(16, 20, 25));
            HFONT font = CreateUiFont(22, FW_SEMIBOLD);

            HGDIOBJ oldPen = SelectObject(hdc, accentPen);
            HGDIOBJ oldBrush = SelectObject(hdc, panelBrush);
            HGDIOBJ oldFont = SelectObject(hdc, font);
            SetBkMode(hdc, TRANSPARENT);

            Rectangle(hdc, panel.left, panel.top, panel.right, panel.bottom);

            COLORREF textColor = running ? RGB(80, 255, 140) : RGB(196, 207, 218);
            SetTextColor(hdc, textColor);

            RECT text{ 16, 0, panel.right - 12, panel.bottom };
            DrawTextW(hdc, status.c_str(), static_cast<int>(status.size()), &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            SelectObject(hdc, oldFont);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(font);
            DeleteObject(panelBrush);
            DeleteObject(accentPen);
        }

    }  // namespace

    void SetHostWindow(HWND hwnd) { g_hostWnd = hwnd; }
    void SetHudWindow(HWND hwnd) { g_hudWnd = hwnd; }
    HWND HudWindow() { return g_hudWnd; }

    void LoadSettings() { LoadSettingsInternal(); }

    bool OverlayEnabled() { return g_overlayEnabled.load(std::memory_order_relaxed); }
    bool SilentMode() { return !g_overlayEnabled.load(std::memory_order_relaxed); }

    int TapHoldMs() { return g_tapHoldMs.load(std::memory_order_relaxed); }
    int TapGapMs() { return g_tapGapMs.load(std::memory_order_relaxed); }

    void SetRunning(bool running) {
        g_running.store(running, std::memory_order_relaxed);
        Repaint();
    }

    void SetStatusText(const std::wstring& text) {
        std::lock_guard<std::mutex> lock(g_textMutex);
        g_status = text;
        Repaint();
    }

    void SetLogText(const std::wstring& text) {
        std::lock_guard<std::mutex> lock(g_textMutex);
        g_lastLog = text;
    }

    void Repaint() {
        if (!g_hudWnd || g_repaintPending.exchange(true, std::memory_order_relaxed)) return;
        InvalidateRect(g_hudWnd, nullptr, FALSE);
    }

    RECT InitialHudRect() {
        RECT anchor{};
        HMONITOR monitor = nullptr;
        if (FindWindowRectByExe(L"GTA5_Enhanced.exe", anchor) ||
            FindWindowRectByExe(L"GTA5.exe", anchor)) {
            monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        }
        else {
            POINT cursor{};
            GetCursorPos(&cursor);
            monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(monitor, &info)) info.rcMonitor = VirtualDesktopRect();
        const int left = info.rcMonitor.right - CurrentWidth();
        const int maxTop = std::max(static_cast<int>(info.rcMonitor.top),
            static_cast<int>(info.rcMonitor.bottom) - CurrentHeight());
        const int top = std::clamp(static_cast<int>(info.rcMonitor.top) + 50,
            static_cast<int>(info.rcMonitor.top),
            maxTop);
        return RECT{ left, top, info.rcMonitor.right, top + CurrentHeight() };
    }

    int HudWidth() { return CurrentWidth(); }
    int HudHeight() { return CurrentHeight(); }

    LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            g_repaintPending.store(false, std::memory_order_relaxed);
            PAINTSTRUCT ps{};
            HDC paintDc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC memoryDc = CreateCompatibleDC(paintDc);
            HBITMAP buffer = CreateCompatibleBitmap(paintDc, rc.right, rc.bottom);
            HGDIOBJ oldBitmap = SelectObject(memoryDc, buffer);
            HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(memoryDc, &rc, clear);
            DeleteObject(clear);
            DrawPanel(memoryDc, PanelRect());
            BitBlt(paintDc, 0, 0, rc.right, rc.bottom, memoryDc, 0, 0, SRCCOPY);
            SelectObject(memoryDc, oldBitmap);
            DeleteObject(buffer);
            DeleteDC(memoryDc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            if (g_hostWnd) PostMessageW(g_hostWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

}  // namespace gta5::app::ui