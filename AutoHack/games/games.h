#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <thread>

namespace gta5::games::slider {
void SetHostWindow(HWND hwnd);
void SetHudWindow(HWND hwnd);
void SetCursorWindow(HWND hwnd);
void SetMarksWindow(HWND hwnd);
HWND HudWindow();
HWND CursorWindow();
HWND MarksWindow();
bool OverlayEnabled();
void SetOverlayEnabled(bool enabled);
int TapHoldMs();
int TapGapMs();
int HotkeyVk();
std::wstring HotkeyName();
void LoadPersistentSettings();
void ApplyHotkey(HWND hwnd);
bool Running();
void RequestStop();
void MarkRunning(bool running);
void ResetStopFlag();
bool StopRequested();
std::thread& WorkerThread();
void PostModuleLog(const std::wstring& text);
void PostModuleStatus(const std::wstring& text);
void RepaintHud();
void ClearOverlayState();
void HideTransientOverlays();
bool DetectInGame();
void RunSession();
RECT InitialHudRect();
int HudWidth();
int HudHeight();
int CursorSize();
int HotkeyId();
bool IsListeningHotkey();
void UpdatePreviewRunning(bool running);
void SetHudLogText(const std::wstring& text);
void SetHudStatusText(const std::wstring& text);
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK CursorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MarksWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
}  // namespace gta5::games::slider

namespace gta5::games::flashing {
bool DetectInGame();
HWND OverlayWindow();
void SetOverlayWindow(HWND hwnd);
void HideOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::flashing

namespace gta5::games::fingerprint {
bool DetectInGame();
HWND OverlayWindow();
void SetOverlayWindow(HWND hwnd);
void SetUiThread();
void InitStateLock();
void DeleteStateLock();
void ClearOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::fingerprint
