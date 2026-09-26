// 首选项页布局与配色 —— 纯函数，离线可测。
//
// 【为什么值得单独测】这一页是全自绘的：没有控件，一切都是"算矩形 + 画"。
// 布局一旦算错，表现是控件叠在一起、或者跑到看不见的地方，而这类问题
// 在截图里靠肉眼找很费劲 —— 但它本身恰恰是纯计算，单测台能精确钉住。
//
// 用户 2026-09-26 的原话是「这个界面有点老旧，在 lyricus 那个二级界面
// 实现一个更现代化的面板」；重做之前先把"不会画歪"这件事固定下来。

#include "prefs_layout.h"

#include <cstdio>

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

using lyricus::PrefsLayout;

bool IsEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

bool Overlap(const RECT& a, const RECT& b) {
    if (IsEmpty(a) || IsEmpty(b)) return false;   // 没画的东西不参与
    return a.left < b.right && b.left < a.right &&
           a.top  < b.bottom && b.top < a.bottom;
}

int W(const RECT& r) { return r.right - r.left; }
int H(const RECT& r) { return r.bottom - r.top; }

constexpr int kDpis[] = { 96, 120, 144, 192 };

// 按"逻辑尺寸 + dpi"算出物理尺寸，再布局
PrefsLayout LayoutLogical(int wLogical, int hLogical, int dpi) {
    return lyricus::ComputePrefsLayout(MulDiv(wLogical, dpi, 96),
                                       MulDiv(hLogical, dpi, 96), dpi);
}

PrefsLayout Default96() {
    return LayoutLogical(lyricus::kPrefsWidth96, lyricus::kPrefsHeight96, 96);
}

// ---------------------------------------------------------------------------
void TestDefaultLayout() {
    std::printf("\n== 默认尺寸下的整体布局 ==\n");

    const PrefsLayout L = Default96();

    int missing = 0;
    for (int i = 0; i < lyricus::kPrefsColorCount; ++i) {
        if (IsEmpty(L.cards[i]) || IsEmpty(L.cardLabels[i])) ++missing;
    }
    Check(missing == 0, "★ 六个色块和它们的名称都有位置");
    Check(!IsEmpty(L.titleColors), "分组标题一有位置");
    Check(!IsEmpty(L.titleAlpha),  "分组标题二有位置");
    Check(!IsEmpty(L.alphaValue),  "不透明度数值有位置");
    Check(!IsEmpty(L.slider),      "滑块有位置");
    Check(!IsEmpty(L.hint),        "底部说明有位置");
    Check(!IsEmpty(L.reset),       "「恢复默认」按钮有位置");
}

// ---------------------------------------------------------------------------
// 全自绘最典型的 bug：元素叠在一起。这条是本次重做最该钉住的东西。
// ---------------------------------------------------------------------------
void TestNothingOverlaps() {
    std::printf("\n== 任何尺寸下元素都不重叠 ==\n");

    int bad = 0;
    for (int dpi : kDpis) {
        for (int w = 220; w <= 900; w += 7) {
            for (int h = 200; h <= 700; h += 11) {
                const PrefsLayout L = LayoutLogical(w, h, dpi);
                if (IsEmpty(L.cards[0])) continue;   // 这一档整体降级了

                // 色块两两不叠
                for (int i = 0; i < lyricus::kPrefsColorCount; ++i) {
                    for (int j = i + 1; j < lyricus::kPrefsColorCount; ++j) {
                        if (Overlap(L.cards[i], L.cards[j])) ++bad;
                    }
                    // 色块与自己的名称也不能叠（名称在下方）
                    if (Overlap(L.cards[i], L.cardLabels[i])) ++bad;
                }

                // 色块与下面那些东西也不叠
                for (int i = 0; i < lyricus::kPrefsColorCount; ++i) {
                    if (Overlap(L.cards[i], L.slider))      ++bad;
                    if (Overlap(L.cards[i], L.reset))       ++bad;
                    if (Overlap(L.cards[i], L.titleAlpha))  ++bad;
                    if (Overlap(L.cardLabels[i], L.slider)) ++bad;
                    if (Overlap(L.cardLabels[i], L.reset))  ++bad;
                }

                if (Overlap(L.slider, L.reset))  ++bad;
                if (Overlap(L.slider, L.hint))   ++bad;
                if (Overlap(L.reset,  L.hint))   ++bad;

                // 两个标题不能叠（它们分属上下两组）
                if (Overlap(L.titleColors, L.titleAlpha)) ++bad;
            }
        }
    }
    Check(bad == 0, "★ 220~900 x 200~700、四种 DPI 全扫：没有任何两个元素重叠");
}

