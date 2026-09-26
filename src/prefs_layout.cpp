#include "prefs_layout.h"

// ---------------------------------------------------------------------------
// 首选项页的布局与配色。
//
// 布局是「从上往下码」的：每画完一段就把 y 往下推，段与段之间的间距写成
// 具名常量而不是散落的魔数 —— 全自绘界面里，"某个东西突然压到另一个上面"
// 几乎总是因为间距被改乱了。
//
// 所有尺寸都先按 **96 dpi 的逻辑像素**写，再用 S() 缩放。这样高 DPI 下
// 整套比例一致，不用给每个数字各写一份。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

// 逻辑像素 -> 物理像素
int S(int dpi, int v) { return MulDiv(v, dpi, 96); }

// a 与 b 按 t 混合（t=0 全取 a，t=1 全取 b）。
//
// 用它从"背景色"推导出卡片底/悬停/边框：这样无论宿主是纯白、纯黑还是
// 某个带色调的主题，推导出来的几档都是**同一个色系**，不会打架。
COLORREF Blend(COLORREF a, COLORREF b, double t) {
    auto mix = [t](int x, int y) {
        int v = static_cast<int>(x * (1.0 - t) + y * t + 0.5);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return v;
    };
    return RGB(mix(GetRValue(a), GetRValue(b)),
               mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

// 卡片区一共两行、每行最多三列。
constexpr int kCols = 3;
constexpr int kRows = 2;

} // namespace

int PrefsLuminance(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

PrefsLayout ComputePrefsLayout(int width, int height, int dpi) {
    PrefsLayout out;
    if (width <= 0 || height <= 0 || dpi <= 0) return out;
    out.dpi = dpi;

    auto Sx = [dpi](int v) { return MulDiv(v, dpi, 96); };

    const int padX = Sx(20);
    out.padX = padX;

    // ---- 色块网格 ----
    const int gap    = Sx(8);
    const int colW   = (width - padX * 2 - gap * (kCols - 1)) / kCols;
    const int cardH  = Sx(44);
    const int labelH = Sx(14);

    // 窄到这个地步就别画了 —— 画出来也点不中，不如让宿主自己看起来空着。
    if (colW < Sx(48) || height < Sx(120)) return out;

    const int labelGap = Sx(4);
    const int rowGap   = Sx(12);

    int y = Sx(16);

    // ---- 分组标题一 ----
    out.titleColors = RECT{ padX, y, width - padX, y + Sx(14) };
    y += Sx(14) + Sx(10);

    // ---- 两行色块 ----
    for (int row = 0; row < kRows; ++row) {
        for (int c = 0; c < kCols; ++c) {
            const int i = row * kCols + c;
            if (i >= kPrefsColorCount) break;

            const int x = padX + c * (colW + gap);
            out.cards[i]      = RECT{ x, y, x + colW, y + cardH };
            out.cardLabels[i] = RECT{ x, y + cardH + labelGap,
                                      x + colW, y + cardH + labelGap + labelH };
        }
        y += cardH + labelGap + labelH + rowGap;
    }

    // ---- 分组标题二（左标题 + 右侧数值）----
    out.titleAlpha = RECT{ padX, y, padX + Sx(140), y + Sx(14) };
    out.alphaValue = RECT{ width - padX - Sx(90), y, width - padX, y + Sx(14) };
    y += Sx(14) + Sx(8);

    // ---- 不透明度滑块 ----
    const int sliderH = Sx(24);
    out.slider = RECT{ padX, y, width - padX, y + sliderH };
    y += sliderH + Sx(14);

    // ---- 底部说明 ----
    out.hint = RECT{ padX, y, width - padX, y + Sx(32) };
    y += Sx(32) + Sx(10);

    // ---- 按钮 ----
    // 贴着左下角；面板不够高时它会探出去 —— 那种情况下由绘制侧照常画，
    // 因为宿主给的区域本来就装不下整页（用户把首选项窗口拖小了）。
    const int btnW = Sx(88), btnH = Sx(26);
    out.reset = RECT{ padX, y, padX + btnW, y + btnH };

    return out;
}

PrefsTheme MakePrefsTheme(bool dark, COLORREF bg, COLORREF fg) {
    PrefsTheme t;
    t.pageBg = bg;
    t.text   = fg;

    // ⚠️ 不只信 dark 参数，而是**以背景的实际亮度为准**。
    // 宿主偶尔会给一个和 dark 标志不一致的背景（例如 dark=true 但底色其实很浅），
    // 那时按亮度走才不会出现"浅底 + 浅字"这种读不了字的组合。
    // dark 只在亮度处于中间地带（不黑不白）时用来打破平局。
    const int lum    = PrefsLuminance(bg);
    const bool bgDark = (lum < 128) || (lum >= 100 && lum < 160 && dark);

    if (bgDark) {
        t.textDim = Blend(fg, bg, 0.45);
        t.cardBg  = Blend(bg, RGB(255, 255, 255), 0.08);
        t.cardHot = Blend(bg, RGB(255, 255, 255), 0.16);
        t.border  = Blend(bg, RGB(255, 255, 255), 0.22);
        t.accent  = RGB(76, 160, 235);
    } else {
        t.textDim = Blend(fg, bg, 0.42);
        t.cardBg  = Blend(bg, RGB(0, 0, 0), 0.05);
        t.cardHot = Blend(bg, RGB(0, 0, 0), 0.10);
        t.border  = Blend(bg, RGB(0, 0, 0), 0.16);
        t.accent  = RGB(0, 120, 212);
    }
    return t;
}

} // namespace lyricus
