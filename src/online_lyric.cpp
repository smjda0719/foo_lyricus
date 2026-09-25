#include "stdafx.h"
#include "online_lyric.h"

#include "debug_log.h"
#include "lyric.h"          // Utf8ToWide / WideToUtf8 —— 宽窄转换沿用工程里已有的实现
#include "lyric_search.h"   // NormalizeLyricStem —— 核验网易云候选时复用本地搜索那套归一化

#include <winhttp.h>

#include <atomic>      // 网易云限流的进程级冷却
#include <algorithm>   // std::sort —— 没匹配到的曲目名单要排序
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>      // fabs —— 合并翻译时比时间戳
#include <cstring>
#include <cwctype>   // towlower —— 剥版本标记时大小写无关地比对
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// WinHTTP 是系统自带的（winhttp.dll + Windows SDK 的 winhttp.lib），
// 不是第三方库 —— 符合"不引入 libcurl"的约束。
//
// 用 #pragma comment 而不是去动 build\foo_lyricus.vcxproj 的
// <AdditionalDependencies>：工程文件由人手工维护，这里自报依赖，
// 少一个"改了 .cpp 还得记得同步改工程"的坑。
// 想移到工程文件里也可以，两者等价，删掉这行即可。
#pragma comment(lib, "winhttp.lib")

// ===========================================================================
//  LRCLIB 在线歌词查询
//
//  用到的两个真实接口（URL 与参数名照抄官方文档，不要改）：
//    精确：GET https://lrclib.net/api/get?artist_name=<A>&track_name=<T>
//                                     [&album_name=<Al>][&duration=<秒>]
//          命中 -> 200 + JSON 对象；未命中 -> 404
//    模糊：GET https://lrclib.net/api/search?q=<关键词>
//          -> 200 + JSON 数组（查不到时是空数组 "[]"，**不是 404**）
//
//  响应里关心的字段：id / trackName / artistName / albumName / duration /
//  instrumental / plainLyrics / syncedLyrics。
//  取值优先 syncedLyrics（带时间轴的 LRC），为空再退回 plainLyrics。
//
//  ── 这些结论是拿真接口实测过的，不是照文档猜的 ──────────────────────
//   * duration= 传**空值会 400**（"duration=" 和 "duration=abc" 都是 400），
//     所以时长未知时**必须把整个参数去掉**；传小数（239.5 / 239.123456）正常。
//   * track_name / artist_name 是必填，任一为空 -> 400。
//   * album_name 可以整个不带，照样 200。
//   * /api/search 查不到东西时返回 "[]" + 200，不是 404。
//   * 字段值里**确实**会出现转义的双引号 \" （歌词里有英文引号时），
//     所以找字符串结尾不能简单地找下一个 '"'。
//   * "syncedLyrics":null 这种**裸 null** 在真实响应里存在（纯音乐条目）。
//   * 中文/日文目前是原样 UTF-8 发过来的，**不用** \uXXXX；
//     但这是实现细节，随时可能变，所以反转义按标准全做。
//   * 会被限流：实测连续快查会返回 **503**。所以 5xx 一律当"暂时失败"，
//     **绝不**写成"这首歌没有歌词"的缓存（否则用户要等 7 天才可能查得到）。
// ===========================================================================

namespace lyricus {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// LRCLIB 要求标明来源，否则可能被拒。这个串同时也是 WinHttpOpen 的
// pszAgentW —— WinHttpOpen 的第一个参数就是 User-Agent，
// 比事后用 WinHttpSetOption(WINHTTP_OPTION_USER_AGENT, ...) 更省事，
// 也避开了那个选项"长度按字符还是按字节"的歧义。
constexpr wchar_t kUserAgent[] = L"Lyricus/0.1.0 (foobar2000 component)";
constexpr wchar_t kHost[]      = L"lrclib.net";

// 第二个在线源：网易云音乐。
//
// 【这是**非官方**接口】网易云没有公开的歌词 API，这里是社区逆向出来的
// `music.163.com/api/...` 路径（NeteaseCloudMusicApi 那一套用的同一批端点）。
// 它没有文档、没有版本承诺，随时可能改加密或直接封掉 ——
// 所以它只作为补充，LRCLIB 那条路必须保持可用。
//
// 【实测：不需要任何自定义请求头】2026-09-24 在用户机器上打过 6 种组合
//   （自定义 UA / 浏览器 UA × 带 Referer+Cookie / 只带 Referer / 都不带），
//   **全部 code=200**。社区代码里常见的 Referer 和 Cookie 在这个端点上不是必需的。
//   所以这里不伪造浏览器 User-Agent —— 用我们自己的 kUserAgent 就够了。
//   ⚠️ 将来要是开始返回 403/空结果，第一件事就是回头试 Referer 和浏览器 UA。
constexpr wchar_t kNetEaseHost[] = L"music.163.com";

// 超时必须设，否则网络不通（或者对方把我们限流挂起）时后台线程会挂很久。
// 四个都设上：DNS 解析 / 建连 / 发送 / 接收。
// 任务要求"连接 10 秒、接收 20 秒"，这里照办，另外两个也给了合理值。
// 最坏情况一次请求约 10+10+20=40 秒，这是刻意选择的"宁可慢也不要挂死"。
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs    = 10000;
constexpr int kReceiveTimeoutMs = 20000;

// 响应体上限。实测一次中文搜索的响应就有 100 KB 量级，
// 但不设上限就意味着对方（或者中间人）可以让我们无限吃内存。
constexpr size_t kMaxBodyBytes = 8u * 1024u * 1024u;

// URL 上限。整个"路径+查询串"都得塞进 WinHttpOpenRequest 的
// pwszObjectName，太长了 WinHTTP 会直接失败。
constexpr size_t kMaxUrlChars = 8192;

// 未命中标记的有效期：7 天。
// 必须有过期策略 —— 否则用户今天查不到、明天 LRCLIB 上有人补了歌词，
// 我们的插件却会因为那个"没有"的标记永远不再查。
constexpr long long kMissTtlSeconds = 7 * 24 * 60 * 60;

// 退避重试。
//
// 【为什么需要】LRCLIB 实测会限流/过载，返回
//   503 {"message":"The server is busy, please retry in a moment",
//        "name":"ServerOverloaded","statusCode":503}
// 这不是"这首歌没有歌词"，而是"你现在别问我"。直接当成否定的答案，
// 用户就会莫名其妙地查不到歌 —— 过一会儿再点一次又好了。
// 所以 429 / 5xx / 超时都要退避重试。
//
// 【为什么要有总预算】重试会让最坏耗时成倍增长。每次尝试自己就可能
// 吃满超时（10s 解析 + 10s 连接 + 20s 接收），无脑重试三次就是两分钟；
// 而 foobar2000 退出时会等这个后台任务结束（fb2k::splitTask 的语义），
// 也就是说用户关播放器得等这么久。
//
// 所以这里设一个**整轮共享的墙钟截止时刻**（不是每个接口一份，
// 而是整个 FetchLyricOnline 一份 —— 每个接口一份的话，两个接口
// 各自都能重试，最坏耗时还是要翻倍）。重试前先看有没有越过截止时刻，
// 越过了就不再重试，把已经拿到的结论直接返回。
//
// 得到的性质正好是我们想要的：
//   * 服务器快速回 503（实测约 200ms）-> 预算几乎没动，重试照跑，
//     通常第二次就成功了；
//   * 服务器不理我们（一路吃满超时）-> 第一次尝试就把预算耗光，
//     于是不重试 —— 因为"超时"这种失败重试一次基本还是超时，
//     白等而已；
//   * 200 / 404 这类确定性回答本来就不重试。
//
// 因此重试能带来的最坏额外耗时只有一个退避间隔加一次快速尝试，
// 不会让"关播放器"多等一个超时周期。
//
// 这两个数都是可以调的：想更激进就加大 kMaxHttpAttempts，
// 想更保守就调小 kTotalRetryBudgetMs。
constexpr int    kMaxHttpAttempts       = 3;      // 1 次初试 + 最多 2 次重试
constexpr DWORD  kRetryBackoffMs[]      = { 600, 1500 };
constexpr ULONGLONG kTotalRetryBudgetMs = 25000;  // 整轮共享的重试预算

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

// 本模块自己的日志入口。
//
// 【为什么要包一层】debug_log.h 开头写着 DebugLog「只在主线程调用，内部没有
// 加锁」，而本模块绝大多数调用点都在后台线程上，还可能同时有好几个后台取词
// 在跑。这里用一把模块内的互斥锁把所有写日志的动作串行化，避免
// debug_log.cpp 里那串 fopen/fprintf/fclose 被交叉执行把日志写花。
//
// （console::printf 自己就是线程安全的 —— SDK/console.h 第 4 行明说
//   "All functions are fully multi-thread safe"。要保护的只是写文件那段。）
void OnlineLog(const char* fmt, ...) {
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    static std::mutex s_logMutex;
    std::lock_guard<std::mutex> lock(s_logMutex);
    DebugLog("%s", body);
}

// 把一坨可能带换行/控制字符的文本收拾成"一行"。
// 日志文件是"一行一条"的格式（debug_log.cpp 每条都加时间戳前缀），
// 被塞进去的响应体里一旦有换行，一条记录就被劈成好几行，
// 事后用脚本筛日志会非常难受。
std::string LogSafe(const std::string& s, size_t maxLen = 200) {
    std::string out;
    out.reserve(s.size() < maxLen ? s.size() : maxLen);
    for (size_t i = 0; i < s.size() && out.size() < maxLen; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out.push_back(c < 0x20 ? '.' : static_cast<char>(c));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 字符串小工具
// ---------------------------------------------------------------------------

// 去首尾空白。判定集合和 lyric.cpp 里的 Trim 保持一致
// （0x3000 是全角空格，中文标签里很常见）。
std::wstring TrimWs(const std::wstring& s) {
    auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
    };
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 缓存键用的归一化。
//
// 做什么：首尾去空白 -> 内部连续空白压成一个空格 -> ASCII 大写转小写。
//
// 为什么只折 ASCII 的大小写（不用 towlower / CharLowerW）：
// 那两个的结果取决于进程或用户的区域设置，同一台机器换个 locale
// 就会算出另一个哈希，缓存整片作废。曲名里的中文、日文没有大小写，
// 折叠与否都不受影响，所以这个"简化"没有实际损失。
//
// 为什么要压缩内部空白：标签里的曲名可能写 "Song  Title"（两个空格），
// 而 LRCLIB 上是 "Song Title"；不归一化的话同一首歌会算出两个键，
// 缓存命中率凭运气。
std::wstring NormalizeForKey(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (wchar_t ch : s) {
        const bool isSpace = (ch == L' ' || ch == L'\t' || ch == L'\r' ||
                              ch == L'\n' || ch == 0x3000);
        if (isSpace) {
            if (!out.empty()) pendingSpace = true;   // 开头的空白直接丢
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
        out.push_back(ch);
    }
    // 结尾的 pendingSpace 故意不落盘 => 等价于去尾空白
    return out;
}

// ---------------------------------------------------------------------------
// 稳定哈希 + 缓存文件名
// ---------------------------------------------------------------------------

// FNV-1a 64 位。
//
// 【为什么不用 std::hash<std::wstring>】标准**不保证**它的算法：
// 不同 STL 实现之间、甚至同一实现的不同版本之间都可能不同。
// 换了编译器或者升级一次 VS，同一首歌算出来的哈希就变了，
// 用户的一整份缓存会无声无息地全部失效（不报错，只是每次都要重新联网）。
//
// FNV-1a 的常数是公开固定的，自己写六行就把结果永久锁死了。
uint64_t Fnv1a64(const std::string& utf8) {
    uint64_t h = 0xcbf29ce484222325ULL;   // FNV offset basis (64-bit)
    for (char ch : utf8) {
        h ^= static_cast<unsigned char>(ch);
        h *= 0x100000001b3ULL;            // FNV prime (64-bit)
    }
    return h;
}

std::wstring HashHex(uint64_t h) {
    wchar_t buf[17];
    swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// 缓存键：归一化后的 "<artist>|<title>|<duration>"。
//
// duration 取**四舍五入到整数秒**：同一个文件用不同解码器/不同版本的标签
// 读出来，时长会差零点几秒（239.4 / 239.6）；不舍入的话同一首歌会按
// 239 / 240 各存一份缓存，命中率凭运气。
// 未知时长（durationSec <= 0）用固定串 "nodur" ——
// 它不可能和任何真实秒数的十进制写法撞上。
std::wstring MakeCacheKey(const OnlineLyricRequest& req) {
    wchar_t dur[32];
    if (req.durationSec > 0.0) {
        const long long sec = static_cast<long long>(req.durationSec + 0.5);
        swprintf_s(dur, L"%lld", sec);
    } else {
        wcscpy_s(dur, L"nodur");
    }
    return NormalizeForKey(req.artist) + L"|" + NormalizeForKey(req.title) +
           L"|" + dur;
}

struct CachePaths {
    std::wstring lrc;    // 命中结果（UTF-8 的 LRC 原文，和 LRCLIB 给的一模一样）
    std::wstring meta;   // 命中结果的元数据（哪首歌），JSON 对象
    std::wstring miss;   // "查过但没有"的标记
};

// 缓存文件名**只用哈希的十六进制**，一个原始曲名的字都不掺。
//
// 拿曲名当文件名的坑，随便一个都能让写文件失败或者写错地方：
//   * 中文/日文曲名的编码不确定（写文件时按什么编码？）；
//   * Windows 非法字符 \ / : * ? " < > | —— 曲名里出现 "?" 或 ":" 极常见；
//   * 保留设备名 CON / PRN / AUX / NUL / COM1...；
//   * 超长路径（MAX_PATH）和结尾的点/空格。
// 哈希是定长十六进制，上面这些问题一个都不存在。
CachePaths MakeCachePaths(const std::wstring& cacheDir, const OnlineLyricRequest& req) {
    const std::wstring hex = HashHex(Fnv1a64(WideToUtf8(MakeCacheKey(req))));

    std::wstring base = cacheDir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') {
        base.push_back(L'\\');
    }

    CachePaths p;
    p.lrc  = base + hex + L".lrc";
    p.meta = base + hex + L".meta";
    p.miss = base + hex + L".miss";
    return p;
}

// ---------------------------------------------------------------------------
// 文件与目录（只用 kernel32，不加新依赖）
// ---------------------------------------------------------------------------

// 逐级建目录。
//
// 不用 SHCreateDirectoryExW：那是 shell32.lib 的，而本工程只链了
// shlwapi / comctl32 这一批（见 build\foo_lyricus.vcxproj）。
// 为了建一个目录去动工程文件不划算，CreateDirectoryW 是 kernel32 的，
// 永远都在。
bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;

    for (size_t i = 1; i <= dir.size(); ++i) {
        const bool atEnd = (i == dir.size());
        if (!atEnd && dir[i] != L'\\' && dir[i] != L'/') continue;

        const std::wstring prefix = dir.substr(0, i);
        // "D:" 单独作为一级没有意义（那表示"当前目录"），跳过；
        // "D:\" 之类的根再交给 CreateDirectoryW 报 ERROR_ALREADY_EXISTS。
        if (prefix.size() == 2 && prefix[1] == L':') continue;

        if (!CreateDirectoryW(prefix.c_str(), nullptr)) {
            const DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) {
                // 不在这里就放弃：多级路径中间失败（比如 UNC 前缀）
                // 未必代表最终目录建不出来，最后由属性检查定论。
            }
        }
    }

    const DWORD attr = GetFileAttributesW(dir.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 读整个文件。**文件不存在是常态**（第一次查询必然不存在），
// 返回 false 让调用方安静地往下走，不要当错误刷日志。
bool ReadWholeFile(const std::wstring& path, std::string& out) {
    out.clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (4 << 20)) {
        CloseHandle(h);
        return false;   // 0 字节（上次写到一半崩了）也当没有
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);

    if (!ok || read == 0) {
        out.clear();
        return false;
    }
    out.resize(read);
    return true;
}

bool WriteWholeFile(const std::wstring& path, const void* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = (len == 0)
        ? TRUE
        : WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE && written == len;
}

// 删文件，"本来就不存在"不算错误。
//
// 三个调用点都要求这种宽容语义：
//   * 命中后删旧的未命中标记（多半本来就没有）；
//   * 发现缓存文件坏了要删掉（可能已经被别的线程删了）；
//   * 未命中标记过期要删掉。
// 把它们当成错误会让日志里全是噪音，真正的问题反而被淹掉。
bool DeleteFileIfExists(const std::wstring& path) {
    return DeleteFileW(path.c_str()) != 0;
}

// UTC 的 Unix 秒。用 FILETIME 换算而不是 _time64：
// 不碰 CRT 的时区状态，也就不会因为别处调了 tzset 而变。
long long NowUnixSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);          // 已经是 UTC
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME 是"1601-01-01 起的 100ns 计数"；11644473600 是 1601->1970 的秒数
    return static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

// 未命中标记的内容：两行文本 " <写入时刻> \n <当时的 HTTP 状态码> "。
//
// 特意把状态码一起存下来：读缓存时能如实还原当时到底是 404 还是
// "搜索成功但没有结果（200）"，而不是编一个假的 404 出来骗调用方。
//
// 【第三个字段：匹配逻辑版本】后加的，但很关键 ——
// 见 kMissLogicVersion 的说明。
struct MissMarker {
    long long stamp  = 0;
    long      status = 0;
    int       logic  = 0;
};

// 匹配/核验逻辑的版本号。**改动候选筛选规则时必须 +1。**
//
// 【为什么必须有这个东西】未命中标记管 7 天。而「未命中」这个结论
// 是**当时的匹配逻辑**算出来的 —— 逻辑一改，旧结论就可能整个是错的，
// 但文件还在，于是用户被告知"6 天后再来"。
//
// 这不是假想：2026-09-24 演唱者闸门过严，把用户「再见，碳酸海」整张专辑
// 误判成"没有歌词"，7 天标记当场写下去。修好代码之后**用户还是要等 6 天**
// 才可能看到歌词 —— 除非有版本号让旧标记作废。
//
// 版本 1：初版（演唱者硬否决）
// 版本 2：演唱者改为排序加分项（繁简写法的误杀）
// 版本 3：曲名改用 MakeTitleCandidates（音轨号「02 遗忘山丘」/「艺术家 - 曲名」前缀），
//         且时长与演唱者**二者其一**对上即可 —— remaster 与原版差 34 秒也要能配上
// 版本 4：识别网易云的**业务码**（HTTP 200 + {"code":405} 的限流）。
//         v3 之前写下的"没有"里混着限流造成的假结论，必须全部作废重查。
// 版本 5：搜索词剥音轨号 + 线索兜底。
// 版本 6：核对时用**实际查询用的歌手**（含用户填的线索），而不是标签里的占位符。
//         v5 期间「爸爸……（Interlude）」「春风来（Love Elegia Ver.）」这类
//         被误判成"没有"（差 15 / 7.1 秒，而演唱者那一步手里是空的），要重查。
// 版本 7：版本标记（[Remastered] 之类）在**搜索词**和**调用方核对**两处也要剥。
//         v6 期间「最后的歌（LA LA LA）[Remastered]」「心加心 [Remastered]」
//         被源头选中却在核对时丢掉；「远恋 [Remastered]」
//         「依存症（Love Theory Ver.）[Remastered]」连候选都没搜到。
//
// ── v8（2026-09-25）：繁简折叠 ──
//         NormalizeLyricStem 里加了繁->简折叠（FoldToSimplified），
//         曲名硬闸 / 演唱者软闸 / 本地文件名比对**同时**变得繁简无关。
//         起因：标定时翻日志看到 `純白P - 扁桃体` 过曲名闸 1 条、
//         演唱者对上 0、时长对上 0，怀疑是 D-034 那个老病根。
// ── v9（2026-09-25 晚）：演唱者闸认"汉字骨架" ──
//         「純白P」=「Soda纯白」（去掉拉丁字母/数字后骨架都是「纯白」）。
// ── v10（2026-09-25 晚，紧随 v9）：时长倍数上限 1.5 ──
//         ⚠️ v8/v9 的**出发点被用户否掉了**：`扁桃体` 那条候选
//         （2:06 vs 本地 4:02）用户明确说「不是一首，是另一个版本」。
//         而 v9 让演唱者闸认得出它 -> 在"时长或演唱者其一即可"的规则下
//         会被接受 -> 错配。所以补一道时长倍数上限：同一个人、同一个歌名，
//         长度差一倍就是另一版录音，谁对上都不算。
//         边界由实测数据卡出（心加心 1.12 收、扁桃体 1.92 拒），见
//         kSameRecordingRatioCeiling 的说明。
// ── v11（2026-09-25 深夜）：取词路径从 lv=1 改成 lv=-1 ──
//         lv=1 = "只要第 1 版歌词"，新歌的词只存在于更高版本 -> 服务端返回空，
//         而空被当成"各源都没有"写进 7 天负缓存。实测隔离见 BuildNetEaseLyricPath。
//         这一版必须作废旧标记，否则用户要干等一周才看到修复。
constexpr int kMissLogicVersion = 11;

// 时长倍数上限：两边长度相差超过这个倍数，就当成**另一版录音**，直接拒。
//
// 【为什么需要它，以及为什么是 1.5】原来的规则是"曲名（硬）+ 时长或演唱者
// 其一即可（软）" —— 也就是说**光靠演唱者对得上就能接受一条长度差一倍的候选**。
// 2026-09-25 用户实测确认了那个后果：
//   `純白P - 扁桃体` 本地 4:02，网易云那条（Soda纯白/洛天依）2:06 ——
//   用户明确说「**不是一首，网易云那条是另一个版本**」。
//   而演唱者闸刚被改成认得出 `純白P = Soda纯白`，于是它会被接受 -> 错配。
//
// 边界由三组**实测**数据卡出来，不是拍的：
//   心加心   308.0 / 273.9 = 1.12  -> 接受（用户拍板"宁可偏，也要有词"）
//   春风来   247.1 / 240.0 = 1.03  -> 接受（那条 7 秒差是剪辑版本）
//   扁桃体   242.3 / 126.0 = 1.92  -> **拒绝**（用户：另一个版本）
// 1.5 落在最后一个真实分界（1.12 与 1.92）之间，两边都留了余量。
//
// ⚠️ 只在**两边时长都已知**时才判 —— 时长未知的候选交还给原来的两道软闸。
constexpr double kSameRecordingRatioCeiling = 1.5;

// 未命中时日志里最多列几条候选。
//
// 5 条足够看出"是搜索词不对"（候选全不相干）还是"闸门太严"
//（正确答案就在前几条里却没通过）；列 20 条只会把日志刷乱。
constexpr int kDiagCandidates = 5;

// 把候选曲名列表拼成一行给日志用（`[哀歌] [歌]`）。空列表返回 `(空)`。
std::wstring JoinForLog(const std::vector<std::wstring>& v) {
    if (v.empty()) return L"(空)";
    std::wstring s;
    for (const std::wstring& e : v) {
        if (!s.empty()) s += L" ";
        s += L"[" + e + L"]";
    }
    return s;
}

bool ReadMissMarker(const std::wstring& path, MissMarker& out) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;

    long long stamp = 0;
    long      status = 0;
    int       logic  = 0;
    // 老标记只有两个字段 —— 这里必须容忍，然后由下面的版本判断把它作废
    const int got = sscanf_s(text.c_str(), "%lld %ld %d", &stamp, &status, &logic);
    if (got < 2) return false;
    if (stamp <= 0) return false;

    out.stamp  = stamp;
    out.status = status;
    out.logic  = (got >= 3) ? logic : 0;
    return true;
}

bool WriteMissMarker(const std::wstring& path, long httpStatus,
                     const std::string& identity) {
    // 【为什么要把曲目身份写进去】原先只存 `时间戳 HTTP码 逻辑版本`，
    // 结果事后**完全没法追查**：用户说"这首没匹配到"，我打开 .miss 也看不出
    // 是哪一首，于是分不清"在线源确实没有"还是"我们的匹配逻辑错了"。
    // 而这两件事的处理方式完全相反（前者该收工，后者该改代码）。
    //
    // identity 放在**第一行之外**（换行分隔），这样解析时按下标取，
    // 老格式（没有这一行）依然读得出来 —— 不用为它升逻辑版本。
    char head[80];
    sprintf_s(head, "%lld %ld %d\n", NowUnixSeconds(), httpStatus, kMissLogicVersion);

    std::string text = head;
    if (!identity.empty()) {
        text += identity;
        text += '\n';
    }
    return WriteWholeFile(path, text.c_str(), text.size());
}

// ===========================================================================
//  极简 JSON 取值器
//
//  ⚠️⚠️ 这**不是**通用 JSON 解析器，是专门为 LRCLIB 那一种响应形状
//      写的一次性工具。请**不要**拿它去解析别的服务的响应。
//
//  它明确**不做**的事：
//    * 不做语法校验（畸形的输入只会让某个函数返回 false，不会给出"哪里错了"）；
//    * 不建 DOM、不保留键值对集合，只把用得上的几个字段捞出来；
//    * 不处理重复键的语义（后出现的会覆盖先出现的，仅此而已）；
//    * 不认 BOM、不认非 UTF-8 的输入（LRCLIB 回的就是 UTF-8）；
//    * 不保证对任意畸形输入都不越界 —— 只保证对"看起来像 LRCLIB 形状"的
//      输入是安全的（所有下标访问前都判了边界）。
//
//  之所以自己写而不引第三方 JSON 库：LRCLIB 的响应是"一个平坦对象"
//  或者"这种对象的数组"，字段类型只有字符串/数字/布尔/null，
//  为这点需求往一个 foobar2000 组件里塞一个 JSON 库不划算。
//
//  但是有三件事**必须**做对，下面每个函数都专门处理了：
//    1. 字符串值要**反转义**（\" \\ \/ \b \f \n \r \t \uXXXX）；
//    2. 找字符串的结束引号**不能**简单找下一个 '"'（值里可能有 \"）；
//    3. 值可能是**裸的 null**（不带引号），要当空处理。
// ===========================================================================

inline bool IsJsonWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

size_t SkipWs(const std::string& s, size_t i) {
    while (i < s.size() && IsJsonWs(s[i])) ++i;
    return i;
}

// 把一个 Unicode 码点追加成 UTF-8。
// （\uXXXX 反转义之后必须重新编码成 UTF-8 —— 内部统一用 UTF-8 字节串，
//   最后一次性交给 Utf8ToWide，不要在解析中途混用宽窄两种表示。）
void AppendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadHex4(const std::string& s, size_t i, unsigned int& out) {
    if (i + 4 > s.size()) return false;
    unsigned int v = 0;
    for (int k = 0; k < 4; ++k) {
        const int h = HexVal(s[i + k]);
        if (h < 0) return false;
        v = (v << 4) | static_cast<unsigned int>(h);
    }
    out = v;
    return true;
}

// ---------------------------------------------------------------------------
// 解析一个 JSON 字符串字面量：s[i] 必须是 '"'。
// 成功时 out 是**已反转义**的 UTF-8 内容，end 指向结束引号的下一格。
//
// 【这里是最容易写错的地方，逐条说明为什么】
//
//  * 结束引号**不能**用 s.find('"', i+1) 找。
//    值内部可能出现被转义的 \" —— 那个引号属于内容，不是结尾。
//    必须逐字符扫描，遇到 '\\' 就把它和**后面那一个字符**一起吞掉，
//    这样才不会把 \n 里的 'n' 或者 \" 里的 '"' 误判。
//    实测 LRCLIB 的真实响应里确实含 \" （歌词里有英文双引号时），
//    所以这不是理论上的洁癖，是必然会踩的坑。
//
//  * 反不反转义不是"锦上添花"，是**能不能用**的区别。
//    LRC 的时间轴靠换行分行，而 JSON 里的换行是字面两字符 "\\n"。
//    直接截取两个引号之间的原文，拿到的是一整行挤在一起、
//    中间夹着 "\n" 字面量的废文本 —— 一行歌词都切不出来。
//
//  * \uXXXX 可能是**代理对**。一个非 BMP 字符（emoji、部分生僻字）
//    在 JSON 里写成 \uD83D\uDE00 这种高低两个 4 位码元，
//    必须合成一个码点再编码；各自单独编码成 UTF-8 会得到非法字节序列。
//    实测 LRCLIB 现在**不用** \u 转义（中文/日文原样发 UTF-8），
//    但这是对方实现的细节、随时可变，所以按标准做全。
//
//  * 不认识的转义（JSON 里本来非法）按"原样收下那个字符"处理，不报错。
//    宁可多留一个字符，也不要因为一个怪字符把整份歌词丢掉。
// ---------------------------------------------------------------------------
bool ParseJsonString(const std::string& s, size_t i, std::string& out, size_t& end) {
    out.clear();
    if (i >= s.size() || s[i] != '"') return false;
    ++i;

    while (i < s.size()) {
        const char c = s[i];

        if (c == '"') {                 // 这才是真正的结束引号
            end = i + 1;
            return true;
        }
        if (c != '\\') {                // 普通字符（含所有 UTF-8 字节）
            out.push_back(c);
            ++i;
            continue;
        }

        // ---- 转义序列 ----
        if (i + 1 >= s.size()) break;   // 响应被截断
        const char e = s[i + 1];

        if (e == 'u') {
            unsigned int cp = 0;
            if (!ReadHex4(s, i + 2, cp)) return false;
            i += 6;

            // 高代理后面紧跟低代理 -> 合成一个码点
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() &&
                s[i] == '\\' && s[i + 1] == 'u') {
                unsigned int lo = 0;
                if (ReadHex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 6;
                }
            }
            AppendUtf8(out, cp);
            continue;
        }

        switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            default:   out.push_back(e);    break;   // 未定义的转义：原样收下
        }
        i += 2;
    }
    return false;   // 没找到收尾引号
}

