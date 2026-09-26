#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// 窗口 / 系统 DPI —— 供多个模块共用。
//
// 【为什么是运行时取地址】GetDpiForWindow / GetDpiForSystem 在 winuser.h 里
// 受 WINVER 宏保护，当前编译环境没暴露出来。改成运行时从 user32.dll 取地址，
// 顺便兼容老系统（取不到就退回设备上下文问）。
//
// 【为什么单独一个头】control_window.cpp（浮动面板）和 prefs_page.cpp
// （首选项页）都要用。原先各自在自己的匿名命名空间里写了一份 ——
// 两份实现一旦漂移，就会出现"面板按 192 算、首选项按 96 算"这种
// 极难查的错位（界面看起来只是"有点挤"，没人会想到是 DPI 来源不同）。
// 两处用同一个函数，就不会有这个问题。
//
// 函数放在 lyricus 命名空间里：两个调用方本来就都在这个命名空间内
// （或已 using namespace lyricus），所以调用点一个字都不用改。
// ---------------------------------------------------------------------------

namespace lyricus {

inline UINT GetDpiForWindowSafe(HWND hwnd) {
    using fn_t = UINT(WINAPI*)(HWND);
    static const auto p = reinterpret_cast<fn_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (p != nullptr && hwnd != nullptr) return p(hwnd);

    HDC dc = GetDC(hwnd);
    const UINT dpi = (dc != nullptr) ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96u;
    if (dc != nullptr) ReleaseDC(hwnd, dc);
    return dpi;
}

inline UINT GetDpiForSystemSafe() {
    using fn_t = UINT(WINAPI*)();
    static const auto p = reinterpret_cast<fn_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (p != nullptr) return p();

    HDC dc = GetDC(nullptr);
    const UINT dpi = (dc != nullptr) ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96u;
    if (dc != nullptr) ReleaseDC(nullptr, dc);
    return dpi;
}

} // namespace lyricus
