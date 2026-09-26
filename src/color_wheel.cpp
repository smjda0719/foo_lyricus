#include "color_wheel.h"

#include <cmath>

// ---------------------------------------------------------------------------
// 色环取色器的纯逻辑。
//
// 这里没有任何 GDI 调用 —— 只有"坐标 <-> 颜色"的换算和一块矩形的划分。
// 窗口与绘制在 color_picker.cpp；这样切开之后，最容易出错的换算部分
// 可以在离线单测台里反复扫（见 test_color_wheel.cpp）。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

constexpr double kPi = 3.14159265358979323846;

int S(int dpi, int v) { return MulDiv(v, dpi, 96); }

double Clamp01(double v) {
    if (!(v > 0.0)) return 0.0;   // 同时挡住 NaN
    if (v > 1.0) return 1.0;
    return v;
}

int To255(double v) {
    const int i = static_cast<int>(v * 255.0 + 0.5);
    return (i < 0) ? 0 : ((i > 255) ? 255 : i);
}

bool IsEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

bool Inside(const RECT& r, POINT pt) {
    return !IsEmpty(r) &&
           pt.x >= r.left && pt.x < r.right &&
           pt.y >= r.top  && pt.y < r.bottom;
}

} // namespace

// ---------------------------------------------------------------------------
// HSV <-> RGB
// ---------------------------------------------------------------------------

COLORREF HsvToRgb(const HsvColor& c) {
    double h = std::fmod(c.h, 360.0);
    if (h < 0.0) h += 360.0;

    const double s = Clamp01(c.s);
    const double v = Clamp01(c.v);

    const double chroma = v * s;
    const double x = chroma * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
    const double m = v - chroma;

    double r = 0.0, g = 0.0, b = 0.0;
    if      (h <  60.0) { r = chroma; g = x;      b = 0.0;    }
    else if (h < 120.0) { r = x;      g = chroma; b = 0.0;    }
    else if (h < 180.0) { r = 0.0;    g = chroma; b = x;      }
    else if (h < 240.0) { r = 0.0;    g = x;      b = chroma; }
    else if (h < 300.0) { r = x;      g = 0.0;    b = chroma; }
    else                { r = chroma; g = 0.0;    b = x;      }

    return RGB(To255(r + m), To255(g + m), To255(b + m));
}

