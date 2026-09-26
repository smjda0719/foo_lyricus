// 控制条布局 —— 纯函数，离线可测。
//
// 【为什么这段值得单独测】它在 2026-09-26 一天之内改了两轮，而两轮都只能靠
// "把数算一遍 + 截图看"来验证：
//   * 第一轮：横向拖窄时进度条被挤没 —— 判据 `progR > progL + S(40)` 一不成立
//     就整条不画，而用户那块 832 物理像素（= 416 逻辑像素 @200%）的面板
//     正好卡在 `340 > 340` 为假；
//   * 第二轮：改成按优先级降级之后，还要保证被降级掉的空矩形不会在
//     客户区左上角画出鬼影。
//
// 这类"给定宽度 -> 一组矩形"的计算正是单测台最擅长的东西，下面把它钉死。
// 特别地：**用户报的那个宽度本身成了一条断言**（TestUserReportedWidth）——
// 那个数字是真实测量出来的，不该再被改回去。

#include "control_bar_layout.h"

#include <cstdio>

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

using lyricus::ControlBarRects;

// 测试覆盖的 DPI。写成具名数组而不是 range-for 的花括号列表 ——
// 后者要 <initializer_list>，而本文件刻意只依赖 windows.h。
constexpr int kAllDpis[] = { 96, 120, 144, 192 };
constexpr int kTwoDpis[] = { 96, 192 };

bool IsEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
int  Width(const RECT& r) { return r.right - r.left; }

// 客户区矩形：左上角归零，方便直接看相对坐标
RECT Rc(int w, int h) { return RECT{ 0, 0, w, h }; }

lyricus::ControlBarRects Layout(int logicalW, int dpi, int logicalH = 150) {
    return lyricus::ComputeControlBarLayout(Rc(MulDiv(logicalW, dpi, 96),
                                               MulDiv(logicalH, dpi, 96)), dpi);
}

// 水平方向是否重叠。空矩形不参与（它压根不画）。
bool OverlapX(const RECT& a, const RECT& b) {
    if (IsEmpty(a) || IsEmpty(b)) return false;
    return a.left < b.right && b.left < a.right;
}

// ---------------------------------------------------------------------------
// 用户报的那个宽度 —— 最要紧的一条
// ---------------------------------------------------------------------------
void TestUserReportedWidth() {
    std::printf("\n== 用户报的那个临界宽度（回归）==\n");

    // 2026-09-26：面板 832x328 物理像素（200% 缩放 = 416x164 逻辑）时，
    // 进度条整条消失。当时判据是 `progR > progL + S(40)`，正好 340 > 340 为假。
    const auto r = lyricus::ComputeControlBarLayout(Rc(832, 328), 192);
    Check(!IsEmpty(r.progress), "★ 416 逻辑像素宽 —— 进度条必须存在（就是用户报的那个宽度）");
    Check(Width(r.progress) > 0, "★ 而且它得有实际宽度，不能是个零宽的壳");

    // 这个宽度下**两样都该看得到**：新布局把旧版多留的一段 gap*2 省掉了，
    // 于是 416 逻辑像素刚好容得下「按钮 + 进度条 + 时间 + 音量条」全部。
    // （旧布局在这个宽度上进度条是整条不画的 —— 那正是用户报的现象。）
    Check(!IsEmpty(r.volumeBar), "★ 音量条也还在 —— 修好之后这个宽度下两样都看得到");
    Check(!IsEmpty(r.time), "★ 时间同样在");
    Check(!IsEmpty(r.playPause) && !IsEmpty(r.prev) && !IsEmpty(r.next),
          "★ 三个走带按钮任何时候都在");
}

// ---------------------------------------------------------------------------
// 降级顺序：按钮 > 进度条 > 时间 > 音量条
// ---------------------------------------------------------------------------
void TestDegradeOrder() {
    std::printf("\n== 降级顺序 ==\n");

    const auto wide = Layout(700, 96);
    Check(!IsEmpty(wide.progress) && !IsEmpty(wide.time) && !IsEmpty(wide.volumeBar),
          "700 逻辑像素：进度条 / 时间 / 音量条 全在");

    // 第一个临界点：416 逻辑像素。
    // avail = w - pad(20) - leftEnd(122)；全部显示需要 need = 234，且进度条还要 minProg = 40
    //   20 + 122 + 234 + 40 = 416
    Check(!IsEmpty(Layout(416, 96).volumeBar), "416 逻辑像素：音量条**刚好**还在（边界内）");
    Check(IsEmpty(Layout(415, 96).volumeBar),  "★ 415 逻辑像素：音量条掉了（差一个像素就降级）");
    Check(!IsEmpty(Layout(415, 96).progress),  "★ 但进度条必须还在 —— 这才是这次修复的重点");
    Check(!IsEmpty(Layout(415, 96).time),      "★ 时间也还在（它排在音量条前面）");

    // 第二个临界点：338 逻辑像素（= 160 + 122 + 156）
    Check(!IsEmpty(Layout(338, 96).time),   "338 逻辑像素：时间**刚好**还在");
    Check(IsEmpty(Layout(337, 96).time),    "★ 337 逻辑像素：时间也掉了（第二档降级）");
    Check(!IsEmpty(Layout(337, 96).progress), "★ 到这一步进度条依然在");

    // 面板能被拖到的最小尺寸（kMinPanelW96 = 320）下，进度条仍要有像样的宽度
    const auto minPanel = Layout(320, 96);
    Check(!IsEmpty(minPanel.progress), "320 逻辑像素（面板最小宽度）：进度条还在");
    Check(Width(minPanel.progress) >= MulDiv(40, 96, 96),
          "★ 而且宽度不小于它自己的下限 40 —— 到最小面板尺寸都用得动");
}

