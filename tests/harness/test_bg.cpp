// 面板背景图的纯计算 —— 离线可测。
//
// 【为什么值得单独测】模糊和适配是全套功能里最容易出错的两块，而它们恰恰
// 是"在界面上试不出来"的：
//   * 适配差一像素 —— 纯色底上看不出来，有边缘的图上会看出"歪了一点"；
//   * 源矩形越界 —— 浮点误差在边界上冒出 1 像素，后果是 WIC 报错或者画出黑边，
//     而那看起来像"图坏了"，不像"算错了"；
//   * 模糊半径大于图宽 —— 直接崩或者整幅变黑。
//
// 这一组还钉着两条**设计意图**，它们防的是将来有人"顺手优化"掉：
//   ① 模糊**不碰 alpha** —— 碰了图片边缘会渗出一圈半透明；
//   ② 压暗和不透明度都在**加载时**一次算完 —— 挪到每帧去算的话，
//      面板重绘会平白多几百万次乘法。

#include "bg_math.h"

#include <cstdio>
#include <vector>

// 这些名字全在 lyricus 里。下面大量用 BgFit::Cover 这种写法，
// 加一条 using 比到处补 lyricus:: 更好读，也不容易漏。
using namespace lyricus;

// 手动构图的参数范围（D-103）
using lyricus::kBgZoomMinPct;
using lyricus::kBgZoomMaxPct;
using lyricus::kBgOffsetMinPct;
using lyricus::kBgOffsetMaxPct;

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

bool EqRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

int RectW(const RECT& r) { return r.right - r.left; }
int RectH(const RECT& r) { return r.bottom - r.top; }

// 一块纯色 BGRA 缓冲，用来验证模糊和压暗
std::vector<unsigned char> MakeSolid(int w, int h, unsigned char b, unsigned char g,
                                     unsigned char r, unsigned char a) {
    std::vector<unsigned char> v(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < v.size(); i += 4) {
        v[i] = b; v[i + 1] = g; v[i + 2] = r; v[i + 3] = a;
    }
    return v;
}

// ---------------------------------------------------------------------------
void TestPlacement() {
    std::printf("\n== 适配（ComputeBgPlacement）==\n");

    // 退化输入一律 invalid —— 绘制侧靠这个标志跳过，而不是靠"算出来是空矩形"
    Check(!ComputeBgPlacement(0, 100, 100, 100, BgFit::Cover).valid,   "源宽 0 -> invalid");
    Check(!ComputeBgPlacement(100, 0, 100, 100, BgFit::Cover).valid,   "源高 0 -> invalid");
    Check(!ComputeBgPlacement(100, 100, 0, 100, BgFit::Cover).valid,   "目标宽 0 -> invalid");
    Check(!ComputeBgPlacement(100, 100, 100, 0, BgFit::Cover).valid,   "目标高 0 -> invalid");
    Check(!ComputeBgPlacement(-5, 100, 100, 100, BgFit::Cover).valid,  "负数 -> invalid");

    // Stretch：整图 -> 整个目标区域
    {
        const auto p = ComputeBgPlacement(200, 100, 400, 300, BgFit::Stretch);
        Check(p.valid && !p.tile, "Stretch 有效且不平铺");
        Check(EqRect(p.src, RECT{0, 0, 200, 100}), "★ Stretch：源 = 整图");
        Check(EqRect(p.dst, RECT{0, 0, 400, 300}), "★ Stretch：目标 = 整个区域");
    }

    // Tile：按**原尺寸**铺，不缩放
    {
        const auto p = ComputeBgPlacement(200, 100, 400, 300, BgFit::Tile);
        Check(p.tile, "★ Tile：标了平铺");
        Check(RectW(p.dst) == 200 && RectH(p.dst) == 100,
              "★ Tile：按原尺寸（不被缩放到目标大小）");
    }

    // Cover：铺满目标，源图被裁
    {
        // 图 400x100 -> 区域 200x200。比例取 max(0.5, 2.0) = 2.0
        // 源宽 = 200/2 = 100（裁掉四分之三宽度），源高 = 200/2 = 100（用满）
        const auto p = ComputeBgPlacement(400, 100, 200, 200, BgFit::Cover);
        Check(EqRect(p.dst, RECT{0, 0, 200, 200}), "★ Cover：目标被完全铺满");
        Check(RectW(p.src) == 100, "★ Cover：源宽度被裁到 100");
        Check(RectH(p.src) == 100, "★ Cover：源高度用满");
        Check(p.src.left >= 0 && p.src.top >= 0 &&
              p.src.right <= 400 && p.src.bottom <= 100, "★ Cover：源矩形在图内");
    }

    // Contain：整图可见，居中留边
    {
        // 图 400x100 -> 区域 200x200。比例取 min(0.5, 2.0) = 0.5
        // 画出来 200x50，垂直居中 (200-50)/2 = 75
        const auto p = ComputeBgPlacement(400, 100, 200, 200, BgFit::Contain);
        Check(EqRect(p.src, RECT{0, 0, 400, 100}), "★ Contain：源 = 整图（不裁）");
        Check(RectW(p.dst) == 200 && RectH(p.dst) == 50, "★ Contain：按较小比例缩放");
        Check(p.dst.top == 75 && p.dst.left == 0, "★ Contain：居中");
    }

    // ★ 源矩形永不越界 —— 这是整套里最要紧的一条不变量。
    //   浮点误差最容易在"图比区域小"和"长宽比刚好对不上"这两类组合上冒头，
    //   所以这里把尺寸扫得密一些。
    {
        int bad = 0, n = 0, checkedPlacement = 0;
        for (int iw = 1; iw <= 200; iw += 7) {
            for (int ih = 1; ih <= 200; ih += 11) {
                for (int dw = 1; dw <= 300; dw += 23) {
                    for (int dh = 1; dh <= 300; dh += 29) {
                        for (int f = 0; f <= lyricus::kBgFitMax; ++f) {
                            const auto p = ComputeBgPlacement(iw, ih, dw, dh,
                                                              static_cast<lyricus::BgFit>(f));
                            ++n;
                            if (!p.valid) { ++bad; continue; }
                            if (p.tile) { ++checkedPlacement; continue; }
                            ++checkedPlacement;
                            if (p.src.left < 0 || p.src.top < 0 ||
                                p.src.right > iw || p.src.bottom > ih ||
                                RectW(p.src) <= 0 || RectH(p.src) <= 0) ++bad;
                        }
                    }
                }
            }
        }
        Check(bad == 0, "★ 上千种尺寸组合：源矩形始终在图内且非空");
        Check(checkedPlacement > 1000, "（确实扫到了足够多的组合）");
    }

    // 越界的另一种可能：**目标**矩形。Contain 会自己算 dst，那里也可能算飞。
    {
        int bad = 0;
        for (int iw = 1; iw <= 120; iw += 13) {
            for (int ih = 1; ih <= 120; ih += 17) {
                for (int dw = 1; dw <= 200; dw += 31) {
                    for (int dh = 1; dh <= 200; dh += 37) {
                        const auto p = ComputeBgPlacement(iw, ih, dw, dh, lyricus::BgFit::Contain);
                        if (!p.valid) continue;
                        // Contain 的 dst 必须整个落在目标区域内（否则图会被画出界）
                        if (p.dst.left < 0 || p.dst.top < 0 ||
                            p.dst.right > dw || p.dst.bottom > dh) ++bad;
                    }
                }
            }
        }
        Check(bad == 0, "★ Contain 的目标矩形始终落在区域内");
    }
}

