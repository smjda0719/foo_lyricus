// 色环取色器的纯逻辑 —— 离线可测。
//
// 【为什么值得单独测】用户 2026-09-26 说「虽然稍微好点，但我还是想要类似色环的」。
// 色环要自绘，而自绘里最容易悄悄出错的是两件**互逆**的换算：
//   * 鼠标坐标 -> 颜色（点击/拖动时）
//   * 颜色     -> 坐标（画那个小标记时）
// 两者一旦不一致，表现是"点红色却选中橙色"，或者"标记画在别的地方"——
// 在截图里几乎看不出来，但它本身是纯数学，单测台能精确钉住。
// 所以下面最要紧的两组是 TestRingGeometry（36 个角度往返）和
// TestDragKeepsOtherComponents（拖色环不许顺手改饱和度）。

#include "color_wheel.h"

#include <cmath>
#include <cstdio>

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

using lyricus::HsvColor;
using lyricus::ColorWheelLayout;
using lyricus::WheelHit;

constexpr int kDpis[] = { 96, 120, 144, 192 };

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int ColorDiff(COLORREF a, COLORREF b) {
    const int dr = std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b)));
    const int dg = std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b)));
    const int db = std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
    return (dr > dg) ? ((dr > db) ? dr : db) : ((dg > db) ? dg : db);
}

ColorWheelLayout DefaultLayout(int dpi = 96) {
    return lyricus::ComputeColorWheelLayout(MulDiv(lyricus::kPickerWidth96, dpi, 96),
                                            MulDiv(lyricus::kPickerHeight96, dpi, 96), dpi);
}

// ---------------------------------------------------------------------------
void TestKnownColors() {
    std::printf("\n== 几个必须准确的固定值 ==\n");

    const COLORREF red   = lyricus::HsvToRgb(HsvColor{   0.0, 1.0, 1.0 });
    const COLORREF green = lyricus::HsvToRgb(HsvColor{ 120.0, 1.0, 1.0 });
    const COLORREF blue  = lyricus::HsvToRgb(HsvColor{ 240.0, 1.0, 1.0 });
    const COLORREF white = lyricus::HsvToRgb(HsvColor{   0.0, 0.0, 1.0 });
    const COLORREF black = lyricus::HsvToRgb(HsvColor{   0.0, 1.0, 0.0 });

    Check(red   == RGB(255, 0, 0),   "H=0   S=1 V=1 -> 纯红");
    Check(green == RGB(0, 255, 0),   "H=120 S=1 V=1 -> 纯绿");
    Check(blue  == RGB(0, 0, 255),   "H=240 S=1 V=1 -> 纯蓝");
    Check(white == RGB(255, 255, 255), "S=0 V=1 -> 白");
    Check(black == RGB(0, 0, 0),     "V=0 -> 黑");

    // 色相是环形的：360 与 0 同色，-60 与 300 同色
    Check(lyricus::HsvToRgb(HsvColor{ 360.0, 1.0, 1.0 }) == red,   "H=360 等同 H=0（环形）");
    Check(lyricus::HsvToRgb(HsvColor{ -60.0, 1.0, 1.0 }) ==
          lyricus::HsvToRgb(HsvColor{ 300.0, 1.0, 1.0 }),          "负角度也绕回环上");
}

// ---------------------------------------------------------------------------
void TestRgbToHsv() {
    std::printf("\n== RGB -> HSV ==\n");

    const HsvColor r = lyricus::RgbToHsv(RGB(255, 0, 0));
    Check(Near(r.h, 0.0, 0.5) && Near(r.s, 1.0, 0.01) && Near(r.v, 1.0, 0.01),
          "纯红 -> H=0 S=1 V=1");
    Check(Near(lyricus::RgbToHsv(RGB(0, 255, 0)).h, 120.0, 0.5), "纯绿 -> H=120");
    Check(Near(lyricus::RgbToHsv(RGB(0, 0, 255)).h, 240.0, 0.5), "纯蓝 -> H=240");

    // 灰阶：饱和度必须是 0，而且不能算出 NaN（除零）
    const HsvColor grey = lyricus::RgbToHsv(RGB(128, 128, 128));
    Check(Near(grey.s, 0.0, 0.001), "中灰 -> S=0");
    Check(grey.h == grey.h, "★ 灰色不会算出 NaN（除零被挡住了）");
    Check(Near(grey.v, 128.0 / 255.0, 0.01), "中灰 -> V≈0.5");

    Check(Near(lyricus::RgbToHsv(RGB(0, 0, 0)).v, 0.0, 0.001), "黑 -> V=0");
    Check(Near(lyricus::RgbToHsv(RGB(255, 255, 255)).s, 0.0, 0.001), "白 -> S=0");
}