// ParseJsonString 的便捷包装：原地推进 i。
//
// 【为什么要有这一层】直接写 ParseJsonString(s, i, out, i) 是把同一个 i
// 同时喂给"起始位置"（按值）和"结束位置"（按引用）。C++ 保证所有实参
// 在进入函数体之前求值完毕，所以按值那个拿到的是旧的 i、按引用那个写回
// 调用方的 i，**结果是正确的**。但这种自别名调用读起来像 bug，
// 后人"顺手修一下"或者调整一下参数顺序就会真出问题。
// 这里用一个显式的中间变量把顺序写死，谁看都不用再推理一遍。
bool ParseJsonStringAt(const std::string& s, size_t& i, std::string& out) {
    size_t end = i;
    if (!ParseJsonString(s, i, out, end)) return false;
    i = end;
    return true;
}

// 跳过任意一个 JSON 值。进来时 i 指向值的第一个字符，返回时指向值的后面。
bool SkipValue(const std::string& s, size_t& i) {
    i = SkipWs(s, i);
    if (i >= s.size()) return false;

    const char c = s[i];

    if (c == '"') {
        std::string discard;
        return ParseJsonStringAt(s, i, discard);
    }

    if (c == '{' || c == '[') {
        // 靠配平括号跳过整个容器。
        // 【关键】遇到字符串必须整段跳过去，不能只看括号 ——
        // 歌词内容里出现一个 '}' 或 ']' 字符是家常便饭，
        // 不做字符串感知的配平会提前收尾，后面全乱。
        int depth = 0;
        while (i < s.size()) {
            const char d = s[i];
            if (d == '"') {
                std::string discard;
                if (!ParseJsonStringAt(s, i, discard)) return false;
                continue;
            }
            if (d == '{' || d == '[') { ++depth; ++i; continue; }
            if (d == '}' || d == ']') {
                --depth;
                ++i;
                if (depth == 0) return true;
                continue;
            }
            ++i;
        }
        return false;
    }

    // 数字 / true / false / null：一直吃到分隔符为止
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !IsJsonWs(s[i])) {
        ++i;
    }
    return i > start;
}

// 解析 JSON 数字。
//
// 不用 strtod：strtod 会受 CRT 的 locale 影响（在有些区域设置里
// 小数点是逗号），而 JSON 规定小数点一定是 '.'。
// 自己解析二十来行，换来与 locale 无关的确定行为。
bool ParseJsonNumber(const std::string& tok, double& out) {
    size_t i = 0;
    bool neg = false;
    if (i < tok.size() && (tok[i] == '-' || tok[i] == '+')) {
        neg = (tok[i] == '-');
        ++i;
    }

    double v = 0.0;
    bool any = false;

    while (i < tok.size() && tok[i] >= '0' && tok[i] <= '9') {
        v = v * 10.0 + (tok[i] - '0');
        ++i;
        any = true;
    }
    if (i < tok.size() && tok[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < tok.size() && tok[i] >= '0' && tok[i] <= '9') {
            v += (tok[i] - '0') * scale;
            scale *= 0.1;
            ++i;
            any = true;
        }
    }
    if (!any) return false;

    if (i < tok.size() && (tok[i] == 'e' || tok[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < tok.size() && (tok[i] == '-' || tok[i] == '+')) {
            eneg = (tok[i] == '-');
            ++i;
        }
        int exp = 0;
        bool eany = false;
        while (i < tok.size() && tok[i] >= '0' && tok[i] <= '9') {
            exp = exp * 10 + (tok[i] - '0');
            ++i;
            eany = true;
        }
        if (eany && exp < 300) {
            double p = 1.0;
            for (int k = 0; k < exp; ++k) p *= 10.0;
            v = eneg ? v / p : v * p;
        }
    }

    out = neg ? -v : v;
    return true;
}

// 从 i 开始吃一段"裸 token"（数字/true/false/null）并解析成 double
bool ReadNumberToken(const std::string& s, size_t& i, double& out) {
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !IsJsonWs(s[i])) {
        ++i;
    }
    if (i == start) return false;
    return ParseJsonNumber(s.substr(start, i - start), out);
}

// LRCLIB 一个曲目条目里我们关心的字段。
//
// hasSynced / hasPlain 的含义是"这个字段存在且**非空**" ——
// 裸 null 和空字符串都不算，因为对调用方来说它们等价于"没有"。
struct LrclibEntry {
    bool        hasSynced    = false;
    bool        hasPlain     = false;
    bool        hasId        = false;
    bool        hasDuration  = false;
    bool        instrumental = false;

    // Lyricus 自己的标记：这份结果是从哪个接口来的。
    // 真实 LRCLIB 响应里**没有**这个键（所以解析真实响应时它恒为 false），
    // 它只出现在我们自己写的 .meta 文件里，用来把"这是模糊搜索来的、
    // 调用方必须核对"这件事一起持久化到缓存。
    bool        fromSearch   = false;

    double      id           = 0.0;
    double      duration     = 0.0;
    std::string synced;   // 已反转义的 UTF-8
    std::string plain;

    // 元数据：命中的到底是哪首歌。供调用方核对，也用来读写 .meta。
    // 这几个字段**即使为空也算"存在"** —— 空专辑名是合法值，
    // 和"字段缺失"不是一回事。
    std::string artistName;
    std::string trackName;
    std::string albumName;
};

// 解析**一个** LRCLIB 曲目对象。
// objStart 必须指向 '{'；end 返回指向配对 '}' 的下一格。
//
// 一趟扫完：读 key -> 读 ':' -> 按 key 决定怎么处理 value。
// 顶层遇到不认识的 key 一律 SkipValue 跳过 ——
// LRCLIB 会随版本加字段（实测响应里就有文档没提过的 "name" 键），
// 写死"按顺序读第 N 个值"的代码迟早会读串。
bool ParseEntry(const std::string& s, size_t objStart, LrclibEntry& out, size_t& end) {
    out = LrclibEntry();
    end = objStart;

    size_t i = SkipWs(s, objStart);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;

    for (;;) {
        i = SkipWs(s, i);
        if (i >= s.size()) return false;

        if (s[i] == '}') {          // 对象正常结束
            end = i + 1;
            return true;
        }
        if (s[i] == ',') { ++i; continue; }   // 成员之间的逗号
        if (s[i] != '"') return false;        // 畸形：key 必须是字符串

        std::string key;
        if (!ParseJsonStringAt(s, i, key)) return false;

        i = SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        i = SkipWs(s, i);
        if (i >= s.size()) return false;

        // ---- 字符串字段：syncedLyrics / plainLyrics ----
        //
        // 【裸 null 在这里处理】null 是**不带引号**的字面量。
        // 如果这里不判、直接当字符串去读，会读到 n/u/l/l 四个字符
        // 然后一路错位。所以：是引号才按字符串解析，否则 SkipValue 掉。
        if (key == "syncedLyrics" || key == "plainLyrics") {
            std::string val;
            if (s[i] == '"') {
                if (!ParseJsonStringAt(s, i, val)) return false;
            } else {
                if (!SkipValue(s, i)) return false;    // null 或者别的类型
            }
            if (!val.empty()) {
                if (key == "syncedLyrics") { out.synced = std::move(val); out.hasSynced = true; }
                else                       { out.plain  = std::move(val); out.hasPlain  = true; }
            }
            continue;
        }

        // ---- 字符串字段：元数据 ----
        //
        // 和上面两个歌词字段不同，这里**即使值是空串也要赋值**：
        // 空专辑名是合法的，和"字段缺失"不是一回事 ——
        // 调用方要拿这几个值和本地标签比对，把空串丢掉会让比对结果失真。
        if (key == "artistName" || key == "trackName" || key == "albumName") {
            std::string val;
            if (s[i] == '"') {
                if (!ParseJsonStringAt(s, i, val)) return false;
            } else {
                if (!SkipValue(s, i)) return false;    // 裸 null
            }
            if (key == "artistName")     out.artistName = std::move(val);
            else if (key == "trackName") out.trackName  = std::move(val);
            else                         out.albumName  = std::move(val);
            continue;
        }

        // ---- 数字字段：id / duration ----
        if (key == "id" || key == "duration") {
            if (s[i] == '"') {                 // 万一是字符串形式的数字
                std::string discard;
                if (!ParseJsonStringAt(s, i, discard)) return false;
                continue;
            }
            double v = 0.0;
            if (ReadNumberToken(s, i, v)) {
                if (key == "id") { out.id = v; out.hasId = true; }
                else             { out.duration = v; out.hasDuration = true; }
            }
            continue;
        }

        // ---- 布尔字段：instrumental / fromSearch ----
        // 只认小写的 true；其余（false / null / 怪东西）统一跳过。
        // fromSearch 不是 LRCLIB 的键，是我们自己写进 .meta 的，
        // 解析真实响应时它永远走不到，等价于 false —— 正是我们想要的默认值
        // （真实响应只可能来自 /api/get，那是精确查询）。
        if (key == "instrumental" || key == "fromSearch") {
            bool v = false;
            if (s.compare(i, 4, "true") == 0) {
                v = true;
                i += 4;
            } else if (!SkipValue(s, i)) {
                return false;
            }
            if (key == "instrumental") out.instrumental = v;
            else                       out.fromSearch   = v;
            continue;
        }

        // ---- 其它字段（trackName / artistName / albumName / name / ...）----
        if (!SkipValue(s, i)) return false;
    }
}