// ---------------------------------------------------------------------------
void TestBlur() {
    std::printf("\n== 盒式模糊（BoxBlurBgra）==\n");

    // 半径 0 = 不模糊，原样返回
    {
        auto v = MakeSolid(8, 8, 10, 20, 30, 200);
        const auto before = v;
        lyricus::BoxBlurBgra(v.data(), 8, 8, 0);
        Check(v == before, "★ 半径 0 -> 一个像素都不动");
    }

    // 纯色模糊之后还是那个色（盒式模糊的滑动平均对常量是恒等的）
    {
        auto v = MakeSolid(16, 16, 10, 20, 30, 200);
        lyricus::BoxBlurBgra(v.data(), 16, 16, 4);
        Check(v[0] == 10 && v[1] == 20 && v[2] == 30, "★ 纯色图模糊后颜色不变");
    }

    // ★ alpha 通道**一点都不动**。
    //   模糊它会让图片边缘渗出一圈半透明 —— 整幅图的透明度由 bgOpacity
    //   统一控制，不是逐像素的事。
    {
        auto v = MakeSolid(24, 24, 100, 100, 100, 137);
        lyricus::BoxBlurBgra(v.data(), 24, 24, 6);
        bool alphaOk = true;
        for (size_t i = 3; i < v.size(); i += 4) {
            if (v[i] != 137) { alphaOk = false; break; }
        }
        Check(alphaOk, "★ 模糊不碰 alpha（碰了边缘会渗半透明）");
    }

    // 交界处会被抹平：左黑右白 -> 模糊后中间出现灰
    {
        auto v = MakeSolid(32, 8, 0, 0, 0, 255);
        for (int y = 0; y < 8; ++y) {
            for (int x = 16; x < 32; ++x) {
                unsigned char* px = v.data() + (static_cast<size_t>(y) * 32 + x) * 4;
                px[0] = px[1] = px[2] = 255;
            }
        }
        lyricus::BoxBlurBgra(v.data(), 32, 8, 6);
        const unsigned char mid = v[(static_cast<size_t>(4) * 32 + 16) * 4];
        Check(mid > 20 && mid < 235, "★ 黑白交界被抹出中间灰（确实模糊了）");
    }

    // 半径大于图宽/高：不能崩、不能整幅变黑
    {
        auto v = MakeSolid(4, 3, 200, 100, 50, 255);
        lyricus::BoxBlurBgra(v.data(), 4, 3, lyricus::kBgBlurMax);
        Check(v.size() == 4u * 3 * 4, "★ 半径远大于图尺寸：缓冲大小不变（没越界写）");
        Check(v[0] > 150 && v[2] > 20, "★ 半径远大于图尺寸：颜色还在（没变全黑）");
    }

    // 半径越界会被夹到 [0, kBgBlurMax]，而不是当成负数去算
    {
        auto a = MakeSolid(12, 12, 60, 90, 120, 255);
        auto b = a;
        lyricus::BoxBlurBgra(a.data(), 12, 12, 9999);
        lyricus::BoxBlurBgra(b.data(), 12, 12, lyricus::kBgBlurMax);
        Check(a == b, "★ 半径 9999 被夹到上限（等于 kBgBlurMax 的结果）");
    }

    // 空指针 / 尺寸非法：静默返回，不崩
    {
        lyricus::BoxBlurBgra(nullptr, 10, 10, 4);
        auto v = MakeSolid(4, 4, 1, 2, 3, 4);
        const auto before = v;
        lyricus::BoxBlurBgra(v.data(), 0, 4, 4);
        lyricus::BoxBlurBgra(v.data(), 4, -1, 4);
        Check(v == before, "★ 空指针 / 非法尺寸：静默返回不崩");
    }
}