// ---------------------------------------------------------------------------
// ★ 关键：两个方向必须互为逆运算。点红色就该选中红色。
// ---------------------------------------------------------------------------
void TestHsvRgbRoundTrip() {
    std::printf("\n== HSV -> RGB -> HSV 往返 ==\n");

    int bad = 0, worst = 0;
    for (int hi = 0; hi < 360; hi += 7) {
        for (int si = 0; si <= 10; ++si) {
            for (int vi = 0; vi <= 10; ++vi) {
                const HsvColor a{ static_cast<double>(hi),
                                  si / 10.0, vi / 10.0 };
                const COLORREF rgb = lyricus::HsvToRgb(a);
                const HsvColor b = lyricus::RgbToHsv(rgb);
                const COLORREF back = lyricus::HsvToRgb(b);

                // 先比回不回到同一个 RGB（这个才是用户看得见的）
                const int d = ColorDiff(rgb, back);
                if (d > worst) worst = d;
                if (d > 1) ++bad;
            }
        }
    }
    Check(bad == 0, "★ 四千多个 (H,S,V) 组合往返后 RGB 完全一致（误差 <= 1）");
    std::printf("      （最大分量偏差 %d）\n", worst);
}

// ---------------------------------------------------------------------------
// ★ 关键：角度 -> 坐标 -> 角度 必须自洽。
// ---------------------------------------------------------------------------
void TestRingGeometry() {
    std::printf("\n== 色环的角度与坐标 ==\n");

    const ColorWheelLayout L = DefaultLayout();
    Check(L.outerR > 0, "默认尺寸下算得出色环");

    const int rMid = (L.outerR + L.innerR) / 2;

    // 四个正方向先肉眼可验
    const POINT p0   = lyricus::HuePointOnRing(L,   0.0, rMid);
    const POINT p90  = lyricus::HuePointOnRing(L,  90.0, rMid);
    const POINT p180 = lyricus::HuePointOnRing(L, 180.0, rMid);
    const POINT p270 = lyricus::HuePointOnRing(L, 270.0, rMid);

    Check(p0.x == L.cx && p0.y < L.cy,     "H=0 在正上方");
    Check(p90.x > L.cx && p90.y == L.cy,   "H=90 在正右（顺时针）");
    Check(p180.x == L.cx && p180.y > L.cy, "H=180 在正下");
    Check(p270.x < L.cx && p270.y == L.cy, "H=270 在正左");

    // ★ 36 个角度全面往返
    int bad = 0;
    double worst = 0.0;
    for (int i = 0; i < 36; ++i) {
        const double hue = i * 10.0;
        const POINT p = lyricus::HuePointOnRing(L, hue, rMid);
        const auto pick = lyricus::HitTestColorWheel(L, p, HsvColor{ hue, 1.0, 1.0 });
        if (pick.hit != WheelHit::Ring) { ++bad; continue; }
        double d = std::fabs(pick.hsv.h - hue);
        if (d > 180.0) d = 360.0 - d;    // 环绕
        if (d > worst) worst = d;
        if (d > 1.0) ++bad;
    }
    Check(bad == 0, "★ 36 个角度「角度 -> 坐标 -> 命中 -> 角度」全部自洽（误差 <= 1 度）");
    std::printf("      （最大角度偏差 %.2f 度）\n", worst);

    // 内外半径之外应当不命中
    const POINT far1{ L.cx, L.cy - L.outerR - L.outerR };
    const POINT near1{ L.cx, L.cy };   // 圆心落在 SV 方块里，那是 SvBox 不是 Ring
    Check(lyricus::HitTestColorWheel(L, far1, HsvColor{}).hit == WheelHit::None,
          "环外远处不命中");
    Check(lyricus::HitTestColorWheel(L, near1, HsvColor{}).hit == WheelHit::SvBox,
          "圆心落在 SV 方块里");
}