// /api/get 的响应体：一个对象。（允许前面有空白。）
bool ParseEntryRoot(const std::string& body, LrclibEntry& out) {
    const size_t start = SkipWs(body, 0);
    size_t end = start;
    return ParseEntry(body, start, out, end);
}

// /api/search 的响应体：一个**数组**，逐个元素解析。
//
// 【数组 vs 对象】这是两个接口最容易混淆的地方：
//   /api/get     -> 一个对象  {...}
//   /api/search  -> 一个数组  [{...},{...}]
// 把数组当对象解析（或者反过来）会直接在这里返回 false。
bool ParseSearchRoot(const std::string& body, std::vector<LrclibEntry>& out) {
    out.clear();

    size_t i = SkipWs(body, 0);
    if (i >= body.size() || body[i] != '[') return false;
    ++i;

    for (;;) {
        i = SkipWs(body, i);
        if (i >= body.size()) return false;

        if (body[i] == ']') return true;        // 数组正常结束
        if (body[i] == ',') { ++i; continue; }

        if (body[i] == '{') {
            LrclibEntry e;
            size_t end = i;
            if (!ParseEntry(body, i, e, end)) return false;
            out.push_back(std::move(e));
            i = end;
            continue;
        }

        // 数组里出现了非对象元素（理论上不该有）：跳过它，别整个失败
        if (!SkipValue(body, i)) return false;
    }
}

// ---------------------------------------------------------------------------
// 网易云音乐 —— 第二个在线源
//
// 【为什么加它】实测用户曲库里大量同人曲 / OST 在 LRCLIB 上**一首都没有**
// （8 次标题搜索 6 次返回 0 条），而网易云连「塞壬唱片-MSR - Battleplan
// Obliteration」这种都能精确命中。所以查询顺序是 本地 -> 网易云 -> LRCLIB。
//
// 【接口是非官方的】见 kNetEaseHost 的说明。
//
// 响应形状（实测，2026-09-24）：
//   搜索  https://music.163.com/api/search/get/web?s=<kw>&type=1&limit=10
//         {"result":{"songs":[{...}],"songCount":N},"code":200}
//         每首歌里我们要的：id / name / duration(毫秒!) / artists[].name / album.name
//   歌词  https://music.163.com/api/song/lyric?id=<id>&lv=1&kv=1&tv=-1
//         {"sgc":false,"sfy":false,"qfy":false,
//          "lrc":{"version":..,"lyric":"[00:00.000] ..."},
//          "klyric":{...},"tlyric":{...},"code":200}
//         tlyric 是翻译 —— 这次不用（面板只有 2~3 行，双语会把可见歌词砍半），
//         但字段留着，将来要做双语直接取。
// ---------------------------------------------------------------------------

// 网易云搜索返回的一首歌（只留做匹配要用的字段）。
struct NetEaseSong {
    bool        hasId      = false;
    double      id         = 0.0;
    double      durationMs = 0.0;   // ⚠️ 毫秒，不是秒 —— 换算是调用方的事
    std::string name;               // 曲名
    std::string artist;             // 演唱者，多个用 "/" 连接
    std::string album;
};

// 走一个 JSON 对象，取出指定字符串键的值；其它键一律 SkipValue 跳过。
// 网易云响应里的嵌套对象（album / lrc）都靠它。
//
// 【进入时先清空 out】键缺失、或者值是裸 null 时，这个函数**不会写 out** ——
// 不清空的话调用方拿到的是缓冲里上一次的残留。现在调用点用的都是新变量
// 所以看不出来，但这属于"靠运气才对"的契约，早晚会咬人
// （单测里给 out 预置了哨兵字符串，当场就抓出来了）。
bool ParseJsonStringFieldInObject(const std::string& s, size_t i,
                                  const char* wanted, std::string& out, size_t& end) {
    out.clear();
    i = SkipWs(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;

    for (;;) {
        i = SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '}') { end = i + 1; return true; }
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '"') return false;

        std::string key;
        if (!ParseJsonStringAt(s, i, key)) return false;

        i = SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        i = SkipWs(s, i);
        if (i >= s.size()) return false;

        if (key == wanted && s[i] == '"') {
            // 裸 null 也走这里：不是引号就 SkipValue 掉（同 ParseEntry 的处理）
            if (!ParseJsonStringAt(s, i, out)) return false;
            continue;
        }
        if (!SkipValue(s, i)) return false;
    }
}

// artists 是**数组**：[{"id":..,"name":".."}, ...]，把每个 name 拼起来。
bool ParseNetEaseArtists(const std::string& s, size_t i, std::string& out, size_t& end) {
    i = SkipWs(s, i);
    if (i >= s.size() || s[i] != '[') return false;
    ++i;

    for (;;) {
        i = SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == ']') { end = i + 1; return true; }
        if (s[i] == ',') { ++i; continue; }

        if (s[i] == '{') {
            std::string nm;
            size_t e = i;
            if (!ParseJsonStringFieldInObject(s, i, "name", nm, e)) return false;
            if (!nm.empty()) {
                if (!out.empty()) out += "/";
                out += nm;
            }
            i = e;
            continue;
        }
        if (!SkipValue(s, i)) return false;
    }
}

// 解析搜索结果里的一首歌。objStart 指向 '{'。
bool ParseNetEaseSong(const std::string& s, size_t objStart, NetEaseSong& out, size_t& end) {
    size_t i = SkipWs(s, objStart);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;

    for (;;) {
        i = SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '}') { end = i + 1; return true; }
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '"') return false;

        std::string key;
        if (!ParseJsonStringAt(s, i, key)) return false;

        i = SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        i = SkipWs(s, i);
        if (i >= s.size()) return false;

        if (key == "name") {
            if (s[i] == '"') { if (!ParseJsonStringAt(s, i, out.name)) return false; }
            else if (!SkipValue(s, i)) return false;
            continue;
        }
        if (key == "id" || key == "duration") {
            double v = 0.0;
            if (ReadNumberToken(s, i, v)) {
                if (key == "id") { out.id = v; out.hasId = true; }
                else             { out.durationMs = v; }
            }
            continue;
        }
        if (key == "artists") {
            if (s[i] == '[') {
                size_t e = i;
                if (!ParseNetEaseArtists(s, i, out.artist, e)) return false;
                i = e;
            } else if (!SkipValue(s, i)) return false;
            continue;
        }
        if (key == "album") {
            if (s[i] == '{') {
                size_t e = i;
                if (!ParseJsonStringFieldInObject(s, i, "name", out.album, e)) return false;
                i = e;
            } else if (!SkipValue(s, i)) return false;
            continue;
        }
        if (!SkipValue(s, i)) return false;
    }
}

// 搜索响应体：{"result":{"songs":[...]},"code":200}
//
// 定位 "songs":[ 再逐个对象解析，而不是从头走一遍完整 JSON ——
// 我们只要这一个数组，而它埋在两三层嵌套里。
bool ParseNetEaseSearchRoot(const std::string& body, std::vector<NetEaseSong>& out) {
    out.clear();

    static const char kSongsKey[] = "\"songs\"";
    const size_t k = body.find(kSongsKey);
    if (k == std::string::npos) return false;   // 无结果时网易云根本不带这个键

    size_t i = SkipWs(body, k + sizeof(kSongsKey) - 1);
    if (i >= body.size() || body[i] != ':') return false;
    ++i;
    i = SkipWs(body, i);
    if (i >= body.size() || body[i] != '[') return false;
    ++i;

    for (;;) {
        i = SkipWs(body, i);
        if (i >= body.size()) return false;
        if (body[i] == ']') return true;
        if (body[i] == ',') { ++i; continue; }

        if (body[i] == '{') {
            NetEaseSong song;
            size_t e = i;
            if (!ParseNetEaseSong(body, i, song, e)) return false;
            out.push_back(std::move(song));
            i = e;
            continue;
        }
        if (!SkipValue(body, i)) return false;
    }
}

// 歌词响应体：取 lrc.lyric（原文）。outTranslation 非空时顺便取 tlyric.lyric（翻译）。
//
// 【为什么翻译要单独取】tlyric 的**行数和原文不一定一样**（实测《夜に駆ける》：
// 原文 64 行、翻译 60 行），所以它不能按行号对齐，只能按**时间戳**合并。
// 合并成什么样见 MergeTranslationLines。
bool ParseNetEaseLyricRoot(const std::string& body, std::string& out,
                           std::string* outTranslation = nullptr) {
    static const char kLrcKey[] = "\"lrc\"";
    const size_t k = body.find(kLrcKey);
    if (k == std::string::npos) return false;

    size_t i = SkipWs(body, k + sizeof(kLrcKey) - 1);
    if (i >= body.size() || body[i] != ':') return false;
    ++i;

    size_t end = i;
    if (!ParseJsonStringFieldInObject(body, i, "lyric", out, end)) return false;

    if (outTranslation != nullptr) {
        outTranslation->clear();
        static const char kTlKey[] = "\"tlyric\"";
        const size_t t = body.find(kTlKey);
        if (t != std::string::npos) {
            size_t j = SkipWs(body, t + sizeof(kTlKey) - 1);
            if (j < body.size() && body[j] == ':') {
                ++j;
                size_t e2 = j;
                // 取不到（没有翻译、或者是裸 null）就留空，不算失败
                ParseJsonStringFieldInObject(body, j, "lyric", *outTranslation, e2);
            }
        }
    }
    return true;
}

// 前置声明：定义在文件更靠后的「前奏分隔线」那一节。
// 那里是为了解析 LRC 时间戳写的 —— 这里合并翻译要用**同一个**，不另写一套
//（两套解析迟早会在某个畸形时间戳上给出不同答案）。
bool ToDoubleAscii(const std::string& s, double& out);

// 从一行 LRC 里取出起始时间戳（秒）。取不到返回 -1。
double LineTimestampSec(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != '[') return -1.0;

    const size_t close = line.find(']', i);
    if (close == std::string::npos) return -1.0;

    const std::string tag = line.substr(i + 1, close - i - 1);
    const size_t colon = tag.find(':');
    if (colon == std::string::npos) return -1.0;

    double mm = 0.0, ss = 0.0;
    if (!ToDoubleAscii(tag.substr(0, colon), mm)) return -1.0;
    if (!ToDoubleAscii(tag.substr(colon + 1), ss)) return -1.0;
    return mm * 60.0 + ss;
}

// 把翻译**按时间戳**合并进原文：翻译行紧跟在同时间戳的原文行之后。
//
// 合并后的形状就是通行的双语 LRC 写法 —— 同一条时间戳出现两行，原文在前：
//     [00:56.848]寂しい目をしてたんだ
//     [00:56.848]眼神却显得如此寂寞
//
// 【为什么是"追加同时间戳的行"而不是给 LyricLine 加字段】
// 解析器（lyric.cpp）**一行都不用动** —— 两行都是普通的 LRC 行，它照单全收。
// 想给它加字段就得改解析器，而那是全工程共用的（D-038 那次一改就打挂 26 条断言）。
// "这两行是一对"这件事由**用的人**判断：LyricDocument::LineIndexAt 返回
// 同时间戳组的**第一行**，渲染层则把同组的多行画成"原文 + 小字参照行"。
//
// ⚠️ 时间戳对不上的翻译行**直接丢掉** —— 宁可少一行翻译，
//    也不要让它对到错的原文上去（差几百毫秒还能忍，差几句就是错词）。
std::string MergeTranslationLines(const std::string& lrc, const std::string& translation) {
    if (lrc.empty() || translation.empty()) return lrc;

    // 原文里有那些时间戳
    std::vector<double> stamps;
    {
        size_t pos = 0;
        while (pos <= lrc.size()) {
            size_t nl = lrc.find('\n', pos);
            if (nl == std::string::npos) nl = lrc.size();
            const double t = LineTimestampSec(lrc.substr(pos, nl - pos));
            if (t >= 0.0) stamps.push_back(t);
            if (nl >= lrc.size()) break;
            pos = nl + 1;
        }
    }
    if (stamps.empty()) return lrc;

    std::string out;
    out.reserve(lrc.size() + translation.size());
    size_t pos = 0;
    size_t merged = 0;

    // ⚠️ 循环条件是 `<` 而不是 `<=`，而且只在**原本有换行**时才补换行。
    //    用 `<=` 的话，输入结尾的 \n 之后还会再走一轮空行，
    //    输出就比输入多一个 \n —— 对解析无害（空行本来就丢），
    //    但"没合并任何东西时就该逐字节原样返回"是应该守住的，
    //    否则任何一个"译文对不上"的输入都会悄悄改变原文。
    //    （这条是被单测「时间戳对不上的翻译被丢弃」抓出来的。）
    while (pos < lrc.size()) {
        size_t nl = lrc.find('\n', pos);
        const bool hasNl = (nl != std::string::npos);
        if (!hasNl) nl = lrc.size();
        const std::string line = lrc.substr(pos, nl - pos);
        out += line;
        if (hasNl) out += '\n';

        // 这一行的翻译跟在它后面
        const double t = LineTimestampSec(line);
        if (t >= 0.0) {
            size_t tp = 0;
            while (tp < translation.size()) {
                size_t tnl = translation.find('\n', tp);
                if (tnl == std::string::npos) tnl = translation.size();
                const std::string tl = translation.substr(tp, tnl - tp);

                const double tt = LineTimestampSec(tl);
                if (tt >= 0.0 && std::fabs(tt - t) < 0.01) {
                    // 用原文那行的时间戳文本，保证两行完全同戳
                    const size_t close = line.find(']');
                    const size_t tlClose = tl.find(']');
                    if (close != std::string::npos && tlClose != std::string::npos) {
                        out += line.substr(0, close + 1);
                        out += tl.substr(tlClose + 1);
                        out += '\n';
                        ++merged;
                    }
                }
                if (tnl >= translation.size()) break;
                tp = tnl + 1;
            }
        }

        if (!hasNl) break;
        pos = nl + 1;
    }

    if (merged > 0) {
        OnlineLog("在线歌词：网易云 —— 合并了 %zu 行翻译（同时间戳）", merged);
    }
    return out;
}

// 剥掉网易云 LRC **开头**那段带时间戳的制作人员名单。
//
// 实测（拿用户曲库里的真歌打的）：
//     [00:00.000] 音乐设计/监制 : MSR Studio
//     [00:20.250]鼓：陈柏州
//     [00:22.510]录音助理：刘勇志
//     [00:29.260]故事的小黄花        ← 真歌词从这儿才开始
// 不处理的话，面板上会滚过一串「混音：某某」。
//
// 【判据保守到几乎不可能误伤】一行要被剥掉，必须**同时**满足：
//   * 行首是 [mm:ss.xx]（可以连几个时间戳）；
//   * 去掉时间戳后形如「词：值」，冒号（半角/全角都认）左边的词 ≤ 10 个字符；
//   * 那个词命中下面的小词表（忽略大小写和空格）。
// 而且**一旦遇到第一行不满足的就立刻停手**，后面原样保留。
// 所以最坏情况是漏剥一行名单，不可能吃掉真歌词 ——
// 这是刻意的取舍：名单多显示一行只是难看，歌词少一行是错。
bool IsNetEaseCreditLine(const std::string& raw) {
    static const char* kKeys[] = {
        // 中文
        "作词", "作曲", "编曲", "制作", "制作人", "监制", "混音", "录音", "母带",
        "吉他", "贝斯", "鼓", "键盘", "和声", "弦乐", "出品", "发行", "统筹",
        "企划", "设计", "插画", "配唱", "录音室", "混音室", "母带室", "词", "曲",
        "OP", "SP", "人声", "演唱", "演奏", "合声", "编写", "助理",
        // 英文（网易云偶尔用英文写名单）
        "producer", "mixing", "mix", "recording", "mastering", "guitar", "bass",
        "drums", "keys", "vocal", "vocals", "composer", "lyricist", "arranger",
        "studio", "written", "music",
    };

    size_t i = 0;
    // 跳过行首空白
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r')) ++i;

    // 跳过**连续的**时间戳 [..]
    size_t stamps = 0;
    while (i < raw.size() && raw[i] == '[') {
        const size_t close = raw.find(']', i);
        if (close == std::string::npos) return false;
        i = close + 1;
        ++stamps;
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
    }
    if (stamps == 0) return false;

    // 冒号左边那段
    size_t colon = std::string::npos;
    for (size_t j = i; j < raw.size(); ++j) {
        if (raw[j] == ':' || raw[j] == '\xEF') {   // 0xEF 是全角冒号 UTF-8 的首字节
            if (raw[j] == '\xEF') {
                if (raw.compare(j, 3, "\xEF\xBC\x9A") != 0) continue;
            }
            colon = j;
            break;
        }
    }
    if (colon == std::string::npos) return false;

    std::string key = raw.substr(i, colon - i);
    // 去掉首尾空白与结尾的空格（「制作 : 某某」这种写法）
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    size_t b = 0;
    while (b < key.size() && (key[b] == ' ' || key[b] == '\t')) ++b;
    key.erase(0, b);

    // 左边必须短 —— 真歌词里第一个冒号通常在更长的一段文字之后
    if (key.empty() || key.size() > 24) return false;   // 中文 3 字节/字，24 字节 ≈ 8 字

    // 匹配方式分两档，这是被真实数据逼出来的：
    //
    //   长词（≥ 2 个汉字）用**包含**匹配。名单里的写法五花八门 ——
    //   实测就撞到过「混音工程」「录音助理」「录音工程」，
    //   而词表里只有「混音」「录音」。全等匹配会漏掉一大半。
    //
    //   短词（「词」「曲」「OP」「SP」以及英文缩写）必须**全等**。
    //   包含匹配会让「词」命中「歌词」「戏曲」这类，误伤面太大。
    for (const char* k : kKeys) {
        const size_t klen = std::strlen(k);
        if (klen < 6) {                       // 短词：全等
            if (key.size() != klen) continue;
        } else {                              // 长词：包含
            if (key.size() < klen) continue;
        }

        bool same = false;
        if (klen < 6 && key.size() == klen) {
            same = true;
            for (size_t j = 0; j < key.size(); ++j) {
                char a = key[j], c = k[j];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (a != c) { same = false; break; }
            }
        } else if (klen >= 6) {
            for (size_t start = 0; start + klen <= key.size() && !same; ++start) {
                same = true;
                for (size_t j = 0; j < klen; ++j) {
                    char a = key[start + j], c = k[j];
                    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    if (a != c) { same = false; break; }
                }
            }
        }
        if (same) return true;
    }
    return false;
}