// ---------------------------------------------------------------------------
void TestDimOpacity() {
    std::printf("\n== 压暗与不透明度（ApplyDimAndOpacity）==\n");

    // dim=0 且 op=100 = 什么都不做（提前返回，不白跑一趟）
    {
        auto v = MakeSolid(8, 8, 100, 150, 200, 220);
        const auto before = v;
        lyricus::ApplyDimAndOpacity(v.data(), 8, 8, 0, 100);
        Check(v == before, "★ dim=0 且 op=100 -> 一个像素都不动");
    }

    // 压暗：颜色变小，alpha 不动
    {
        auto v = MakeSolid(4, 4, 100, 100, 100, 200);
        lyricus::ApplyDimAndOpacity(v.data(), 4, 4, 50, 100);
        Check(v[0] == 50 && v[1] == 50 && v[2] == 50, "★ dim=50 -> 颜色减半");
        Check(v[3] == 200, "★ 压暗不动 alpha");
    }

    // 不透明度：只动 alpha
    {
        auto v = MakeSolid(4, 4, 100, 100, 100, 200);
        lyricus::ApplyDimAndOpacity(v.data(), 4, 4, 0, 50);
        Check(v[0] == 100 && v[1] == 100 && v[2] == 100, "★ op=50 不动颜色");
        Check(v[3] == 100, "★ op=50 -> alpha 减半");
    }

    // op=0 -> 整幅全透明（用户把图"关掉"但不删路径时的效果）
    {
        auto v = MakeSolid(4, 4, 200, 200, 200, 255);
        lyricus::ApplyDimAndOpacity(v.data(), 4, 4, 0, 0);
        bool allZero = true;
        for (size_t i = 3; i < v.size(); i += 4) {
            if (v[i] != 0) { allZero = false; break; }
        }
        Check(allZero, "★ op=0 -> alpha 全 0（图完全不可见）");
    }

    // 越界夹取：dim=100 会让图全黑（没意义），所以上限是 90
    {
        auto a = MakeSolid(4, 4, 250, 250, 250, 255);
        auto b = a;
        lyricus::ApplyDimAndOpacity(a.data(), 4, 4, 100, 100);
        lyricus::ApplyDimAndOpacity(b.data(), 4, 4, lyricus::kBgDimMax, 100);
        Check(a == b, "★ dim=100 被夹到上限 90（不是全黑）");
    }
    {
        auto a = MakeSolid(4, 4, 100, 100, 100, 255);
        auto b = a;
        lyricus::ApplyDimAndOpacity(a.data(), 4, 4, -50, -20);
        lyricus::ApplyDimAndOpacity(b.data(), 4, 4, 0, 0);
        Check(a == b, "★ 负数被夹到下限（不是当成反向着色）");
    }

    // 两者叠加：先压暗再乘 alpha，且**不越界到回绕**
    {
        auto v = MakeSolid(4, 4, 255, 255, 255, 255);
        lyricus::ApplyDimAndOpacity(v.data(), 4, 4, 10, 10);
        Check(v[0] == 229 || v[0] == 230, "★ 255 压暗 10% -> 约 229（没回绕）");
        Check(v[3] == 25 || v[3] == 26, "★ 255 * 10% -> 约 25");
    }

    // 空指针 / 非法尺寸
    {
        lyricus::ApplyDimAndOpacity(nullptr, 10, 10, 50, 50);
        auto v = MakeSolid(4, 4, 1, 2, 3, 4);
        const auto before = v;
        lyricus::ApplyDimAndOpacity(v.data(), 0, 0, 50, 50);
        Check(v == before, "★ 空指针 / 零尺寸：静默返回不崩");
    }
}

} // namespace