// ---------------------------------------------------------------------------
void TestInsideClientArea() {
    std::printf("\n== 元素不越出客户区 ==\n");

    int bad = 0, checked = 0;
    for (int dpi : kDpis) {
        for (int w = 220; w <= 900; w += 7) {
            for (int h = 500; h <= 700; h += 11) {   // 只扫"高度够"的档
                const int wp = MulDiv(w, dpi, 96), hp = MulDiv(h, dpi, 96);
                const PrefsLayout L = LayoutLogical(w, h, dpi);
                if (IsEmpty(L.cards[0])) continue;
                ++checked;

                const RECT all[] = {
                    L.cards[0], L.cards[1], L.cards[2],
                    L.cards[3], L.cards[4], L.cards[5],
                    L.titleColors, L.titleAlpha, L.alphaValue,
                    L.slider, L.hint, L.reset,
                };
                for (const RECT& r : all) {
                    if (IsEmpty(r)) continue;
                    if (r.left < 0 || r.top < 0 || r.right > wp || r.bottom > hp) ++bad;
                }
            }
        }
    }
    Check(bad == 0, "★ 高度充足的档位里，所有元素都落在客户区内");
    Check(checked > 0, "（确实扫到了有布局的档位）");
}

// ---------------------------------------------------------------------------
void TestGrid() {
    std::printf("\n== 色块网格 ==\n");

    const PrefsLayout L = Default96();

    // 第一行三张：top 相同
    Check(L.cards[0].top == L.cards[1].top && L.cards[1].top == L.cards[2].top,
          "第一行三张色块顶对齐");
    Check(L.cards[3].top == L.cards[4].top && L.cards[4].top == L.cards[5].top,
          "第二行三张色块顶对齐");
    Check(L.cards[0].top < L.cards[3].top, "第二行在第一行下面");

    // 列对齐
    Check(L.cards[0].left == L.cards[3].left, "同一列的色块左对齐");

    // 等宽等高
    Check(W(L.cards[0]) == W(L.cards[1]) && W(L.cards[1]) == W(L.cards[2]),
          "同一行色块等宽");
    Check(H(L.cards[0]) == H(L.cards[3]), "两行色块等高");

    // 水平递增（不重叠且有序）
    Check(L.cards[0].right <= L.cards[1].left &&
          L.cards[1].right <= L.cards[2].left, "同一行的色块左右不相接");

    // 名称紧贴在色块下方（间距应当很小，而不是飘到很远）
    const int gapPx = L.cardLabels[0].top - L.cards[0].bottom;
    Check(gapPx >= 0 && gapPx <= 8, "名称紧贴色块下方（0~8 像素）");
    Check(W(L.cardLabels[0]) == W(L.cards[0]), "名称与色块等宽");
}

// ---------------------------------------------------------------------------
void TestDegrade() {
    std::printf("\n== 太窄就不画 ==\n");

    Check(IsEmpty(LayoutLogical(120, 400, 96).cards[0]),
          "★ 宽度 120 逻辑像素（连一张卡片都放不下）-> 返回空布局，不画");
    Check(IsEmpty(LayoutLogical(400, 90, 96).cards[0]),
          "★ 高度 90 逻辑像素 -> 同样不画");

    Check(IsEmpty(lyricus::ComputePrefsLayout(0,   400, 96).cards[0]), "宽 0 -> 空");
    Check(IsEmpty(lyricus::ComputePrefsLayout(400, 0,   96).cards[0]), "高 0 -> 空");
    Check(IsEmpty(lyricus::ComputePrefsLayout(400, 400, 0).cards[0]),  "dpi 0 -> 空");
    Check(IsEmpty(lyricus::ComputePrefsLayout(-5,  400, 96).cards[0]), "负宽 -> 空");

    // 回到正常尺寸应当恢复
    Check(!IsEmpty(LayoutLogical(400, 400, 96).cards[0]), "恢复正常尺寸后又画得出来");
}

