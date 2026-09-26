#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// 颜色工具 —— **纯函数**，不依赖 SDK，所以能进离线单测台。
//
// 【为什么单独一层】`BlendColor` 曾经在四个地方各有一份：
//   dui_element.cpp / cui_panel.cpp（都是 float 版）、ui_draw.cpp（double 版）、
//   以及 prefs_layout.cpp 里一个改名叫 Blend 的。plan.md 的"已知小问题"里
//   一直记着"该提到公共层"。
//
// 它**不能**挂在 ui_draw.h 上 —— 那个文件要取宿主主题、依赖 SDK，
// 而 prefs_layout（布局与配色推导）必须保持无 SDK 才能进单测台。
// 所以工具归这里，界面归 ui_draw，两边都 include 这一份。
// ---------------------------------------------------------------------------

namespace lyricus {

// 按 t 把 a 与 b 混合：t=0 全取 a，t=1 全取 b。
//
// t **可以是负数或大于 1** —— 那是**外推**，用来把结果推得比两端更远。
// dui_element.cpp 就靠 `BlendColor(normalText, background, -0.5)` 让当前行
// 比正文色更亮（往背景的反方向推）。合并这几份实现时特意保留了这个能力，
// 否则那份调用会被静默改成"夹在两端之间"，看起来只是"高亮没了"。
//
// 结果按分量 clamp 到 0..255（外推很容易越界）。
COLORREF BlendColor(COLORREF a, COLORREF b, double t);

// 感知亮度 0..255。加权而不是简单平均 —— 纯蓝和纯黄的"平均"一样，
// 人眼看上去差得远。自绘时用它判断"这个底色上该压黑字还是白字"。
int ColorLuminance(COLORREF c);

} // namespace lyricus
