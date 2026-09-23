#pragma once

// ---------------------------------------------------------------------------
// 极简诊断日志。
//
// 组件跑在 foobar2000 进程里，调试器不好挂、控制台也未必看得见，
// 所以直接把诊断信息写到 <profile>\lyricus-debug.log，外部脚本可以直接读。
//
// 只在主线程调用，内部没有加锁。
// ---------------------------------------------------------------------------

namespace lyricus {

void DebugLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// 作用域计时器 —— 只在**超过阈值**时才记一笔。
//
// 为什么要有阈值：热路径不能每次都打日志。
// 比如窗口定时器每 250ms 跑一次，如果每次都记一行，日志会被刷爆，
// 真正的问题反而淹没在里面。只在「这次明显慢了」时记，
// 既能抓到卡顿，又保持平时的日志可读。
//
// 用法：
//     ScopedTimer t("ReloadLyrics", 5.0);
//     ... 要量的代码 ...
// 作用域结束时如果耗时 >= 5ms，就写一行日志。
// ---------------------------------------------------------------------------
class ScopedTimer {
public:
    explicit ScopedTimer(const char* what, double warnMs = 20.0);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    const char* m_what;
    double      m_warnMs;
    long long   m_start;
};

} // namespace lyricus