// 混合单独放一个函数里（逻辑上属于"背景图怎么贴上去"那一组）
namespace {

void TestBlend() {
    std::printf("\n== 背景图混合（BlendBgOver —— source-over）==\n");

    // ★ 手算验证的基准例：黑白各半。
    //   aOut = 128 + 255*(255-128)/255 = 128 + 127 = 255
    //   num  = 255*128*255 + 0        = 8323200
    //   v    = 8323200 / (255*255)    = 128
    {
        auto dst = MakeSolid(1, 1, 0, 0, 0, 255);          // 底色：黑，不透明
        auto src = MakeSolid(1, 1, 255, 255, 255, 128);    // 图：白，半透明
        unsigned char mask = 0;
        lyricus::BlendBgOver(dst.data(), src.data(), 1, &mask);
        Check(dst[0] > 125 && dst[0] < 131, "★ 黑白各半 -> 约 128（这条是手算对过的）");
        Check(dst[3] == 255, "★ 合成 alpha = 128 + 255*127/255 = 255");
        Check(mask == 1, "★ 掩码标出这个像素被图盖过");
    }

    // 全不透明的图 -> 完全盖住，alpha 也是 255
    {
        auto dst = MakeSolid(2, 2, 10, 20, 30, 200);
        auto src = MakeSolid(2, 2, 100, 110, 120, 255);
        std::vector<unsigned char> mask(4, 0);
        lyricus::BlendBgOver(dst.data(), src.data(), 4, mask.data());
        Check(dst[0] == 100 && dst[1] == 110 && dst[2] == 120, "★ 图全不透明 -> 盖住底色");
        Check(dst[3] == 255, "★ 图全不透明 -> 合成 alpha = 255");
        Check(mask[0] == 1 && mask[3] == 1, "★ 掩码全 1");
    }

    // 全透明的图 -> 底色和 alpha 都完全不变。
    // ⚠️ 这条挡的是"透明区被慢慢拉黑"：不早退的话那三次除法会把底色
    //    一点点拉向 src 的黑色（透明区 RGB = 0），面板边角会越来越暗。
    {
        auto dst = MakeSolid(2, 2, 10, 20, 30, 200);
        auto src = MakeSolid(2, 2, 0, 0, 0, 0);
        std::vector<unsigned char> mask(4, 9);
        const auto before = dst;
        lyricus::BlendBgOver(dst.data(), src.data(), 4, mask.data());
        Check(dst == before, "★ 图全透明 -> 底色和 alpha 都完全不变（没被拉黑）");
        Check(mask[0] == 0 && mask[1] == 0 && mask[2] == 0 && mask[3] == 0,
              "★ 全透明处的掩码是 0（下游据此判定这里没有图）");
    }

    // ★★ 用户报的那个：图盖过的地方，alpha 必须反映**图的不透明度**，
    //     而不是被下游的"和底色不同就算文字"修正拉到 255。
    //     拉到 255 的话图那一块会比周围更"实"，
    //     用户调的「图片不透明度」完全看不出来 ——
    //     表现就是"不会显现出透明的感觉"。
    {
        auto dst = MakeSolid(1, 1, 28, 28, 30, 215);       // 面板底色 + 面板 alpha
        auto src = MakeSolid(1, 1, 200, 100, 50, 100);     // 图，alpha = 100
        unsigned char mask = 0;
        lyricus::BlendBgOver(dst.data(), src.data(), 1, &mask);
        // aOut = 100 + 215*(255-100)/255 = 100 + 130 = 230
        Check(dst[3] == 230, "★★ 图 alpha=100 叠在面板 alpha=215 上 -> 230（不是 255）");
    }

    // 逐像素独立：同一个缓冲里有的盖住、有的透出来
    {
        auto dst = MakeSolid(2, 1, 0, 0, 0, 255);
        auto src = MakeSolid(2, 1, 200, 200, 200, 255);
        src[7] = 0;   // 第二个像素的 alpha 改成 0
        std::vector<unsigned char> mask(2, 0);
        lyricus::BlendBgOver(dst.data(), src.data(), 2, mask.data());
        Check(dst[0] == 200 && mask[0] == 1, "★ 第一个像素被盖住且标了掩码");
        Check(dst[4] == 0 && mask[1] == 0,   "★ 第二个像素（透明）保留底色且掩码为 0");
        Check(dst[3] == 255 && dst[7] == 255, "★ 两处的 alpha 各自正确");
    }

    // 掩码是可选参数：不传的时候行为要和以前一致
    {
        auto dst = MakeSolid(1, 1, 0, 0, 0, 255);
        auto src = MakeSolid(1, 1, 255, 255, 255, 255);
        lyricus::BlendBgOver(dst.data(), src.data(), 1);
        Check(dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255,
              "★ 不传掩码时结果一样（默认参数）");
    }

    // 空指针不崩
    {
        auto v = MakeSolid(2, 2, 1, 2, 3, 4);
        const auto before = v;
        lyricus::BlendBgOver(nullptr, v.data(), 4);
        lyricus::BlendBgOver(v.data(), nullptr, 4);
        Check(v == before, "★ 空指针：静默返回不崩");
    }
}

} // namespace

