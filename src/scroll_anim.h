#pragma once

#include <windows.h>   // ULONGLONG

#include <cstddef>

// ---------------------------------------------------------------------------
// 歌词的两种"动"：长行横滚 + 换行上滑
//
// 【为什么单独一个文件、为什么是纯的】时间线要能离线验证。
// 这里**不碰窗口、不碰 DC、也不自己取时钟** —— nowMs 由宿主传进来，
// 单测就能把时间轴推着走，把「静止 → 左移 → 停在尾部」整条曲线走完，
// 不用开窗口、不用肉眼看（见 tests/harness/test_view.cpp 的 TestAnimator）。
//
// 三个宿主各持有一个实例：动画进度按**绝对时间**算，所以三个面板天然同步，
// 不需要互相通知。
//
// 设计背景与取舍见 docs/design-lyric-motion.md。
// ---------------------------------------------------------------------------

namespace lyricus {

// 一帧的动画输出。
struct LyricAnimFrame {
    int  scrollX   = 0;      // 当前行向左移出多少像素（0 = 不动）
    int  slideY    = 0;      // 整块歌词下移多少像素（0 = 最终位置）

    // 还需不需要后续帧 —— 宿主据此决定动画定时器开不开。
    //
    // ⚠️ 它**不是**"这一帧要不要重绘"。宿主必须按内容判断：
    //    帧和上一帧不一样就重绘，再看这个标志决定要不要继续开快定时器。
    //    反例：上滑结束的那一帧 slideY 从 >0 变成 0，内容变了（要重绘），
    //    但它同时把 animating 置回 false（不用再要下一帧）——
    //    只看 animating 的话，最后那一帧会被吞掉，画面停在偏移位置上。
    bool animating = false;
};

// 起步前先静止这么久 —— 让人先读到开头，再开始滚。
constexpr ULONGLONG kScrollLeadMs = 900;

// 基准滚动速度（逻辑像素/秒）。用速度而不是"每字多少毫秒"，
// 否则长句会滚得没完没了。
constexpr int kScrollPxPerSec = 60;

// 滚动时长的上下限。上限的意思是：超长行**靠提速**解决，不许拖着滚。
constexpr ULONGLONG kScrollMinMs = 600;
constexpr ULONGLONG kScrollMaxMs = 5000;

// 换行时整块歌词上滑的时长。
constexpr ULONGLONG kSlideMs = 200;

class LyricAnimator {
public:
    // 每次逻辑拍（250ms）和每次动画拍都调一次。
    //
    //   nowMs      宿主给的单调时钟（GetTickCount64）。
    //              **不要在这里面调 GetTickCount64** —— 单测要注入时间。
    //   lineIndex  当前正在显示的歌词行号。变了就是换行了。
    //   overflowPx 当前行比可用宽度宽出多少（<=0 表示放得下，不滚）。
    //              每次调用都重新给：面板被拉宽之后同一行可能就放得下了。
    //   stepPx     上滑的起始位移（一般取"上一行作为上下文行时的步距"）。
    //
    // ⚠️ `animating` 的语义见 LyricAnimFrame —— 核心是"起步前的静止期必须是
    //    false"：那 900ms 里画面一个像素都不变，让动画定时器空转 25fps
    //    是白烧主线程。静止期结束时会有一次 250ms 的逻辑拍把它翻成 true，
    //    代价是最多晚 250ms 起步，而滚动位置是按**绝对时间**算的，
    //    所以不会丢距离、也不会漂。
    LyricAnimFrame Update(ULONGLONG nowMs, std::size_t lineIndex,
                          int overflowPx, int stepPx);

    // 清掉全部状态：换曲、seek、改设置时调。
    //
    // 清完之后第一次 Update 会把当前行当成"新的一行"，所以**不会**上滑 ——
    // 这正是 seek 之后想要的行为。
    void Reset();

private:
    std::size_t m_lineIndex  = 0;
    ULONGLONG   m_lineStart  = 0;
    bool        m_hasLine    = false;

    bool        m_sliding    = false;
    ULONGLONG   m_slideStart = 0;

    // 上一次 Update 的 overflow —— 只在"放得下"和"放不下"之间翻转时
    // 才需要重置时间线（面板被拉宽，原本要滚的行突然放得下了）。
    int         m_lastOverflow = 0;
};

} // namespace lyricus