// 剥掉名单后补在开头的那条分隔线。
//
// 用户 2026-09-24 定的形态：原来提的是"加一行空歌词"，
// 后来改成"换成分隔线" —— 因为空行要多改一处解析器，而那是全工程共用的，
// 随便动有解析风险。分隔线是一行普通的 LRC，解析器原样接受。
//
// 【想换样式就改这里一行】候选：`———`、`♪`、`· · ·`、`┈┈┈`。
// ⚠️ 用 WideToUtf8 而不是直接写窄字符串字面量：后者对不对取决于工程的
//    源码/执行字符集设置（/utf-8），换个构建配置就可能变成乱码。
//    走一次转换就与那些设置无关了。
const std::string& NetEaseIntroSeparator() {
    static const std::string s = WideToUtf8(L"———");
    return s;
}

std::string StripNetEaseCredits(const std::string& lrc) {
    std::string out;
    out.reserve(lrc.size() + 64);

    bool stripping = true;
    int  removed   = 0;
    std::string firstStamp;   // 第一行被剥掉的那个时间戳，用来放分隔线
    size_t pos = 0;

    while (pos <= lrc.size()) {
        size_t nl = lrc.find('\n', pos);
        if (nl == std::string::npos) nl = lrc.size();
        const std::string line = lrc.substr(pos, nl - pos);

        bool drop = false;
        if (stripping) {
            if (IsNetEaseCreditLine(line)) {
                if (removed == 0) {
                    // 记住第一个被剥掉的时间戳（形如 "[00:00.000]"）。
                    // 后面要在**同一个时刻**放一行分隔线，把时间轴补齐 ——
                    // 见下面那段说明。
                    const size_t open  = line.find('[');
                    const size_t close = (open == std::string::npos)
                                             ? std::string::npos : line.find(']', open);
                    if (close != std::string::npos) {
                        firstStamp = line.substr(open, close - open + 1);
                    }
                }
                drop = true;
                ++removed;
            } else {
                stripping = false;   // 第一行不像名单 —— 后面全部原样保留
            }
        }
        if (!drop) { out += line; out += '\n'; }

        if (nl >= lrc.size()) break;
        pos = nl + 1;
    }

    if (removed > 0) {
        OnlineLog("在线歌词：网易云 —— 剥掉了开头 %d 行制作人员名单", removed);
    }

    // 判空：整篇都是名单的条目（纯音乐，网易云上很常见）当"没有歌词"处理。
    //
    // 【分隔线**不在这里**补】那是**显示层**的修补，放在取词的统一出口
    // （EnsureLeadInSeparator）—— 放这里只能覆盖"本次新取到的词"，
    // 缓存里已经存下的 .lrc 根本不会再走这段代码。用户实测就撞上了这个：
    // 「缓存到本地的歌词还没有第一行的分隔线」。
    bool hasContent = false;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] != '[' && out[i] != ']' && out[i] != ':' && out[i] != '.' &&
            out[i] != '\n' && out[i] != '\r' && out[i] != ' ' && out[i] != '\t' &&
            !(out[i] >= '0' && out[i] <= '9')) { hasContent = true; break; }
    }
    return hasContent ? out : std::string();
}

// 取网易云响应体里的业务错误码。
//
// ===========================================================================
//  ⚠️ 网易云把错误码放在**响应体**里，HTTP 状态码仍然是 200
// ===========================================================================
//
// 实测（2026-09-24，被自己的探测打到限流）：
//     HTTP/1.1 200 OK
//     {"msg":"操作频繁，请稍候再试","code":405,"message":"操作频繁，请稍候再试"}
//
// 只看 HTTP 状态码的话，这会被当成「查到了，但确实没有结果」——
// 于是：搜索结果为空 -> 转去 LRCLIB -> 也空 -> **写下 7 天有效的"没有"标记**。
// 用户那批「还是没识别出来」里就有 4 次是这么来的（日志里
// 「搜索无结果（响应里没有 songs 数组）」正是限流的签名）。
//
// 而且我们的重试是基于 HTTP 状态码做的（HttpGet 里的 ShouldRetry），
// 所以 405 **永远不会被重试**。
//
// 返回码：找不到就是 -1（当作"形状不对"，交给调用方决定）。
int ParseNetEaseCode(const std::string& body) {
    // 两个响应（搜索 / 歌词）的 "code" 都在**最后**一个键的位置，
    // 用 rfind 避开嵌套对象里可能同名的字段。
    static const char kKey[] = "\"code\"";
    const size_t k = body.rfind(kKey);
    if (k == std::string::npos) return -1;

    size_t i = SkipWs(body, k + sizeof(kKey) - 1);
    if (i >= body.size() || body[i] != ':') return -1;
    ++i;
    i = SkipWs(body, i);

    double v = 0.0;
    if (!ReadNumberToken(body, i, v)) return -1;
    return static_cast<int>(v);
}

// 网易云的这个业务码值不值得重试。
//
//   405 —— 限流（实测的形态：HTTP 200 + {"code":405,"msg":"操作频繁"}），
//          退避一下通常就能过，**必须**重试；
//   5xx —— 服务器侧问题，也值得再试一次；
//   其它（400 参数错、404 之类）—— 重试多少次都一样，别浪费时间。
//
// 但**无论哪个码**，都不能当成"这首歌没有歌词"（见 ParseNetEaseCode）。
bool IsNetEaseRetryableCode(int code) {
    return code == 405 || (code >= 500 && code < 600);
}

// 网易云限流的**进程级冷却**。
//
// ===========================================================================
//  撞到墙就别再撞 —— 这是被真实日志逼出来的
// ===========================================================================
//
// 实测（2026-09-24）：一旦被限流，405 会**持续好几分钟**。而我们的重试是
// 800/1600/2500ms —— 在几分钟的封禁窗口里等于白等 5 秒，然后才转去 LRCLIB。
// 日志里一次换曲白跑了 9.5 秒后台，全都是注定失败的请求。
//
// 为什么会触发限流：我为了看候选连打了三十几次搜索接口（教训见 D-036），
// 加上用户快速连点切歌。总之它会发生，插件就该学会停手。
//
// 行为：撞到限流就记下"封禁到这个时刻"，期间**直接跳过网易云**
// （不发请求、不重试），转去 LRCLIB。连续撞到就指数加长，上限 10 分钟。
// 冷却期间**绝不**写未命中标记 —— 那是"没问出来"，不是"没有"。
//
// 用 atomic 是因为查询跑在后台线程（fb2k::splitTask），可能同时有好几个。
std::atomic<ULONGLONG> g_netEaseBlockedUntil{0};
std::atomic<int>       g_netEaseBlockStreak{0};
std::atomic<bool>      g_netEaseSkipLogged{false};

constexpr ULONGLONG kNetEaseCooldownBaseMs = 60u * 1000u;      // 首次冷却 1 分钟
constexpr ULONGLONG kNetEaseCooldownMaxMs  = 10u * 60u * 1000u; // 上限 10 分钟

void NetEaseEnterCooldown() {
    const int streak = g_netEaseBlockStreak.fetch_add(1) + 1;

    ULONGLONG ms = kNetEaseCooldownBaseMs;
    for (int i = 1; i < streak && ms < kNetEaseCooldownMaxMs; ++i) ms *= 2;
    if (ms > kNetEaseCooldownMaxMs) ms = kNetEaseCooldownMaxMs;

    g_netEaseBlockedUntil.store(GetTickCount64() + ms);
    g_netEaseSkipLogged.store(false);
    OnlineLog("在线歌词：网易云限流 —— 冷却 %llu 秒后再试（连续第 %d 次）",
              ms / 1000, streak);
}

// 冷却期内的最短重试间隔（毫秒）。**不是**平均间隔的限速，
// 只是防止同一瞬间挤进好几个请求。
constexpr ULONGLONG kNetEaseMinIntervalMs = 1200;
std::atomic<ULONGLONG> g_netEaseLastRequestTick{0};

// 在若干候选里挑一份歌词。
//
// 规则是**两趟**，不是一趟：
//   第一趟找第一个真的有 syncedLyrics 的；
//   一个都没有，第二趟才退而求其次找第一个有 plainLyrics 的。
//
// 【为什么不能一趟搞定】一趟写出来必然是"第一个（有 synced **或** 有 plain）的"，
// 于是当数组第 0 项只有纯文本、第 1 项才有带时间轴的 LRC 时，
// 我们会拿到**差的那一份**（没有时间轴，不能跟着唱）。
// 这种 bug 在大部分曲目上看不出来 —— 因为多数候选项都有 syncedLyrics，
// 只有少数曲目才会踩到，非常难查。
const LrclibEntry* PickBest(const std::vector<LrclibEntry>& v) {
    for (const auto& e : v) {
        if (e.hasSynced) return &e;
    }
    for (const auto& e : v) {
        if (e.hasPlain) return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 缓存条目的元数据文件（.meta）
//
// 【为什么不能只存 .lrc】
// /api/search 是模糊接口，可能返回**完全不相干**的歌
// （实测搜 "Best Wishes" 回来的是 Duane Betts 的同名曲）。
// 所以命中结果必须带上"这到底是哪首歌"，让调用方能和本地标签核对。
// 这个信息如果不落盘，缓存命中时调用方就无从核对，
// 于是"第一次查错了"会变成"以后每次都错"，而且再也纠不回来。
//
// 【为什么用独立的 .meta 而不是塞进 .lrc 里】
// .lrc 是要保持"和 LRCLIB 给的一模一样"的原文（缓存文件用户可能直接
// 拷进播放器用），往里插我们自己的记号就把这个契约破坏了。
// 元数据是我们自己的账，记在自己的文件里。
//
// 【格式】就是一个 JSON 对象，键名沿用 LRCLIB 的字段名
// （artistName / trackName / albumName / duration / id），
// 外加一个我们自己加的 fromSearch。
// 这样读回来时能**直接复用**验证过的 ParseEntryRoot，
// 不用再为"我们自己的格式"写第二套解析器 —— 少一套解析器就少一类 bug。
//
// 【缓存条目的完整性】.lrc 和 .meta 必须**同时**存在才算命中：
// 缺任何一个都当缓存无效、删掉重新联网取。
// 这样就不会出现"有歌词但没有元数据、调用方无法核对"的中间状态。
// ---------------------------------------------------------------------------

// 把一个 UTF-8 字节串写成 JSON 字符串字面量（含两侧引号）。
//
// 只转义 JSON 规定必须转义的那几个字符；>= 0x80 的字节原样放行 ——
// 整个文档就是 UTF-8，本来就不需要 \uXXXX。
// （和对方发给我们的形状一致，我们自己的解析器也照样认。）
void AppendJsonString(std::string& out, const std::string& utf8) {
    out.push_back('"');
    for (char ch : utf8) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // 其余控制字符没有短转义，只能写成 \u00XX
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
}

// 把条目的元数据序列化成 JSON 对象文本。
// duration 只在响应里真的有的时候才写数字，否则写 null ——
// 写 0 会让人分不清"响应里是 0 秒"和"响应里没有这个字段"。
std::string BuildMetaJson(const LrclibEntry& e, bool fromSearch) {
    std::string out = "{";
    out += "\"artistName\":";
    AppendJsonString(out, e.artistName);
    out += ",\"trackName\":";
    AppendJsonString(out, e.trackName);
    out += ",\"albumName\":";
    AppendJsonString(out, e.albumName);
    out += ",\"duration\":";
    if (e.hasDuration) {
        char buf[64];
        sprintf_s(buf, "%.3f", e.duration);
        out += buf;
    } else {
        out += "null";
    }
    out += ",\"id\":";
    if (e.hasId) {
        char buf[64];
        sprintf_s(buf, "%.0f", e.id);
        out += buf;
    } else {
        out += "null";
    }
    out += fromSearch ? ",\"fromSearch\":true" : ",\"fromSearch\":false";
    out += "}";
    return out;
}

bool WriteMetaFile(const std::wstring& path, const LrclibEntry& e, bool fromSearch) {
    const std::string text = BuildMetaJson(e, fromSearch);
    return WriteWholeFile(path, text.data(), text.size());
}

// 读回元数据。读不出来就返回 false，调用方据此把整个缓存条目判为无效。
bool ReadMetaFile(const std::wstring& path, LrclibEntry& out, bool& fromSearch) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;

    // 复用 LRCLIB 那套解析：.meta 的键名和响应里的字段名是同一套
    if (!ParseEntryRoot(text, out)) return false;

    fromSearch = out.fromSearch;
    return true;
}

// ---------------------------------------------------------------------------
// URL 构造
// ---------------------------------------------------------------------------

// URL 查询参数的百分号编码。
//
// 【为什么必须做】URL 里只允许出现 ASCII 的一个很小的子集。
// 中文曲名原样（哪怕是 UTF-8 字节）塞进去，LRCLIB 直接回 **400**。
//
// 【顺序也不能错】必须**先**把宽字符转成 UTF-8，**再**逐字节编码。
// 反过来（按 wchar_t 逐字符编码）在非 BMP 字符上会算出错误的码元，
// 而且中文会被按 UTF-16 码元拆开，编出来的东西对方认不出来。
//
// 保留字符取最保守的做法：只放行 RFC 3986 的 unreserved
// （A-Z a-z 0-9 - _ . ~），其余全部编码。
// 空格编成 %20 而**不是** '+'：'+' 在查询串里语义上等价于空格是
// HTML 表单那一套的约定，LRCLIB 的解析器按字面加号处理，搜不到东西。
std::wstring PercentEncode(const std::wstring& raw) {
    const std::string utf8 = WideToUtf8(raw);
    static const char kHex[] = "0123456789ABCDEF";

    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (char ch : utf8) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';

        if (unreserved) {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(kHex[c >> 4]));
            out.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
        }
    }
    return out;
}

// duration 参数的格式化。
// 手工拼字符串而不用 swprintf 的 %f：同 ParseJsonNumber 的理由，
// %f 会受 CRT locale 的小数点影响，编出 "239,000" 这种对方解析不了的东西。
// 只在 durationSec > 0 时才会被调用（空值会 400，见 BuildExactPath）。
std::wstring FormatDurationParam(double sec) {
    if (sec < 0.0) sec = 0.0;
    const long long ms = static_cast<long long>(sec * 1000.0 + 0.5);
    wchar_t buf[64];
    swprintf_s(buf, L"%lld.%03lld", ms / 1000, ms % 1000);
    return buf;
}

// /api/get 的"路径+查询串"。
//
// 【参数的有无都是有讲究的，全是实测结论】
//   * artist_name / track_name 缺一不可，少哪个都直接回 400。
//     （所以调用方要先判空，见 FetchLyricOnline 里的分支。）
//   * album_name 可以整个不带（实测不带照样 200），空的时候干脆省掉，
//     比传一个空串干净。
//   * duration：**空值会 400**。
//     "duration=" 和 "duration=abc" 实测都是 400。
//     所以时长未知时**必须把整个参数去掉**，绝不能顺手拼一个
//     "&duration=" 上去 —— 那样这个请求就永远 400，而且日志里只会看到
//     "HTTP 400"，很难想到是这里的问题。
std::wstring BuildExactPath(const OnlineLyricRequest& req) {
    std::wstring path = L"/api/get?artist_name=" + PercentEncode(TrimWs(req.artist)) +
                        L"&track_name=" + PercentEncode(TrimWs(req.title));

    const std::wstring album = TrimWs(req.album);
    if (!album.empty()) {
        path += L"&album_name=" + PercentEncode(album);
    }
    if (req.durationSec > 0.0) {
        path += L"&duration=" + FormatDurationParam(req.durationSec);
    }
    return path;
}

// /api/search 的"路径+查询串"。
//
// 关键词用 "演唱者 曲名"：LRCLIB 的搜索是子串匹配，
// 两个词一起给，命中率比只给曲名明显高（只给曲名会搜出一堆翻唱和同名曲）。
// 只有曲名时就只给曲名。
std::wstring BuildSearchPath(const OnlineLyricRequest& req) {
    std::wstring kw = TrimWs(req.artist);
    const std::wstring title = TrimWs(req.title);
    if (!kw.empty() && !title.empty()) kw += L' ';
    kw += title;
    return L"/api/search?q=" + PercentEncode(kw);
}

// ---------------------------------------------------------------------------
// WinHTTP
// ---------------------------------------------------------------------------

// 把 WinHTTP 的错误码翻译成人能读的中文。
// 光看 "错误码 12007" 是查不出问题的，而这几条恰好是网络层最常见的失败。
std::wstring DescribeWinHttpError(DWORD e, const wchar_t* host = kHost) {
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT:                  return L"请求超时";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:        return L"域名解析失败（DNS 查不到 " + std::wstring(host) + L"）";
        case ERROR_WINHTTP_CANNOT_CONNECT:           return L"无法连接到服务器（端口被挡或对方不可达）";
        case ERROR_WINHTTP_CONNECTION_ERROR:         return L"连接被中断";
        case ERROR_WINHTTP_SECURE_FAILURE:           return L"TLS 握手或证书校验失败";
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:  return L"服务器要求客户端证书";
        case ERROR_WINHTTP_INVALID_URL:              return L"URL 非法（可能是查询串太长或含非法字符）";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:      return L"无法识别的 URL 协议";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:  return L"服务器响应无法解析";
        default: break;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"WinHTTP 错误码 %lu", e);
    return buf;
}

// WinHTTP 句柄的 RAII 包装。
//
// 【为什么非要包一层】WinHTTP 的句柄链是 session -> connect -> request，
// 中途每一步都可能失败返回 NULL；而且查询过程中还有多处提前 return
// （超时、状态码不对、响应过大……）。手工在每个 return 前面写一遍
// WinHttpCloseHandle，迟早会漏一个 —— 漏掉的后果是句柄泄漏，
// 而这个函数是会被反复调用的（每首歌一次），泄漏会累积。
// 有了析构函数，"所有路径都关"就是编译器保证的，不用靠人记。
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : m_h(h) {}

    ~WinHttpHandle() { reset(); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : m_h(other.m_h) { other.m_h = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_h = other.m_h;
            other.m_h = nullptr;
        }
        return *this;
    }

    HINTERNET get() const { return m_h; }
    explicit operator bool() const { return m_h != nullptr; }

    void reset(HINTERNET h = nullptr) {
        if (m_h != nullptr) {
            WinHttpCloseHandle(m_h);
            m_h = nullptr;
        }
        m_h = h;
    }

private:
    HINTERNET m_h = nullptr;
};

struct HttpReply {
    bool         transportOk = false;   // 传输层成功（拿到了 HTTP 响应，**不论状态码**）
    DWORD        status      = 0;
    std::string  body;                  // 原始字节（LRCLIB 回的是 UTF-8）
    std::wstring error;                 // 传输层失败的中文原因
    int          attempts    = 0;       // 实际尝试了几次（含重试）
    bool         retryable   = true;    // 重试有没有意义（见 HttpGet 的说明）
};