// 手动构图（D-103）单独一组
namespace {

void TestManual() {
    std::printf("\n== 手动构图（BgFit::Manual）==\n");

    // 夹取
    {
        BgManual m;
        m.zoomPct = 10; m.offsetXPct = -500; m.offsetYPct = 500;
        const BgManual c = lyricus::ClampBgManual(m);
        Check(c.zoomPct == kBgZoomMinPct, "★ 缩放被夹到下限 100（再小会露边）");
        Check(c.offsetXPct == kBgOffsetMinPct && c.offsetYPct == kBgOffsetMaxPct,
              "★ 偏移被夹到 ±100");
    }

    // zoom=100 + offset=0 = 刚好铺满且居中 —— 也就是 Cover 的效果
    {
        const auto p = ComputeBgPlacement(400, 100, 200, 200, BgFit::Manual, BgManual{});
        Check(p.valid && !p.tile, "Manual 有效且不平铺");
        Check(EqRect(p.src, RECT{0, 0, 400, 100}), "★ Manual：源不裁（整图参与）");
        // 比例 = max(200/400, 200/100) = 2.0 -> 画出来 800x200，居中 y = (200-200)/2 = 0
        Check(RectW(p.dst) == 800 && RectH(p.dst) == 200, "★ zoom=100 -> 按铺满比例");
        Check(p.dst.left == -300 && p.dst.top == 0, "★ offset=0 -> 居中");
    }

    // 三个锚点：offset -100 / 0 / +100 分别对应"左贴 / 居中 / 右贴"
    {
        BgManual m;
        m.zoomPct = 200;   // 放大一倍，这样横向有余量可移
        // 图 200x200 -> 区域 200x200。base = 1.0，zoom 2.0 -> 画出来 400x400
        // rangeX = (400-200)/2 = 100
        m.offsetXPct = -100;
        auto p = ComputeBgPlacement(200, 200, 200, 200, BgFit::Manual, m);
        Check(p.dst.left == 0, "★ offset=-100 -> 图左边贴住区域左边");

        m.offsetXPct = 0;
        p = ComputeBgPlacement(200, 200, 200, 200, BgFit::Manual, m);
        Check(p.dst.left == -100, "★ offset=0 -> 居中（左边在 -range）");

        m.offsetXPct = 100;
        p = ComputeBgPlacement(200, 200, 200, 200, BgFit::Manual, m);
        Check(p.dst.right == 200, "★ offset=+100 -> 图右边贴住区域右边");
        Check(p.dst.left == -200, "★ 同上，左边相应在 -2*range");
    }

    // BgManualRange 和实际摆放要对得上（预览控件靠它换算鼠标位移）
    {
        BgManual m; m.zoomPct = 150;
        int rx = 0, ry = 0;
        lyricus::BgManualRange(200, 200, 200, 200, m, rx, ry);
        const auto p = ComputeBgPlacement(200, 200, 200, 200, BgFit::Manual, m);
        Check(rx == -p.dst.left, "★ range 和居中的左边距一致（预览换算才对得上）");
        Check(ry == -p.dst.top, "★ 纵向同理");
        Check(rx > 0, "★ zoom=150 时确实有余量可移");
    }

    // zoom=100 时横向可能没有余量（比例刚好），不能算出负数范围
    {
        int rx = 0, ry = 0;
        lyricus::BgManualRange(400, 100, 200, 200, BgManual{}, rx, ry);
        Check(rx >= 0 && ry >= 0, "★ zoom=100 时范围不为负");
        Check(rx == 300, "★ 但纵向铺满时横向余量很大（400x100 的图铺 200x200）");
    }

    // ★★ D-108 的新不变量：**zoom = 100 时图的高度正好等于区域高度**。
    //
    //    这是"初始尺寸"的定义 —— 用户要的是"一进来别就把图放大"。
    //    旧的不变量是"图必须完全覆盖区域"（Cover 语义），
    //    但那个语义下宽图会被按宽度铺满、比工作区高得多，一打开就是放大的。
    //    露边是有意的：强制盖满就回到了 Cover。
    {
        int bad = 0, n = 0, leaked = 0;
        for (int iw = 40; iw <= 1200; iw += 97) {
            for (int ih = 40; ih <= 1200; ih += 89) {
                for (int dw = 60; dw <= 900; dw += 71) {
                    for (int dh = 40; dh <= 600; dh += 67) {
                        BgManual m;   // 默认 zoom=100、offset=0
                        const auto p = ComputeBgPlacement(iw, ih, dw, dh,
                                                          BgFit::Manual, m);
                        ++n;
                        if (!p.valid) { ++bad; continue; }
                        // ⚠️ 跳过**封顶**的情况：图很小而区域很大时，
                        //    drawW 会被压到 kMaxDrawSide（8192）而触发保护性缩小，
                        //    那时高度不再等于区域高度 —— 那是有意的
                        //    （宁可图小一点，也不要几百 MB 的中间缓冲）。
                        //    注意是 `>=`：封顶后的值**正好等于** 8192，
                        //    写 `>` 的话一个都跳不过去（探针里实测到 8192x527）。
                        if (RectW(p.dst) >= 8192 || RectH(p.dst) >= 8192) continue;
                        // 高度允许 ±1 像素的取整误差
                        const int h = RectH(p.dst);
                        if (h < dh - 1 || h > dh + 1) ++bad;
                        // 水平位置：offset=0 的语义是**始终居中**（D-112）。
                        // 图比区域宽时 dst.left 是负的（超出部分两边各一半），
                        // 窄时是正的（露出的底色两边各一半）—— 都居中。
                        // ⚠️ 这里我改过两次：一开始按"永远居中"写、窄图那半边
                        //    就全判错了；后来改成"窄图贴左"配合当时的实现，
                        //    而实现改成有符号余量之后**又**变成居中了。
                        //    "图小时该居中还是贴左"是实现里的一个选择，
                        //    断言得跟着那个选择走，不能想当然。
                        const int expectLeft = -(RectW(p.dst) - dw) / 2;
                        if (p.dst.left != expectLeft) ++bad;
                        if (RectW(p.dst) < dw) ++leaked;
                    }
                }
            }
        }
        Check(bad == 0, "★★ zoom=100 时图的高度始终等于区域高度（±1 像素取整）");
        Check(n > 500, "（确实扫到了足够多的组合）");
        Check(leaked > 0, "★ 窄图确实会左右露边（那是有意的，不是 bug）");
    }

    // 放大之后高度必须**超出**区域（否则"放大"没意义）
    {
        BgManual m; m.zoomPct = 200;
        const auto p = ComputeBgPlacement(400, 300, 200, 200, BgFit::Manual, m);
        // base = 200/300 -> 图 400x300 画成 267x200；zoom 2 -> 533x400
        Check(RectH(p.dst) == 400, "★ zoom=200 时高度翻倍");
        Check(RectH(p.dst) > 200 && RectW(p.dst) > 200, "★ 放大后两个方向都超出区域");
    }

    // ★★ 余量的语义（D-112）：**图比区域小时那个方向照样能拖**。
    //
    //    幅度 = |(图 - 区域) / 2|，两种情形都是正数：
    //      图大 -> 能往里挪（挪到头图仍盖满区域）
    //      图小 -> 能往外挪（挪到头图仍完全落在区域里）
    //    ⚠️ 之前只认"图大"的情形，图小时幅度算成 0、拖动**完全没反应** ——
    //    而"高度铺满"下纵向恰好等于区域高、竖图横向又比区域窄，
    //    两个方向同时为 0，于是用户报"没办法拖动图片"。
    //
    //    图 100x400 按高度铺满进 400x400：draw = 100x400
    //      zoom=100 -> halfW = (100-400)/2 = -150 -> 幅度 150；halfH = 0
    //      zoom=200 -> draw = 200x800 -> 100 / 200
    //      zoom=400 -> draw = 400x1600 -> 0 / 600
    {
        int rx = 0, ry = 0;
        lyricus::BgManualRange(100, 400, 400, 400, BgManual{}, rx, ry);
        Check(rx == 150, "★★ 竖图 zoom=100：横向能挪 150（图比区域窄，照样能拖）");
        Check(ry == 0,   "★ 纵向 0（高度正好铺满，几何上确实没余地）");

        BgManual m; m.zoomPct = 200;
        lyricus::BgManualRange(100, 400, 400, 400, m, rx, ry);
        Check(rx == 100 && ry == 200, "★ zoom=200：横向 100、纵向 200");

        m.zoomPct = kBgZoomMaxPct;
        lyricus::BgManualRange(100, 400, 400, 400, m, rx, ry);
        Check(rx == 0 && ry == 600, "★ zoom=400：宽度正好等于区域，横向归 0");

        // 宽图：高度铺满后左右超出，横向同样是"有余量"，只是语义相反
        // 图 800x200 -> base = 400/200 = 2.0 -> draw = 1600x400
        // halfW = (1600-400)/2 = 600
        lyricus::BgManualRange(800, 200, 400, 400, BgManual{}, rx, ry);
        Check(rx == 600, "★ 宽图 zoom=100：左右各超出 600 -> 余量 600");
        Check(ry == 0, "（但上下正好铺满）");
    }

    // 越界的参数（手改配置）也要被夹住
    {
        BgManual m;
        m.zoomPct = -50;               // 夹到下限 10%
        m.offsetXPct = 9999;           // 夹到 +100
        m.offsetYPct = -9999;          // 夹到 -100
        const auto p = ComputeBgPlacement(300, 200, 150, 150, BgFit::Manual, m);
        Check(p.valid, "越界参数仍能算出布局");

        // 下限放开到 10% 之后图会明显变小 —— 那是允许的
        Check(RectW(p.dst) < 150 && RectH(p.dst) < 150,
              "★ 缩放被夹到下限后确实缩小了（下限已放开到 10%）");

        // offset 的语义（D-114）：**增大 = 图往左**
        //   +100 -> 图往左推到底 -> 左边贴住区域左边
        //   -100 -> 图往右推到底 -> 右边贴住区域右边
        Check(p.dst.left == 0, "★ offsetX 夹到 +100 -> 图左边贴住区域左边");
        // 纵向同理：-100 -> 图往下推到底 -> 底边贴住区域底边
        //（允许 1 像素取整误差）
        Check(p.dst.bottom >= 149 && p.dst.bottom <= 150,
              "★ offsetY 夹到 -100 -> 图底边贴住区域底边");
    }
}

// ---- 镜像与旋转（D-133）----
//
// 【为什么值得测】"旋转把像素搬对位置了没有"是那种**看着对、实际差一格**
// 的地方：一张风景图转 90° 之后，你没法靠肉眼判断左右是不是反了。
// 而它一旦错了，用户要到很久以后才会发现（或者永远发现不了）。
//
// 手法：造一张每个像素带**唯一编号**的小图，转完逐个查编号落在哪。
// 编号就是"这个像素原来在哪个位置"，一查便知对错。
static std::vector<unsigned char> MakeTagged(int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
            const int tag = y * w + x;                              // 唯一
            p[0] = static_cast<unsigned char>(tag & 0xFF);          // B
            p[1] = static_cast<unsigned char>((tag >> 8) & 0xFF);   // G
            p[2] = 0xAB;                                            // R 哨兵
            p[3] = 255;
        }
    }
    return px;
}
static int TagAt(const std::vector<unsigned char>& px, int w, int x, int y) {
    const unsigned char* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
    return p[0] | (p[1] << 8);
}

