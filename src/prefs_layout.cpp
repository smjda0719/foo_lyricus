#include "prefs_layout.h"
#include "color_util.h"   // BlendColor / ColorLuminance（与其它自绘处共用同一份）

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

// 卡片区一共两行、每行最多三列。
constexpr int kCols = 3;
constexpr int kRows = 2;

} // namespace

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

    // ---- 控件配色（D-093）----
    //
    // 放在外观预设**之前**：它和上面那些配色是一类东西（都是"面板长什么样"），
    // 而外观预设是"整套快照"的操作，属于另一层。
    out.titleCtrl = RECT{ padX, y, padX + Sx(120), y + Sx(14) };

    // 模式开关和标题同一行、贴右边 —— 它是个两态切换，单独占一行太浪费。
    // ⚠️ 放不下就不给位置（保持空矩形，绘制侧会跳过）。
    const int modeW = Sx(112), modeH = Sx(22);
    if (padX + Sx(120) + Sx(8) + modeW <= width - padX) {
        out.ctrlModeBtn = RECT{ width - padX - modeW, y, width - padX, y + modeH };
    }
    y += Sx(14) + Sx(10);

    // 2 行 2 列的基色块。复用上面那套 cardH / labelH / gap / rowGap，
    // 这样两个色块区看起来是同一个体系。
    {
        const int cCols = 2;
        const int cColW = (width - padX * 2 - gap * (cCols - 1)) / cCols;
        if (cColW >= Sx(48)) {
            for (int row = 0; row < 2; ++row) {
                for (int c = 0; c < cCols; ++c) {
                    const int i = row * cCols + c;
                    if (i >= kPrefsCtrlColorCount) break;
                    const int x = padX + c * (cColW + gap);
                    out.ctrlCards[i]      = RECT{ x, y, x + cColW, y + cardH };
                    out.ctrlCardLabels[i] = RECT{ x, y + cardH + labelGap,
                                                  x + cColW, y + cardH + labelGap + labelH };
                }
                y += cardH + labelGap + labelH + rowGap;
            }
        }
    }

    // ---- 背景图（D-098）----
    //
    // 位置在**控件配色之后、外观预设之前**。这两个的相对顺序是这么定的：
    // 背景图属于"逐项调外观"那一类（和上面的配色、控件配色是一伙的），
    // 而外观预设是"整套快照"的操作（保存 / 删除 / 导入 / 导出），
    // 放在最后更像一个收尾动作 —— 用户调完所有项目之后才想到要存下来。
    //
    // ⚠️ 加了这一区之后页面超过 800 逻辑像素，绝大多数窗口装不下 ——
    //    所以**滚动条是这一区能存在的前提**（D-094）。
    //    没那条滚动条的话，下面的外观预设和底部按钮会被整个切掉。
    out.titleBg = RECT{ padX, y, padX + Sx(120), y + Sx(14) };

    // 「清除」和标题同一行、贴右边。设了图才给位置（没图时它没有意义）。
    const int clrW = Sx(64), clrH = Sx(22);
    if (padX + Sx(120) + Sx(8) + clrW <= width - padX) {
        out.bgClear = RECT{ width - padX - clrW, y, width - padX, y + clrH };
    }
    y += Sx(14) + Sx(10);

    // 预览区（D-103）。按浮动面板的默认长宽比给高度 ——
    // 布局函数不知道用户实际把面板拖成了多大，用默认比例是为了
    // "看个大概构图"。真正的构图比例由面板自己呈现。
    {
        const int pw = width - padX * 2;
        // 460x150 是面板的出厂尺寸，比例约 3.07。宽高比要**夹具**：
        // 窗口极窄时按比例算出来的高度会小到看不出来，而极宽时会高得离谱。
        int ph = pw * 150 / 460;
        const int minH = Sx(80), maxH = Sx(200);
        if (ph < minH) ph = minH;
        if (ph > maxH) ph = maxH;
        out.bgPreview = RECT{ padX, y, padX + pw, y + ph };
        y += ph + Sx(6);
        out.bgPreviewHint = RECT{ padX, y, padX + pw, y + Sx(14) };
        y += Sx(14) + Sx(10);
    }

    // 「选择图片…」按钮：整行，同时兼任"当前路径"的显示位。
    // 单独再放一个只读路径框的话，窄窗口下两个都会被压扁，不如合成一个。
    out.bgPick = RECT{ padX, y, width - padX, y + Sx(26) };
    y += Sx(26) + Sx(10);

    // 适配方式：**点击循环**而不是下拉。
    // 四个值，循环点三下就转一圈 —— 比弹菜单少一次交互，也少一份要测的代码。
    out.bgFit = RECT{ padX, y, width - padX, y + Sx(24) };
    y += Sx(24) + Sx(14);

    // 三个参数滑块。和上面的不透明度滑块用同一套排布（标题左、数值右、滑块整行）。
    const int bgSliderH = Sx(24);
    struct SliderSlot {
        RECT* label; RECT* value; RECT* slider; const wchar_t* text;
    };
    const SliderSlot slots[3] = {
        { &out.bgOpacityLabel, &out.bgOpacityValue, &out.bgOpacitySlider, L"图片不透明度" },
        { &out.bgBlurLabel,    &out.bgBlurValue,    &out.bgBlurSlider,    L"磨砂强度"     },
        { &out.bgDimLabel,     &out.bgDimValue,     &out.bgDimSlider,     L"压暗（保证歌词可读）" },
    };
    for (const SliderSlot& s : slots) {
        *s.label = RECT{ padX, y, padX + Sx(150), y + Sx(14) };
        *s.value = RECT{ width - padX - Sx(70), y, width - padX, y + Sx(14) };
        y += Sx(14) + Sx(6);
        *s.slider = RECT{ padX, y, width - padX, y + bgSliderH };
        y += bgSliderH + Sx(12);
    }

    // ---- 外观预设（D-088）----
    //
    // 放在**主体设置之后、底部按钮之前**。放最后会被切掉：宿主给的区域
    // 不够高时，最后那几行本来就探出客户区（见下面按钮那段的说明）。
    const int btnW2 = Sx(88), btnH2 = Sx(26);

    out.titlePreset = RECT{ padX, y, width - padX, y + Sx(14) };
    y += Sx(14) + Sx(8);

    // 第一行：[当前预设名 v] [保存]
    // 下拉宽度**吃满剩余空间** —— 预设名是用户自己起的，可能挺长
    //（「深夜蓝 · 给专辑封面用」），定宽会把它截成一个没用的前缀。
    const int saveLeft = width - padX - btnW2;
    const int comboW   = saveLeft - Sx(8) - padX;
    if (comboW >= Sx(100)) {          // 太窄就不给这一行，绘制侧会跳过
        out.presetCombo = RECT{ padX, y, padX + comboW, y + btnH2 };
        out.presetSave  = RECT{ saveLeft, y, saveLeft + btnW2, y + btnH2 };
    }
    y += btnH2 + Sx(6);

    // 第二行：删除 / 导入 / 导出。三个等宽按钮，间距一致。
    const int gap2 = Sx(6);
    const int rowW = btnW2 * 3 + gap2 * 2;
    if (rowW <= width - padX * 2) {
        out.presetDelete = RECT{ padX, y, padX + btnW2, y + btnH2 };
        out.presetImport = RECT{ out.presetDelete.right + gap2, y,
                                 out.presetDelete.right + gap2 + btnW2, y + btnH2 };
        out.presetExport = RECT{ out.presetImport.right + gap2, y,
                                 out.presetImport.right + gap2 + btnW2, y + btnH2 };
    }
    y += btnH2 + Sx(14);

    // ---- 底部说明 ----
    out.hint = RECT{ padX, y, width - padX, y + Sx(32) };
    y += Sx(32) + Sx(10);

    // ---- 按钮 ----
    // 贴着左下角；面板不够高时它会探出去 —— 那种情况下由绘制侧照常画，
    // 因为宿主给的区域本来就装不下整页（用户把首选项窗口拖小了）。
    const int btnW = Sx(88), btnH = Sx(26);
    out.reset = RECT{ padX, y, padX + btnW, y + btnH };

    // 「字体...」放在它右边。比「恢复默认」宽得多 —— 按钮上要显示**当前字体名**，
    // 「Microsoft YaHei UI」这种名字窄了就只能看到开头几个字母，等于没显示。
    //
    // ⚠️ 放不下就**不给位置**（保持空矩形，绘制侧会跳过）。首选项窗口被拖得很窄时
    //    宁可不显示这个按钮，也不要画到客户区外面去 —— 那会糊在旁边的控件上。
    //    实测 284 逻辑宽是它的下限（padX 20 + 88 + 间距 8 + 148 + padX 20），
    //    而客户区检查是从 220 开始扫的，所以这条分支真的会走到。
    //    窄窗口下用户仍可以从高级首选项里改字体（lyricus.fontFace）。
    const int fontW    = Sx(148);
    const int fontLeft = out.reset.right + Sx(8);
    if (fontLeft + fontW <= width - padX) {
        out.fontBtn = RECT{ fontLeft, y, fontLeft + fontW, y + btnH };
    }

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
    const int lum    = ColorLuminance(bg);
    const bool bgDark = (lum < 128) || (lum >= 100 && lum < 160 && dark);

    if (bgDark) {
        t.textDim = BlendColor(fg, bg, 0.45);
        t.cardBg  = BlendColor(bg, RGB(255, 255, 255), 0.08);
        t.cardHot = BlendColor(bg, RGB(255, 255, 255), 0.16);
        t.border  = BlendColor(bg, RGB(255, 255, 255), 0.22);
        t.accent  = RGB(76, 160, 235);
    } else {
        t.textDim = BlendColor(fg, bg, 0.42);
        t.cardBg  = BlendColor(bg, RGB(0, 0, 0), 0.05);
        t.cardHot = BlendColor(bg, RGB(0, 0, 0), 0.10);
        t.border  = BlendColor(bg, RGB(0, 0, 0), 0.16);
        t.accent  = RGB(0, 120, 212);
    }
    return t;
}

} // namespace lyricus