// ---------------------------------------------------------------------------
void TestDpiScaling() {
    std::printf("\n== DPI 缩放 ==\n");

    const PrefsLayout a = LayoutLogical(lyricus::kPrefsWidth96, lyricus::kPrefsHeight96, 96);
    const PrefsLayout b = LayoutLogical(lyricus::kPrefsWidth96, lyricus::kPrefsHeight96, 192);

    Check(W(b.cards[0]) == W(a.cards[0]) * 2, "DPI 翻倍 -> 色块宽度也翻倍");
    Check(H(b.cards[0]) == H(a.cards[0]) * 2, "DPI 翻倍 -> 色块高度也翻倍");
    Check(b.slider.top == a.slider.top * 2,   "DPI 翻倍 -> 纵向位置也翻倍");
    Check(b.dpi == 192 && a.dpi == 96,        "dpi 回传给绘制侧");
}

// ---------------------------------------------------------------------------
void TestTheme() {
    std::printf("\n== 主题配色 ==\n");

    using lyricus::PrefsLuminance;

    Check(PrefsLuminance(RGB(255, 255, 255)) > 250, "白色的亮度接近 255");
    Check(PrefsLuminance(RGB(0, 0, 0)) == 0,        "黑色的亮度是 0");
    Check(PrefsLuminance(RGB(0, 0, 255)) < PrefsLuminance(RGB(0, 255, 0)),
          "纯绿比纯蓝亮得多（加权而不是平均）");

    // 深色主题：底色暗、文字亮，且两者差别足够大
    const auto dark = lyricus::MakePrefsTheme(true, RGB(32, 32, 34), RGB(238, 238, 242));
    Check(PrefsLuminance(dark.pageBg) < 128, "深色主题的页面底是暗的");
    Check(PrefsLuminance(dark.text) > 128,   "深色主题的文字是亮的");
    Check(PrefsLuminance(dark.text) - PrefsLuminance(dark.pageBg) > 100,
          "★ 文字与底的亮度差 > 100（保证读得清）");
    Check(PrefsLuminance(dark.cardBg) > PrefsLuminance(dark.pageBg),
          "深色下卡片底比页面底**亮**一点（浮起来）");
    Check(PrefsLuminance(dark.textDim) > PrefsLuminance(dark.pageBg),
          "深色下次要文字仍比底亮");

    // 浅色主题：反过来
    const auto light = lyricus::MakePrefsTheme(false, RGB(255, 255, 255), RGB(26, 26, 28));
    Check(PrefsLuminance(light.pageBg) > 200, "浅色主题的页面底是亮的");
    Check(PrefsLuminance(light.text) < 100,   "浅色主题的文字是暗的");
    Check(PrefsLuminance(light.text) - PrefsLuminance(light.pageBg) < -100,
          "★ 文字与底的亮度差 < -100");
    Check(PrefsLuminance(light.cardBg) < PrefsLuminance(light.pageBg),
          "浅色下卡片底比页面底**暗**一点");
    Check(PrefsLuminance(light.textDim) < PrefsLuminance(light.pageBg),
          "浅色下次要文字仍比底暗");

    // ⚠️ 关键：以**背景亮度**为准，而不是只信 dark 参数。
    // 万一宿主给了个和标志不一致的底色，也不能出现"浅底浅字"。
    const auto mismatch = lyricus::MakePrefsTheme(true, RGB(250, 250, 250), RGB(20, 20, 20));
    Check(PrefsLuminance(mismatch.cardBg) < PrefsLuminance(mismatch.pageBg),
          "★ 声明 dark=true 但底色其实是浅的 -> 按浅色处理，卡片底仍然比底暗");
    Check(PrefsLuminance(mismatch.text) < PrefsLuminance(mismatch.pageBg),
          "★ 且文字仍然比底暗（不会出现浅底浅字）");
}

} // namespace

int main() {
    std::printf("======== 首选项页布局与配色（纯逻辑）========\n");
    TestDefaultLayout();
    TestNothingOverlaps();
    TestInsideClientArea();
    TestGrid();
    TestDegrade();
    TestDpiScaling();
    TestTheme();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