// ---------------------------------------------------------------------------
void TestSvBox() {
    std::printf("\n== 环心的饱和度/明度方块 ==\n");

    const ColorWheelLayout L = DefaultLayout();
    const RECT& b = L.svBox;
    Check(b.right > b.left && b.bottom > b.top, "方块有实际大小");

    // 四角
    const auto tl = lyricus::HitTestColorWheel(L, POINT{ b.left,  b.top }, HsvColor{});
    const auto tr = lyricus::HitTestColorWheel(L, POINT{ b.right - 1, b.top }, HsvColor{});
    const auto bl = lyricus::HitTestColorWheel(L, POINT{ b.left,  b.bottom - 1 }, HsvColor{});
    const auto br = lyricus::HitTestColorWheel(L, POINT{ b.right - 1, b.bottom - 1 }, HsvColor{});

    Check(tl.hit == WheelHit::SvBox && Near(tl.hsv.s, 0.0, 0.02) && Near(tl.hsv.v, 1.0, 0.02),
          "左上角 = S0 V1（最淡）");
    Check(tr.hit == WheelHit::SvBox && Near(tr.hsv.s, 1.0, 0.02) && Near(tr.hsv.v, 1.0, 0.02),
          "右上角 = S1 V1（最艳）");
    Check(bl.hit == WheelHit::SvBox && Near(bl.hsv.s, 0.0, 0.02) && Near(bl.hsv.v, 0.0, 0.02),
          "左下角 = S0 V0（黑）");
    Check(br.hit == WheelHit::SvBox && Near(br.hsv.s, 1.0, 0.02) && Near(br.hsv.v, 0.0, 0.02),
          "右下角 = S1 V0（也黑，但饱和度满）");

    // 坐标 -> 值 -> 坐标 往返
    const auto mid = lyricus::HitTestColorWheel(L, POINT{ (b.left + b.right) / 2,
                                                          (b.top + b.bottom) / 2 },
                                                HsvColor{});
    Check(Near(mid.hsv.s, 0.5, 0.02) && Near(mid.hsv.v, 0.5, 0.02), "方块正中 = S0.5 V0.5");

    const POINT back = lyricus::SvPointInBox(b, mid.hsv.s, mid.hsv.v);
    Check(std::abs(back.x - (b.left + b.right) / 2) <= 1 &&
          std::abs(back.y - (b.top + b.bottom) / 2) <= 1,
          "★ 坐标 -> S/V -> 坐标 往返一致（画标记用的就是这个）");

    // 夹取：方块外的点不该给出越界的 s/v
    const auto clamp = lyricus::HitTestColorWheel(L, POINT{ b.left, b.top }, HsvColor{});
    Check(clamp.hsv.s >= 0.0 && clamp.hsv.s <= 1.0 &&
          clamp.hsv.v >= 0.0 && clamp.hsv.v <= 1.0, "S/V 恒在 0..1");
}

// ---------------------------------------------------------------------------
// ★ 拖色环时不许顺手改饱和度/明度 —— 这是最容易写错、也最恼人的地方。
// ---------------------------------------------------------------------------
void TestDragKeepsOtherComponents() {
    std::printf("\n== 拖一个分量不许动另一个 ==\n");

    const ColorWheelLayout L = DefaultLayout();
    const int rMid = (L.outerR + L.innerR) / 2;

    const HsvColor cur{ 200.0, 0.42, 0.77 };   // 随便一组"非默认"值

    int bad = 0;
    for (int i = 0; i < 36; ++i) {
        const POINT p = lyricus::HuePointOnRing(L, i * 10.0, rMid);
        const auto pick = lyricus::HitTestColorWheel(L, p, cur);
        if (pick.hit != WheelHit::Ring) { ++bad; continue; }
        if (!Near(pick.hsv.s, cur.s, 1e-9)) ++bad;   // 饱和度必须原样
        if (!Near(pick.hsv.v, cur.v, 1e-9)) ++bad;   // 明度必须原样
    }
    Check(bad == 0, "★ 拖色环：S 与 V 一个都没被改");

    bad = 0;
    const RECT& b = L.svBox;
    for (int sx = 1; sx < 5; ++sx) {
        for (int sy = 1; sy < 5; ++sy) {
            const POINT p{ b.left + (b.right - b.left) * sx / 5,
                           b.top  + (b.bottom - b.top) * sy / 5 };
            const auto pick = lyricus::HitTestColorWheel(L, p, cur);
            if (pick.hit != WheelHit::SvBox) { ++bad; continue; }
            if (!Near(pick.hsv.h, cur.h, 1e-9)) ++bad;   // 色相必须原样
        }
    }
    Check(bad == 0, "★ 拖 S/V 方块：色相一个都没被改");

    // 按钮：颜色完全不该动
    const auto ok = lyricus::HitTestColorWheel(
        L, POINT{ (L.ok.left + L.ok.right) / 2, (L.ok.top + L.ok.bottom) / 2 }, cur);
    Check(ok.hit == WheelHit::Ok && Near(ok.hsv.h, cur.h, 1e-9), "命中确定键时颜色不动");
    const auto cc = lyricus::HitTestColorWheel(
        L, POINT{ (L.cancel.left + L.cancel.right) / 2, (L.cancel.top + L.cancel.bottom) / 2 }, cur);
    Check(cc.hit == WheelHit::Cancel, "命中取消键");
}

