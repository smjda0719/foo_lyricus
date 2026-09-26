#include "bg_math.h"

#include <algorithm>
#include <cmath>

namespace lyricus {

namespace {

int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

BgPlacement ComputeBgPlacement(int imgW, int imgH, int dstW, int dstH, BgFit fit) {
    BgPlacement p;
    if (imgW <= 0 || imgH <= 0 || dstW <= 0 || dstH <= 0) return p;   // valid 保持 false

    p.valid = true;

    switch (fit) {
    case BgFit::Tile:
        // 平铺：整图按原尺寸反复贴。src/dst 只表达"一个 tile 有多大"，
        // 真正铺满由绘制侧循环做。
        p.src = RECT{ 0, 0, imgW, imgH };
        p.dst = RECT{ 0, 0, imgW, imgH };
        p.tile = true;
        return p;

    case BgFit::Stretch:
        // 拉伸：整图 -> 整个目标区域。会变形，但用户选了这条路。
        p.src = RECT{ 0, 0, imgW, imgH };
        p.dst = RECT{ 0, 0, dstW, dstH };
        p.tile = false;
        return p;

    case BgFit::Cover:
    case BgFit::Contain:
        break;
    }

    const double sx = static_cast<double>(dstW) / imgW;
    const double sy = static_cast<double>(dstH) / imgH;
    // Cover 取大的那个比例（铺满、裁掉多余），Contain 取小的（整图可见、留边）
    const double s = (fit == BgFit::Cover) ? (std::max)(sx, sy) : (std::min)(sx, sy);

    // 图按这个比例缩放后在目标区域里占多大
    const int drawW = static_cast<int>(std::lround(imgW * s));
    const int drawH = static_cast<int>(std::lround(imgH * s));

    // ⚠️ 居中用**除法分配余数**，不是 (dstW - drawW) / 2 各取一半。
    //    两者在差为奇数时差 1 像素，而 1 像素的偏心在纯色背景上看不出来、
    //    在有明显边缘的图上会看得很清楚（图歪了一点点）。
    //    Cover 时 dst 通常等于目标区域，这段只在 Contain 下有意义。
    const int dstX = (dstW - drawW) / 2;
    const int dstY = (dstH - drawH) / 2;

    if (fit == BgFit::Cover) {
        // Cover：目标区域被完全铺满，所以**源图要裁**。
        // srcW = imgW / s —— 用比例反推，而不是 imgW - dstW / s 那种写法，
        // 后者在 s < 1（图比区域大）时容易出负数。
        int srcW = static_cast<int>(std::lround(dstW / s));
        int srcH = static_cast<int>(std::lround(dstH / s));

        // ⚠️ 向上取整之后再夹回图内。lround 可能算出比图大 1 的值
        //   （dstW / s 本应 ≤ imgW，但浮点误差会让它冒出 0.5 个像素），
        //   那一步不夹的话源矩形会越界，WIC 那边直接报错或者画出黑边。
        if (srcW > imgW) srcW = imgW;
        if (srcH > imgH) srcH = imgH;
        if (srcW < 1) srcW = 1;
        if (srcH < 1) srcH = 1;

        const int srcX = (imgW - srcW) / 2;
        const int srcY = (imgH - srcH) / 2;

        p.src = RECT{ srcX, srcY, srcX + srcW, srcY + srcH };
        p.dst = RECT{ 0, 0, dstW, dstH };
    } else {
        // Contain：整图可见，源不裁，缩到目标区域居中
        p.src = RECT{ 0, 0, imgW, imgH };
        p.dst = RECT{ dstX, dstY, dstX + drawW, dstY + drawH };
    }
    return p;
}

void BoxBlurBgra(unsigned char* bgra, int w, int h, int radiusPx) {
    if (bgra == nullptr || w <= 0 || h <= 0) return;

    const int r = ClampInt(radiusPx, 0, kBgBlurMax);
    if (r == 0) return;   // 0 = 不模糊，直接返回（省掉整趟遍历）

    // 盒子宽度 = 2r+1。三次迭代近似高斯（中心极限定理）。
    const int box = 2 * r + 1;
    const int iterations = 3;

    // 行缓冲和列缓冲：滑动窗口求和，所以每趟是 O(w*h) 与半径**无关**。
    //
    // ⚠️ `(std::max)(w, h)` 那对括号是**必需的**，不是风格问题：
    //    `windows.h` 把 max/min 定义成了宏，预处理器在模板解析**之前**
    //    就会把 `std::max(w, h)` 拆成 `std::(((w)>(h))?(w):(h))`，报 C2589。
    //    包一层括号能让宏匹配不上。
    //    这个坑项目里踩过（见 lyrics_view.cpp 的注释「已经踩过一次，
    //    索性全部手写比较」），lyric_search.cpp 也留着同样的括号。
    std::vector<int> acc((std::max)(w, h), 0);

    // 只模糊 BGR 三个通道 —— alpha 保持原样。
    // 模糊 alpha 会让图片边缘渗出一圈半透明，那是错的：整幅图的透明度
    // 由 bgOpacity 统一控制，不是逐像素的事。
    auto blurPass = [&](bool horizontal) {
        const int outer = horizontal ? h : w;   // 有多少条线
        const int inner = horizontal ? w : h;   // 每条线多长
        const int stepX = horizontal ? 4 : w * 4;
        const int stepY = horizontal ? w * 4 : 4;

        for (int o = 0; o < outer; ++o) {
            unsigned char* line = bgra + (horizontal ? o * w * 4 : o * 4);

            for (int ch = 0; ch < 3; ++ch) {
                // 初始窗口：前 r 个像素各按"镜像"补（用边界像素顶替），
                // 这样边缘不会往内吸暗色（不补的话图四周会发黑一圈）。
                long long sum = 0;
                for (int i = -r; i <= r; ++i) {
                    const int idx = ClampInt(i, 0, inner - 1);
                    sum += line[idx * stepX + ch];
                }

                for (int i = 0; i < inner; ++i) {
                    acc[i] = static_cast<int>(sum / box);

                    // 窗口右移一格：加进右边新来的，减去左边离开的
                    const int addIdx = ClampInt(i + r + 1, 0, inner - 1);
                    const int subIdx = ClampInt(i - r,     0, inner - 1);
                    sum += line[addIdx * stepX + ch];
                    sum -= line[subIdx * stepX + ch];
                }

                for (int i = 0; i < inner; ++i) {
                    line[i * stepX + ch] = static_cast<unsigned char>(acc[i]);
                }
            }
            // 换下一条线
            (void)stepY;
        }
    };

    for (int it = 0; it < iterations; ++it) {
        blurPass(true);    // 横
        blurPass(false);   // 竖
    }
}

void ApplyDimAndOpacity(unsigned char* bgra, int w, int h, int dimPct, int opacityPct) {
    if (bgra == nullptr || w <= 0 || h <= 0) return;

    const int dim = ClampInt(dimPct,     kBgDimMin,     kBgDimMax);
    const int op  = ClampInt(opacityPct, kBgOpacityMin, kBgOpacityMax);

    // 两个都是"乘法"就可以合成一次，不必逐像素做两遍。
    //   压暗：c * (1 - dim/100)
    //   透明：a * op/100
    // 用整数查表避免每像素两次除法 —— 缓冲可能有几百万像素。
    unsigned char dimLut[256];
    const int dimNum = 100 - dim;
    for (int i = 0; i < 256; ++i) dimLut[i] = static_cast<unsigned char>(i * dimNum / 100);

    const bool needDim = (dim != 0);
    const bool needOp  = (op != 100);

    if (!needDim && !needOp) return;   // 什么都不用做，别白跑一趟

    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        unsigned char* px = bgra + i * 4;   // BGRA
        if (needDim) {
            px[0] = dimLut[px[0]];
            px[1] = dimLut[px[1]];
            px[2] = dimLut[px[2]];
        }
        if (needOp) {
            px[3] = static_cast<unsigned char>(px[3] * op / 100);
        }
    }
}

} // namespace lyricus
