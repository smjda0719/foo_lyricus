// ---------------------------------------------------------------------------
// FindLyricFile 的成本结构 —— 基准测试
//
// 【为什么要量这个】日志里抓到一次换曲时
//     FindLyricFile（目录搜索） 用了 11.5 ms
// 而那个目录 F:\thoughts 只有 10 个文件、**一个歌词都没有**。
// 10 个文件要 11.5ms 说不通，所以必须先搞清楚这 11.5ms 到底花在哪：
//
//     (a) 每个歌词文件都要做一次归一化 —— 那就是 CPU 开销，随文件数线性涨
//     (b) 目录枚举本身（外置 USB 盘冷下来时的首次访问）—— 与文件数无关
//
// 这两者的修法完全不同：(a) 要优化归一化或加缓存，(b) 要把搜索挪出主线程。
// 猜是没用的，所以在**真实源码**上量。
//
// 同一份 lyric_search.cpp，同一套 shim，和 test_search.cpp 完全一致。
// ---------------------------------------------------------------------------

#include "lyric_search.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace lyricus {
void DebugLog(const char* fmt, ...) { (void)fmt; }
std::string WideToUtf8(const std::wstring&) { return std::string(); }
} // namespace lyricus

namespace {

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    const size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring(L".") : p.substr(0, s);
}

