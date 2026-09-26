#pragma once

#include <windows.h>

namespace lyricus {

// 弹出色环取色器（模态）。
//
// 返回 true = 用户点了确定，inOut 更新为选中的颜色；
// false = 取消（或窗口建不起来），inOut 保持不动。
//
// 【为什么不用系统的 ChooseColor】用户 2026-09-26 看过修复后的界面说
// 「虽然稍微好点，但我还是想要类似色环的」—— 系统那个是 Win95 年代的样子。
//
// 坐标 <-> 颜色的换算、几何、命中测试都在 color_wheel.h 里（纯函数、
// 可离线单测，wheel 组 50 项）；这个文件只负责窗口、位图与绘制。
bool PromptColorWheel(HWND parent, COLORREF& inOut, const wchar_t* title = nullptr);

} // namespace lyricus
