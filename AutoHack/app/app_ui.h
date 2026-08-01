#pragma once

#include <windows.h>
#include <string>

namespace gta5::app::ui {

	void SetHostWindow(HWND hwnd);
	void SetHudWindow(HWND hwnd);
	HWND HudWindow();

	void LoadSettings();

	bool OverlayEnabled();
	bool SilentMode();

	int TapHoldMs();
	int TapGapMs();

	void SetRunning(bool running);
	void SetStatusText(const std::wstring& text);
	void SetLogText(const std::wstring& text);
	void Repaint();

	int HudWidth();
	int HudHeight();
	RECT InitialHudRect();

	LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace gta5::app::ui