void TestFlipRotate() {
    // --- 有效尺寸：只有 90/270 互换 ---
    {
        BgManual m;
        int w = 0, h = 0;
        m.rotate90 = 0; BgEffectiveSize(640, 480, m, w, h);
        Check(w == 640 && h == 480, "有效尺寸 0 度 = 原尺寸");
        m.rotate90 = 1; BgEffectiveSize(640, 480, m, w, h);
        Check(w == 480 && h == 640, "★ 有效尺寸 90 度宽高互换");
        m.rotate90 = 2; BgEffectiveSize(640, 480, m, w, h);
        Check(w == 640 && h == 480, "有效尺寸 180 度不变");
        m.rotate90 = 3; BgEffectiveSize(640, 480, m, w, h);
        Check(w == 480 && h == 640, "★ 有效尺寸 270 度宽高互换");
    }

    // --- ★ 缩放基准要用**有效高**，不是原图高 ---
    //
    // 这是"转一下图突然放大/缩小"的直接来源：基准若还按原图的高算，
    // 90 度之后图的高度变成了原来的宽，比例就整个错了。
    {
        BgManual m;
        m.rotate90 = 0;
        const double s0 = BgManualScale(400, 100, 200, m);   // 高度铺满 200/100 = 2
        Check(s0 > 1.99 && s0 < 2.01, "0 度缩放基准 = dstH / 原图高");

        m.rotate90 = 1;                                       // 有效高变成 400
        const double s1 = BgManualScale(400, 100, 200, m);
        Check(s1 > 0.49 && s1 < 0.51, "★ 90 度缩放基准 = dstH / 原图宽（有效高）");
        Check(s1 < s0, "★ 同一张图转 90 度后缩放因子跟着变（否则图会突然放大）");
    }

    // --- 旋转倍数的取模 ---
    {
        BgManual m;
        m.rotate90 = 4;
        Check(ClampBgManual(m).rotate90 == 0,
              "★ rotate90=4 取模成 0（转一整圈），不是夹到 3");
        m.rotate90 = -1;
        Check(ClampBgManual(m).rotate90 == 3, "rotate90=-1 取模成 3");
        m.rotate90 = 7;
        Check(ClampBgManual(m).rotate90 == 3, "rotate90=7 取模成 3");
    }

    // --- 像素：用带编号的 3x2 图验证每个像素去哪了 ---
    //
    // 原图（x 向右，y 向下）：
    //     0 1 2
    //     3 4 5
    {
        std::vector<unsigned char> px = MakeTagged(3, 2);
        int w = 3, h = 2;
        ApplyFlipRotate(px, w, h, true, false, 0);
        Check(w == 3 && h == 2, "水平镜像不改尺寸");
        Check(TagAt(px, w, 0, 0) == 2 && TagAt(px, w, 2, 0) == 0,
              "★ 水平镜像：第 0 行变成 2 1 0");
        Check(TagAt(px, w, 0, 1) == 5 && TagAt(px, w, 2, 1) == 3,
              "★ 水平镜像：第 1 行变成 5 4 3");
    }
    {
        std::vector<unsigned char> px = MakeTagged(3, 2);
        int w = 3, h = 2;
        ApplyFlipRotate(px, w, h, false, true, 0);
        Check(TagAt(px, w, 0, 0) == 3 && TagAt(px, w, 0, 1) == 0,
              "★ 垂直镜像：上下两行互换");
    }
    {
        // 顺时针 90：原 (x,y) -> 新 (h-1-y, x)，新宽=原高=2、新高=原宽=3
        std::vector<unsigned char> px = MakeTagged(3, 2);
        int w = 3, h = 2;
        ApplyFlipRotate(px, w, h, false, false, 1);
        Check(w == 2 && h == 3, "★ 顺时针 90 度后尺寸互换（3x2 -> 2x3）");
        Check(TagAt(px, w, 1, 0) == 0, "★ 90 度：原 (0,0) 到新 (1,0)");
        Check(TagAt(px, w, 1, 1) == 1, "★ 90 度：原 (1,0) 到新 (1,1)");
        Check(TagAt(px, w, 1, 2) == 2, "★ 90 度：原 (2,0) 到新 (1,2)");
        Check(TagAt(px, w, 0, 0) == 3, "★ 90 度：原 (0,1) 到新 (0,0)");
        Check(TagAt(px, w, 0, 2) == 5, "★ 90 度：原 (2,1) 到新 (0,2)");
    }
    {
        std::vector<unsigned char> px = MakeTagged(3, 2);
        int w = 3, h = 2;
        ApplyFlipRotate(px, w, h, false, false, 2);
        Check(w == 3 && h == 2, "180 度不改尺寸");
        Check(TagAt(px, w, 0, 0) == 5 && TagAt(px, w, 2, 1) == 0,
              "★ 180 度：左上和右下互换");
    }
    {
        std::vector<unsigned char> px = MakeTagged(3, 2);
        int w = 3, h = 2;
        ApplyFlipRotate(px, w, h, false, false, 3);
        Check(w == 2 && h == 3, "270 度尺寸互换");
        Check(TagAt(px, w, 0, 0) == 2, "★ 270 度：原 (2,0) 到新 (0,0)");
    }

    // --- 幂等性 / 周期性：比逐个查位置更能抓住"大致对但有偏差"的实现 ---
    {
        const std::vector<unsigned char> orig = MakeTagged(5, 3);

        std::vector<unsigned char> a = orig;
        int wa = 5, ha = 3;
        ApplyFlipRotate(a, wa, ha, true, false, 0);
        ApplyFlipRotate(a, wa, ha, true, false, 0);
        Check(wa == 5 && ha == 3 && a == orig, "★ 水平镜像两次 = 原图（幂等）");

        std::vector<unsigned char> b = orig;
        int wb = 5, hb = 3;
        for (int i = 0; i < 4; ++i) ApplyFlipRotate(b, wb, hb, false, false, 1);
        Check(wb == 5 && hb == 3 && b == orig, "★ 旋转 90 度连做四次 = 原图");
    }

    // --- 空 / 非法输入不该崩 ---
    {
        std::vector<unsigned char> empty;
        int w = 0, h = 0;
        ApplyFlipRotate(empty, w, h, true, true, 1);
        Check(w == 0 && h == 0 && empty.empty(), "空图：不动也不崩");

        std::vector<unsigned char> shortBuf(8, 0);   // 不够 4x4x4
        int sw = 4, sh = 4;
        ApplyFlipRotate(shortBuf, sw, sh, false, false, 1);
        Check(sw == 4 && sh == 4, "缓冲比声明的尺寸小：原样返回，不改尺寸");
    }

    // --- ★ BgManual::operator==：缓存比对的唯一入口 ---
    //
    // 【为什么单独钉这一组】D-133 的实测 bug 就出在这里：缓存比对
    // （bg_image.cpp 的 SameParams）从前是**手写逐字段**的，加了
    // flipH/flipV/rotate90 却忘了往那里补 —— 于是**改了翻转，预览纹丝不动**
    //（缓存认为"参数没变"），而症状看起来像"预览不刷新"。
    //
    // 现在 SameParams 改成整体比（`e.manual == manual`），
    // 这几条断言保证"再加字段时 operator== 会一起长大"。
    // ⚠️ 加字段的人如果只改了结构体、没改 operator==，这里会红。
    {
        const BgManual a;
        BgManual b;
        Check(a == b, "默认构造的两个 BgManual 相等");

        b = BgManual{}; b.zoomPct = 200;
        Check(a != b, "改 zoomPct 判为不等");
        b = BgManual{}; b.offsetXPct = 30;
        Check(a != b, "改 offsetXPct 判为不等");
        b = BgManual{}; b.offsetYPct = -30;
        Check(a != b, "改 offsetYPct 判为不等");
        b = BgManual{}; b.flipH = true;
        Check(a != b, "★ 只改 flipH 也必须判为不等（否则缓存不失效、预览不刷新）");
        b = BgManual{}; b.flipV = true;
        Check(a != b, "★ 只改 flipV 也必须判为不等");
        b = BgManual{}; b.rotate90 = 1;
        Check(a != b, "★ 只改 rotate90 也必须判为不等");
        b = BgManual{}; b.locked = true;
        Check(a != b, "★ 只改 locked 也必须判为不等");
        b = BgManual{}; b.lockedW = 600;
        Check(a != b, "★ 只改 lockedW 也必须判为不等");

        // 反向：全同才相等（防止 operator== 被写成恒 true）
        b = a;
        Check(a == b, "逐字段相同 -> 相等");
    }

    // --- ★ 接缝：底色不透明时，叠图后 alpha 必须恒定（D-134）---
    //
    // 【这条钉的是用户报的那个观感问题】「图片和底色透明度不一样时会有接缝」。
    //
    // 根因：铺底时写的是**面板 alpha**（比如 200），紧接着把图 source-over 上去，
    //       于是
    //           露边区 alpha = 200
    //           图区域 alpha = 图alpha + 200*(1 - 图alpha)     ← 比如 233
    //       两者不等，图的边缘就有一圈可见的接缝。
    //
    // 修法：铺底写 **255**（把"底色 + 图"当成一层**不透明**的背景），
    //       面板透明度留到最后统一乘。于是 da 恒为 255：
    //           A_out = sa + 255*(255-sa)/255 = sa + 255 - sa = **255**
    //       —— 和 sa 无关。图区域和露边区必然相等。
    //
    // ⚠️ 这几条断言同时是**回归保护**：谁要是把铺底的 255 改回 alpha，
    //    这里立刻会红，而不是等用户又看见接缝。
    {
        for (int sa : {0, 64, 128, 153, 200, 255}) {
            unsigned char dst[4] = {10, 20, 30, 255};
            unsigned char src[4] = {200, 100, 50, static_cast<unsigned char>(sa)};
            BlendBgOver(dst, src, 1, nullptr);
            Check(dst[3] == 255,
                  "★ 底色 alpha=255 时，叠任何 alpha 的图，结果 alpha 恒为 255");
        }

        // 反证：底色若被调透明过，叠图就会改变 alpha —— 那就是接缝的来历。
        // （留着它是为了让"为什么会接缝"这件事在测试里也读得出来。）
        {
            unsigned char dst2[4] = {10, 20, 30, 200};
            unsigned char src2[4] = {200, 100, 50, 153};
            BlendBgOver(dst2, src2, 1, nullptr);
            Check(dst2[3] != 200,
                  "反证：底色 alpha=200 时叠图会改变 alpha（接缝的成因）");
            Check(dst2[3] == 233,
                  "反证：200 + 153*(1-153/255) 算出来正好是 233（和实测一致）");
        }

        // 全透明的图不该改变底色 alpha —— 露边区的样子就是它本来的样子
        {
            unsigned char dst3[4] = {10, 20, 30, 255};
            unsigned char src3[4] = {99, 99, 99, 0};
            BlendBgOver(dst3, src3, 1, nullptr);
            Check(dst3[3] == 255 && dst3[0] == 10,
                  "全透明图：底色原样保留，alpha 不变");
        }
    }
}

} // namespace

int main() {
    std::printf("======== 面板背景图（纯计算）========\n");
    TestPlacement();
    TestManual();
    TestBlur();
    TestDimOpacity();
    TestBlend();
    TestFlipRotate();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