void Touch(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::wstring MakeDir(const wchar_t* name) {
    const std::wstring base = ExeDir() + L"\\bench";
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring d = base + L"\\" + name;
    RemoveDirectoryW(d.c_str());          // 只删空目录；下面按需重建
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// 建一个目录：1 个音频 + n 个**不相干**的歌词文件。
// 「不相干」是故意的 —— 这样每条策略都走完全程（都要落空），
// 量的就是最坏情况的成本，也正是日志里那种「目录里一个都没命中」的场景。
std::wstring BuildDir(const wchar_t* name, int n) {
    const std::wstring d = MakeDir(name);
    Touch(d + L"\\5 Titania.flac");
    wchar_t buf[64];
    for (int i = 0; i < n; ++i) {
        _snwprintf_s(buf, _TRUNCATE, L"\\Unrelated Track %04d.lrc", i);
        Touch(d + buf);
    }
    return d;
}

double NowMs() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// 命令行参数是 UTF-8（控制台代码页），转成宽字符给 FindLyricFile。
std::wstring Utf8ToWideLocal(const char* s) {
    if (s == nullptr || *s == '\0') return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

struct Stat {
    double minMs = 1e9;
    double avgMs = 0.0;
    double maxMs = 0.0;
};

// 跑 iters 次 FindLyricFile，返回单次耗时的统计。
Stat Bench(const std::wstring& dir, bool useFuzzy, int iters) {
    lyricus::LyricSearchConfig cfg;
    cfg.fuzzy   = useFuzzy;
    cfg.useTags = true;

    const std::wstring audio = dir + L"\\5 Titania.flac";

    // 先跑一次做预热（首次访问文件系统、首次分配缓冲都在这里付掉）
    lyricus::FindLyricFile(audio, L"Aki Sz", L"Titania", L"thoughts", cfg);

    Stat st;
    double sum = 0.0;
    for (int i = 0; i < iters; ++i) {
        const double t0 = NowMs();
        lyricus::FindLyricFile(audio, L"Aki Sz", L"Titania", L"thoughts", cfg);
        const double dt = NowMs() - t0;
        st.minMs = (std::min)(st.minMs, dt);
        st.maxMs = (std::max)(st.maxMs, dt);
        sum += dt;
    }
    st.avgMs = sum / iters;
    return st;
}

} // namespace

// 直接量一个**真实目录**。
//
// 【为什么需要这个】上面那些用例都建在 C:（SSD）上，而日志里那次 11.5ms 发生在
// F:（WD My Passport，外置 USB + exFAT）、目录里只有 10 个文件、一个歌词都没有 ——
// 按上面的数字那应该是 0.07ms，差了 160 倍。
// 所以必须能在**那块盘上**量同一段代码，才能分清是「我们的 CPU 开销」
// 还是「那块盘的目录访问延迟」。
//
// 用法：bench_search.exe F:\thoughts [再来一个目录...]
void ProbeDir(const char* dir) {
    const std::wstring wdir = Utf8ToWideLocal(dir);
    if (wdir.empty()) return;

    lyricus::LyricSearchConfig cfg;
    cfg.fuzzy   = true;
    cfg.useTags = true;

    // 音频文件不需要真实存在 —— 搜索只用它的目录和文件名主干。
    const std::wstring audio = wdir + L"\\__lyricus_bench__.flac";

    const int kIters = 200;
    double mn = 1e9, mx = 0.0, sum = 0.0;
    std::vector<double> all;
    all.reserve(kIters);

    for (int i = 0; i < kIters; ++i) {
        const double t0 = NowMs();
        lyricus::FindLyricFile(audio, L"Bench", L"Probe", L"", cfg);
        const double dt = NowMs() - t0;
        mn = (std::min)(mn, dt);
        mx = (std::max)(mx, dt);
        sum += dt;
        all.push_back(dt);
    }
    std::sort(all.begin(), all.end());

    std::printf("  %-34s 最快 %6.3f  中位 %6.3f  平均 %6.3f  p95 %6.3f  最慢 %7.3f ms\n",
                dir, mn, all[kIters / 2], sum / kIters, all[kIters * 95 / 100], mx);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::printf("真实目录上的 FindLyricFile（%d 次/目录）\n", 200);
        for (int i = 1; i < argc; ++i) ProbeDir(argv[i]);
        return 0;
    }

    std::printf("FindLyricFile 成本结构（单位 ms，越小越好）\n");
    std::printf("%-28s %8s %8s %8s   %s\n",
                "目录内容", "最快", "平均", "最慢", "相对空目录的增量");

    const int    counts[] = { 0, 50, 500, 3000 };
    const int    kIters   = 40;
    double baseAvg = 0.0;

    for (int idx = 0; idx < 4; ++idx) {
        const int n = counts[idx];
        wchar_t name[32];
        _snwprintf_s(name, _TRUNCATE, L"n%d", n);

        const std::wstring dir = BuildDir(name, n);
        const Stat st = Bench(dir, true, kIters);
        if (idx == 0) baseAvg = st.avgMs;

        char label[64];
        _snprintf_s(label, _TRUNCATE, "音频 + %d 个歌词文件", n);
        std::printf("%-28s %8.3f %8.3f %8.3f   %+.3f\n",
                    label, st.minMs, st.avgMs, st.maxMs, st.avgMs - baseAvg);
    }

    // 归一化本身的成本：一个文件一次，所以斜率才是关键
    std::printf("\n每个歌词文件的边际成本（由上面 0->3000 的斜率算出）\n");
    {
        const std::wstring d0 = BuildDir(L"slope0", 0);
        const std::wstring d3 = BuildDir(L"slope1", 3000);
        const Stat s0 = Bench(d0, true, kIters);
        const Stat s3 = Bench(d3, true, kIters);
        const double per = (s3.avgMs - s0.avgMs) / 3000.0 * 1000.0;   // 微秒
        std::printf("  3000 个文件比 0 个多花 %.3f ms  ->  每个约 %.3f 微秒\n",
                    s3.avgMs - s0.avgMs, per);
        std::printf("  换算：一个 500 文件的专辑目录约多花 %.3f ms\n",
                    per * 500.0 / 1000.0);
    }

    // 关掉模糊匹配再看一次 —— 用来判断模糊打分是不是大头
    std::printf("\n关掉模糊匹配后（判断模糊打分占多少）\n");
    {
        const std::wstring d = BuildDir(L"nofuzzy", 3000);
        const Stat on  = Bench(d, true,  kIters);
        const Stat off = Bench(d, false, kIters);
        std::printf("  3000 文件  开模糊 %.3f ms   关模糊 %.3f ms   差 %.3f ms\n",
                    on.avgMs, off.avgMs, on.avgMs - off.avgMs);
    }

    // 归一化单独量：它是每个文件都要走的
    {
        const double t0 = NowMs();
        volatile size_t sink = 0;
        for (int i = 0; i < 10000; ++i) {
            sink = sink + lyricus::NormalizeLyricStem(L"Unrelated Track 0042").size();
        }
        const double dt = NowMs() - t0;
        (void)sink;
        std::printf("\nNormalizeLyricStem 单次：%.3f 微秒（10000 次共 %.2f ms）\n",
                    dt * 1000.0 / 10000.0, dt);
    }

    return 0;
}
