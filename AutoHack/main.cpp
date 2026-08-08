#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "games/games.h"
#include "app/app_ui.h"
#include "app/app_runtime.h"
#include "capture/game_window.h"
#include "input/key_input.h"

namespace {
    // 修改 T 函数，返回 std::wstring 而不是 const wchar_t*
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
            {"game.find_number", L"Find the number"},
            {"status.find_number_locating", L"Find number: locating"},
            {"status.find_number_analyzing", L"Find number: analyzing"},
            {"status.find_number_moving", L"Find number: moving"},
            {"status.find_number_verifying", L"Find number: verifying position"},
            {"status.find_number_submitting", L"Find number: submitting"},
            {"status.find_number_complete", L"Find number: completed"},
            {"status.find_number_exited", L"Find number: minigame exited"},
            {"game.match", L"Signal matching"},
            {"status.match_locating", L"Signal matching: locating"},
            {"status.match_analyzing", L"Signal matching: analyzing"},
            {"status.match_reading", L"Signal matching: reading values"},
            {"status.match_connecting", L"Signal matching: connecting"},
            {"status.match_verifying", L"Signal matching: verifying connection"},
            {"status.match_complete", L"Signal matching: completed"},
            {"status.match_exited", L"Signal matching: minigame exited"},
            {"status.match_no_solution", L"Signal matching: no valid combination"},
            {"game.slider", L"Slider"},
            {"game.flashing", L"Flashing"},
            {"game.choose_fingerprint", L"Fingerprint"},
            {"game.sort_fingerprint", L"Sort Fingerprint"},
            {"game.fleeca", L"Fleeca"},
            {"game.none", L"None"},
            {"log.start", L"Started"},
            {"log.found_game", L"GTA5 window found"},
            {"log.ready", L"Ready"},
        };
        auto it = map.find(key);
        if (it != map.end()) return it->second;
        return L"";
    }

    constexpr UINT kMsgLog = WM_APP + 1;
    constexpr UINT kMsgStatus = WM_APP + 2;
    constexpr UINT kMsgWorkerDone = WM_APP + 3;

    HWND g_host = nullptr;
    HANDLE g_singleInstanceMutex = nullptr;
    HWND g_cursorOverlay = nullptr;
    HWND g_marksOverlay = nullptr;
    HWND g_flashingOverlay = nullptr;
    HWND g_chooseFingerprintOverlay = nullptr;
    HWND g_sortFingerprintOverlay = nullptr;
    HWND g_matchOverlay = nullptr;

    enum class GameKind {
        None,
        Slider,
        Flashing,
        ChooseFingerprint,
        SortFingerprint,
        Fleeca,
        FindNumber,
        Match,
    };

    std::wstring GameName(GameKind game) {
        switch (game) {
        case GameKind::Slider: return T("game.slider");
        case GameKind::Flashing: return T("game.flashing");
        case GameKind::ChooseFingerprint: return T("game.choose_fingerprint");
        case GameKind::SortFingerprint: return T("game.sort_fingerprint");
        case GameKind::Fleeca: return T("game.fleeca");
        case GameKind::FindNumber: return T("game.find_number");
        case GameKind::Match: return T("game.match");
        default: return T("game.none");
        }
    }

    void HideAllGameOverlays() {
        gta5::games::slider::HideTransientOverlays();
        gta5::games::flashing::HideOverlay();
        gta5::games::choose_fingerprint::ClearOverlay();
        gta5::games::sort_fingerprint::ClearOverlay();
        gta5::games::match::ClearOverlay();
    }

    void ResetAllInGameCaches() {
        gta5::games::slider::ResetInGameCache();
        gta5::games::flashing::ResetInGameCache();
        gta5::games::choose_fingerprint::ResetInGameCache();
        gta5::games::sort_fingerprint::ResetInGameCache();
        gta5::games::fleeca::ResetInGameCache();
        gta5::games::find_number::ResetInGameCache();
        gta5::games::match::ResetInGameCache();
    }

    GameKind DetectGame() {
        if (gta5::games::slider::DetectInGame()) return GameKind::Slider;
        if (gta5::games::flashing::DetectInGame()) return GameKind::Flashing;
        if (gta5::games::choose_fingerprint::DetectInGame()) return GameKind::ChooseFingerprint;
        if (gta5::games::sort_fingerprint::DetectInGame()) return GameKind::SortFingerprint;
        if (gta5::games::fleeca::DetectInGame()) return GameKind::Fleeca;
        if (gta5::games::find_number::DetectInGame()) return GameKind::FindNumber;
        if (gta5::games::match::DetectInGame()) return GameKind::Match;
        return GameKind::None;
    }

    void PostStatus(const std::wstring& text) {
        auto* payload = new std::wstring(text);
        if (g_host) PostMessageW(g_host, kMsgStatus, 0, reinterpret_cast<LPARAM>(payload));
        else delete payload;
    }

    void PostLog(const std::wstring& text) {
        auto* payload = new std::wstring(text);
        if (g_host) PostMessageW(g_host, kMsgLog, 0, reinterpret_cast<LPARAM>(payload));
        else delete payload;
    }

    std::wstring LocalizeRuntimeStatus(const std::wstring& text) {
        struct StatusTranslation {
            const wchar_t* source;
            const char* key;
        };
        const StatusTranslation translations[] = {
            {L"analysis latency too high; stopped", "status.slider_latency"},
            {L"capture failed", "status.capture_failed"},
            {L"active bar timeout; stopped", "status.slider_timeout"},
            {L"completed; stopped", "status.completed"},
            {L"waiting yellow outline", "status.waiting_yellow"},
            {L"in minigame", "status.in_minigame"},
            {L"stopped", "status.stopped"},
            {L"searching minigame", "status.search_minigame"},
            {L"fingerprint: locating", "status.fingerprint_locating"},
            {L"fingerprint: exited", "status.fingerprint_exited"},
            {L"fingerprint: confirming exit", "status.fingerprint_confirm_exit"},
            {L"fingerprint: level complete", "status.fingerprint_complete"},
            {L"fingerprint: auto input", "status.fingerprint_input"},
            {L"fingerprint: waiting next level", "status.fingerprint_next"},
            {L"sort_fingerprint: locating", "status.sort_locating"},
            {L"sort_fingerprint: analyzing", "status.sort_analyzing"},
            {L"sort_fingerprint: waiting next round", "status.sort_next"},
            {L"sort_fingerprint: analyzing next round", "status.sort_analyzing_next"},
            {L"sort_fingerprint: round complete", "status.sort_complete"},
            {L"sort_fingerprint: verifying input", "status.sort_verifying"},
            {L"flashing: locating", "status.flashing_locating"},
            {L"flashing: next level", "status.flashing_next_level"},
            {L"flashing: exited", "status.flashing_exited"},
            {L"flashing: confirming exit", "status.flashing_confirm_exit"},
            {L"flashing: waiting next level", "status.flashing_wait_next"},
            {L"flashing: verifying column", "status.flashing_verify"},
            {L"flashing: reading pattern", "status.flashing_read"},
            {L"flashing: auto input", "status.flashing_input"},
            {L"fleeca: planning route", "status.fleeca_planning"},
            {L"fleeca: starting signal", "status.fleeca_starting"},
            {L"fleeca: navigating", "status.fleeca_navigating"},
            {L"fleeca: waiting for result", "status.fleeca_wait_result"},
            {L"fleeca: detecting result", "status.fleeca_detect_result"},
            {L"fleeca: result detected; waiting for game", "status.fleeca_result_delay"},
            {L"fleeca: advancing", "status.fleeca_advancing"},
            {L"fleeca: waiting for next round", "status.fleeca_wait_next"},
            {L"fleeca: next round", "status.fleeca_next"},
            {L"fleeca: completed", "status.fleeca_completed"},
            {L"fleeca: route unavailable", "status.fleeca_route_failed"},
            {L"fleeca: signal not detected", "status.fleeca_signal_failed"},
            {L"fleeca: direction not confirmed", "status.fleeca_direction_failed"},
            {L"fleeca: result not detected", "status.fleeca_result_failed"},
            {L"find_number: locating", "status.find_number_locating"},
            {L"find_number: analyzing", "status.find_number_analyzing"},
            {L"find_number: moving", "status.find_number_moving"},
            {L"find_number: verifying position", "status.find_number_verifying"},
            {L"find_number: submitting", "status.find_number_submitting"},
            {L"find_number: completed", "status.find_number_complete"},
            {L"find_number: minigame exited", "status.find_number_exited"},
            {L"match: locating", "status.match_locating"},
            {L"match: analyzing", "status.match_analyzing"},
            {L"match: reading puzzle", "status.match_reading"},
            {L"match: connecting", "status.match_connecting"},
            {L"match: verifying connection", "status.match_verifying"},
            {L"match: completed", "status.match_complete"},
            {L"match: minigame exited", "status.match_exited"},
            {L"match: no solution", "status.match_no_solution"},
        };
        for (const StatusTranslation& translation : translations) {
            if (text == translation.source) return T(translation.key);
        }
        return text;
    }

    void WorkerMain() {
        gta5::app::runtime::ConfigureLatencySensitiveThread();
        PostStatus(T("status.search_game"));
        HideAllGameOverlays();

        bool completed = false;
        bool gameWindowFound = false;
        while (!gta5::app::runtime::StopRequested()) {
            if (!gameWindowFound && !gta5::capture::FindGameWindow()) {
                PostStatus(T("status.search_game"));
                Sleep(100);
                continue;
            }
            if (!gameWindowFound) {
                PostLog(T("log.found_game"));
                PostStatus(T("status.search_minigame"));
                gameWindowFound = true;
            }
            GameKind game = DetectGame();
            if (game == GameKind::None) {
                Sleep(30);
                continue;
            }

            PostLog(T("status.running") + L" " + GameName(game));
            PostStatus(T("status.running") + L" " + GameName(game));
            switch (game) {
            case GameKind::Slider:
                gta5::games::slider::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); });
                completed = true;
                break;
            case GameKind::Flashing:
                completed = gta5::games::flashing::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); },
                    [] { return gta5::app::ui::OverlayEnabled(); },
                    [](const std::wstring& text) { PostStatus(text); });
                break;
            case GameKind::ChooseFingerprint:
                completed = gta5::games::choose_fingerprint::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); },
                    [] { return gta5::app::ui::OverlayEnabled(); },
                    [](const std::wstring& text) { PostStatus(text); });
                break;
            case GameKind::SortFingerprint:
                completed = gta5::games::sort_fingerprint::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); },
                    [] { return gta5::app::ui::OverlayEnabled(); },
                    [](const std::wstring& text) { PostStatus(text); },
                    [](const std::wstring& text) { PostLog(text); });
                break;
            case GameKind::Fleeca:
                completed = gta5::games::fleeca::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); },
                    [](const std::wstring& text) { PostStatus(text); });
                break;
            case GameKind::FindNumber:
                completed = gta5::games::find_number::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); },
                    [](const std::wstring& text) { PostStatus(text); });
                break;
            case GameKind::Match:
                completed = gta5::games::match::RunSession(
                    [] { return gta5::app::runtime::StopRequested(); },
                    [] { return gta5::app::ui::OverlayEnabled(); },
                    [](const std::wstring& text) { PostStatus(text); });
                break;
            default:
                break;
            }
            break;
        }

        gta5::input::CancelAll();

        if (!completed && !gta5::app::runtime::StopRequested()) {
            PostLog(gameWindowFound ? L"no supported minigame detected" : L"GTA5 window not found");
            PostStatus(gameWindowFound ? T("status.detect_timeout") : T("status.game_timeout"));
        }
        else {
            PostStatus(completed ? T("status.completed") : T("status.stopped"));
        }

        HideAllGameOverlays();
        ResetAllInGameCaches();
        gta5::capture::ClearGameWindow();
        gta5::app::runtime::SetRunning(false);
        gta5::app::ui::SetRunning(false);

        Sleep(500);
        ExitProcess(0);
    }

    void StartWorker() {
        if (gta5::app::runtime::Running()) return;
        gta5::input::CancelAll();
        ResetAllInGameCaches();
        gta5::input::ConfigureSequenceTiming(
            std::chrono::milliseconds(gta5::app::ui::TapHoldMs()),
            std::chrono::milliseconds(gta5::app::ui::TapGapMs()));
        gta5::app::runtime::ResetStopRequest();
        gta5::games::slider::HideTransientOverlays();
        gta5::app::runtime::SetRunning(true);
        gta5::app::ui::SetRunning(true);
        gta5::app::ui::SetStatusText(T("status.starting"));
        PostStatus(T("status.starting"));
        gta5::app::ui::Repaint();
        gta5::app::runtime::WorkerThread() = std::thread(WorkerMain);
        gta5::app::ui::Repaint();
    }

    void StopWorker() {
        if (!gta5::app::runtime::Running()) return;
        gta5::app::ui::SetStatusText(T("status.stopping"));
        PostStatus(T("status.stopping"));
        gta5::app::ui::Repaint();
        gta5::app::runtime::RequestStop();
        gta5::input::CancelAll();
        HideAllGameOverlays();
        auto& worker = gta5::app::runtime::WorkerThread();
        if (worker.joinable()) worker.join();
        gta5::app::runtime::SetRunning(false);
        gta5::app::ui::SetRunning(false);
        PostStatus(T("status.stopped"));
        gta5::app::ui::Repaint();
    }

    void DestroyGameOverlayWindows();

    LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case kMsgLog: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            OutputDebugStringW(text->c_str());
            OutputDebugStringW(L"\n");
            gta5::app::ui::SetLogText(*text);
            gta5::app::ui::Repaint();
            return 0;
        }
        case kMsgStatus: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            gta5::app::ui::SetStatusText(LocalizeRuntimeStatus(*text));
            gta5::app::ui::Repaint();
            return 0;
        }
        case WM_CLOSE:
            StopWorker();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            StopWorker();
            DestroyGameOverlayWindows();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void RegisterClasses(HINSTANCE inst) {
        WNDCLASSW host{};
        host.lpfnWndProc = HostProc;
        host.hInstance = inst;
        host.hCursor = LoadCursor(nullptr, IDC_ARROW);
        host.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        host.lpszClassName = L"Gta7In1HostV2";
        RegisterClassW(&host);

        WNDCLASSW hud{};
        hud.lpfnWndProc = gta5::app::ui::HudProc;
        hud.hInstance = inst;
        hud.hCursor = LoadCursor(nullptr, IDC_ARROW);
        hud.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        hud.lpszClassName = L"Gta7In1HudV2";
        RegisterClassW(&hud);

        WNDCLASSW cursor{};
        cursor.lpfnWndProc = gta5::games::slider::CursorWindowProc;
        cursor.hInstance = inst;
        cursor.hCursor = LoadCursor(nullptr, IDC_ARROW);
        cursor.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        cursor.lpszClassName = L"Gta7In1CursorV2";
        RegisterClassW(&cursor);

        WNDCLASSW marks{};
        marks.lpfnWndProc = gta5::games::slider::MarksWindowProc;
        marks.hInstance = inst;
        marks.hCursor = LoadCursor(nullptr, IDC_ARROW);
        marks.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        marks.lpszClassName = L"Gta7In1MarksV2";
        RegisterClassW(&marks);

        WNDCLASSW flashing{};
        flashing.lpfnWndProc = gta5::games::flashing::OverlayWindowProc;
        flashing.hInstance = inst;
        flashing.hCursor = LoadCursor(nullptr, IDC_ARROW);
        flashing.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        flashing.lpszClassName = L"Gta7In1FlashingOverlayV2";
        RegisterClassW(&flashing);

        WNDCLASSW fingerprint{};
        fingerprint.lpfnWndProc = gta5::games::choose_fingerprint::OverlayWindowProc;
        fingerprint.hInstance = inst;
        fingerprint.hCursor = LoadCursor(nullptr, IDC_ARROW);
        fingerprint.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        fingerprint.lpszClassName = L"Gta7In1ChooseFingerprintOverlayV2";
        RegisterClassW(&fingerprint);

        WNDCLASSW sortFingerprint{};
        sortFingerprint.lpfnWndProc = gta5::games::sort_fingerprint::OverlayWindowProc;
        sortFingerprint.hInstance = inst;
        sortFingerprint.hCursor = LoadCursor(nullptr, IDC_ARROW);
        sortFingerprint.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        sortFingerprint.lpszClassName = L"Gta7In1SortFingerprintOverlayV2";
        RegisterClassW(&sortFingerprint);

        WNDCLASSW match{};
        match.lpfnWndProc = gta5::games::match::OverlayWindowProc;
        match.hInstance = inst;
        match.hCursor = LoadCursor(nullptr, IDC_ARROW);
        match.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        match.lpszClassName = L"Gta7In1MatchOverlayV1";
        RegisterClassW(&match);

    }

    void DestroyGameOverlayWindows() {
        HideAllGameOverlays();
        if (g_cursorOverlay) DestroyWindow(g_cursorOverlay);
        if (g_marksOverlay) DestroyWindow(g_marksOverlay);
        if (g_flashingOverlay) DestroyWindow(g_flashingOverlay);
        if (g_chooseFingerprintOverlay) DestroyWindow(g_chooseFingerprintOverlay);
        if (g_sortFingerprintOverlay) DestroyWindow(g_sortFingerprintOverlay);
        if (g_matchOverlay) DestroyWindow(g_matchOverlay);
        g_cursorOverlay = nullptr;
        g_marksOverlay = nullptr;
        g_flashingOverlay = nullptr;
        g_chooseFingerprintOverlay = nullptr;
        g_sortFingerprintOverlay = nullptr;
        g_matchOverlay = nullptr;
        gta5::games::slider::SetCursorWindow(nullptr);
        gta5::games::slider::SetMarksWindow(nullptr);
        gta5::games::flashing::SetOverlayWindow(nullptr);
        gta5::games::choose_fingerprint::SetOverlayWindow(nullptr);
        gta5::games::sort_fingerprint::SetOverlayWindow(nullptr);
        gta5::games::match::SetOverlayWindow(nullptr);
    }

    void CreateGameOverlayWindows(HINSTANCE inst, const RECT& hudRect) {
        if (gta5::app::ui::SilentMode()) return;

        g_cursorOverlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Gta7In1CursorV2", L"Auto Hack 7in1 Cursor", WS_POPUP,
            hudRect.right + 12, hudRect.top, gta5::games::slider::CursorSize(),
            gta5::games::slider::CursorSize(), nullptr, nullptr, inst, nullptr);
        gta5::games::slider::SetCursorWindow(g_cursorOverlay);
        if (g_cursorOverlay) {
            SetLayeredWindowAttributes(g_cursorOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
            ShowWindow(g_cursorOverlay, SW_HIDE);
        }

        g_marksOverlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Gta7In1MarksV2", L"Auto Hack 7in1 Marks", WS_POPUP,
            hudRect.right + 84, hudRect.top, 1, 1, nullptr, nullptr, inst, nullptr);
        gta5::games::slider::SetMarksWindow(g_marksOverlay);
        if (g_marksOverlay) {
            SetLayeredWindowAttributes(g_marksOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
            ShowWindow(g_marksOverlay, SW_HIDE);
        }

        g_flashingOverlay = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            L"Gta7In1FlashingOverlayV2", L"Auto Hack 7in1 Flashing Overlay", WS_POPUP,
            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            nullptr, nullptr, inst, nullptr);
        gta5::games::flashing::SetOverlayWindow(g_flashingOverlay);
        if (g_flashingOverlay) {
            SetLayeredWindowAttributes(g_flashingOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
            ShowWindow(g_flashingOverlay, SW_HIDE);
        }

        g_chooseFingerprintOverlay = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Gta7In1ChooseFingerprintOverlayV2", L"Auto Hack 7in1 Choose Fingerprint Overlay",
            WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            nullptr, nullptr, inst, nullptr);
        gta5::games::choose_fingerprint::SetOverlayWindow(g_chooseFingerprintOverlay);
        if (g_chooseFingerprintOverlay) {
            SetLayeredWindowAttributes(g_chooseFingerprintOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
            ShowWindow(g_chooseFingerprintOverlay, SW_HIDE);
        }

        const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_sortFingerprintOverlay = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Gta7In1SortFingerprintOverlayV2", L"Auto Hack 7in1 Sort Fingerprint Overlay",
            WS_POPUP, virtualX, virtualY, virtualW, virtualH, nullptr, nullptr, inst, nullptr);
        gta5::games::sort_fingerprint::SetOverlayWindow(g_sortFingerprintOverlay);
        if (g_sortFingerprintOverlay) {
            SetLayeredWindowAttributes(g_sortFingerprintOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
            ShowWindow(g_sortFingerprintOverlay, SW_HIDE);
        }

        g_matchOverlay = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Gta7In1MatchOverlayV1", L"Auto Hack 7in1 Match Overlay", WS_POPUP,
            virtualX, virtualY, virtualW, virtualH, nullptr, nullptr, inst, nullptr);
        gta5::games::match::SetOverlayWindow(g_matchOverlay);
        if (g_matchOverlay) {
            SetLayeredWindowAttributes(g_matchOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
            ShowWindow(g_matchOverlay, SW_HIDE);
        }
    }

    bool CreateWindows(HINSTANCE inst) {
        g_host = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Gta7In1HostV2", L"Auto Hack 7in1 Host",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!g_host) return false;
        gta5::app::ui::SetHostWindow(g_host);
        gta5::games::slider::SetHostWindow(g_host);

        RECT hudRect = gta5::app::ui::InitialHudRect();

        bool silentMode = gta5::app::ui::SilentMode();
        DWORD hudExStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

        HWND hud = nullptr;
        if (!silentMode) {
            hud = CreateWindowExW(hudExStyle,
                L"Gta7In1HudV2", L"Auto Hack 7in1 HUD",
                WS_POPUP, hudRect.left, hudRect.top,
                gta5::app::ui::HudWidth(), gta5::app::ui::HudHeight(),
                nullptr, nullptr, inst, nullptr);
            gta5::app::ui::SetHudWindow(hud);
            if (hud) {
                SetLayeredWindowAttributes(hud, RGB(0, 0, 0), 255, LWA_COLORKEY);
                ShowWindow(hud, SW_SHOWNA);
            }
        }
        else {
            gta5::app::ui::SetHudWindow(nullptr);
        }

        CreateGameOverlayWindows(inst, hudRect);

        return true;
    }

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    gta5::app::runtime::ConfigureLatencySensitiveProcess();
    gta5::app::ui::LoadSettings();

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\AutoHack7in1SingleInstance");
    if (!g_singleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_singleInstanceMutex) {
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = nullptr;
        }
        return 0;
    }

    gta5::games::choose_fingerprint::SetUiThread();
    gta5::games::choose_fingerprint::InitStateLock();

    RegisterClasses(inst);
    if (!CreateWindows(inst)) {
        return 1;
    }

    PostLog(T("log.ready"));
    PostStatus(T("status.idle"));

    StartWorker();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    gta5::games::choose_fingerprint::DeleteStateLock();
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }
    return 0;
}