// ---------------------------------------------------------------------------
void TestLayout() {
    std::printf("\n== 布局 ==\n");

    const ColorWheelLayout L = DefaultLayout();
    Check(L.outerR > L.innerR && L.innerR > 0, "外半径 > 内半径 > 0（环有宽度）");
    Check(L.svBox.right > L.svBox.left, "SV 方块有大小");
    Check(L.ok.right > L.ok.left && L.cancel.right > L.cancel.left, "两个按钮都有大小");

    // 方块必须落在环**里面**（四角到圆心的距离小于内半径）
    const double hw = (L.svBox.right - L.svBox.left) / 2.0;
    const double hh = (L.svBox.bottom - L.svBox.top) / 2.0;
    const double corner = std::sqrt(hw * hw + hh * hh);
    Check(corner <= L.innerR + 1.0, "★ SV 方块整个落在色环内圈里（不压到环上）");

    // 两按钮不重叠
    Check(L.ok.right <= L.cancel.left, "确定与取消不重叠");

    // 太小的窗口直接放弃，而不是画一团叠在一起的东西
    Check(lyricus::ComputeColorWheelLayout(80, 80, 96).outerR == 0,
          "★ 80x80 太小 -> 返回空布局，不画");
    Check(lyricus::ComputeColorWheelLayout(0, 400, 96).outerR == 0,   "宽 0 -> 空");
    Check(lyricus::ComputeColorWheelLayout(400, 0, 96).outerR == 0,   "高 0 -> 空");
    Check(lyricus::ComputeColorWheelLayout(400, 400, 0).outerR == 0,  "dpi 0 -> 空");

    // 空布局上命中任何地方都不该崩、也不该命中
    const ColorWheelLayout empty{};
    Check(lyricus::HitTestColorWheel(empty, POINT{ 10, 10 }, HsvColor{}).hit == WheelHit::None,
          "空布局上命中测试安全返回 None");
}

// ---------------------------------------------------------------------------
void TestDpiScaling() {
    std::printf("\n== DPI 缩放 ==\n");

    const ColorWheelLayout a = DefaultLayout(96);
    const ColorWheelLayout b = DefaultLayout(192);
    Check(b.outerR == a.outerR * 2, "DPI 翻倍 -> 色环外半径也翻倍");
    Check(b.cx == a.cx * 2 && b.cy == a.cy * 2, "圆心同样翻倍");
    Check(b.dpi == 192 && a.dpi == 96, "dpi 回传给绘制侧");

    // 每个 dpi 下几何关系都得成立
    int bad = 0;
    for (int dpi : kDpis) {
        const ColorWheelLayout L = DefaultLayout(dpi);
        if (L.outerR <= L.innerR || L.innerR <= 0) { ++bad; continue; }
        const double hw = (L.svBox.right - L.svBox.left) / 2.0;
        const double hh = (L.svBox.bottom - L.svBox.top) / 2.0;
        if (std::sqrt(hw * hw + hh * hh) > L.innerR + 1.0) ++bad;
        if (L.ok.right > L.cancel.left) ++bad;
    }
    Check(bad == 0, "★ 四种 DPI 下：环有宽度、方块在圈内、按钮不重叠");
}

} // namespace

int main() {
    std::printf("======== 色环取色器纯逻辑 ========\n");
    TestKnownColors();
    TestRgbToHsv();
    TestHsvRgbRoundTrip();
    TestRingGeometry();
    TestSvBox();
    TestDragKeepsOtherComponents();
    TestLayout();
    TestDpiScaling();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