// 一次 HTTPS GET，**不重试**。重试策略在外面那层（HttpGet）。
//
// 【HTTPS 到底是怎么定下来的 —— 这里最容易想歪】
// WinHttpOpenRequest 的签名是
//     WinHttpOpenRequest(hConnect, pwszVerb, pwszObjectName, pwszVersion,
//                        pwszReferrer, ppwszAcceptTypes, dwFlags)
// **没有 scheme 参数**。协议是由下面两件事共同决定的：
//     1) WinHttpConnect 时用的端口 —— 这里用 INTERNET_DEFAULT_HTTPS_PORT(443)；
//     2) WinHttpOpenRequest 的 dwFlags 里带 WINHTTP_FLAG_SECURE。
// 少任何一个都会退化成明文 HTTP（443 端口上走明文会直接失败），
// 或者发到 80 端口去。所以这两处必须成对出现。
// （字符串 L"https" 本身只会出现在 WinHttpCrackUrl 解析 URL 的场景里，
//   本模块是自己拼 host + path 的，用不上它。）
//
// 另外注意 pwszObjectName **只能**是 /path?query，
// 不能把 scheme 或 host 拼进去 —— 那会让 WinHTTP 拼出一个非法 URL。
HttpReply HttpGetOnce(const std::wstring& host, const std::wstring& pathAndQuery) {
    HttpReply reply;
    reply.attempts = 1;

    // 出错信息里的主机名要跟着实际请求走 —— 否则查网易云失败时
    // 报"DNS 查不到 lrclib.net"，会把排查方向整个带偏。
    auto describe = [&host](DWORD e) { return DescribeWinHttpError(e, host.c_str()); };

    if (pathAndQuery.size() > kMaxUrlChars) {
        reply.error = L"URL 太长（" + std::to_wstring(pathAndQuery.size()) +
                      L" 字符），已放弃本次请求";
        reply.retryable = false;   // 同样的参数重试多少次都还是太长
        return reply;
    }

    // ---- session ----
    // User-Agent 就是 WinHttpOpen 的第一个参数（kUserAgent）。
    //
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY：走系统/当前用户的代理设置
    //   （含 IE 代理配置、PAC 脚本、按网卡区分的配置、失败切换、认证）。
    //   官方文档明确"Windows 8.1 及更新版本推荐用它"，而它取代的
    //   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 只读注册表里的静态代理、
    //   **不继承浏览器代理设置** —— 在需要 PAC 的公司网络里会直接不通。
    //   所以默认用 AUTOMATIC_PROXY；万一在极老的系统上不支持，
    //   WinHttpOpen 会返回 NULL，下面退回 DEFAULT_PROXY 再试一次。
    WinHttpHandle session(WinHttpOpen(kUserAgent,
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS,
                                      0));
    if (!session) {
        OnlineLog("在线歌词：WinHttpOpen(AUTOMATIC_PROXY) 失败，改用 DEFAULT_PROXY 重试");
        session.reset(WinHttpOpen(kUserAgent,
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS,
                                  0));
        if (!session) {
            reply.error = L"WinHttpOpen 失败：" + describe(GetLastError());
            return reply;
        }
    }

    // ---- 超时 ----
    // 参数顺序是 (解析, 连接, 发送, 接收)，四个都要给。
    // 不设的话 WinHTTP 用默认值，其中连接/接收的默认值偏长，
    // 网络不通时后台线程会挂在那儿很久 —— 而我们的未命中标记和缓存
    // 都要等它回来才会写，用户看到的就是"查歌词卡住不动"。
    if (!WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs,
                            kSendTimeoutMs, kReceiveTimeoutMs)) {
        // 设不上不致命，但一定要留痕：后面真挂住了，这条日志就是唯一的线索。
        OnlineLog("在线歌词：WinHttpSetTimeouts 失败（%lu），本次请求可能长时间挂起",
                  GetLastError());
    }

    // ---- connect（端口决定协议，见上面的说明）----
    WinHttpHandle connect(WinHttpConnect(session.get(), host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) {
        reply.error = L"WinHttpConnect 失败：" + describe(GetLastError());
        return reply;
    }

    // ---- request ----
    // WINHTTP_FLAG_SECURE 是 HTTPS 的另一半，不能省。
    // WINHTTP_FLAG_REFRESH 让本次请求绕开 WinHTTP 自己的 URL 缓存 ——
    // 免得服务器上刚补了歌词，我们却一直吃到本地缓存的旧 404。
    // pwszVersion 传 nullptr 表示用默认的 HTTP/1.1（官方示例也是这么写的）。
    WinHttpHandle request(WinHttpOpenRequest(connect.get(), L"GET",
                                             pathAndQuery.c_str(),
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
    if (!request) {
        reply.error = L"WinHttpOpenRequest 失败：" + describe(GetLastError());
        return reply;
    }

    // 在请求句柄上再设一遍超时。会话上的设置理论上会被继承，
    // 但重复设一次没有副作用，而"某些路径下没继承到"是排查起来很痛的。
    WinHttpSetTimeouts(request.get(), kResolveTimeoutMs, kConnectTimeoutMs,
                       kSendTimeoutMs, kReceiveTimeoutMs);

    // 【不要加 Accept-Encoding: gzip】WinHTTP 的 WinHttpReadData **不会**
    // 自动解压。一旦请求了 gzip，拿回来的是二进制压缩流，
    // JSON 解析会在第一个字节就失败，而且症状是"响应看起来是乱码"，
    // 很容易怀疑到解析器头上。让服务器回明文最省事。
    if (!WinHttpSendRequest(request.get(),
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0)) {
        reply.error = L"WinHttpSendRequest 失败：" + describe(GetLastError());
        return reply;
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        reply.error = L"WinHttpReceiveResponse 失败：" + describe(GetLastError());
        return reply;
    }

    // ---- 状态码 ----
    // WinHttpQueryHeaders 必须在 WinHttpReceiveResponse 完成之后调用
    // （SDK 文档对 hRequest 的前提条件写得很明确）。
    // WINHTTP_QUERY_FLAG_NUMBER 让它按 DWORD 返回而不是字符串。
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        reply.error = L"读取 HTTP 状态码失败：" + describe(GetLastError());
        return reply;
    }
    reply.status = status;
    reply.transportOk = true;   // 已经从"传输层"这一关毕业了，状态码是多少不影响

    // ---- 读 body ----
    // 循环 WinHttpQueryDataAvailable / WinHttpReadData。
    // 返回可用字节数为 0 就表示传输结束（不是错误）。
    // 分块传输（chunked）由 WinHTTP 自己拆好，这里拿到的就是净荷。
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &avail)) {
            reply.transportOk = false;
            reply.body.clear();
            reply.error = L"WinHttpQueryDataAvailable 失败：" + describe(GetLastError());
            return reply;
        }
        if (avail == 0) break;   // 读完

        if (reply.body.size() + avail > kMaxBodyBytes) {
            reply.transportOk = false;
            reply.body.clear();
            reply.error = L"响应体超过 " + std::to_wstring(kMaxBodyBytes) + L" 字节，已放弃";
            reply.retryable = false;   // 对方就是这么胖，重试也是一样胖
            return reply;
        }

        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buf.data(), avail, &read)) {
            reply.transportOk = false;
            reply.body.clear();
            reply.error = L"WinHttpReadData 失败：" + describe(GetLastError());
            return reply;
        }
        if (read == 0) break;

        reply.body.append(buf.data(), read);
    }

    return reply;
    // session / connect / request 在这里由各自的析构函数关掉 ——
    // 上面每一条提前 return 的路径也一样，这就是 RAII 的意义。
}

// 判断这一轮要不要重试。
//
// 分两类，判据完全不同：
//   * HTTP 拿到了、但状态码是 429 / 5xx —— 这是服务器在说
//     "我现在忙/出错了，等会儿再来"。**不是**"这首歌没有歌词"。
//     实测 LRCLIB 过载时就是 503 ServerOverloaded。
//   * 传输层根本没成功（超时、断连、DNS 临时抽风）—— 也值得再来一次。
//   * 其余（200 / 404 / 400 这类确定性回答）不重试：
//     重复问一遍不会有不同的答案，只会拖长时间。
bool ShouldRetry(const HttpReply& r) {
    if (!r.retryable) return false;
    if (!r.transportOk) return true;                    // 传输层失败
    return r.status == 429u || r.status >= 500u;        // 服务器端问题
}

// 把一次尝试的结果写成人能读的一行。给日志和 error 用。
// 单独抽出来是为了避免在 printf 的实参里现场拼 std::string 临时对象 ——
// 那种写法虽然合法（临时对象活到整个完整表达式结束），但很容易被
// 后人"顺手优化"成先存指针再用，那就是悬垂指针了。
std::string DescribeReply(const HttpReply& r) {
    if (r.transportOk) {
        return "HTTP " + std::to_string(r.status);
    }
    return WideToUtf8(r.error);
}

// 带退避重试的 HTTPS GET。
//
// 两道闸：次数（kMaxHttpAttempts）和**整轮共享的墙钟截止时刻**
// （deadlineTick，由调用方算一次传进来，见 kTotalRetryBudgetMs 的说明）。
// 截止时刻只管"要不要再试一次"，不会拦下第一次尝试 ——
// 第一次尝试是功能本身，重试才是可选的。
//
// 重试之间 Sleep 在**后台线程**上，不占主线程 —— 这是这个函数
// 只允许在后台线程调用的原因之一。
HttpReply HttpGet(const std::wstring& host, const std::wstring& pathAndQuery,
                  ULONGLONG deadlineTick) {
    constexpr size_t kBackoffCount = sizeof(kRetryBackoffMs) / sizeof(kRetryBackoffMs[0]);

    HttpReply reply = HttpGetOnce(host, pathAndQuery);
    reply.attempts = 1;

    for (int attempt = 1; attempt < kMaxHttpAttempts; ++attempt) {
        if (!ShouldRetry(reply)) break;

        const ULONGLONG now = GetTickCount64();
        if (now >= deadlineTick) {
            OnlineLog("在线歌词：重试预算已用完，不再重试（已尝试 %d 次，%s）",
                      reply.attempts, DescribeReply(reply).c_str());
            break;
        }

        // 退避表用完之后就一直用最后一档 —— 这样把 kMaxHttpAttempts
        // 调大也不会越界（退避表是手写的固定长度数组）。
        const size_t backoffIdx = (static_cast<size_t>(attempt) - 1 < kBackoffCount)
                                      ? static_cast<size_t>(attempt) - 1
                                      : kBackoffCount - 1;
        const DWORD waitMs = kRetryBackoffMs[backoffIdx];

        OnlineLog("在线歌词：第 %d 次尝试失败（%s），%lu ms 后重试",
                  attempt, DescribeReply(reply).c_str(), waitMs);
        Sleep(waitMs);

        reply = HttpGetOnce(host, pathAndQuery);
        reply.attempts = attempt + 1;
    }

    if (reply.attempts > 1) {
        OnlineLog("在线歌词：共尝试 %d 次，最终 %s",
                  reply.attempts, DescribeReply(reply).c_str());
    }
    return reply;
}

// ---------------------------------------------------------------------------
// 网易云：URL 构造 + 一轮查询
// ---------------------------------------------------------------------------

// 搜索用的关键词。artist 和 title 都用上 —— 只用 title 会召回一大堆
// 同名不同人的版本，而覆盖判定又要靠 artist，等于把工作推给后面。
//
// 百分号编码复用上面那个 PercentEncode（LRCLIB 那条路也在用）——
// 别再写第二个：同一件事有两份实现，将来改一处漏一处就是"中文曲名
// 在 LRCLIB 上好好的、在网易云上 400"这种最难查的 bug。
// 前置声明：定义在文件更靠后的「曲名末尾的版本标记」那一节
//（那节是后加的，位置在 PickNetEaseCandidate 之前，而本函数在它之前）。
std::wstring StripEditionMarker(const std::wstring& title);

// titleOverride 为空 = 用剥掉音轨号之后的曲名（默认，也是绝大多数情况该用的）。
// hintOverride  为空 = 用 req.searchHint。传非空 = 用指定的线索。
// 两者都是为了支持多趟搜索 —— 见 TryNetEase 里那段说明。
std::wstring BuildNetEaseSearchPath(const OnlineLyricRequest& req,
                                    const std::wstring& titleOverride = std::wstring(),
                                    const std::wstring& hintOverride  = std::wstring()) {
    // ⚠️ 搜索词里的曲名必须**先剥掉音轨号**。
    //
    // 实测（2026-09-24）这个前缀会把搜索整个带偏：
    //     查「02 遗忘山丘」 -> 青山不改与君携 / 讨好 / 遗憾 …（正确答案连前 6 都进不去）
    //     查「遗忘山丘」    -> 遗忘山丘 by 阿良良木健（242.8s，本地 242.1s，差 0.7 秒）
    // 原来只在候选核验里剥、搜索词没剥，于是第一关就搜错了东西 ——
    // 后面两道闸门再准也救不回来（日志里表现成「过曲名闸 0 条」，
    // 看起来像"网易云没有这首歌"，其实是"我们搜错词了"）。
    // 两条路都要剥干净：TryNetEase 传进来的 titleOverride 已经在那边剥过，
    // 而**默认这条路**（不传 override）也必须一样 —— 否则同一个函数
    // 因为调用方式不同而给出不同的搜索词，测试一断言就露馅了
    // （这条正是被单测 `搜索词里没有 [Remastered]` 抓出来的）。
    const std::wstring t = titleOverride.empty()
                               ? StripLeadingTrackNumber(StripEditionMarker(TrimWs(req.title)))
                               : titleOverride;

    std::wstring q = TrimWs(req.artist);
    if (!q.empty() && !t.empty()) q += L" ";
    q += t;

    // 线索（通常是专辑名）再缀在后面。
    //
    // 放在**末尾**是有意的：搜索引擎对靠前的词权重更高，曲名才是最强的判据；
    // 线索只是用来把"同名不同专辑"的那一堆挤下去。
    const std::wstring hint = hintOverride.empty() ? TrimWs(req.searchHint)
                                                   : TrimWs(hintOverride);
    if (!hint.empty()) {
        if (!q.empty()) q += L" ";
        q += hint;
    }

    return L"/api/search/get/web?s=" + PercentEncode(q) + L"&type=1&limit=10";
}

// 取歌词的路径。
//
// ⚠️⚠️ `lv=-1` **不是笔误，改成 1 会让一整类歌"明明有词却说没有"**。
//
// 【2026-09-25 实测发现】用户报「还是有几首不行，在网易云上都有」。
// 候选核验已经选中了正确的条目、时长分毫不差，却卡在这一步：
//     `网易云 id=2725479909 的 lrc.lyric 为空（多半是纯音乐）`
// 而那条**不是纯音乐** —— 直接打接口逐项隔离：
//
//     /api/song/lyric?id=X&lv=1&kv=1&tv=-1          -> lrc 长度 0      （原写法）
//     /api/song/lyric?id=X&lv=-1&kv=-1&tv=-1        -> lrc 长度 638  ✔
//     /api/song/lyric?os=pc&id=X&lv=1&kv=1&tv=-1    -> lrc 长度 0
//     /api/song/lyric?os=pc&id=X&lv=-1&kv=-1&tv=-1  -> 老歌 1232 字符 ✔ 没被弄坏
//
// 结论：**`lv` 才是决定性的那个参数，`os=pc` 加不加都一样。**
//   `lv=1` = "只要第 1 版歌词"；新歌的词只存在于更高版本 -> 服务端返回空。
//   `lv=-1` = "给最新版"，新旧通吃。
// 代价特别大，因为空结果会被当成"各源都没有"，写进 **7 天有效**的负缓存 ——
// 用户要干等一周，而且日志里写着「多半是纯音乐」，把人往错的方向引。
//
// 这个"纯音乐"的说法现在也不准确了，见调用点的日志。
std::wstring BuildNetEaseLyricPath(double id) {
    wchar_t buf[64];
    swprintf_s(buf, L"/api/song/lyric?id=%.0f&lv=-1&kv=-1&tv=-1", id);
    return buf;
}

// 网易云的搜索结果里挑一条，够可信就返回它的 id。
//
// 【必须核验，不能拿到就用】实测搜「周杰伦 晴天」的**首条是翻唱**
// （「晴天 (原唱 周杰伦)」by RyaVocal）。所以这里是和 LRCLIB 那边
// 同样口径的两道闸：
//   * 时长 —— 网易云给的是毫秒，和本地时长比对，容差沿用调用方的口径；
//   * 名字 —— 曲名归一化后必须相等；演唱者至少有一方包含另一方，
//     或者双方都为空（无标签曲目）。
//
// 候选核验用的时长容差。
//
// 与播放器侧那道闸门（playback_state.cpp 的 kDurationToleranceSec）**取同一个值**：
// 两处口径不一致的话，会出现「这里放行、那边又否掉」的自相矛盾，
// 排查起来非常费劲。
constexpr double kCandidateDurationTolSec = 5.0;

// ---------------------------------------------------------------------------
// 曲名末尾的「版本标记」
//
// 【为什么需要剥】用户有一张 remaster 专辑《恋爱理论》，网易云上只有原版。
// 实测：
//     本地「白夜梦 [Remastered]」 271.2s
//     网易云「白夜梦」by 阿良良木健 271.2s   ← 曲名全等、时长差 0 秒
// 唯一障碍就是那个 [Remastered]：归一化后变成「白夜梦remastered」，
// 和「白夜梦」不相等，于是被曲名闸挡掉。
// ---------------------------------------------------------------------------

// 这个版本标记是不是**不影响内容**的。
//
// 判据是：换的是母带 / 编码，不是演唱或编曲 —— 所以歌词和**时间轴**都一样，
// 拿原版的词配 remaster 是安全的。
//
// ⚠️ 明确**不**包含 Live / Instrumental / Cover / Ver. / TV Size / Remix：
//    那些版本的内容真的不一样（有的干脆没词），
//    剥掉标记会把歌词配到错误的版本上 —— 那比没有歌词更糟。
bool IsContentNeutralEdition(const std::wstring& lower) {
    static const wchar_t* kWords[] = {
        L"remaster",          // remastered / remaster / re-mastered
        L"hi-res", L"hires",  // 高解析度重制

        // 曲目**类型**标记，同样不改变内容与时间轴。
        //
        // 实测来源（2026-09-24）：本地 `06 爸爸.wav`（49.0s），
        // 网易云上是 `爸爸……（Interlude）`（64s，专辑「奇爱人生 LOVE ELEGIA」）。
        // 不剥这个后缀的话曲名闸直接毙掉 —— 而那确实是同一段东西。
        //
        // ⚠️ 只收"类型"标记，**不收** Live / Instrumental / Cover / Ver. ——
        //    那些版本的内容真的不一样。
        L"interlude",
    };
    for (const wchar_t* w : kWords) {
        if (lower.find(w) != std::wstring::npos) return true;
    }
    return false;
}

// 去掉曲名末尾「同内容」的版本标记：`白夜梦 [Remastered]` -> `白夜梦`
//
// 循环剥是为了 `白夜梦 [2011 Remastered] [Hi-Res]` 这种叠了好几个的情况。
// 只在**末尾**动手，中间的括号（往往是曲名的一部分）一律不碰。
std::wstring StripEditionMarker(const std::wstring& title) {
    std::wstring s = TrimWs(title);

    for (;;) {
        if (s.empty()) break;

        // 末尾必须是成对的括号，并认出对应的开括号
        wchar_t open = 0;
        const wchar_t last = s.back();
        if      (last == L']') open = L'[';
        else if (last == L')') open = L'(';
        else if (last == L'）') open = L'（';
        if (open == 0) break;

        const size_t at = s.find_last_of(open);
        if (at == std::wstring::npos) break;
        if (s.size() < at + 2) break;

        std::wstring inner = s.substr(at + 1, s.size() - at - 2);
        for (wchar_t& c : inner) c = static_cast<wchar_t>(towlower(c));

        if (!IsContentNeutralEdition(inner)) break;   // 不是"同内容"标记，别动它

        s = TrimWs(s.substr(0, at));
    }
    return s;
}