// ---------------------------------------------------------------------------
// 扫描：进度条不该在任何"面板允许的宽度"下消失
// ---------------------------------------------------------------------------
void TestProgressNeverVanishesAcrossRange() {
    std::printf("\n== 全宽度扫描 ==\n");

    int missing = 0, tooNarrow = 0, firstBad = -1;
    for (int dpi : kAllDpis) {
        for (int w = 320; w <= 1600; w += 4) {      // 320 = 面板最小宽度
            const auto r = Layout(w, dpi);
            if (IsEmpty(r.progress)) { ++missing; if (firstBad < 0) firstBad = w; }
            else if (Width(r.progress) < MulDiv(20, dpi, 96)) ++tooNarrow;
        }
    }
    Check(missing == 0, "★ 320~1600 逻辑像素 x 4 种 DPI：进度条一次都没消失");
    Check(tooNarrow == 0, "★ 而且没有出现窄得没法拖动的进度条（>= 20 逻辑像素）");
    if (missing != 0) std::printf("      （最早缺失于 %d 逻辑像素）\n", firstBad);
}

// ---------------------------------------------------------------------------
// 不重叠、不越界
// ---------------------------------------------------------------------------
void TestNoOverlapNoOverflow() {
    std::printf("\n== 不重叠 / 不越界 ==\n");

    int overlap = 0, overflow = 0, badOrder = 0;
    for (int dpi : kTwoDpis) {
        for (int w = 320; w <= 1600; w += 4) {
            const RECT rc = Rc(MulDiv(w, dpi, 96), MulDiv(150, dpi, 96));
            const auto r = lyricus::ComputeControlBarLayout(rc, dpi);

            if (OverlapX(r.prev, r.playPause) || OverlapX(r.playPause, r.next) ||
                OverlapX(r.next, r.progress) || OverlapX(r.progress, r.volumeIcon) ||
                OverlapX(r.volumeIcon, r.time) || OverlapX(r.time, r.volumeBar)) {
                ++overlap;
            }

            const RECT* all[] = { &r.prev, &r.playPause, &r.next, &r.progress,
                                  &r.time, &r.volumeIcon, &r.volumeBar };
            for (const RECT* p : all) {
                if (IsEmpty(*p)) continue;
                if (p->left < rc.left || p->right > rc.right) ++overflow;
            }

            // 左侧按钮组必须在右侧元素组的左边（顺序反了就说明算错了）
            if (!IsEmpty(r.next) && !IsEmpty(r.volumeIcon) &&
                r.next.right > r.volumeIcon.left) ++badOrder;
        }
    }
    Check(overlap == 0,   "★ 任何宽度下相邻控件都不重叠");
    Check(overflow == 0,  "★ 任何宽度下控件都不越出客户区");
    Check(badOrder == 0,  "★ 左边按钮组始终在右边元素组的左侧");
}

// ---------------------------------------------------------------------------
// 空矩形约定 —— 绘制侧靠它判断"这一项不用画"
// ---------------------------------------------------------------------------
void TestEmptyRectConvention() {
    std::printf("\n== 空矩形约定 ==\n");

    auto allZero = [](const RECT& r) {
        return r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0;
    };

    const auto narrow = Layout(320, 96);
    Check(allZero(narrow.volumeBar), "★ 被降级的音量条是**全零**矩形，不是任意的负宽度");
    Check(allZero(narrow.time),      "★ 被降级的时间同理");

    // 对照：宽面板下它们不该是零
    const auto wide = Layout(700, 96);
    Check(!allZero(wide.volumeBar) && !allZero(wide.time), "宽面板下这两项都不是零矩形");
}

// ---------------------------------------------------------------------------
// DPI：同一个逻辑宽度，物理尺寸按比例
// ---------------------------------------------------------------------------
void TestDpiScaling() {
    std::printf("\n== DPI 缩放 ==\n");

    const auto a = Layout(460, 96);
    const auto b = Layout(460, 192);
    Check(Width(b.playPause) == Width(a.playPause) * 2, "DPI 翻倍 -> 按钮宽度也翻倍");
    Check(Width(b.progress)  == Width(a.progress)  * 2, "DPI 翻倍 -> 进度条宽度也翻倍");

    const auto c = Layout(460, 144);
    Check(Width(c.playPause) == Width(a.playPause) * 3 / 2, "144 DPI -> 1.5 倍");
}

// ---------------------------------------------------------------------------
// barTop
// ---------------------------------------------------------------------------
void TestBarTop() {
    std::printf("\n== 控制条上沿 ==\n");

    const auto r = Layout(460, 96, 150);
    Check(r.barTop > 0 && r.barTop < r.prev.top,
          "★ barTop 在按钮上方 —— 它是歌词区的下界，压到按钮上就会盖住控制条");
    Check(r.barTop < 150, "barTop 落在客户区内");
}

} // namespace

int main() {
    std::printf("======== 控制条布局（纯逻辑）========\n");
    TestUserReportedWidth();
    TestDegradeOrder();
    TestProgressNeverVanishesAcrossRange();
    TestNoOverlapNoOverflow();
    TestEmptyRectConvention();
    TestDpiScaling();
    TestBarTop();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