HsvColor RgbToHsv(COLORREF c) {
    const double r = GetRValue(c) / 255.0;
    const double g = GetGValue(c) / 255.0;
    const double b = GetBValue(c) / 255.0;

    const double mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    const double mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    const double d  = mx - mn;

    HsvColor out;
    out.v = mx;
    out.s = (mx > 0.0) ? (d / mx) : 0.0;

    // 灰色（含黑与白）：色相没有意义，返回 0 而不是让除零算出 NaN。
    // 调用方通常会把 h 原样保留，只换 s/v。
    if (d <= 0.0) { out.h = 0.0; return out; }

    double h;
    if      (mx == r) h = 60.0 * std::fmod((g - b) / d, 6.0);
    else if (mx == g) h = 60.0 * (((b - r) / d) + 2.0);
    else              h = 60.0 * (((r - g) / d) + 4.0);

    if (h < 0.0) h += 360.0;
    out.h = h;
    return out;
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

ColorWheelLayout ComputeColorWheelLayout(int width, int height, int dpi) {
    ColorWheelLayout L;
    if (width <= 0 || height <= 0 || dpi <= 0) return L;
    L.dpi = dpi;

    auto Sx = [dpi](int v) { return MulDiv(v, dpi, 96); };

    const int padX    = Sx(20);
    const int padY    = Sx(18);
    const int buttonH = Sx(28);
    const int buttonW = Sx(88);
    const int gap     = Sx(10);

    // 底部：按钮行
    const int btnTop = height - padY - buttonH;
    if (btnTop <= padY) return ColorWheelLayout{};   // 太矮，整体放弃

    // 环能用的高度 = 按钮行以上
    const int availH = btnTop - padY - Sx(10);
    if (availH < Sx(60)) return ColorWheelLayout{};

    // 色环：外径由**高度**决定（高度比宽度紧张），但不能超出宽度的一半。
    // 右侧还要留出预览块的宽度。
    const int previewW = Sx(96);
    const int wheelMaxByW = (width - padX * 3 - previewW);
    int outerR = availH / 2;
    if (outerR * 2 > wheelMaxByW) outerR = wheelMaxByW / 2;
    if (outerR < Sx(40)) return ColorWheelLayout{};

    const int ringW = Sx(30);                       // 环本身的宽度
    L.outerR = outerR;
    L.innerR = outerR - ringW;
    if (L.innerR < Sx(20)) L.innerR = outerR * 2 / 3;   // 太小就让环占比例

    L.cx = padX + outerR;
    L.cy = padY + availH / 2;

    // 环心的 S/V 方块：内圆的内接正方形是 innerR*sqrt(2)，
    // 取 1.35 倍半径留一点边距，免得四角贴到环上。
    int svSide = static_cast<int>(L.innerR * 1.35);
    if (svSide > L.innerR * 2) svSide = L.innerR * 2;
    if (svSide < Sx(24)) return ColorWheelLayout{};
    L.svBox = RECT{ L.cx - svSide / 2, L.cy - svSide / 2,
                    L.cx - svSide / 2 + svSide, L.cy - svSide / 2 + svSide };

    // 右列：预览 + 十六进制 + 按钮
    const int colX = L.cx + outerR + Sx(20);
    if (colX + previewW > width) return ColorWheelLayout{};   // 放不下就不画

    L.preview  = RECT{ colX, padY, colX + previewW, padY + Sx(56) };
    L.hexLabel = RECT{ colX, L.preview.bottom + Sx(8), colX + previewW,
                       L.preview.bottom + Sx(8) + Sx(16) };

    // 按钮右对齐到同一列
    L.ok     = RECT{ colX + previewW - buttonW * 2 - gap, btnTop,
                     colX + previewW - buttonW - gap,     btnTop + buttonH };
    L.cancel = RECT{ colX + previewW - buttonW, btnTop,
                     colX + previewW,           btnTop + buttonH };

    return L;
}

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------

WheelPick HitTestColorWheel(const ColorWheelLayout& L, POINT pt, const HsvColor& cur) {
    WheelPick out;
    out.hsv = cur;   // 默认原样返回：没碰到的分量不该被改

    if (Inside(L.ok, pt))     { out.hit = WheelHit::Ok;     return out; }
    if (Inside(L.cancel, pt)) { out.hit = WheelHit::Cancel; return out; }

    if (Inside(L.svBox, pt)) {
        const double bw = L.svBox.right - L.svBox.left;
        const double bh = L.svBox.bottom - L.svBox.top;
        out.hsv.s = Clamp01((pt.x - L.svBox.left) / bw);
        out.hsv.v = Clamp01(1.0 - (pt.y - L.svBox.top) / bh);
        out.hit = WheelHit::SvBox;
        return out;
    }

    const double dx = pt.x - L.cx;
    const double dy = pt.y - L.cy;
    const double dist = std::sqrt(dx * dx + dy * dy);

    // 环外留一点容差（半个环宽），免得贴着边缘就点不中。
    const double tolerance = (L.outerR - L.innerR) / 2.0;
    if (dist > L.outerR + tolerance || dist < L.innerR - tolerance) {
        out.hit = WheelHit::None;
        return out;
    }

    // atan2(dx, -dy)：0 = 正上方，顺时针增加。
    // 用 -dy 而不是 dy，是因为屏幕 y 轴向下；这样"上"才是 0 度。
    double ang = std::atan2(dx, -dy) * 180.0 / kPi;
    if (ang < 0.0) ang += 360.0;

    out.hsv.h = ang;
    out.hit = WheelHit::Ring;
    return out;
}

POINT HuePointOnRing(const ColorWheelLayout& L, double hue, int r) {
    const double rad = hue * kPi / 180.0;
    POINT pt;
    pt.x = L.cx + static_cast<int>(std::sin(rad) * r + 0.5);
    pt.y = L.cy - static_cast<int>(std::cos(rad) * r + 0.5);
    return pt;
}

POINT SvPointInBox(const RECT& box, double s, double v) {
    POINT pt;
    pt.x = box.left + static_cast<int>((box.right - box.left) * Clamp01(s) + 0.5);
    pt.y = box.top  + static_cast<int>((box.bottom - box.top) * (1.0 - Clamp01(v)) + 0.5);
    return pt;
}

} // namespace lyricus
