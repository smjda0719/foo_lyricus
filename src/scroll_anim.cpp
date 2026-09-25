#include "stdafx.h"
#include "scroll_anim.h"

namespace lyricus {
namespace {

// 单调时钟求差。理论上 GetTickCount64 不会回退，但这里**不能假设** ——
// 单测会故意塞乱序的时间点，而真机上时钟异常时"负数转 ULONGLONG"
// 会变成一个天文数字，直接把动画跳到终点。夹一下最省心。
ULONGLONG Elapsed(ULONGLONG now, ULONGLONG start) {
    return (now >= start) ? (now - start) : 0;
}

// 滚完一趟要多久：按基准速度算，两端夹取。
//
// 夹下限是为了**很窄的溢出也能看清**（溢出 10px 时 60px/s 只要 0.17s，
// 快到像闪一下）；夹上限是为了超长行不拖着滚（靠提速解决）。
ULONGLONG ScrollDurationMs(int overflowPx) {
    if (overflowPx <= 0) return 0;

    ULONGLONG ms = static_cast<ULONGLONG>(overflowPx) * 1000 / kScrollPxPerSec;
    if (ms < kScrollMinMs) ms = kScrollMinMs;
    if (ms > kScrollMaxMs) ms = kScrollMaxMs;
    return ms;
}

// 比例缩放，走 64 位中间量：overflow 和 t 都可能是几万，
// 直接相乘在 32 位下会溢出（这个坑在别处踩过）。
int Scale(int total, ULONGLONG part, ULONGLONG whole) {
    if (whole == 0) return 0;
    if (part >= whole) return total;
    return static_cast<int>(static_cast<long long>(total) *
                            static_cast<long long>(part) /
                            static_cast<long long>(whole));
}

} // namespace

void LyricAnimator::Reset() {
    m_hasLine      = false;
    m_lineIndex    = 0;
    m_lineStart    = 0;
    m_sliding      = false;
    m_slideStart   = 0;
    m_lastOverflow = 0;
}

LyricAnimFrame LyricAnimator::Update(ULONGLONG nowMs, std::size_t lineIndex,
                                     int overflowPx, int stepPx) {
    LyricAnimFrame f;

    if (overflowPx < 0) overflowPx = 0;

    // ---- 换行 ----
    if (!m_hasLine || lineIndex != m_lineIndex) {
        // 「顺序推进」才上滑：往后 seek（行号变小）不滑，
        // 跨行 seek（+2 以上）不滑，换曲（行号回到 0）不滑，首次调用不滑。
        //
        // ⚠️ 往前正好跳一句会**误滑**一次 —— 无害（那就是"上滑"的样子），
        //    不值当为它加一套"这是不是 seek"的状态。
        const bool forwardByOne = m_hasLine && (lineIndex == m_lineIndex + 1);

        m_lineIndex = lineIndex;
        m_lineStart = nowMs;
        m_hasLine   = true;

        m_sliding    = forwardByOne && (stepPx > 0);
        m_slideStart = nowMs;

        m_lastOverflow = overflowPx;
    } else if (m_lastOverflow == 0 && overflowPx > 0) {
        // 同一行，但面板被拉窄到放不下了 —— 时间线从这一刻重新起算。
        //
        // 不重置的话 elapsed 会是"这一行已经显示了多久"（可能几十秒），
        // 于是滚动**直接落在终点**、连起步的停顿都没有，看着就是跳一下。
        //
        // ⚠️ 只在 0 -> 正 这个方向重置。滚动途中继续拉窄（300 -> 500）
        //    不重置 —— 否则用户拖一次边框就重置几十次，滚动永远走不动。
        m_lineStart    = nowMs;
        m_lastOverflow = overflowPx;
    } else {
        m_lastOverflow = overflowPx;
    }

    // ---- 横滚：只滚一次，滚到能看见尾巴就停住 ----
    //
    // 这是用户 2026-09-25 定的取向（三选一里最安静的那个）：
    // 循环跑马灯会一直动，来回乒乓同一句要扫两遍，都太吵。
    if (overflowPx > 0) {
        const ULONGLONG elapsed = Elapsed(nowMs, m_lineStart);

        if (elapsed <= kScrollLeadMs) {
            // 起步前的静止期。**这里不置 animating** —— 画面没变化，
            // 让动画定时器空转是白烧主线程（见头文件里的说明）。
            f.scrollX = 0;
        } else {
            const ULONGLONG t   = elapsed - kScrollLeadMs;
            const ULONGLONG dur = ScrollDurationMs(overflowPx);

            if (t >= dur) {
                f.scrollX = overflowPx;      // 停在尾部，直到换行
            } else {
                f.scrollX   = Scale(overflowPx, t, dur);
                f.animating = true;
            }
        }
    }

    // ---- 上滑 ----
    if (m_sliding) {
        const ULONGLONG t = Elapsed(nowMs, m_slideStart);
        if (t >= kSlideMs) {
            m_sliding = false;
            f.slideY  = 0;
        } else {
            f.slideY    = Scale(stepPx, kSlideMs - t, kSlideMs);
            f.animating = true;
        }
    }

    return f;
}

} // namespace lyricus
