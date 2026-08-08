#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace gta5::games::slider {
void SetHostWindow(HWND hwnd);
void SetCursorWindow(HWND hwnd);
void SetMarksWindow(HWND hwnd);
HWND CursorWindow();
HWND MarksWindow();
void ClearOverlayState();
void HideTransientOverlays();
bool DetectInGame();
void RunSession();
int CursorSize();
int TapHoldMs();
int TapGapMs();
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

namespace gta5::games::choose_fingerprint {
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
}  // namespace gta5::games::choose_fingerprint

namespace gta5::games::sort_fingerprint {
bool DetectInGame();
void SetOverlayWindow(HWND hwnd);
void ClearOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status,
                const std::function<void(const std::wstring&)>& log);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::sort_fingerprint

namespace gta5::games::fleeca {
bool DetectInGame();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status);
}  // namespace gta5::games::fleeca
