#pragma once

#include <windows.h>
#include <vector>

// ---------------------------------------------------------------------------
// 面板背景图的**纯计算**部分。
//
// 【为什么单独一层】这里全是"给一块 BGRA 缓冲做数学"的函数，不碰 WIC、不碰
// GDI、不碰 SDK —— 所以能进离线单测台。图片**解码**在 bg_image.h 那边，
// 那个必须依赖 COM / WIC，测不了。
//
// 划这条线是有代价意识的：模糊和适配是最容易出错的两块（差一像素、边界溢出、
// 半径大于图宽……），而它们恰恰是最需要测的。把它们和解码混在一起的话，
// 整块都进不了单测。
//
// 像素格式统一 BGRA（和 svg_icon 的 RasterIcon、控制面板分层渲染的
// m_layeredBits 一致）—— 三处同一种布局，混合时不用来回转。
// ---------------------------------------------------------------------------

namespace lyricus {

// 背景图的适配方式。
//
// 默认 Cover：面板尺寸千变万化，而用户挑的图长宽比多半对不上 ——
// Cover 保证**铺满不留边**，代价是裁掉一部分。Contain 反过来（整图可见、
// 可能留边）。这两个覆盖绝大多数场景；Stretch 和 Tile 是给特定素材的。
enum class BgFit : int {
    Cover   = 0,   // 填满，多余裁掉
    Contain = 1,   // 整图可见，居中留边
    Stretch = 2,   // 拉伸到目标尺寸（会变形）
    Tile    = 3,   // 原尺寸平铺
};

constexpr int kBgFitMin = 0;
constexpr int kBgFitMax = 3;

// 背景图的参数范围。
constexpr int kBgBlurMin     = 0;
constexpr int kBgBlurMax     = 40;    // 磨砂半径（96dpi 逻辑像素）
constexpr int kBgOpacityMin  = 0;
constexpr int kBgOpacityMax  = 100;   // 图片不透明度 %（0 = 完全看不见图）
constexpr int kBgDimMin      = 0;
constexpr int kBgDimMax      = 90;    // 压暗 %（100 会让图全黑，没意义）

// "该把源图的哪一块画到目标区域的哪里"。
struct BgPlacement {
    RECT src{};          // 源图里要用的部分（像素）
    RECT dst{};          // 目标区域里要画到的地方（像素）
    bool tile  = false;  // true = 按原尺寸平铺满目标区域（此时 src/dst 只用尺寸）
    bool valid = false;
};

// 算适配。四种方式都返回**同一套** src/dst，绘制侧不必分支 ——
// 只有 Tile 特殊（它靠 tile 标志，由绘制侧循环贴）。
//
// 【为什么值得单独测】这是最容易差一像素的地方：Cover 的裁剪要
// "宁可少一像素也不要越界"，Contain 的居中要处理奇数差（不能用整数除 2 了事，
// 否则 3 像素的差会偏 1 像素，图看着就是歪的）。
BgPlacement ComputeBgPlacement(int imgW, int imgH, int dstW, int dstH, BgFit fit);

// 盒式模糊（原地），作用在 BGRA 缓冲上。radius 是 96dpi 逻辑像素，内部按
// scale 放大成物理像素。
//
// 【为什么是盒式而不是高斯】三次盒式模糊在人眼上已经很接近高斯，而它是
// O(w*h) 与半径**无关**（用滑动窗口求和）—— 高斯是 O(w*h*r)，半径 40 时
// 慢两个数量级。面板背景只需要"看着糊"，不需要数学上的高斯。
//
// alpha 通道**不模糊**：背景图最终要整幅按 bgOpacity 混合，alpha 保持原样
// 才不会在边缘出现一圈半透明。
void BoxBlurBgra(unsigned char* bgra, int w, int h, int radiusPx);

// 压暗 + 按不透明度预乘 alpha（原地）。
//
// 【为什么要有"压暗"】贴了图之后歌词可能读不清（用户挑的图未必是深色的）。
// dimPct 把整幅图往黑压，保证文字对比度 —— 这是"能读"和"好看"之间那个
// 必须由用户决定的东西，程序猜不出来。
//
// 【为什么在这里就乘 alpha】分层渲染路径是把背景像素和歌词像素混在一起，
// 在那里再逐像素乘一遍等于把同一件事做两遍；而且那样每帧都要算。
// 在这里一次做完，"每帧"只剩一次内存拷贝。
void ApplyDimAndOpacity(unsigned char* bgra, int w, int h, int dimPct, int opacityPct);

// 把背景图 source-over 混合到**已经铺好底色的** BGRA 缓冲上（原地改 dst）。
//
// 【为什么要"叠"而不是"替"】背景图带自己的 alpha（由 bgOpacity 决定），
// 半透明的图下面必须有底色兜着 —— 直接替换的话面板会变成"图有多透明、
// 面板就有多透明"，直接透出桌面。而用户要的是"面板底色上有一张图"。
//
// ⚠️ **dst 的 alpha 通道一个像素都不动。**
//    面板整体的不透明度（cfg 里的 alpha）和图的不透明度是两件独立的事：
//    前者决定面板有多透明，后者决定图有多显眼。混在一起的话，
//    用户调"图片不透明度"会连带把整个面板弄透明 —— 那不是他要的。
//
// 两条渲染路径（分层的 m_layeredBits、非分层的 GDI）都调这一份，
// 两处各写一遍的话迟早会出现"分层模式下图偏亮"这种诡异差异。
void BlendBgOver(unsigned char* dst, const unsigned char* src, size_t pixelCount);

} // namespace lyricus
