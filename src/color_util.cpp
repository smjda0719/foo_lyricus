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

} // namespace lyricus
