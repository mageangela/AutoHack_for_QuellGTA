#include "game_window.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace gta5::capture {
namespace {

std::mutex gMutex;
HWND gWindow = nullptr;
RECT gClientRect{};
bool gHasClientRect = false;
std::uint64_t gWindowGeneration = 1;

bool ReadClientRect(HWND window, RECT& rect) {
  if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
  RECT client{};
  if (!GetClientRect(window, &client)) return false;
  POINT topLeft{client.left, client.top};
  POINT bottomRight{client.right, client.bottom};
  if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return false;
  rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
  return rect.right > rect.left && rect.bottom > rect.top;
}

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top &&
         a.right == b.right && a.bottom == b.bottom;
}

bool RefreshWindowLocked() {
  RECT current{};
  if (!gWindow || !ReadClientRect(gWindow, current)) {
    if (gWindow || gHasClientRect) ++gWindowGeneration;
    gWindow = nullptr;
    gClientRect = {};
    gHasClientRect = false;
    return false;
  }
  if (!gHasClientRect || !SameRect(current, gClientRect)) {
    gClientRect = current;
    gHasClientRect = true;
    ++gWindowGeneration;
  }
  return true;
}

std::unordered_set<DWORD> FindGameProcesses() {
  std::unordered_set<DWORD> result;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return result;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"GTA5_Enhanced.exe") == 0 ||
          _wcsicmp(entry.szExeFile, L"GTA5.exe") == 0) {
        result.insert(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

struct WindowSearch {
  const std::unordered_set<DWORD>* processes = nullptr;
  HWND best = nullptr;
  long long bestArea = 0;
};

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM param) {
  auto& search = *reinterpret_cast<WindowSearch*>(param);
  DWORD processId = 0;
  GetWindowThreadProcessId(window, &processId);
  if (!search.processes->count(processId) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
  RECT rect{};
  if (!ReadClientRect(window, rect)) return TRUE;
  const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
  if (area > search.bestArea) {
    search.best = window;
    search.bestArea = area;
  }
  return TRUE;
}

}  // namespace

bool FindGameWindow() {
  const auto processes = FindGameProcesses();
  if (processes.empty()) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gWindow || gHasClientRect) ++gWindowGeneration;
    gWindow = nullptr;
    gClientRect = {};
    gHasClientRect = false;
    return false;
  }
  WindowSearch search{&processes};
  EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
  std::lock_guard<std::mutex> lock(gMutex);
  RECT nextRect{};
  const bool found = ReadClientRect(search.best, nextRect);
  if (!found || search.best != gWindow || !gHasClientRect || !SameRect(nextRect, gClientRect)) {
    ++gWindowGeneration;
  }
  gWindow = found ? search.best : nullptr;
  gClientRect = found ? nextRect : RECT{};
  gHasClientRect = found;
  return gHasClientRect;
}

bool GetGameClientRect(RECT& rect) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (!RefreshWindowLocked()) return false;
  rect = gClientRect;
  return true;
}

void ClearGameWindow() {
  std::lock_guard<std::mutex> lock(gMutex);
  if (gWindow || gHasClientRect) ++gWindowGeneration;
  gWindow = nullptr;
  gClientRect = {};
  gHasClientRect = false;
}

namespace {

struct CaptureResources {
  HDC memory = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  void* bits = nullptr;
  int width = 0;
  int height = 0;

  ~CaptureResources() {
    if (memory && previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
  }

  bool resize(HDC screen, int nextWidth, int nextHeight) {
    if (memory && bitmap && width == nextWidth && height == nextHeight) return true;
    if (memory && previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    bitmap = nullptr;
    previous = nullptr;
    bits = nullptr;
    if (!memory) memory = CreateCompatibleDC(screen);
    if (!memory) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = nextWidth;
    info.bmiHeader.biHeight = -nextHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return false;
    previous = SelectObject(memory, bitmap);
    width = nextWidth;
    height = nextHeight;
    return true;
  }
};

thread_local CaptureResources gCapture;

}  // namespace

bool CaptureGameFrame(GameFrame& frame, const RECT* screenRegion) {
  RECT client{};
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!RefreshWindowLocked()) return false;
    client = gClientRect;
    generation = gWindowGeneration;
  }
  if (!CaptureGameFrameFromClientRect(frame, client, screenRegion)) return false;
  frame.windowGeneration = generation;
  return true;
}

bool CaptureGameFrameFromClientRect(GameFrame& frame, const RECT& client, const RECT* screenRegion) {
  RECT source = client;
  if (screenRegion) {
    if (!IntersectRect(&source, &client, screenRegion)) return false;
  }
  const int clientH = client.bottom - client.top;
  const int sourceW = source.right - source.left;
  const int sourceH = source.bottom - source.top;
  if (clientH <= 0 || sourceW <= 0 || sourceH <= 0) return false;

  const double scale = clientH > 1080 ? 1080.0 / clientH : 1.0;
  const int width = std::max(1, static_cast<int>(std::lround(sourceW * scale)));
  const int height = std::max(1, static_cast<int>(std::lround(sourceH * scale)));
  HDC screen = GetDC(nullptr);
  if (!screen) return false;
  if (!gCapture.resize(screen, width, height)) {
    ReleaseDC(nullptr, screen);
    return false;
  }
  BOOL copied = FALSE;
  if (width == sourceW && height == sourceH) {
    copied = BitBlt(gCapture.memory, 0, 0, width, height, screen,
                    source.left, source.top, SRCCOPY);
  } else {
    SetStretchBltMode(gCapture.memory, COLORONCOLOR);
    copied = StretchBlt(gCapture.memory, 0, 0, width, height, screen,
                        source.left, source.top, sourceW, sourceH, SRCCOPY);
  }
  LARGE_INTEGER capturedAt{};
  QueryPerformanceCounter(&capturedAt);
  ReleaseDC(nullptr, screen);
  if (!copied) return false;

  frame.screenX = source.left;
  frame.screenY = source.top;
  frame.screenW = sourceW;
  frame.screenH = sourceH;
  frame.width = width;
  frame.height = height;
  frame.clientHeight = clientH;
  frame.clientWidth = client.right - client.left;
  frame.captureQpc = capturedAt.QuadPart;
  frame.toScreenX = sourceW / static_cast<double>(width);
  frame.toScreenY = sourceH / static_cast<double>(height);
  frame.bgra.resize(static_cast<size_t>(width) * height);
  std::memcpy(frame.bgra.data(), gCapture.bits, frame.bgra.size() * sizeof(std::uint32_t));
  return true;
}

}  // namespace gta5::capture