// 在搜索结果里挑一条。命中返回 true 并填好 song。
//
// ===========================================================================
//  闸门的分工（两次被真实数据打回来之后才定下来的）
// ===========================================================================
//
//  **硬闸：曲名。** 两道软闸：时长、演唱者 —— **二者其一对上即可**。
//
//  ── 为什么演唱者不能是硬闸（D-034）──
//  初版做成硬否决，把用户「再见，碳酸海」整张专辑误杀：标签是繁体「純白P」，
//  网易云写简体「Soda纯白」，NormalizeLyricStem 不折繁简，于是一个都过不了。
//  真正挡翻唱的是曲名闸 —— 实测搜「周杰伦 晴天」首条是「晴天 (原唱 周杰伦)」，
//  归一化后与「晴天」不等，那道闸自己就够了。
//
//  ── 为什么时长也不能是硬闸（用户 2026-09-24 拍板"宁可偏，也要有词"）──
//  用户的 remaster 专辑《恋爱理论》里「心加心」本地 308.0s、
//  网易云原版 273.9s，**差 34 秒**；但曲名全等、演唱者（阿良良木健）对得上。
//  同一个歌手的同一首曲子，差的是编曲长度 —— 拿原版的词配上，
//  时间轴会整体偏，但总比一片空白强。
//
//  ── 但两道软闸不能一起失效 ──
//  曲名对、时长差很多、演唱者又对不上 —— 那多半是**同名的另一首歌**
//  （D-024 实测过：搜 "Best Wishes" 回来的是完全不相干的歌，328s vs 240s）。
//  这种仍然要拒。
//
//  ── 曲名用和调用方**完全相同**的候选集合 ──
//  playback_state.cpp 的 OnlineResultTrustworthy 用 MakeTitleCandidates()
//  生成候选（它会切「艺术家 - 曲名」前缀和音轨号「02 遗忘山丘」→「遗忘山丘」）。
//  源头这边要是只比原样，就会出现「这里拒了、那边本来会接受」的白白错过 ——
//  用户那首无标签的「02 遗忘山丘.wav」正是栽在这一条上。
bool PickNetEaseCandidate(const std::vector<NetEaseSong>& songs,
                          const OnlineLyricRequest& req, NetEaseSong& out) {
    // 曲名候选：先剥掉"同内容"的版本标记（[Remastered]），再交给
    // MakeTitleCandidates 切前缀和音轨号。两边都先剥再切，口径一致。
    std::vector<std::wstring> wantTitles;
    for (const std::wstring& c : MakeTitleCandidates(StripEditionMarker(req.title))) {
        const std::wstring n = NormalizeLyricStem(c);
        if (!n.empty()) wantTitles.push_back(n);
    }

    const std::wstring wantArtist = TrimWs(req.artist);
    const double wantDur = req.durationSec;

    int    bestScore = -1;
    size_t bestIdx   = 0;
    int    passedTitle = 0, passedDur = 0, passedArtist = 0;
    int    rejectedByLength = 0;   // 被"时长差太多 = 另一版录音"直接拒掉的条数

    // ---- 诊断用：前几条候选长什么样 ----
    //
    // 【为什么必须记】拒绝时原来只有一句「过曲名闸 N 条」，知道了数量、
    // 不知道**为什么**。而要弄清为什么就得回头再打一次接口 ——
    // 既慢，又会撞限流（我自己探测时就把用户的网易云弄限流过，见 D-033）。
    // 一次查询本来就把候选拿在手里，顺手记下来几乎不花钱。
    //
    // 只在**一条都没通过**时才写进日志（下面那段），正常查询不产生噪音。
    std::string candDiag;
    int diagShown = 0;

    for (size_t i = 0; i < songs.size(); ++i) {
        const NetEaseSong& s = songs[i];
        if (!s.hasId || s.name.empty()) continue;

        if (diagShown < kDiagCandidates) {
            ++diagShown;
            if (!candDiag.empty()) candDiag += "  ";
            // name / artist 已经是 UTF-8（直接从 JSON 里取出来的），不用转
            candDiag += "[" + s.name + " | " + s.artist;
            if (s.durationMs > 0) {
                char dbuf[48];
                sprintf_s(dbuf, " | %.1fs",
                          s.durationMs / 1000.0 - (wantDur > 0 ? wantDur : 0.0));
                candDiag += dbuf;
                if (wantDur > 0) candDiag += "(差)";
            }
            candDiag += "]";
        }

        // ---- 硬闸：曲名 ----
        if (!wantTitles.empty()) {
            const std::wstring got =
                NormalizeLyricStem(StripEditionMarker(Utf8ToWide(s.name.c_str())));
            bool hit = false;
            for (const std::wstring& w : wantTitles) {
                if (w == got) { hit = true; break; }
            }
            if (!hit) continue;
        }
        ++passedTitle;

        // ---- 软闸 1：时长 ----
        const double durSec = s.durationMs / 1000.0;
        bool durOk = false;
        if (wantDur > 0.0 && durSec > 0.0) {
            const double d = (durSec > wantDur) ? (durSec - wantDur) : (wantDur - durSec);
            durOk = (d <= kCandidateDurationTolSec);

            // ---- 时长"是不是同一版录音"的天花板 ----
            //
            // 这一条**优先于演唱者闸**：长度差一倍就不是同一版录音了，
            // 演唱者再对也不能把它的时间轴套到我们这个文件上（见上面常量说明）。
            const double hi = (durSec > wantDur) ? durSec : wantDur;
            const double lo = (durSec > wantDur) ? wantDur : durSec;
            if (hi / lo > kSameRecordingRatioCeiling) {
                ++rejectedByLength;
                continue;
            }
        }
        if (durOk) ++passedDur;

        // ---- 软闸 2：演唱者 ----
        // 「?」「未知艺术家」是 foobar2000 对**没有这个字段**的占位符，
        // 不是真的歌手名 —— 拿它去比只会永远对不上，先当成空。
        const bool artistOk = !IsPlaceholderTag(wantArtist) &&
                              ArtistNamesOverlap(wantArtist, Utf8ToWide(s.artist.c_str()));
        if (artistOk) ++passedArtist;

        // 两道软闸**都**没过 -> 多半是同名的另一首歌，拒掉
        if (!durOk && !artistOk) continue;

        const int score = (artistOk ? 2 : 0) + (durOk ? 1 : 0);
        if (score > bestScore) {
            bestScore = score;
            bestIdx   = i;
        }
    }

    if (bestScore < 0) {
        // 把"卡在哪一道闸"写进日志 —— 出问题时能一眼看出问题在哪，
        // 不必再去打接口猜（两次都是靠它定位的）。
        OnlineLog("在线歌词：网易云候选核验 —— 过曲名闸 %d 条，其中时长对上 %d 条、"
                  "演唱者对上 %d 条，可用 0 条", passedTitle, passedDur, passedArtist);

        if (rejectedByLength > 0) {
            OnlineLog("    其中 %d 条因**时长相差超过 %.1f 倍**被当成另一版录音直接拒"
                      "（这一条优先于演唱者闸）", rejectedByLength, kSameRecordingRatioCeiling);
        }

        // 再补两行"我们拿什么去比"和"候选长什么样" —— 有了这两行，
        // 一次未命中在日志里就是**自解释**的，不用回头重打接口（会撞限流）。
        OnlineLog("    期望: 曲名=%s | 演唱者=%s | 时长=%.1fs",
                  WideToUtf8(JoinForLog(wantTitles)).c_str(),
                  wantArtist.empty() ? "(空)" : WideToUtf8(wantArtist).c_str(),
                  wantDur);
        if (!candDiag.empty()) {
            OnlineLog("    候选(前 %d 条): %s", diagShown, candDiag.c_str());
        } else {
            OnlineLog("    候选: （搜索一条都没返回）");
        }
        return false;
    }

    OnlineLog("在线歌词：网易云候选核验 —— 过曲名闸 %d 条（时长对上 %d、演唱者对上 %d），"
              "选中得分 %d 的那条", passedTitle, passedDur, passedArtist, bestScore);
    out = songs[bestIdx];
    return true;
}

// 一轮网易云查询。返回 true 表示拿到了一份可用的歌词（已填进 entry）。
//
// **只允许在后台线程调用**（它内部走 HttpGet，会 Sleep 重试）。
// 取消检查：调用方换曲 / 重查时，后台这一轮就没必要再发请求了。
//
// 【放在哪里】每个**联网动作之前**，加上写未命中标记之前。
// 中间那段（解析、合并翻译）是纯计算，几百微秒，不值得再插检查点 ——
// 插多了只是把代码弄乱。
bool IsCancelled(const OnlineCancelFlag& flag) {
    return flag != nullptr && flag->load(std::memory_order_relaxed);
}

bool TryNetEase(const OnlineLyricRequest& req, ULONGLONG retryDeadline,
                LrclibEntry& entry, bool& requestFailed, std::wstring& note,
                const OnlineCancelFlag& cancel = nullptr) {
    requestFailed = false;

    // 已取消就直接走人 —— 一次请求都不发。
    if (IsCancelled(cancel)) return false;

    const std::wstring title = TrimWs(req.title);
    if (title.empty()) return false;   // 没有曲名就没法搜

    // ---- 0. 冷却中就直接跳过，一个请求都不发 ----
    //
    // 这一步是**省时间**的关键：被限流时每次换曲本来要白烧 ~5 秒在重试上。
    // 跳过时仍然置 requestFailed（= "没问出来"），所以不会写假的"没有"。
    if (GetTickCount64() < g_netEaseBlockedUntil.load()) {
        requestFailed = true;
        note = L"网易云处于限流冷却中";
        // 只记第一条，别每次换曲都刷屏
        if (!g_netEaseSkipLogged.exchange(true)) {
            OnlineLog("在线歌词：网易云仍在限流冷却中，本轮直接跳过（改查 LRCLIB）");
        }
        return false;
    }

    // ---- 0.5 同一瞬间别挤进多个请求 ----
    // 快速连点切歌时会连着发，这多半就是触发限流的原因之一。
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG last = g_netEaseLastRequestTick.load();
        if (last != 0 && now - last < kNetEaseMinIntervalMs) {
            const ULONGLONG waitMs = kNetEaseMinIntervalMs - (now - last);
            Sleep(static_cast<DWORD>(waitMs));
        }
        g_netEaseLastRequestTick.store(GetTickCount64());
    }

    // ---- 1. 搜索 ----
    //
    // ⚠️ 这里有个**重试循环**，而不是一次 HttpGet 就完事。
    //    原因：网易云限流时返回的是 HTTP 200 + 响应体里的 {"code":405}，
    //    而 HttpGet 的退避重试只看 HTTP 状态码 —— 对 405 完全不生效。
    //    所以限流要在这一层自己重试（见 ParseNetEaseCode 的说明）。
    constexpr int kNetEaseRateLimitRetries = 3;
    constexpr DWORD kNetEaseBackoffMs[] = { 800, 1600, 2500 };

    // 【为什么要准备最多三个搜索词】曲名开头的音轨号必须先剥掉
    // （见 BuildNetEaseSearchPath），但剥法是「≤3 位数字 + 分隔符」，所以
    // 「7 Years」「99 Problems」这类**曲名本身以数字开头**的会被误剥成
    // 「Years」「Problems」。
    //
    // 这个误剥在**本地搜索**里无害 —— 那里只是多一个候选，原样的还在；
    // 但在搜索词里有害，因为它是**替换**。所以剥过的词搜不到时用原词再试一次。
    // （我自己写测试时就断言"7 Years 不该被剥"，结果被抓出来 —— 见 D-041。）
    //
    // 第三趟是**线索兜底**（通常是文件夹名猜出来的专辑）。它放在最后是刻意的：
    // 那是猜的，猜错会把正确答案挤出前 10，所以只能"加一次机会"，
    // 不能"挤掉原有的机会"。实测两种结果都出现过 ——
    //     `奇爱人生 爸爸`      -> 命中「爸爸……（Interlude）」     ✓
    //     `奇爱人生·终焉版 哀歌` -> 寻爱一生 / 众人划桨开大船 …    ✗
    // ⚠️ 搜索词里的曲名要剥**两样**：版本标记 + 音轨号。
    //
    // 只剥音轨号是不够的 —— 实测（2026-09-24）带着 [Remastered] 发出去，
    // 召回的是完全不相干的东西：
    //     查「阿良良木健 远恋 [Remastered]」           -> 过曲名闸 0 条
    //     查「皓月、阿良良木健 依存症（…）[Remastered]」-> 过曲名闸 0 条
    // 而那两首在网易云上都有（「远恋」237s、「依存症 (Love Theory Ver.)」290.1s）。
    // 剥掉之后搜索词变干净，闸门才有东西可对。
    //
    // 顺序：先剥版本标记（它在末尾），再剥音轨号（它在开头）。
    // 反过来的话「07 哀歌 [Remastered]」会先被音轨号那步当"开头是数字+空格"处理 ——
    // 虽然结果也对，但语义上先剥末尾更清楚。
    const std::wstring noEdition = StripEditionMarker(title);
    const std::wstring strippedTitle = StripLeadingTrackNumber(noEdition);
    const std::wstring hint          = TrimWs(req.searchHint);

    // 每一趟就是一对 (曲名, 线索)。
    struct SearchPass { std::wstring title; std::wstring hint; };
    std::vector<SearchPass> passes;
    passes.push_back({ strippedTitle, std::wstring() });   // 1) 剥版本标记 + 剥音轨号
    if (noEdition != strippedTitle) {
        passes.push_back({ noEdition, std::wstring() });   // 2) 只剥版本标记
    }
    if (!hint.empty()) {
        passes.push_back({ noEdition, hint });             // 3) 线索兜底
    }

    std::vector<NetEaseSong> songs;
    NetEaseSong pick;
    bool picked = false;

    for (size_t pass = 0; pass < passes.size() && !picked; ++pass) {
        if (pass > 0) {
            OnlineLog("在线歌词：网易云第 %zu 趟搜索（曲名「%s」%s）",
                      pass + 1, WideToUtf8(passes[pass].title).c_str(),
                      passes[pass].hint.empty() ? "" : "，带线索");
        }

    HttpReply search;
    for (int attempt = 0; ; ++attempt) {
        search = HttpGet(kNetEaseHost,
                         BuildNetEaseSearchPath(req, passes[pass].title, passes[pass].hint),
                         retryDeadline);

        if (!search.transportOk) {
            requestFailed = true;
            OnlineLog("在线歌词：网易云搜索传输失败（已尝试 %d 次）：%s",
                      search.attempts, WideToUtf8(search.error).c_str());
            note = L"网易云搜索网络错误：" + search.error;
            return false;
        }
        if (search.status != 200) {
            // HTTP 层就失败了。同样当"这一轮没问出结果"，不当作"网易云上没有"。
            requestFailed = true;
            OnlineLog("在线歌词：网易云搜索返回 HTTP %lu（已尝试 %d 次）",
                      search.status, search.attempts);
            note = L"网易云搜索 HTTP " + std::to_wstring(search.status);
            return false;
        }

        const int bizCode = ParseNetEaseCode(search.body);
        if (bizCode == 200) {
            g_netEaseBlockStreak.store(0);   // 通了就把连续计数清零
            break;
        }

        // 业务码非 200：限流之类。**绝不能**当成"没有"。
        if (IsNetEaseRetryableCode(bizCode) &&
            attempt < kNetEaseRateLimitRetries &&
            GetTickCount64() < retryDeadline) {
            const DWORD waitMs = kNetEaseBackoffMs[(attempt < 3) ? attempt : 2];
            OnlineLog("在线歌词：网易云返回业务码 %d，%lu ms 后重试（第 %d 次）",
                      bizCode, waitMs, attempt + 1);
            Sleep(waitMs);
            continue;
        }

        // 重试用尽。限流的话进入**进程级冷却**，省得后面每次换曲都白烧 5 秒
        // （见 g_netEaseBlockedUntil 的说明）。
        if (bizCode == 405) NetEaseEnterCooldown();

        requestFailed = true;   // 不是"没有"，是"没问出来" -> 不写未命中标记
        OnlineLog("在线歌词：网易云业务码 %d（HTTP 200）—— 本轮不写未命中标记", bizCode);
        note = L"网易云业务码 " + std::to_wstring(bizCode) + L"（多半是限流）";
        return false;
    }

    if (!ParseNetEaseSearchRoot(search.body, songs)) {
        // 走到这里 HTTP 是 200、业务码也是 200，所以「没有 songs 数组」
        // 是**确定**的"这个搜索词在网易云上没有结果"。
        //
        // ⚠️ 但这条日志在修好业务码检查之前是会骗人的：那时候
        //    限流的响应（HTTP 200 + {"code":405}）也会走到这里，
        //    日志看起来一模一样，实际却是"没问出来"（见 ParseNetEaseCode）。
        OnlineLog("在线歌词：网易云搜索无结果（业务码 200，响应里没有 songs 数组）");
        continue;   // 换个词可能就有了
    }
    OnlineLog("在线歌词：网易云搜索返回 %zu 个候选", songs.size());
    if (songs.empty()) continue;   // 换个词可能就有了

    // 「卡在哪一道闸」由 PickNetEaseCandidate 自己写日志（过闸计数），这里不重复。
    if (PickNetEaseCandidate(songs, req, pick)) { picked = true; break; }

    // 有候选但一条都没过两道硬闸（曲名 + 时长）。可能是这个搜索词不对，
    // 也可能是这首歌真没有 —— 让 pass 1 用原词再确认一次。
    OnlineLog("在线歌词：网易云 %zu 个候选都没通过硬闸（曲名/时长）", songs.size());

    }   // for (pass ...)

    if (!picked) return false;

    OnlineLog("在线歌词：网易云选中 id=%.0f 「%s」 by %s（%.1fs）",
              pick.id, pick.name.c_str(), pick.artist.c_str(), pick.durationMs / 1000.0);

    // ---- 2. 取歌词 ----
    //
    // 取词是**第二个**请求，中间隔着一次搜索往返（实测 ~200ms），
    // 换曲常常就发生在这一小段里 —— 所以这里必须再查一次取消。
    if (IsCancelled(cancel)) {
        OnlineLog("在线歌词：已取消，跳过取词请求（id=%.0f）", pick.id);
        return false;
    }
    const HttpReply lyric = HttpGet(kNetEaseHost, BuildNetEaseLyricPath(pick.id), retryDeadline);

    if (!lyric.transportOk) {
        requestFailed = true;
        OnlineLog("在线歌词：网易云歌词传输失败（已尝试 %d 次）：%s",
                  lyric.attempts, WideToUtf8(lyric.error).c_str());
        note = L"网易云歌词网络错误：" + lyric.error;
        return false;
    }
    if (lyric.status != 200) {
        requestFailed = true;
        OnlineLog("在线歌词：网易云歌词返回 HTTP %lu（已尝试 %d 次）",
                  lyric.status, lyric.attempts);
        note = L"网易云歌词 HTTP " + std::to_wstring(lyric.status);
        return false;
    }

    // 歌词接口同样把业务码放在响应体里（HTTP 200 + {"code":...}）。
    // 这一步**不能省**：限流时拿到的是一份没有 lrc 字段的响应，
    // 下面会把它当成"这首歌是纯音乐"——那又是一次假的"没有"。
    const int lyricCode = ParseNetEaseCode(lyric.body);
    if (lyricCode != 200) {
        requestFailed = true;
        OnlineLog("在线歌词：网易云歌词业务码 %d（HTTP 200）—— 本轮不写未命中标记",
                  lyricCode);
        note = L"网易云歌词业务码 " + std::to_wstring(lyricCode);
        return false;
    }

    std::string lrc;
    std::string tlyric;
    if (!ParseNetEaseLyricRoot(lyric.body, lrc, &tlyric) || lrc.empty()) {
        // 空的 lrc.lyric 有两种可能，**日志里必须把两种都写上** ——
        // 原先只说"多半是纯音乐"，而 2026-09-25 那批恰恰全都不是纯音乐
        // （是 `lv=1` 取不到新版歌词，见 BuildNetEaseLyricPath），
        // 那句话把人往错的方向引了整整一轮排查。
        OnlineLog("在线歌词：网易云 id=%.0f 的 lrc.lyric 为空 —— "
                  "要么这条真是纯音乐/没上传词，要么取词参数不对（见 BuildNetEaseLyricPath）",
                  pick.id);
        return false;
    }

    // ---- 3. 剥掉开头的制作人员名单 ----
    lrc = StripNetEaseCredits(lrc);
    if (lrc.empty()) {
        OnlineLog("在线歌词：网易云 id=%.0f 剥掉名单后没有正文（纯音乐）", pick.id);
        return false;
    }

    // ---- 3.5 有翻译就按时间戳并进去（双语参照行）----
    //
    // 放在剥名单**之后**：名单行没有翻译，先剥能让时间戳集合更干净。
    // 用户 2026-09-24 定的设计 —— 面板只有 460×150，完整双语会把可见行数砍半，
    // 所以只在**当前行**下面加一行小字，其余行保持单行（渲染层做的）。
    if (!tlyric.empty()) {
        lrc = MergeTranslationLines(lrc, tlyric);
    }

    entry.hasSynced    = true;
    entry.synced       = std::move(lrc);
    entry.hasId        = true;
    entry.id           = pick.id;
    entry.hasDuration  = pick.durationMs > 0.0;
    entry.duration     = pick.durationMs / 1000.0;
    entry.artistName   = pick.artist;
    entry.trackName    = pick.name;
    entry.albumName    = pick.album;
    entry.fromSearch   = true;   // 网易云这条路**永远**是搜索来的，必须让调用方核对
    return true;
}

