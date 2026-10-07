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

BgManual ClampBgManual(const BgManual& m) {
    BgManual r = m;
    // 缩放下限是 100（= 刚好铺满）而不是 0：再小就露边，
    // 而露出来的那块是面板底色 —— 看起来像"图没加载全"。
    if (r.zoomPct < kBgZoomMinPct) r.zoomPct = kBgZoomMinPct;
    if (r.zoomPct > kBgZoomMaxPct) r.zoomPct = kBgZoomMaxPct;
    if (r.offsetXPct < kBgOffsetMinPct) r.offsetXPct = kBgOffsetMinPct;
    if (r.offsetXPct > kBgOffsetMaxPct) r.offsetXPct = kBgOffsetMaxPct;
    if (r.offsetYPct < kBgOffsetMinPct) r.offsetYPct = kBgOffsetMinPct;
    if (r.offsetYPct > kBgOffsetMaxPct) r.offsetYPct = kBgOffsetMaxPct;
    // 锁定宽度（D-132）。夹到 0 而不是 kBgLockedWMin ——
    // 0 表示"还没设过"，而此时 locked 若是 true，BgManualScale 会退回到
    // 百分比模式，不会算出一张 32 像素的图。
    if (r.lockedW < 0) r.lockedW = 0;
    if (r.lockedW > kBgLockedWMax) r.lockedW = kBgLockedWMax;
    // 旋转倍数要取模而不是夹取（D-133）—— 夹取的话 4 会变成 3（270°），
    // 而 4 按定义就是 0（转一整圈）。手改配置写个 5 也不该变成 270°。
    r.rotate90 = ((r.rotate90 % 4) + 4) % 4;
    return r;
}

double BgManualScale(int imgW, int imgH, int dstH, const BgManual& mIn) {
    if (imgW <= 0 || imgH <= 0 || dstH <= 0) return 1.0;
    const BgManual m = ClampBgManual(mIn);

    // ⚠️ 用**变换后**的有效尺寸（D-133）。90°/270° 旋转宽高互换，
    //    继续按原图算的话"高度铺满"的基准就错了 —— 表现是转一下
    //    图突然放大或缩小，而用户只是转了个方向。
    int effW = 0, effH = 0;
    BgEffectiveSize(imgW, imgH, m, effW, effH);

    // 锁定：用绝对宽度。**不乘 dstH** —— 那正是它存在的意义。
    // 注意这里除以 effW 也得是有效宽 —— lockedW 说的是"转完之后多宽"。
    if (m.locked && m.lockedW > 0) {
        return static_cast<double>(m.lockedW) / effW;
    }
    // 默认：高度铺满 × 百分比
    return (static_cast<double>(dstH) / effH) * (m.zoomPct / 100.0);
}

void BgManualRange(int imgW, int imgH, int dstW, int dstH, const BgManual& m,
                   int& outRangeX, int& outRangeY) {
    outRangeX = 0;
    outRangeY = 0;
    if (imgW <= 0 || imgH <= 0 || dstW <= 0 || dstH <= 0) return;

    const BgManual c = ClampBgManual(m);
    // ⚠️ 缩放因子走 BgManualScale —— 它和 ComputeBgPlacement 的 Manual 分支
    //    是**同一份实现**（D-108 / D-132）。从前这里各写一遍 base 表达式、
    //    靠注释提醒"必须保持一致"；现在靠代码结构，想不一致都难。
    const double s = BgManualScale(imgW, imgH, dstH, c);

    // 返回**幅度**（>= 0）：图比区域大时是"能往里挪多少"，
    // 图比区域小时是"能往外挪多少"（D-112）。
    // ⚠️ 两种情形的幅度都必须是正数 —— 图小时返回 0 的话拖动会**完全没反应**，
    //    而"拖不动"和"已经到边了"在用户那边看着一模一样。
    //
    // ⚠️ 用**变换后**的有效尺寸（D-133）。这里的幅度必须和
    //    ComputeBgPlacement 画出来的图一致，否则预览里拖到底、
    //    实际画出来却不在那个位置（D-108 记过这类"两边各算一份"的不一致）。
    int effW = 0, effH = 0;
    BgEffectiveSize(imgW, imgH, c, effW, effH);
    const int halfW = (static_cast<int>(std::lround(effW * s)) - dstW) / 2;
    const int halfH = (static_cast<int>(std::lround(effH * s)) - dstH) / 2;
    outRangeX = (halfW >= 0) ? halfW : -halfW;
    outRangeY = (halfH >= 0) ? halfH : -halfH;
}

