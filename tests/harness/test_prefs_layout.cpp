// 首选项页布局与配色 —— 纯函数，离线可测。
//
// 【为什么值得单独测】这一页是全自绘的：没有控件，一切都是"算矩形 + 画"。
// 布局一旦算错，表现是控件叠在一起、或者跑到看不见的地方，而这类问题
// 在截图里靠肉眼找很费劲 —— 但它本身恰恰是纯计算，单测台能精确钉住。
//
// 用户 2026-09-26 的原话是「这个界面有点老旧，在 lyricus 那个二级界面
// 实现一个更现代化的面板」；重做之前先把"不会画歪"这件事固定下来。

#include "prefs_layout.h"
#include "color_util.h"   // ColorLuminance（从 prefs_layout 移到了公共层）

#include <cstdio>
#include <cstdlib>        // std::abs（<cstdio> 不提供它）

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
    Check(!IsEmpty(L.fontBtn),     "「字体...」按钮有位置（默认尺寸下放得下）");
    // ---- 外观预设（D-088）----
    Check(!IsEmpty(L.titlePreset),  "「外观预设」标题有位置");
    Check(!IsEmpty(L.presetCombo),  "预设下拉有位置");
    Check(!IsEmpty(L.presetSave),   "「保存」有位置");
    Check(!IsEmpty(L.presetDelete), "「删除」有位置");
    Check(!IsEmpty(L.presetImport), "「导入」有位置");
    Check(!IsEmpty(L.presetExport), "「导出」有位置");
    // ---- 控件配色（D-093）----
    Check(!IsEmpty(L.titleCtrl),    "「控件配色」标题有位置");
    Check(!IsEmpty(L.ctrlModeBtn),  "模式开关有位置");
    {
        int miss = 0;
        for (int i = 0; i < lyricus::kPrefsCtrlColorCount; ++i) {
            if (IsEmpty(L.ctrlCards[i]) || IsEmpty(L.ctrlCardLabels[i])) ++miss;
        }
        Check(miss == 0, "4 个控件基色块都有位置");
    }
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

                // 「字体...」按钮（窄窗口下会被布局省略，空矩形跳过）
                if (Overlap(L.fontBtn, L.reset))  ++bad;
                if (Overlap(L.fontBtn, L.slider)) ++bad;
                if (Overlap(L.fontBtn, L.hint))   ++bad;

                // ---- 外观预设区（D-088）----
                // 区内两两之间
                if (Overlap(L.presetCombo,  L.presetSave))    ++bad;
                if (Overlap(L.presetDelete, L.presetImport))  ++bad;
                if (Overlap(L.presetDelete, L.presetExport))  ++bad;
                if (Overlap(L.presetImport, L.presetExport))  ++bad;
                // 和它上面那一行（滑块）、下面那一行（提示 / 按钮）
                if (Overlap(L.titlePreset,  L.slider))        ++bad;
                if (Overlap(L.presetCombo,  L.slider))        ++bad;
                if (Overlap(L.titlePreset,  L.hint))          ++bad;
                if (Overlap(L.presetExport, L.hint))          ++bad;
                if (Overlap(L.presetExport, L.reset))         ++bad;
                if (Overlap(L.presetExport, L.fontBtn))       ++bad;

                // ---- 控件配色区（D-093）----
                if (Overlap(L.titleCtrl,   L.ctrlModeBtn))    ++bad;
                if (Overlap(L.titleCtrl,   L.slider))         ++bad;
                if (Overlap(L.ctrlModeBtn, L.slider))         ++bad;
                if (Overlap(L.titleCtrl,   L.titlePreset))    ++bad;
                // 区内 4 个色块两两之间
                for (int i = 0; i < lyricus::kPrefsCtrlColorCount; ++i) {
                    if (Overlap(L.ctrlCards[i], L.titleCtrl))      ++bad;
                    if (Overlap(L.ctrlCards[i], L.titlePreset))    ++bad;
                    if (Overlap(L.ctrlCards[i], L.ctrlModeBtn))    ++bad;
                    if (Overlap(L.ctrlCardLabels[i], L.titlePreset)) ++bad;
                    for (int j = i + 1; j < lyricus::kPrefsCtrlColorCount; ++j) {
                        if (Overlap(L.ctrlCards[i],      L.ctrlCards[j]))      ++bad;
                        if (Overlap(L.ctrlCardLabels[i], L.ctrlCardLabels[j])) ++bad;
                    }
                }
                for (int i = 0; i < lyricus::kPrefsColorCount; ++i) {
                    if (Overlap(L.presetCombo,  L.cards[i]))      ++bad;
                    if (Overlap(L.presetCombo,  L.cardLabels[i])) ++bad;
                    if (Overlap(L.presetExport, L.cards[i]))      ++bad;
                    if (Overlap(L.presetExport, L.cardLabels[i])) ++bad;
                }
                for (int i = 0; i < lyricus::kPrefsColorCount; ++i) {
                    if (Overlap(L.fontBtn, L.cards[i]))      ++bad;
                    if (Overlap(L.fontBtn, L.cardLabels[i])) ++bad;
                }

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
    // ⚠️ 起始高度**跟着 kPrefsHeight96 走，不写死**。
    //
    // 这个循环的前提是"只扫高度够的档位" —— 而页面每加一区就会长高。
    // 写死起始值的话，那个前提会在某次改动后**悄悄失效**：
    // 循环开始扫放不下的档位，报出来的是"布局越界了"，
    // 而真正的原因是**测试扫错了档位**，会往完全错的方向查。
    // （加控件配色区时正是这么挂的：页面长到 596，这里还从 500 起扫。）
    const int hFrom = lyricus::kPrefsHeight96;
    for (int dpi : kDpis) {
        for (int w = 220; w <= 900; w += 7) {
            for (int h = hFrom; h <= hFrom + 200; h += 11) {
                const int wp = MulDiv(w, dpi, 96), hp = MulDiv(h, dpi, 96);
                const PrefsLayout L = LayoutLogical(w, h, dpi);
                if (IsEmpty(L.cards[0])) continue;
                ++checked;

                const RECT all[] = {
                    L.cards[0], L.cards[1], L.cards[2],
                    L.cards[3], L.cards[4], L.cards[5],
                    L.titleColors, L.titleAlpha, L.alphaValue,
                    L.slider, L.hint, L.reset, L.fontBtn,
                    L.titlePreset, L.presetCombo, L.presetSave,
                    L.presetDelete, L.presetImport, L.presetExport,
                    L.titleCtrl, L.ctrlModeBtn,
                    L.ctrlCards[0], L.ctrlCards[1], L.ctrlCards[2], L.ctrlCards[3],
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

    using lyricus::ColorLuminance;

    Check(ColorLuminance(RGB(255, 255, 255)) > 250, "白色的亮度接近 255");
    Check(ColorLuminance(RGB(0, 0, 0)) == 0,        "黑色的亮度是 0");
    Check(ColorLuminance(RGB(0, 0, 255)) < ColorLuminance(RGB(0, 255, 0)),
          "纯绿比纯蓝亮得多（加权而不是平均）");

    // 深色主题：底色暗、文字亮，且两者差别足够大
    const auto dark = lyricus::MakePrefsTheme(true, RGB(32, 32, 34), RGB(238, 238, 242));
    Check(ColorLuminance(dark.pageBg) < 128, "深色主题的页面底是暗的");
    Check(ColorLuminance(dark.text) > 128,   "深色主题的文字是亮的");
    Check(ColorLuminance(dark.text) - ColorLuminance(dark.pageBg) > 100,
          "★ 文字与底的亮度差 > 100（保证读得清）");
    Check(ColorLuminance(dark.cardBg) > ColorLuminance(dark.pageBg),
          "深色下卡片底比页面底**亮**一点（浮起来）");
    Check(ColorLuminance(dark.textDim) > ColorLuminance(dark.pageBg),
          "深色下次要文字仍比底亮");

    // 浅色主题：反过来
    const auto light = lyricus::MakePrefsTheme(false, RGB(255, 255, 255), RGB(26, 26, 28));
    Check(ColorLuminance(light.pageBg) > 200, "浅色主题的页面底是亮的");
    Check(ColorLuminance(light.text) < 100,   "浅色主题的文字是暗的");
    Check(ColorLuminance(light.text) - ColorLuminance(light.pageBg) < -100,
          "★ 文字与底的亮度差 < -100");
    Check(ColorLuminance(light.cardBg) < ColorLuminance(light.pageBg),
          "浅色下卡片底比页面底**暗**一点");
    Check(ColorLuminance(light.textDim) < ColorLuminance(light.pageBg),
          "浅色下次要文字仍比底暗");

    // ⚠️ 关键：以**背景亮度**为准，而不是只信 dark 参数。
    // 万一宿主给了个和标志不一致的底色，也不能出现"浅底浅字"。
    const auto mismatch = lyricus::MakePrefsTheme(true, RGB(250, 250, 250), RGB(20, 20, 20));
    Check(ColorLuminance(mismatch.cardBg) < ColorLuminance(mismatch.pageBg),
          "★ 声明 dark=true 但底色其实是浅的 -> 按浅色处理，卡片底仍然比底暗");
    Check(ColorLuminance(mismatch.text) < ColorLuminance(mismatch.pageBg),
          "★ 且文字仍然比底暗（不会出现浅底浅字）");
}

// ---------------------------------------------------------------------------
// 颜色工具（BlendColor / ColorLuminance）
//
// 这两条本来分散在四个文件里各有一份（dui_element / cui_panel / ui_draw /
// prefs_layout），2026-09-26 合并进 color_util。这里除了测它本身，
// 还要**证明合并没改行为** —— 见下面复刻的 float 参考实现。
// ---------------------------------------------------------------------------
void TestColorUtil() {
    std::printf("\n== 颜色工具 ==\n");

    using lyricus::BlendColor;
    using lyricus::ColorLuminance;   // 上一个用例里的 using 只活在那个函数内

    const COLORREF black = RGB(0, 0, 0);
    const COLORREF white = RGB(255, 255, 255);

    Check(BlendColor(white, black, 0.0) == white, "t=0 取第一个");
    Check(BlendColor(white, black, 1.0) == black, "t=1 取第二个");
    Check(BlendColor(RGB(10, 20, 30), RGB(200, 100, 50), 0.5) == RGB(105, 60, 40),
          "t=0.5 取中点");
    Check(BlendColor(white, white, 0.7) == white, "同一个颜色怎么混都不变");

    // ★ 外推：t 为负表示朝第二个颜色的**反方向**推。
    //   dui_element.cpp 靠 BlendColor(normalText, background, -0.5)
    //   让当前行比正文色更亮 —— 合并时特意保留了这个能力。
    const COLORREF base = RGB(100, 100, 100);
    const COLORREF out  = BlendColor(base, black, -0.5);
    Check(ColorLuminance(out) > ColorLuminance(base),
          "★ t=-0.5 朝黑色的反方向外推 -> 比原色更亮");
    Check(out == RGB(150, 150, 150), "外推的数值也对（100 往反方向推半格 = 150）");

    // 外推很容易越界，必须夹住
    Check(BlendColor(white, white, -1.0) == white, "★ 外推到超出 255 时被夹住");
    Check(BlendColor(black, black, 2.0)  == black, "★ 外推到低于 0 时被夹住");

    // 亮度
    Check(ColorLuminance(RGB(255, 255, 255)) > 250, "白色亮度接近 255");
    Check(ColorLuminance(RGB(0, 0, 0)) == 0,       "黑色亮度是 0");
    Check(ColorLuminance(RGB(0, 255, 0)) > ColorLuminance(RGB(0, 0, 255)),
          "纯绿比纯蓝亮得多（加权而不是平均）");

    // ★ 合并等价性：复刻合并前 dui_element / cui_panel 用的 float 版，
    //   全范围扫一遍看结果是否一致。合并最怕的就是"看着一样、结果差一点"，
    //   而这种差值在界面上根本看不出来，只会在某次配色偏一点时冒出来。
    auto blendFloatRef = [](COLORREF from, COLORREF to, float t) -> COLORREF {
        const auto mix = [t](BYTE a, BYTE b) -> BYTE {
            const float v = static_cast<float>(a) +
                            (static_cast<float>(b) - static_cast<float>(a)) * t;
            if (v <= 0.0f)   return 0;
            if (v >= 255.0f) return 255;
            return static_cast<BYTE>(v + 0.5f);
        };
        return RGB(mix(GetRValue(from), GetRValue(to)),
                   mix(GetGValue(from), GetGValue(to)),
                   mix(GetBValue(from), GetBValue(to)));
    };

    int diff = 0, worst = 0;
    for (int r = 0; r <= 255; r += 17) {
        for (int t = -10; t <= 30; ++t) {
            const double tt = t / 20.0;                       // -0.5 .. 1.5
            const COLORREF a = RGB(r, 255 - r, (r * 7) % 256);
            const COLORREF b = RGB((r * 3) % 256, 128, 255 - (r % 200));
            const COLORREF x = BlendColor(a, b, tt);
            const COLORREF y = blendFloatRef(a, b, static_cast<float>(tt));
            const int d = std::abs(static_cast<int>(GetRValue(x)) - static_cast<int>(GetRValue(y)))
                        + std::abs(static_cast<int>(GetGValue(x)) - static_cast<int>(GetGValue(y)))
                        + std::abs(static_cast<int>(GetBValue(x)) - static_cast<int>(GetBValue(y)));
            if (d > worst) worst = d;
            if (d > 2) ++diff;                                // 允许取整差 1/分量
        }
    }
    Check(diff == 0, "★ 与合并前的 float 版逐点一致（误差 <= 1/分量）—— 合并没改行为");
    std::printf("      （最大分量偏差合计 %d）\n", worst);
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
    TestColorUtil();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