// ---------------------------------------------------------------------------
// 拿到歌词之后的收尾
// ---------------------------------------------------------------------------

// 严格的 ASCII 小数解析（只认数字和小数点，不接受空白/正负号/科学计数法）。
//
// 用在 LRC 时间戳上：那里的输入形状是确定的，多一分宽容就多一分把
// "看起来像时间戳的东西"当成时间戳的机会。
bool ToDoubleAscii(const std::string& s, double& out) {
    if (s.empty()) return false;
    double v = 0.0;
    double frac = 0.1;
    bool inFrac = false;
    for (char c : s) {
        if (c == '.') {
            if (inFrac) return false;   // 两个小数点
            inFrac = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        if (inFrac) {
            v += static_cast<double>(c - '0') * frac;
            frac /= 10.0;
        } else {
            v = v * 10.0 + static_cast<double>(c - '0');
        }
    }
    out = v;
    return true;
}

// 曲名首行之前有前奏时，补一行**分隔线**。
//
// ===========================================================================
//  这是**显示层**的修补，所以在取词的统一出口做，而不是在某一个源里做
// ===========================================================================
//
// 【治什么】LyricDocument::LineIndexAt() 里有一句
//     if (t < m_lines.front().timeSec) return 0;   // ← 返回**第一行**
// 也就是"第一条时间戳之前"一律算成第一行是当前行。对一首有前奏的歌，
// 表现就是真歌词还没开口，那一行已经被高亮成"当前行"，提前几秒到几十秒。
//
// 补一行之后：前奏期间当前行是这条线，真正要唱的那句作为**下一行**在下面等着。
//
// 【为什么要放在这里，而不是 StripNetEaseCredits 里】
// 那个位置只覆盖「本次从网易云新取到的词」。而**缓存里的 .lrc 是之前写下的**，
// 根本不会再走那段代码 —— 用户实测就撞上了：「缓存到本地的歌词还没有第一行的
// 分隔线」。同一件事在"新取"和"缓存命中"两条路上各写一遍迟早会走岔，
// 所以收敛到**唯一的出口**（见 D-040）。
//
// 【为什么是分隔线而不是空行】
// 解析器**主动丢弃空文本行**（lyric.cpp：夹具里歌词之间夹着空行，不能渲染成
// 空行）。想要空行就得改解析器，而它是全工程共用的 —— 实测那么改会一次打挂
// 26 条解析断言。分隔线是**一行有文字的普通 LRC**，现有解析器原样接受。
//
// 【幂等】首行已经是分隔线就原样返回 —— 新取的词和缓存命中的词会经过同一个
// 函数，不幂等就会叠出好几条线。
std::string EnsureLeadInSeparator(const std::string& lrcUtf8) {
    if (lrcUtf8.empty()) return lrcUtf8;

    // 取首行
    size_t nl = lrcUtf8.find('\n');
    if (nl == std::string::npos) nl = lrcUtf8.size();
    const std::string first = lrcUtf8.substr(0, nl);

    // 幂等：已经有线了就别再加
    if (first.find(NetEaseIntroSeparator()) != std::string::npos) return lrcUtf8;

    // 解析首行的 [mm:ss.xx]
    if (first.empty() || first[0] != '[') return lrcUtf8;
    const size_t close = first.find(']');
    if (close == std::string::npos) return lrcUtf8;

    const std::string tag = first.substr(1, close - 1);
    const size_t colon = tag.find(':');
    if (colon == std::string::npos) return lrcUtf8;

    double minutes = 0.0, seconds = 0.0;
    if (!ToDoubleAscii(tag.substr(0, colon), minutes)) return lrcUtf8;
    if (!ToDoubleAscii(tag.substr(colon + 1), seconds)) return lrcUtf8;

    const double firstSec = minutes * 60.0 + seconds;
    // 首行本来就在 0 附近 -> 没有前奏，不用补
    if (firstSec <= 0.05) return lrcUtf8;

    OnlineLog("在线歌词：首行在 %.2f 秒（有前奏），已在开头补一行分隔线", firstSec);
    return "[00:00.000]" + NetEaseIntroSeparator() + "\n" + lrcUtf8;
}

// 把歌词文本解码好、写进缓存、返回结果。
//
// 【为什么必须顺手删掉 .miss 标记】
// 只写新的 .lrc 而不删旧的 .miss，下次查询会先命中那个
// "查过但没有"的标记（它还在 7 天有效期内）而**直接返回没有**，
// 根本不看 .lrc。表现出来就是"明明刚查到过，重播一遍又说没有"。
// 这一步不能省。
//
// fromSearch 会一并写进 .meta：调用方需要知道"这份词是模糊搜来的、
// 必须核对"，而缓存命中时也要能知道 —— 所以它必须落盘，不能只在内存里。
OnlineLyricResult SaveAndReturn(const CachePaths& paths, bool cacheUsable,
                                const LrclibEntry& entry, DWORD httpStatus,
                                bool fromSearch, const char* via) {
    OnlineLyricResult result;
    result.httpStatus = static_cast<long>(httpStatus);
    result.fromCache = false;

    // 优先 syncedLyrics（带时间轴），没有才退回 plainLyrics。
    //
    // 分隔线在这里补（统一出口），这样**新取到的词**和**缓存命中的词**
    // 走的是同一条规则 —— 见 EnsureLeadInSeparator。
    const std::string text = EnsureLeadInSeparator(entry.hasSynced ? entry.synced : entry.plain);

    result.lrcText = Utf8ToWide(text.c_str());
    if (result.lrcText.empty()) {
        // Utf8ToWide 用的是 MB_ERR_INVALID_CHARS，解不出来就返回空。
        result.ok = false;
        result.error = L"返回的歌词不是合法 UTF-8，已丢弃";
        OnlineLog("在线歌词：%s 命中的歌词解码失败（%zu 字节）",
                  via, text.size());
        return result;
    }

    result.ok = true;
    result.fromSearch = fromSearch;

    // 元数据：命中的到底是哪首歌。调用方要靠这几个值核对，
    // 所以无论缓存写不写得进去，都要如实填好返回。
    result.matchedArtist = Utf8ToWide(entry.artistName.c_str());
    result.matchedTrack  = Utf8ToWide(entry.trackName.c_str());
    result.matchedAlbum  = Utf8ToWide(entry.albumName.c_str());
    result.matchedDuration = entry.hasDuration ? entry.duration : 0.0;

    if (cacheUsable) {
        // 先写 .meta 再写 .lrc。顺序本身不决定正确性（命中要求两个文件
        // 都能读出来），但这样如果中途出事，留下的是"有元数据没歌词"，
        // 下次重取一遍就好，不会反过来。
        const bool metaOk = WriteMetaFile(paths.meta, entry, fromSearch);
        const bool lrcOk  = WriteWholeFile(paths.lrc, text.data(), text.size());

        if (metaOk && lrcOk) {
            DeleteFileIfExists(paths.miss);   // ← 见函数头说明，别删这一行
        } else {
            // 写不进缓存不影响这次的结果，下次重查一遍而已。
            // 但要把半份条目清掉：只剩 .lrc 或只剩 .meta 都算无效条目，
            // 留着只会让下次的读路径多做一次判断。
            OnlineLog("在线歌词：缓存写入失败（%s），已清理半份条目",
                      WideToUtf8(!metaOk ? paths.meta : paths.lrc).c_str());
            DeleteFileIfExists(paths.lrc);
            DeleteFileIfExists(paths.meta);
        }
    }

    OnlineLog("在线歌词：%s 命中 id=%.0f（用 %s，%zu 字节，歌词 %zu 字）"
              "｜匹配到：%s - %s（%.1f 秒）｜来源=%s",
              via, entry.id,
              entry.hasSynced ? "syncedLyrics" : "plainLyrics",
              text.size(), result.lrcText.size(),
              LogSafe(entry.artistName).c_str(), LogSafe(entry.trackName).c_str(),
              entry.hasDuration ? entry.duration : 0.0,
              fromSearch ? "模糊搜索（调用方须核对）" : "精确查询");
    return result;
}

} // namespace

// ===========================================================================
//  对外接口
// ===========================================================================

std::wstring DefaultOnlineCacheDir() {
    // 和 debug_log.cpp 取日志路径的办法保持一致：用 core_api::get_my_instance()
    // 拿本 DLL 的模块句柄，再问它自己的完整路径。
    // 不用 core_api::pathInProfile()：那份路径实测写不进去
    // （debug_log.cpp 的注释里记了这个结论），而 DLL 自己所在目录确定可写。
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(core_api::get_my_instance(), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"cache";   // 兜底：相对当前目录
    }

    std::wstring dir(buf, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir.resize(slash + 1);   // 保留分隔符
    } else {
        dir.clear();
    }
    return dir + L"cache";
}

// 「哪些曲目在所有在线源上都没找到歌词」—— 从 .miss 标记里读回来。
//
// 【为什么要能列出来】用户 2026-09-24 的原话是「还是有部分没有匹配到」。
// 但"部分"是哪些，光看面板永远不知道 —— 面板只在**播到那一首**的时候才告诉你。
// 而 .miss 标记里现在写着 `artist=[..] title=[..] album=[..] duration=..`，
// 于是可以把这张名单直接摆出来给用户看：**知道了才谈得上采取行动**
//（比如给那个文件夹指定搜索线索 —— 那会作废标记并重查，见 SetFolderHint）。
//
// 只返回「当前逻辑版本」写下的标记：旧版本算出来的"没有"不算数，
// 查询路径会把它们作废重查，列出来只会误导。
std::vector<std::wstring> ListUnmatchedTracks() {
    std::vector<std::wstring> out;

    const std::wstring dir = DefaultOnlineCacheDir();
    auto join = [](const std::wstring& d, const wchar_t* name) {
        std::wstring p = d;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        return p + name;
    };

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = join(dir, L"*.miss");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const std::wstring path = join(dir, fd.cFileName);
        std::string raw;
        if (!ReadWholeFile(path, raw)) continue;

        // 第一行是 `时间戳 HTTP码 版本`，第二行（新格式才有）是曲目身份。
        const size_t nl = raw.find('\n');
        if (nl == std::string::npos) continue;          // 老格式：没有身份，列不出来
        std::string identity = raw.substr(nl + 1);
        while (!identity.empty() &&
               (identity.back() == '\n' || identity.back() == '\r')) {
            identity.pop_back();
        }
        if (identity.empty()) continue;

        // 版本不对的直接跳过（和查询路径一个判据 —— 那边会作废它们）
        MissMarker m;
        if (ReadMissMarker(path, m) && m.logic != kMissLogicVersion) continue;

        out.push_back(Utf8ToWide(identity.c_str()));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end());
    return out;
}

std::wstring StripEditionMarkerForMatch(const std::wstring& title) {
    return StripEditionMarker(title);
}

size_t InvalidateMissMarkers() {
    const std::wstring dir = DefaultOnlineCacheDir();

    // 本文件里没有 JoinPath（那是 lyric_search.cpp 的），这里就拼一次
    auto join = [](const std::wstring& d, const wchar_t* name) {
        std::wstring p = d;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        return p + name;
    };

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = join(dir, L"*.miss");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;   // 目录不存在 / 一个标记都没有

    size_t removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (DeleteFileIfExists(join(dir, fd.cFileName))) ++removed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (removed > 0) {
        OnlineLog("在线歌词：搜索线索变了，已作废 %zu 个「没有歌词」结论（.lrc 保留）",
                  removed);
    }
    return removed;
}

// 取消检查见文件上方（TryNetEase 之前）—— 那里是它最早的使用点。