void ApplyFlipRotate(std::vector<unsigned char>& px, int& w, int& h,
                     bool flipH, bool flipV, int rotate90) {
    if (w <= 0 || h <= 0) return;
    if (px.size() < static_cast<size_t>(w) * h * 4) return;

    // ---- 镜像：就地 ----
    if (flipH) {
        for (int y = 0; y < h; ++y) {
            unsigned char* row = px.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0, x2 = w - 1; x < x2; ++x, --x2) {
                for (int c = 0; c < 4; ++c) std::swap(row[x * 4 + c], row[x2 * 4 + c]);
            }
        }
    }
    if (flipV) {
        const size_t stride = static_cast<size_t>(w) * 4;
        for (int y = 0, y2 = h - 1; y < y2; ++y, --y2) {
            unsigned char* a = px.data() + static_cast<size_t>(y)  * stride;
            unsigned char* b = px.data() + static_cast<size_t>(y2) * stride;
            for (size_t i = 0; i < stride; ++i) std::swap(a[i], b[i]);
        }
    }

    // ---- 旋转 ----
    const int r = ((rotate90 % 4) + 4) % 4;
    if (r == 0) return;

    const int nw = (r == 2) ? w : h;
    const int nh = (r == 2) ? h : w;
    std::vector<unsigned char> out(static_cast<size_t>(nw) * nh * 4);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int nx = 0, ny = 0;
            switch (r) {
            case 1:  nx = h - 1 - y; ny = x;         break;   // 顺时针 90°
            case 2:  nx = w - 1 - x; ny = h - 1 - y; break;   // 180°
            default: nx = y;         ny = w - 1 - x; break;   // 顺时针 270°
            }
            const unsigned char* s =
                px.data() + (static_cast<size_t>(y) * w + x) * 4;
            unsigned char* d =
                out.data() + (static_cast<size_t>(ny) * nw + nx) * 4;
            // 用 std::copy 而不是 memcpy —— 这一层不想为了 4 字节引入 <cstring>，
            // 而 <algorithm> 本来就在（上面用了 std::swap）。
            std::copy(s, s + 4, d);
        }
    }
    px.swap(out);
    w = nw;
    h = nh;
}

