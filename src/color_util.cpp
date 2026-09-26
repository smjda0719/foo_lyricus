#include "color_util.h"

// ---------------------------------------------------------------------------
// 颜色工具。见 color_util.h 里"为什么单独一层"。
//
// 这里**只有** windows.h 的 COLORREF / GetRValue 那一套，不碰 SDK ——
// 所以它和 prefs_layout、color_wheel 一样能进离线单测台。
// ---------------------------------------------------------------------------

namespace lyricus {

COLORREF BlendColor(COLORREF a, COLORREF b, double t) {
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

int ColorLuminance(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

ControlBaseColors DeriveControlColors(COLORREF panelBg) {
    const bool     bgDark = (ColorLuminance(panelBg) < 128);
    const COLORREF toward = bgDark ? RGB(255, 255, 255) : RGB(0, 0, 0);
    auto T = [&](double t) { return BlendColor(panelBg, toward, t); };

    ControlBaseColors c;
    // 深底那几个 t 值是**照着原来硬编码的浅灰/白反推**的，
    // 所以默认预设下观感和改之前一致。
    c.button = T(bgDark ? 0.14 : 0.10);
    c.icon   = T(bgDark ? 0.82 : 0.78);
    c.slider = T(bgDark ? 0.80 : 0.76);
    c.text   = T(bgDark ? 0.72 : 0.68);
    return c;
}

COLORREF ShiftControlColor(COLORREF base, double t) {
    const COLORREF dir = (ColorLuminance(base) < 128) ? RGB(255, 255, 255)
                                                      : RGB(0, 0, 0);
    return BlendColor(base, dir, t);
}

} // namespace lyricus
