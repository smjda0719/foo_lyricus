#include "stdafx.h"
#include "debug_log.h"

#include <cstdarg>
#include <cstdio>

namespace lyricus {
namespace {

// 日志跟在 DLL 旁边（即 user-components-xxx\foo_lyricus\）。
// 不用 core_api::pathInProfile —— 实测那份路径下文件从未生成，
// 原因未查明；写在 DLL 自己所在目录是确定可写、也确定找得到的。
pfc::string8 ResolveLogPath() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(core_api::get_my_instance(), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return pfc::string8("lyricus-debug.log");   // 兜底：相对路径
    }

    // 手工做宽字符 -> UTF-8，不依赖 pfc 的转换 API 名字
    char utf8[MAX_PATH * 3]{};
    const int written = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                            utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (written <= 0) {
        return pfc::string8("lyricus-debug.log");
    }
    utf8[written] = '\0';

    pfc::string8 path(utf8);
    const char* slash = strrchr(path.get_ptr(), '\\');
    if (slash != nullptr) {
        path.truncate(slash - path.get_ptr() + 1);
    }
    path += "lyricus-debug.log";
    return path;
}

} // namespace

// 日志上限。超了就把当前文件改名成 .1（覆盖上一个 .1）再重新开始。
//
// 【为什么需要】心跳每 10 秒一行，一天就是 8000 多行、约 700 KB；出问题时
// 各种诊断日志还会更多。实测跑一天 540 KB，一周就是 4 MB —— 而这是个常驻组件，
// 没上限的话它会一直涨。留一份上一代（.1）是为了"刚重启想看上次的日志"，
// 再多就没意义了。
constexpr long long kMaxLogBytes = 2 * 1024 * 1024;

void DebugLog(const char* fmt, ...) {
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    static const pfc::string8 path = ResolveLogPath();

    // 每 256 行查一次大小 —— 每条都去 GetFileAttributesEx 是白费 IO，
    // 而 2MB 的阈值下晚 256 行（约 30KB）完全无所谓。
    static unsigned sinceSizeCheck = 0;
    if (++sinceSizeCheck >= 256) {
        sinceSizeCheck = 0;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(path.get_ptr(), GetFileExInfoStandard, &fad)) {
            const long long size = (static_cast<long long>(fad.nFileSizeHigh) << 32) |
                                    static_cast<long long>(fad.nFileSizeLow);
            if (size > kMaxLogBytes) {
                pfc::string8 rotated(path);
                rotated += ".1";
                DeleteFileA(rotated.get_ptr());
                MoveFileA(path.get_ptr(), rotated.get_ptr());
            }
        }
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path.get_ptr(), "a") == 0 && f != nullptr) {
        fprintf(f, "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        fclose(f);
    }

    // 同时进 foobar2000 控制台（View -> Console），方便现场看
    console::printf("Lyricus: %s", body);
}

// ---------------------------------------------------------------------------
// 作用域计时器。见头文件里的说明。
// ---------------------------------------------------------------------------

ScopedTimer::ScopedTimer(const char* what, double warnMs)
    : m_what(what), m_warnMs(warnMs), m_start(0) {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    m_start = t.QuadPart;
}

ScopedTimer::~ScopedTimer() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return;

    const double ms = static_cast<double>(now.QuadPart - m_start) * 1000.0 /
                      static_cast<double>(freq.QuadPart);
    if (ms >= m_warnMs) {
        DebugLog("慢: %s 用了 %.1f ms", m_what, ms);
    }
}

} // namespace lyricus