OnlineLyricResult FetchLyricOnline(const OnlineLyricRequest& req,
                                   const std::wstring& cacheDir,
                                   OnlineCancelFlag cancel) {
    OnlineLyricResult result;

    // 这个函数**只允许在后台线程调用**（头文件里反复强调了）。
    //
    // PFC_ASSERT 在本工程里其实是空操作 —— 项目只配了 Release，
    // 而 pfc-lite.h:144 的 `#elif ! PFC_DEBUG` 分支把 PFC_ASSERT 展开成
    // ((void)0)。留着它是为了将来谁开了调试构建，第一次误用就当场炸出来。
    PFC_ASSERT(!core_api::is_main_thread());

    // Release 下真正管用的是下面这个兜底：宁可这次查不到，
    // 也不要把主线程冻在这里几十秒 —— 那是比"没有歌词"严重得多的问题。
    if (core_api::is_main_thread()) {
        result.error = L"FetchLyricOnline 被主线程调用了（它只允许在后台线程运行），"
                       L"已拒绝执行以免冻结界面；界面代码请改用 FetchLyricOnlineAsync";
        OnlineLog("在线歌词：%s", WideToUtf8(result.error).c_str());
        return result;
    }

    const std::wstring dir = cacheDir.empty() ? DefaultOnlineCacheDir() : cacheDir;
    const CachePaths paths = MakeCachePaths(dir, req);

    // 缓存目录先建好。建不出来就整轮跳过缓存读写，但联网照常 ——
    // 缓存只是加速手段，不该成为功能本身的前置条件。
    const bool cacheUsable = EnsureDirectory(dir);
    if (!cacheUsable) {
        OnlineLog("在线歌词：缓存目录不可用（%s），本轮跳过缓存读写",
                  WideToUtf8(dir).c_str());
    }

    // ---- 1. 查缓存 ----
    //
    // 一个缓存条目由 .lrc + .meta **两个文件**组成，两个都读得出来才算命中。
    //
    // 【为什么坚持"两个都要"】.meta 里存的是"这份词到底是哪首歌"。
    // 如果允许只命中 .lrc，就会出现一种很坏的状态：歌词有了，
    // 但调用方拿不到元数据、没法核对 —— 而模糊搜索是可能返回
    // 完全不相干的歌的（实测搜 "Best Wishes" 回来的是别的歌）。
    // 与其让调用方在"无法核对"的情况下被迫盲信，不如把这种半份条目
    // 判为无效、重新联网取一次。多一次网络往返，换掉一类静默的错词。
    if (cacheUsable) {
        LrclibEntry meta;
        bool fromSearch = false;
        std::string bytes;

        const bool metaOk = ReadMetaFile(paths.meta, meta, fromSearch);
        const bool lrcOk  = ReadWholeFile(paths.lrc, bytes);

        if (metaOk && lrcOk) {
            // 容忍 BOM：万一有人手工往缓存目录里丢了一份带 BOM 的 LRC
            if (bytes.size() >= 3 &&
                static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF) {
                bytes.erase(0, 3);
            }

            // 分隔线在这里也要补一次 —— **旧缓存里没有**。
            //
            // 缓存里的 .lrc 是**上一次写下的**，那段代码当时可能还没有补线的逻辑
            // （用户实测：「缓存到本地的歌词还没有第一行的分隔线」）。
            // 新写的缓存已经带了（SaveAndReturn 走的是同一个 text），
            // EnsureLeadInSeparator 幂等，所以这里对两种情况都安全。
            const std::string withLeadIn = EnsureLeadInSeparator(
                std::string(bytes.begin(), bytes.end()));

            std::wstring text = Utf8ToWide(withLeadIn.c_str());
            if (!text.empty()) {
                result.ok = true;
                result.fromCache = true;
                result.httpStatus = 200;
                result.lrcText = std::move(text);

                // 元数据从 .meta 原样还原 —— 缓存命中和首次查询看到的东西一致，
                // 调用方的核对逻辑不用分两条路写。
                result.matchedArtist = Utf8ToWide(meta.artistName.c_str());
                result.matchedTrack  = Utf8ToWide(meta.trackName.c_str());
                result.matchedAlbum  = Utf8ToWide(meta.albumName.c_str());
                result.matchedDuration = meta.hasDuration ? meta.duration : 0.0;
                result.fromSearch = fromSearch;

                OnlineLog("在线歌词：缓存命中（%s，%s）",
                          WideToUtf8(paths.lrc).c_str(),
                          fromSearch ? "模糊搜索来的，调用方需核对" : "精确查询");
                return result;
            }

            // .lrc 是合法文件但不是合法 UTF-8 —— 上次写到一半崩了之类的。
            OnlineLog("在线歌词：缓存里的歌词不是合法 UTF-8，整条条目作废并重新查询");
        } else if (metaOk != lrcOk) {
            // 半份条目（只有其中一个）。这种情况值得记一笔：
            // 正常情况下不该出现，出现就说明写入或删除有过异常。
            OnlineLog("在线歌词：缓存条目不完整（%s 缺失），作废并重新查询",
                      metaOk ? "歌词" : "元数据");
        }

        // 走到这里说明条目无效（不完整、坏掉、或者压根不存在）：
        // 把可能残留的半份文件清掉，别让它一直挡在前面。
        if (metaOk || lrcOk) {
            DeleteFileIfExists(paths.lrc);
            DeleteFileIfExists(paths.meta);
        }

        MissMarker marker;
        if (ReadMissMarker(paths.miss, marker)) {
            // 【先看逻辑版本】旧版本算出来的"没有"不算数 —— 见 kMissLogicVersion。
            // 不判这一条，修好匹配逻辑之后用户还得干等 6 天。
            if (marker.logic != kMissLogicVersion) {
                DeleteFileIfExists(paths.miss);
                OnlineLog("在线歌词：未命中标记是旧匹配逻辑（v%d，当前 v%d）写的，"
                          "已作废并重新联网查询",
                          marker.logic, kMissLogicVersion);
            } else {
                const long long ageSec = NowUnixSeconds() - marker.stamp;
                if (ageSec >= 0 && ageSec < kMissTtlSeconds) {
                    // 负结果缓存：连网都不联。
                    // fromCache=true 表示"这个结论来自缓存"，但 ok 依然是 false ——
                    // 这两个维度互不替代，头文件里写了。
                    result.ok = false;
                    result.fromCache = true;
                    result.httpStatus = marker.status;
                    result.error = L"缓存记录：在线源上都没有这首歌的歌词，还有 " +
                                   std::to_wstring((kMissTtlSeconds - ageSec) / 86400) +
                                   L" 天才会重新查询";
                    OnlineLog("在线歌词：命中未命中标记（%lld 秒前写入，HTTP %ld），跳过联网",
                              ageSec, marker.status);
                    return result;
                }

                // 过期了：删掉标记重新查。
                // 没有这一步，用户后来在 LRCLIB 上补了歌词也永远查不到。
                DeleteFileIfExists(paths.miss);
                OnlineLog("在线歌词：未命中标记已过期（%lld 秒前），重新联网查询", ageSec);
            }
        }
    }

    // ---- 2. 联网 ----
    const std::wstring artist = TrimWs(req.artist);
    const std::wstring title  = TrimWs(req.title);

    // 重试预算：整个这一轮（**所有源、所有接口**）共享一个墙钟截止时刻。
    // 只算一次，不给每个接口各算一份 —— 各算一份的话每个源都能重试，
    // 最坏耗时按源的数量翻倍。理由见 kTotalRetryBudgetMs 的说明。
    const ULONGLONG retryDeadline = GetTickCount64() + kTotalRetryBudgetMs;

    // 本轮出现过"没能确定答案"的请求（传输层失败，或者非 404 的异常状态码）。
    // 只要有，就**不写**未命中标记 —— 否则一次 503（实测 LRCLIB 会限流）
    // 或一次断网，就会让用户 7 天内再也查不到这首歌。
    //
    // 【注意这里的取舍】即使某个源成功返回了一个空数组（那本身是
    // 确定性结论），只要同一轮里有别的地方失败了，我们仍然**不写**标记。
    // 理由是"宁可下次重查，也不要记住一个可能是错的'没有'"：
    // 重查的代价是一次网络请求，记错的代价是用户 7 天看不到歌词。
    bool  anyRequestFailed = false;
    DWORD lastHttpStatus   = 0;

    // 把失败原因攒起来塞进 error，调用方要提示用户时能说清楚是网络问题
    // 还是服务器过载（两者的"稍后再试"含义不一样）。
    std::wstring failNotes;
    auto noteFailure = [&failNotes](const std::wstring& s) {
        if (!failNotes.empty()) failNotes += L"；";
        failNotes += s;
    };

    // ---- 2.0 网易云（排在 LRCLIB 前面）----
    //
    // 顺序是用户 2026-09-24 定的。依据是实测：用户曲库里大量同人曲 / OST
    // 在 LRCLIB 上一首都没有（8 次标题搜索 6 次返回 0 条），而网易云连
    // 「塞壬唱片-MSR - Battleplan Obliteration」都能精确命中。
    // LRCLIB 退居兵底，负责它更擅长的欧美曲目。
    //
    // 【它返回的结果 fromSearch 恒为 true】网易云这条路只有搜索接口可用
    // （没有 LRCLIB 那种 artist+title 精确查询），所以调用方**必须**核对。
    if (!title.empty()) {
        bool neteaseFailed = false;
        std::wstring neteaseNote;
        LrclibEntry ne;

        if (TryNetEase(req, retryDeadline, ne, neteaseFailed, neteaseNote, cancel)) {
            OnlineLog("在线歌词：网易云命中，采用该结果");
            return SaveAndReturn(paths, cacheUsable, ne, 200,
                                 /*fromSearch=*/true, "网易云搜索");
        }
        if (neteaseFailed) {
            anyRequestFailed = true;
            noteFailure(neteaseNote);
        }

        // 取消 -> 立刻收工。**这一条必须在"继续查 LRCLIB"之前**，
        // 而且绝不能让后面的写标记路径跑起来（见函数尾部的检查）。
        if (IsCancelled(cancel)) {
            OnlineLog("在线歌词：已取消，不再查 LRCLIB");
            result.cancelled = true;
            result.error = L"已取消（期间换过曲）";
            return result;
        }
        OnlineLog("在线歌词：网易云未命中，继续查 LRCLIB");
    }


    if (title.empty()) {
        // 这是调用方的输入问题（LRCLIB 的 track_name 必填，空着发必然 400），
        // 不是"这首歌没有歌词"，所以**不写**未命中标记。
        result.error = L"曲名为空，无法联网查询（各在线源的曲名都是必填项）";
        OnlineLog("在线歌词：%s", WideToUtf8(result.error).c_str());
        return result;
    }

    // 本轮出现过"没能确定答案"的请求 —— 声明已经提到网易云那一段之前了
    // （两个源要共用同一套失败记账），这里不再重复声明。

    // 2a. 精确查询。
    // 只有 artist 和 title 都齐了才发：缺 artist 会直接 400，
    // 白白多一次往返，还会把 400 记进日志里误导排查。
    if (!artist.empty()) {
        if (IsCancelled(cancel)) {
            OnlineLog("在线歌词：已取消，跳过 LRCLIB 精确查询");
            result.cancelled = true;
            result.error = L"已取消（期间换过曲）";
            return result;
        }
        const HttpReply exact = HttpGet(kHost, BuildExactPath(req), retryDeadline);

        if (!exact.transportOk) {
            anyRequestFailed = true;
            OnlineLog("在线歌词：/api/get 传输失败（已尝试 %d 次）：%s",
                      exact.attempts, WideToUtf8(exact.error).c_str());
            noteFailure(L"/api/get 网络错误：" + exact.error);
        } else {
            lastHttpStatus = exact.status;

            if (exact.status == 200) {
                LrclibEntry entry;
                if (ParseEntryRoot(exact.body, entry)) {
                    if (entry.hasSynced || entry.hasPlain) {
                        // fromSearch=false：这是精确查询（artist+title[+album+duration]
                        // 全部对上才返回 200），调用方可以放心直接用。
                        // （即便这样，matched* 也照样填好，调用方想再核一遍也行。）
                        return SaveAndReturn(paths, cacheUsable, entry, 200,
                                             /*fromSearch=*/false, "精确查询");
                    }
                    // 200 但两个歌词字段都是 null：多半是纯音乐条目。
                    // 不在这里下结论，继续走搜索 —— 同一首歌的另一个版本
                    // 可能是有词的。
                    OnlineLog("在线歌词：/api/get 命中 id=%.0f，但 syncedLyrics/"
                              "plainLyrics 都为空（instrumental=%d），改走搜索",
                              entry.id, entry.instrumental ? 1 : 0);
                } else {
                    OnlineLog("在线歌词：/api/get 返回 200，但响应体不是预期的对象形状"
                              "（前 200 字节：%s）", LogSafe(exact.body).c_str());
                    anyRequestFailed = true;
                    noteFailure(L"/api/get 响应形状异常");
                }
            } else if (exact.status == 404) {
                // 精确没命中，是确定的信息，但还不能下"LRCLIB 上没有"的结论 ——
                // 搜索接口可能还找得到（曲名/演唱者写法不一致时很常见）。
                OnlineLog("在线歌词：/api/get 未命中（404），改走搜索");
            } else {
                // 503 / 429 / 5xx 之类：服务器暂时不可用。
                // HttpGet 内部已经按退避策略重试过了（次数和总预算见
                // kMaxHttpAttempts / kTotalRetryBudgetMs），走到这里说明
                // 重试也没用。**当成本轮失败，一个字都不写进缓存** ——
                // 这是和"404 = 确实没有"最关键的区别。
                anyRequestFailed = true;
                OnlineLog("在线歌词：/api/get 返回 HTTP %lu（已尝试 %d 次），本轮不写缓存",
                          exact.status, exact.attempts);
                noteFailure(L"/api/get HTTP " + std::to_wstring(exact.status) +
                            L"（重试 " + std::to_wstring(exact.attempts) + L" 次）");
            }
        }
    } else {
        OnlineLog("在线歌词：没有演唱者信息，跳过 /api/get，直接搜索");
    }

    // 2b. 模糊搜索。
    {
        if (IsCancelled(cancel)) {
            OnlineLog("在线歌词：已取消，跳过 LRCLIB 模糊搜索");
            result.cancelled = true;
            result.error = L"已取消（期间换过曲）";
            return result;
        }
        const HttpReply search = HttpGet(kHost, BuildSearchPath(req), retryDeadline);

        if (!search.transportOk) {
            anyRequestFailed = true;
            OnlineLog("在线歌词：/api/search 传输失败（已尝试 %d 次）：%s",
                      search.attempts, WideToUtf8(search.error).c_str());
            noteFailure(L"/api/search 网络错误：" + search.error);
        } else {
            lastHttpStatus = search.status;

            if (search.status == 200) {
                std::vector<LrclibEntry> entries;
                if (ParseSearchRoot(search.body, entries)) {
                    OnlineLog("在线歌词：/api/search 返回 %zu 个候选", entries.size());

                    const LrclibEntry* best = PickBest(entries);
                    if (best != nullptr) {
                        // fromSearch=true：这是**模糊**接口，返回的可能压根不是
                        // 我们要找的那首歌（实测搜 "Best Wishes" 回来的是
                        // Duane Betts 的同名曲）。这个标记会一路传到调用方
                        // 和缓存里 —— 调用方看到它就**必须**核对 matched*。
                        return SaveAndReturn(paths, cacheUsable, *best, 200,
                                             /*fromSearch=*/true, "模糊搜索");
                    }
                    // 走到这里有两种情况：
                    //   a) 数组是空的 "[]"；
                    //   b) 有候选，但每一个的歌词字段都是 null（纯音乐）。
                    // 两种都是"LRCLIB 上确实没有这首歌的歌词"这个**确定**结论。
                    //
                    // 【注意】查不到东西时 /api/search 回的是 200 + "[]"，
                    // **不是** 404（实测）。把它当出错处理会导致永远查不到。
                    OnlineLog("在线歌词：/api/search 没有任何带歌词的候选");
                } else {
                    OnlineLog("在线歌词：/api/search 返回 200，但响应体不是预期的数组形状"
                              "（前 200 字节：%s）", LogSafe(search.body).c_str());
                    anyRequestFailed = true;
                    noteFailure(L"/api/search 响应形状异常");
                }
            } else if (search.status == 404) {
                OnlineLog("在线歌词：/api/search 返回 404");
            } else {
                anyRequestFailed = true;
                OnlineLog("在线歌词：/api/search 返回 HTTP %lu（已尝试 %d 次），本轮不写缓存",
                          search.status, search.attempts);
                noteFailure(L"/api/search HTTP " + std::to_wstring(search.status) +
                            L"（重试 " + std::to_wstring(search.attempts) + L" 次）");
            }
        }
    }

    // ---- 3. 收尾 ----
    result.httpStatus = static_cast<long>(lastHttpStatus);
    result.fromCache = false;
    result.ok = false;

    if (anyRequestFailed) {
        // 有请求没问出结果 -> **一个字都不写进缓存**，下次播放还会重试。
        //
        // 这一条是"查不到"和"查失败"的分界线，也是这个模块最重要的
        // 一个设计点：LRCLIB 实测会因为过载返回
        //   503 {"message":"The server is busy, please retry in a moment",
        //        "name":"ServerOverloaded","statusCode":503}
        // 如果把它也记成"这首歌没有歌词"，一次服务端抖动就能让用户
        // 7 天内再也查不到这首歌，而且日志里看不出任何异常。
        result.error = L"联网查询失败，本次不写缓存，下次会重试（" +
                       (failNotes.empty() ? std::wstring(L"未知原因") : failNotes) + L"）";
        OnlineLog("在线歌词：%s", WideToUtf8(result.error).c_str());
        return result;
    }

    // 所有该问的都问了（网易云 + LRCLIB）、都答了，就是没有歌词 —— 这是确定性结论。
    // 7 天有效期（kMissTtlSeconds）是刻意的：太短等于每次播放都白跑一趟网络，
    // 太长则用户后来补了歌词也查不到。
    //
    // ⚠️ 这个标记必须**所有源都问完**之后才写。加了网易云之后这一点变得关键：
    // 早先只有一个源时「LRCLIB 没有」就等于「没有」，现在要是不等网易云，
    // 一次 LRCLIB 的未命中就会把网易云也一起挡在门外 7 天。
    result.error = L"各在线源上都没有找到这首歌的歌词";

    // ★★★ 取消的最后一关，**绝对不能删** ★★★
    //
    // 上面每个检查点都是"提前返回"，但它们的覆盖是**分布式**的：将来谁在
    // 中间插一段不带检查点的联网动作，就会漏到这里。而这里一旦漏了，
    // 一次取消（用户随手切歌，完全无辜的操作）就会写下一个
    // **7 天有效**的"这首歌没有歌词"—— 用户再也查不到它，还会以为是源站的问题。
    //
    // 所以这一关是兜底：不管前面漏没漏，取消过的这一轮一律不下结论。
    if (IsCancelled(cancel)) {
        OnlineLog("在线歌词：本轮已取消 —— 不发结论、不写未命中标记（兜底检查点）");
        result.cancelled = true;
        result.error = L"已取消（期间换过曲）";
        return result;
    }

    // 把"我们当时拿什么去查的"一起记下来 —— 事后追查全靠它。
    // 用 %s 而不是直接拼宽字符：.miss 是 UTF-8 文本，和其它缓存文件一致。
    const std::string identity =
        "artist=[" + WideToUtf8(req.artist) + "] title=[" + WideToUtf8(req.title) +
        "] album=[" + WideToUtf8(req.album) + "] duration=" +
        std::to_string(static_cast<long long>(req.durationSec + 0.5)) + "s";

    if (cacheUsable && WriteMissMarker(paths.miss, lastHttpStatus, identity)) {
        OnlineLog("在线歌词：确定未命中，已写 7 天有效的标记（HTTP %lu）：%s",
                  lastHttpStatus, identity.c_str());
    } else {
        OnlineLog("在线歌词：确定未命中（HTTP %lu），但标记写入失败，下次仍会重查：%s",
                  lastHttpStatus, identity.c_str());
    }
    return result;
}

void FetchLyricOnlineAsync(const OnlineLyricRequest& req,
                           const std::wstring& cacheDir,
                           OnlineLyricCallback cb,
                           OnlineCancelFlag cancel) {
    // 【后台线程用的是 SDK 的机制，不是自己 CreateThread / std::thread】
    //
    // fb2k::splitTask 是 SDK 给"分离线程"的官方入口
    // （SDK/threadsLite.h:4，实现见 SDK/app_close_blocker.cpp:22），
    // 官方示例 foo_sample/ui_and_threads.cpp:116-121 就推荐用它。
    // 它内部会先 async_task_manager::acquire()，也就是说
    // **foobar2000 退出时会等这个任务跑完**才继续 ——
    // 这一点很重要：如果用裸 std::thread，foobar2000 可能在请求还没回来时
    // 就卸载了我们的 DLL，那个线程再回到已经消失的代码段上就是崩溃。
    // 代价是退出时最坏会多等一个网络超时（约 40 秒），这是刻意接受的取舍。
    //
    // 注意 splitTask 收的是 std::function<void()>（按值），
    // 所以 lambda 里**必须**只捕获可拷贝的东西 ——
    // 用 shared_ptr 包住请求/缓存目录/回调是为此，不是为了省拷贝。
    auto sharedReq  = std::make_shared<OnlineLyricRequest>(req);
    auto sharedDir  = std::make_shared<std::wstring>(cacheDir);
    auto sharedCb   = std::make_shared<OnlineLyricCallback>(std::move(cb));
    // 取消令牌本来就是 shared_ptr，直接按值捕获 —— 它必须比这个线程活得久，
    // 而调用方那边换曲时还要用它置位，所以不能是 unique。
    auto sharedCancel = cancel;

    fb2k::splitTask([sharedReq, sharedDir, sharedCb, sharedCancel] {
        // ---- 后台线程 ----
        OnlineLyricResult result;
        try {
            result = FetchLyricOnline(*sharedReq, *sharedDir, sharedCancel);
        } catch (const std::exception& e) {
            // FetchLyricOnline 本身不抛（std::bad_alloc 之类的极端情况除外），
            // 但 std::function 里的异常跑出去会直接 terminate 掉整个进程，
            // 所以这里必须兜住。
            result = OnlineLyricResult();
            result.error = L"查询过程中抛出异常：" + Utf8ToWide(e.what());
            OnlineLog("在线歌词：%s", WideToUtf8(result.error).c_str());
        } catch (...) {
            result = OnlineLyricResult();
            result.error = L"查询过程中抛出未知异常";
            OnlineLog("在线歌词：%s", WideToUtf8(result.error).c_str());
        }

        // ---- 投递回主线程 ----
        //
        // 用 fb2k::inMainThread（SDK/threadsLite.h:21，实现见
        // SDK/main_thread_callback.cpp:30）。它内部就是往
        // main_thread_callback_manager 里塞一个回调，保证 FIFO，
        // 且**可以从任何线程调用**。不用自己 PostMessage 到某个窗口：
        // 那要求调用方必须先给我们一个窗口句柄，而且窗口一销毁消息就丢了。
        //
        // core_api::are_services_available() 这一判是必要的：
        // 服务系统只在"非启动、非关闭"期间可用（core_api.h:16 的说明）。
        // 如果 foobar2000 正在退出，main_thread_callback_manager::get()
        // 会直接触发致命错误。此时**安静地丢掉结果**才是对的 ——
        // 反正主线程上的宿主对象多半也已经没了，回调过去也没人接。
        if (!core_api::are_services_available()) {
            OnlineLog("在线歌词：foobar2000 正在启动/关闭，结果不再投递到主线程");
            return;
        }

        fb2k::inMainThread([sharedReq, sharedCb, result] {
            // ---- 主线程 ----
            // 注意：**这里不检查宿主对象是否还活着** —— 本模块不知道谁是宿主，
            // 也没有它的指针。这个责任在调用方身上，头文件里写明了。
            if (*sharedCb) {
                (*sharedCb)(*sharedReq, result);
            }
        });
    });
}

} // namespace lyricus
