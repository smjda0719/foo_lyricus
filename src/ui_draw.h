#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// 自绘界面共用的小工具：圆角矩形、文字、字体、宿主主题。
//
// 【为什么抽出来】首选项页（prefs_page.cpp）和色环取色器（color_picker.cpp）
// 都是全自绘，两边的这几样东西**必须一模一样**：
//   * 圆角半径、文字对齐方式不一致 -> 两个窗口看着像两个人做的；
//   * 更要紧的是**主题色**：各取各的，就可能一个跟着 foobar2000 的暗色主题走、
//     另一个还是系统亮色，并排打开时非常刺眼。
//
// 所以在出现第二处、而不是第三处时就抽了 —— 这类"看着只是不好看"的漂移
// 最难被发现。
//
// 本文件依赖 SDK（取主题要问 ui_config_manager），所以**不进**离线单测台；
// 纯计算的那些（布局、颜色换算）在 prefs_layout.h / color_wheel.h 里。
// ---------------------------------------------------------------------------

namespace lyricus {

// 宿主主题。
struct HostTheme {
    COLORREF bg   = RGB(255, 255, 255);
    COLORREF fg   = RGB(0, 0, 0);
    bool     dark = false;
};

// 问 foobar2000 要页面底色与文字色。
//
// 走 ui_config_manager::getSysColor()：它先查主题配置、查不到才回退系统色
// （SDK/ui_element.cpp:259），所以窗口底色能和 foobar2000 自己的界面接上，
// 暗色模式也顺带处理了。拿不到服务时退回 GetSysColor。
HostTheme QueryHostTheme();

// 按 t 把 a 与 b 混合（t=0 全取 a，t=1 全取 b）。
//
// 自绘时用它从**宿主背景色**推导出卡片底 / 悬停 / 边框，而不是硬编码几档灰。
// 硬编码的灰在自定义配色下会和背景打架 —— 而且很容易连带出
// "填充是浅色、文字却是白的"这种读不了字的组合（见 color_picker.cpp 里那段）。
COLORREF BlendColor(COLORREF a, COLORREF b, double t);

// 圆角矩形填充 / 描边。半径会自动收缩到不超过矩形的一半。
void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF color);
void StrokeRoundRect(HDC dc, const RECT& r, int radius, int width, COLORREF color);

// 往矩形里写文字。自动设透明背景，并加上 DT_NOPREFIX
//（不然歌词里出现 & 会被吞掉一个）。
void DrawTextIn(HDC dc, const RECT& r, const wchar_t* text, COLORREF color,
                HFONT font, UINT flags);

// 跟随系统 UI 字体、按 dpi 缩放的界面字体。
// 直接写死 9pt 的话，高 DPI 下会比周围控件小一圈。
// 【调用方注意】返回的句柄要自己 DeleteObject。
HFONT MakeUiFont(int dpi, int pt, bool semibold);

} // namespace lyricus
