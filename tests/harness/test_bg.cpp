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

int main() {
    std::printf("======== 面板背景图（纯计算）========\n");
    TestPlacement();
    TestBlur();
    TestDimOpacity();
    TestBlend();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
