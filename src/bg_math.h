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
    Manual  = 4,   // 用户自己拖出来的构图（见 BgManual）
};

constexpr int kBgFitMin = 0;
constexpr int kBgFitMax = 4;

// 手动构图的参数。
//
// ⚠️ 全部存**百分比**，不存像素。
//
// 【为什么】面板尺寸是会变的（用户拖窗口、换 dpi、换预设里的面板大小），
// 存像素的话用户拖一次窗口、或者换台机器，构图就整体偏掉了 ——
// 而且偏得没规律，看起来像"图的定位坏了"。
// 存百分比则永远相对：缩放是"相对于刚好铺满"，偏移是"相对于还能移动的范围"。
struct BgManual {
    int zoomPct    = 100;   // 缩放。100 = 刚好铺满（下限，再小就露边）
    int offsetXPct = 0;     // -100 = 图左边贴住区域左边；+100 = 图右边贴住右边
    int offsetYPct = 0;     // 同上，纵向。0 = 居中

    // ★ 锁定尺寸（D-132）—— 上面那条"存百分比"的规则在这里有个例外。
    //
    // 【为什么需要它】百分比缩放的基准是 `dstH / imgH`（高度铺满），
    //   而那**依赖面板高度**。于是用户花时间拖好的构图，在面板被拉高/拉矮
    //   之后就全乱了：同一个 100% 在不同面板高度下画出不同大小的图。
    //
    //   用户报的正是这个：「拖好之后面板一缩放，构图就变了」。
    //
    // 【为什么这里可以存像素】百分比那条规则防的是"换成另一台机器/另一个 dpi
    //   时构图偏掉"。而锁定尺寸是用户**明确要求**"这张图就按这么大显示"——
    //   他要的就是绝对值不随环境变。两者不矛盾：默认走百分比，
    //   只有用户主动勾了锁才走像素。
    //
    // 【宽度而不是高度】因为基准本来就是"高度铺满"，高度是最先被面板
    //   决定的那个；用户脑子里想的是"这张图多大"，宽度更直观。
    bool locked  = false;
    int  lockedW = 0;       // 锁定时的图宽度（**物理**像素）
};

// 锁定尺寸的下限 / 上限（物理像素）。
//
// 下限 32：再小的图在面板上就是一粒沙子，勾了锁也看不出是什么。
// 上限 8192：和 kMaxDrawSide 一致（那是贴图缓冲的封顶，见 bg_image.cpp）。
constexpr int kBgLockedWMin = 32;
constexpr int kBgLockedWMax = 8192;

// ⚠️ 下限**不是 100**（D-114）。
//
// 早期这里是 100，理由是"再小就露边" —— 而**那个约束已经取消了**
//（D-112：图比区域小时照样居中露边，是允许的）。
// 卡在 100 的话用户想把图缩小、让它作为一角的小装饰都做不到。
//
// 10% 而不是 0：0 会让图彻底消失，而"图不见了"和"图很小"在用户那边
// 是两件事；留一个下限至少保证还看得见、还能拖回来。
constexpr int kBgZoomMinPct    = 10;
constexpr int kBgZoomMaxPct    = 400;   // 再往上就是一块纯色了，没有意义
constexpr int kBgOffsetMinPct  = -100;
constexpr int kBgOffsetMaxPct  = 100;

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

// 算适配。四种自动方式 + 手动构图都返回**同一套** src/dst，
// 绘制侧不必分支 —— 只有 Tile 特殊（它靠 tile 标志，由绘制侧循环贴）。
//
// 【为什么值得单独测】这是最容易差一像素的地方：Cover 的裁剪要
// "宁可少一像素也不要越界"，Contain 的居中要处理奇数差（不能用整数除 2 了事，
// 否则 3 像素的差会偏 1 像素，图看着就是歪的）。
BgPlacement ComputeBgPlacement(int imgW, int imgH, int dstW, int dstH, BgFit fit,
                               const BgManual& manual = BgManual{});

// 夹取手动参数。
//
// ⚠️ 这里**不再**保证"图完全覆盖区域"（D-108）。
//
// 手动构图的基础是**高度铺满**：`zoom = 100` 时图的高度正好等于区域高度，
// 宽度按原图比例。宽图会左右超出（可以拖），**窄图会左右露边**
// （露出面板底色）—— 露边是**有意**的：强制"必须盖满"就回到了 Cover，
// 而用户要的恰恰是"一进来别就把图放大"。
//
// 所以这里只保证两件事：缩放不低于 100（再小就比工作区还矮了）、
// 偏移在 ±100。**能不能拖得动**由 BgManualRange 决定 ——
// 图比区域窄/矮的那个方向范围是 0，也就是拖不动。
BgManual ClampBgManual(const BgManual& m);

// ★ Manual 模式下图的**缩放因子**（D-132）。
//
// ⚠️ **必须是唯一一处**。ComputeBgPlacement 和 BgManualRange 都要用它 ——
//    两边算得不一样的话，预览里拖到底和实际画出来的位置就对不上，
//    而那种不一致**只看着一边的时候发现不了**（D-108 记过这个坑）。
//
// 两种模式：
//   * `locked` —— 用用户指定的**绝对宽度**，不随面板尺寸变。
//     这是用户要的"这张图就按这么大显示"（面板缩放时构图不乱）。
//   * 否则 —— `高度铺满 × 百分比`。这是默认行为，
//     基准是 dstH/imgH，也就是说 100% = 图高等于区域高。
double BgManualScale(int imgW, int imgH, int dstH, const BgManual& m);

// 手动构图的"可移动范围"（像素，单边；**可能为 0**）。
// 供预览控件把鼠标位移换算成 offset 百分比 —— 它必须和 ComputeBgPlacement
// 用**同一个**基数（高度铺满）和同一份范围，否则预览里拖到底和面板里拖到底
// 是两个位置，而那种不一致只看着一边的时候发现不了。
void BgManualRange(int imgW, int imgH, int dstW, int dstH, const BgManual& m,
                   int& outRangeX, int& outRangeY);

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

// 把背景图按 **source-over** 合成到已经铺好底色的 BGRA 缓冲上（原地改 dst）。
//
// outMask 可选：非 0 表示"这个像素被图盖过"。调用方需要它来区分
// "图"和"后来画上去的文字"（见 BlendBgOver 的调用点）。
//
// 【为什么是"叠"而不是"替"】背景图带自己的 alpha（由 bgOpacity 决定），
// 半透明的图下面必须有底色兜着 —— 直接替换的话面板会变成"图有多透明、
// 面板就有多透明"，透出桌面。而用户要的是"面板底色上有一张图"。
//
// ⚠️ **alpha 也要合成**，公式是标准的 source-over：
//        A_out = A_src + A_dst * (1 - A_src)
//        RGB_out = (src*RGB_src + dst*RGB_dst*(1-A_src)) / A_out
//
//    这里曾经刻意"不动 alpha"，理由是"面板整体不透明度和图的不透明度
//    是两件独立的事"。**那个理由是错的** —— 不动 alpha 的话，下游那段
//    "和底色不同就算文字、把 alpha 拉到 255" 的修正会把整片图变成
//    完全不透明，用户调「图片不透明度」就完全看不出来，
//    表现成"图贴上去很硬、没有透明的感觉"（用户 2026-09-26 报的正是这个）。
//
//    两者确实独立，但独立的是**参数**，不是**合成结果** ——
//    图盖在底色上，那一块的最终不透明度本来就应该由两者共同决定。
void BlendBgOver(unsigned char* dst, const unsigned char* src, size_t pixelCount,
                 unsigned char* outMask = nullptr);

} // namespace lyricus
