#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// 色环取色器的**纯逻辑**：HSV 换算、几何布局、命中测试。
//
// 【为什么单独一层】用户 2026-09-26 看过系统取色对话框之后说
// 「虽然稍微好点，但我还是想要类似色环的」。色环要自绘，而自绘里最容易出错的
// 两件事 —— **鼠标坐标往颜色上换算**和**颜色往坐标上换算**（反过来画标记）——
// 都是纯数学，没有理由不测。窗口与绘制在 color_picker.cpp。
//
// 本文件只依赖 windows.h（要 COLORREF / RECT / MulDiv），
// 所以能进 tests/harness 那套 shim 单测台（run.ps1 的 wheel 组）。
// ---------------------------------------------------------------------------

namespace lyricus {

// HSV。h 是 0..360（**0 = 正上方，顺时针增加**，和 Photoshop 的色环一致），
// s / v 是 0..1。
struct HsvColor {
    double h = 0.0;
    double s = 1.0;
    double v = 1.0;
};

COLORREF HsvToRgb(const HsvColor& c);
HsvColor RgbToHsv(COLORREF c);

// 取色器窗口的建议尺寸（逻辑像素）。
constexpr int kPickerWidth96  = 420;
constexpr int kPickerHeight96 = 312;

// 色环 + 环心方块（横轴饱和度、纵轴明度）+ 预览 + 两个按钮。
struct ColorWheelLayout {
    int   cx = 0, cy = 0;     // 色环圆心
    int   outerR = 0;         // 外半径
    int   innerR = 0;         // 内半径；环宽 = outerR - innerR
    RECT  svBox{};            // 饱和度/明度方块（落在环内）
    RECT  preview{};          // 颜色预览块
    RECT  hexLabel{};         // 十六进制值显示
    RECT  ok{};               // 确定
    RECT  cancel{};           // 取消
    int   dpi = 96;
};

// width / height 是**客户区**物理像素。尺寸不够时返回全零（调用方跳过绘制）。
ColorWheelLayout ComputeColorWheelLayout(int width, int height, int dpi);

// 命中目标。
enum class WheelHit {
    None,
    Ring,     // 色相环
    SvBox,    // 环心的饱和度/明度方块
    Ok,
    Cancel,
};

struct WheelPick {
    WheelHit hit = WheelHit::None;
    // 拖到这里之后的颜色。**没被碰到的分量保持原值** ——
    // 拖动色环时不该顺手把饱和度也重置了。
    HsvColor hsv{};
};

// 给定鼠标位置与当前颜色，算出"如果拖到这里会变成什么"。
//
// 【forTarget 是干什么的】传 None = 按鼠标**实际落在哪儿**判定（悬停、按下时用）。
// 传 Ring / SvBox = **目标锁定**：无论鼠标跑到哪里，都只按那一个目标算，
// 而且超出范围时**钳制**而不是落空。
//
// ★ 拖动期间**必须**传目标。否则把鼠标从 SV 方块划到色环上时，
//   判定会翻成 Ring，色相就被顺手改掉了 —— 用户只是想调暗一点，
//   颜色却整个跳了（用户 2026-09-26 报的正是这个：「内层选的时候
//   如果鼠标拖到外部了会误触到色环」）。
WheelPick HitTestColorWheel(const ColorWheelLayout& L, POINT pt, const HsvColor& cur,
                            WheelHit forTarget = WheelHit::None);

// 色相 -> 环上一点（画标记用）。r 是标记所在的半径。
POINT HuePointOnRing(const ColorWheelLayout& L, double hue, int r);

// 饱和度/明度 -> 方块内一点（画标记用）。
POINT SvPointInBox(const RECT& box, double s, double v);

} // namespace lyricus