BgPlacement ComputeBgPlacement(int imgW, int imgH, int dstW, int dstH, BgFit fit,
                               const BgManual& manualIn) {
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

    case BgFit::Manual: {
        // 用户自己拖出来的构图（D-103 / D-108）。
        //
        // 先夹参数再算 —— 后面的范围计算依赖"图 >= 区域"这个前提，
        // 而那个前提正是 ClampBgManual 保证的（zoom >= 100）。
        const BgManual m = ClampBgManual(manualIn);

        // ⚠️ 基数是**高度铺满**，不是 Cover 的"两边都铺满"（D-108）。
        //
        // 【为什么】用户的期望是「初始时图的高度和工作区一样高」。
        // 用 Cover 的话，宽图会被按**宽度**铺满、于是比工作区高得多 ——
        // 一打开预览就是一张放大了的图，得先缩小才能看全，而用户
        // 根本还没做任何操作。高度铺满则一进来就是"高度上尽收眼底"，
        // 宽度按比例。
        //
        // ⚠️ 代价：宽图左右超出（可以拖，没问题），**窄图会左右露边**
        //    （露出面板底色）。露边是**有意**的 —— 强制"必须盖满"
        //    就回到了 Cover，而用户要的恰恰是"别一上来就放大"。
        //
        // ⚠️ 这块基准算在 BgManualScale 里（D-132）—— 除了上面这条"高度铺满"，
        //    它还要处理**锁定尺寸**那种情况：用户主动指定绝对宽度时，
        //    这个 base 整个不用，图的大小不随面板变。
        //    两处（这里和 BgManualRange）必须同一份实现，所以抽出去了。
        const double s = BgManualScale(imgW, imgH, dstH, m);

        // ⚠️ 用**变换后**的有效尺寸（D-133）。90°/270° 旋转宽高互换，
        //    这里还按原图算的话目标矩形的宽高就是反的 ——
        //    表现是转一下图突然变形或者尺寸跳变，而用户只是转了个方向。
        int effW = 0, effH = 0;
        BgEffectiveSize(imgW, imgH, m, effW, effH);
        const int drawW = static_cast<int>(std::lround(effW * s));
        const int drawH = static_cast<int>(std::lround(effH * s));
        // ⚠️ 给缩放结果**封顶**（D-105）。
        //
        // 手动缩放最大 400%，而 base 本身就可能很大（一张小图铺满大面板）——
        // 两者相乘能让 drawW 到几万像素，那个中间缓冲要几百 MB，
        // 而且下游按像素算偏移时在那种量级下更容易出纰漏。
        // 封顶之后最坏是"图被放得没那么大"，而不是崩。
        //
        // 上限取 8192：超过这个尺寸的中间位图在内存和耗时上都不划算，
        // 而 8192 已经远大于任何真实面板（4K 屏上也就 3840 宽）。
        constexpr int kMaxDrawSide = 8192;
        if (drawW > kMaxDrawSide || drawH > kMaxDrawSide) {
            const double shrink = (std::min)(
                static_cast<double>(kMaxDrawSide) / drawW,
                static_cast<double>(kMaxDrawSide) / drawH);
            const int cw = static_cast<int>(std::lround(drawW * shrink));
            const int ch = static_cast<int>(std::lround(drawH * shrink));
            int rx = 0, ry = 0;
            BgManualRange(imgW, imgH, dstW, dstH, m, rx, ry);
            const int x0 = rx;
            const int y0 = ry;
            // 按缩小后的尺寸重新算位置，保持同样的相对构图
            const int nx = -x0 - static_cast<int>(
                               static_cast<long long>(x0) * m.offsetXPct / 100);
            const int ny = -y0 - static_cast<int>(
                               static_cast<long long>(y0) * m.offsetYPct / 100);
            p.src = RECT{ 0, 0, imgW, imgH };
            p.dst = RECT{ nx, ny, nx + cw, ny + ch };
            p.tile = false;
            return p;
        }

        // ★ 可移动幅度用**有符号**的 (图 - 区域) / 2（D-112）。
        //
        //   > 0：图比区域大，幅度是"能往里挪多少"——挪到头图仍盖满区域；
        //   < 0：图比区域小，幅度是"能往外挪多少"——挪到头图仍完全落在区域里；
        //   = 0：正好一样大，那个方向拖不动（几何上确实没余地）。
        //
        // ⚠️ 之前这里只认 > 0 的情形（`range = max(0, half)`），于是
        //    **图比区域小时那个方向的余量算成 0、拖动直接没有反应**。
        const int halfW = (drawW - dstW) / 2;
        const int halfH = (drawH - dstH) / 2;
        const int rangeX = (halfW >= 0) ? halfW : -halfW;
        const int rangeY = (halfH >= 0) ? halfH : -halfH;

        // 三个锚点（横向，纵向同理）：
        //   offset = -100 -> 图往右推到底
        //   offset =    0 -> 居中
        //   offset = +100 -> 图往左推到底
        //
        // ⚠️ **不要**再按 halfW 的正负加符号（D-114）。
        //    加过一版，结果是"图比区域小时拖动方向相反" ——
        //    因为拖动那边（OnBgPreviewDrag）的符号是写死的 `offset -= dx`，
        //    这里再翻一次就和它对不上了。
        //    现在两边都只认一个方向：**offset 增大 = 图往左**。
        //    图宽时含义是"往里挪到头仍盖满"，图小时是"往外挪到头仍全在区域里"，
        //    两种情形下"往右推到底"都是同一个动作，语义是连续的。
        //
        // 用 long long 算中间量：rangeX 在极端 dpi 下可能到几万，乘 100 之后再
        // 叠加仍在 int 内，但留点余量更稳妥。
        const int dstX = -halfW - static_cast<int>(
                             static_cast<long long>(rangeX) * m.offsetXPct / 100);
        const int dstY = -halfH - static_cast<int>(
                             static_cast<long long>(rangeY) * m.offsetYPct / 100);

        // 源**不裁**（整图都参与）：手动模式下"看到图的哪一块"完全由
        // dst 的位置决定 —— 把裁剪也放进 src 会让两套坐标都要维护，
        // 而绘制侧只需要一个 dst 矩形。
        p.src = RECT{ 0, 0, imgW, imgH };
        p.dst = RECT{ dstX, dstY, dstX + drawW, dstY + drawH };
        p.tile = false;
        return p;
    }

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

void BlendBgOver(unsigned char* dst, const unsigned char* src, size_t pixelCount,
                 unsigned char* outMask) {
    if (dst == nullptr || src == nullptr) return;

    for (size_t i = 0; i < pixelCount; ++i) {
        const int sa = src[3];
        const int da = dst[3];

        if (sa == 0) {
            // 全透明：底色原样保留。**必须早退** —— 不判的话下面的
            // 除法会把 dst 一点点拉向 src 的黑色（src 透明区的 RGB 是 0），
            // 面板边角会慢慢变黑。
            if (outMask) outMask[i] = 0;
        } else {
            // 标准 source-over。注意这里算的是**非预乘**的 RGB 和合成后的 alpha。
            //   A_out = A_s + A_d*(1 - A_s)
            const int aOut = sa + da * (255 - sa) / 255;
            if (aOut <= 0) {
                if (outMask) outMask[i] = 0;
                dst += 4; src += 4;
                continue;
            }
            // RGB_out = (src*sa/255 + dst*da/255*(255-sa)/255) / (aOut/255)
            //         = (src*sa*255 + dst*da*(255-sa)) / (255 * aOut)
            //
            // ⚠️ 第二项**不要**在括号里先除 255 —— 那样会把 dst 那一半
            //    算小 255 倍。这是配方子时最容易写错的一处：
            //    先把三项各自化成同一分母（255*255），再一次性除。
            //    手算验证：sa=128, da=255, src=255, dst=0 ->
            //      aOut = 128 + 255*127/255 = 255
            //      num  = 255*128*255 + 0 = 8323200
            //      v    = 8323200 / (255*255) = 128   （黑白各半，对）
            for (int c = 0; c < 3; ++c) {
                const long long num = static_cast<long long>(src[c]) * sa * 255 +
                                      static_cast<long long>(dst[c]) * da * (255 - sa);
                int v = static_cast<int>(num / (static_cast<long long>(aOut) * 255));
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                dst[c] = static_cast<unsigned char>(v);
            }
            dst[3] = static_cast<unsigned char>(aOut);
            if (outMask) outMask[i] = 1;
        }
        dst += 4; src += 4;
    }
}

} // namespace lyricus
