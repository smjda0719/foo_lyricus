#include "control_bar_layout.h"

// ---------------------------------------------------------------------------
// 控制条布局。
//
// 水平方向是「两端固定、中间浮动」：左边三个走带按钮、右边时间与音量，
// 宽度都固定；**进度条吃掉中间剩下的全部宽度**。
//
// 面板变窄时不能让进度条第一个去死 —— 它曾经就是那样，见下面降级那一段。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

// 逻辑像素 -> 物理像素。写成自由函数而不是 lambda：本文件里每一处都要用它。
int S(int dpi, int v) { return MulDiv(v, dpi, 96); }

bool IsEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

} // namespace

ControlBarRects ComputeControlBarLayout(const RECT& rc, int dpi) {
    ControlBarRects out;

    const int btn = S(dpi, 26);
    const int gap = S(dpi, 8);
    const int pad = S(dpi, 20);
    const int top = rc.bottom - S(dpi, 16) - btn;

    out.barTop = top - S(dpi, 6);   // 控制条上沿 = 歌词区的下界

    // ---- 左侧：三个走带按钮，宽度固定，**不参与降级** ----
    int x = rc.left + pad;
    out.prev      = RECT{ x, top, x + btn, top + btn };  x += btn + gap;
    out.playPause = RECT{ x, top, x + btn, top + btn };  x += btn + gap;
    out.next      = RECT{ x, top, x + btn, top + btn };  x += btn + gap;
    const int leftEnd = x;

    // ---- 右侧：按优先级降级 ----
    //
    // 【原先的毛病】这些元素都是写死宽度的，面板一窄，进度条最先被挤没 ——
    // 判据 `progR > progL + S(40)` 一旦不成立就整条不画。而进度条恰恰是
    // 控制条上最不该消失的东西：用户 2026-09-26 报「横向拖拽时进度条会压缩，
    // 在某个地方会消失，现在的窗口就是那个临界点」。实测那一刻面板是
    // 832 物理像素，判据正好卡在 `340 > 340` 为假 —— 数字和现象严丝合缝。
    //
    // 优先级：三个按钮 > **进度条** > 时间 > 音量条。
    // 音量条**最先**牺牲：滚轮就能调音量（面板早就支持），而进度和时间
    // 没有替代品。
    const int iconW   = S(dpi, 20);
    const int volW    = S(dpi, 70);
    const int timeW   = S(dpi, 96);
    const int minProg = S(dpi, 40);

    const int rightEdge = rc.right - pad;
    const int avail     = rightEdge - leftEnd;

    bool showVolBar = true;
    bool showTime   = true;

    // 全都显示时，右侧一共要占多宽（含紧邻它左边的那个 gap）
    int need = gap + volW + gap + iconW + gap * 2 + timeW + gap * 2;
    if (avail - need < minProg) { showVolBar = false; need -= (gap + volW); }
    if (avail - need < minProg) { showTime   = false; need -= (timeW + gap * 2); }

    // 按最终决定从右往左摆。
    // ⚠️ 被牺牲掉的项必须是**空矩形**（`RECT{}`）—— 这是头文件里写死的约定，
    //    绘制侧靠它判断"这一项不用画"。
    int r = rightEdge;
    if (showVolBar) {
        out.volumeBar = RECT{ r - volW, top + S(dpi, 9), r, top + btn - S(dpi, 9) };
        r -= volW + gap;
    }

    // 音量图标（静音开关）**不参与降级** —— 它有独立功能，不是音量条的附属装饰。
    // 但它也不能越到左侧按钮上去：真到那一步（面板窄得离谱）宁可少一个开关，
    // 也不要画出两个叠在一起的控件。
    out.volumeIcon = RECT{ r - iconW, top, r, top + btn };
    if (out.volumeIcon.left < leftEnd + gap) out.volumeIcon = RECT{};
    r -= iconW + gap;

    if (showTime) {
        out.time = RECT{ r - timeW, top, r, top + btn };
        r -= timeW + gap * 2;
    }

    // 中间剩下的**全部**给进度条。上面的降级保证它至少有 minProg 宽；
    // 面板窄到连三个按钮都摆不下时才真的没有（那时 r 已经越过 progL）。
    const int progL = leftEnd + gap;
    if (r > progL) {
        out.progress = RECT{ progL, top + S(dpi, 9), r, top + btn - S(dpi, 9) };
    }

    // ---- 「下拉」音量浮层 ----
    //
    // 【什么时候才给】只在**横向音量条被降级掉**的时候。宽面板上横条就在那儿，
    // 再弹一个浮层是多余的，而且两个音量控件同时可见会让人不知道该拖哪个。
    //
    // 【★ 为什么底边紧贴图标上边、一点缝都不留】鼠标从音量图标往上移到浮层，
    // 中间要是存在一条"既不在图标里、也不在浮层里"的带子，收起逻辑就会在那条
    // 带子上把浮层收掉，下一帧鼠标进了浮层又重新展开 —— 浮层会抖，根本没法用。
    // 零间隙是功能性的，不是审美（test_control_bar.cpp 里有断言钉着）。
    if (!showVolBar && !IsEmpty(out.volumeIcon)) {
        const int popupW = S(dpi, 30);
        const int popupH = S(dpi, 96);
        const int cx     = (out.volumeIcon.left + out.volumeIcon.right) / 2;

        int left  = cx - popupW / 2;
        int right = left + popupW;
        // 贴住面板左右边界，别探出去
        if (left < rc.left)   { left  = rc.left;   right = left + popupW; }
        if (right > rc.right) { right = rc.right;  left  = right - popupW; }

        // 底边 = 图标顶边（零间隙），往上长
        const int bottom = out.volumeIcon.top;
        int       topY   = bottom - popupH;
        if (topY < rc.top) topY = rc.top;      // 顶到面板上沿就截断

        // 太矮就不给 —— 半截滑块比没有更糟（拖不准，还挡歌词）
        if (bottom - topY >= S(dpi, 48)) {
            out.volumePopup = RECT{ left, topY, right, bottom };
        }
    }

    return out;
}

} // namespace lyricus
