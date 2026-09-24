#include "stdafx.h"
#include "lyric_search.h"
#include "debug_log.h"
#include "lyric.h"   // WideToUtf8

#include <algorithm>
#include <cmath>
#include <cstdint>    // uint32_t：二元组打包（见 CharBigrams）
#include <cstring>    // _wcsicmp（MSVC 的 <wchar.h>/<cstring> 扩展名，显式写出不靠传递包含）
#include <cwctype>    // iswpunct / towlower（和 lyric.cpp 一样显式包含）
#include <utility>    // std::move
#include <vector>

// 本文件用的 Win32 声明（FindFirstFileW / GetFileAttributesW / _wcsicmp …）
// 全部来自 <windows.h>：它由 <helpers/foobar2000+atl.h> 经
// SDK/foobar2000-lite.h -> shared/shared.h:24 带进来。
// 和 lyric.cpp 里直接用 CreateFileW/ReadFile 是同一条路子，不另加 include。

namespace lyricus {
namespace {

// ---------------------------------------------------------------------------
// 路径 / 文件名工具
//
// 这几件事 playback_state.h 里的 FileNameOf / FileStemOf 也能做，为什么另写：
//   1. 这里要处理「末尾带分隔符」的目录（"D:\music\" 不能再补一个 '\'），
//      那套工具是给「完整文件路径」写的，没这个前提；
//   2. 搜索模块不想为几个字符串小工具依赖播放状态模块 —— 它最终是要给
//      批量场景（扫描整个库）复用的，不该拖上 metadb 那条线。
// ---------------------------------------------------------------------------

bool IsSep(wchar_t c) {
    return c == L'\\' || c == L'/';
}

bool IsSpaceChar(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'
        || c == 0x3000                       // 全角空格
        || c == 0x00A0 || c == 0x2007        // 不换行空格
        || c == 0x202F || c == 0x205F || c == 0xFEFF;   // 窄不换行空格 / 数学空格 / ZWNBSP
}

// 把首尾空白削掉。**只在构造候选名时用**：候选名是从标签和文件名里切出来的，
// 切点两侧常留空格，不削干净就会出现「明明有文件却对不上」。
// 注意它和归一化是两回事 —— 归一化里空白只折叠、不删除，见 NormalizeLyricStem。
std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpaceChar(s[b])) ++b;
    while (e > b && IsSpaceChar(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 末段之后的那个点才算扩展名（"a.b.c" 的扩展名是 "c"），
// 且点必须在最后一个分隔符之后（"D:\a.b\c" 没有扩展名）。
bool SplitExt(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    stem = name;
    ext.clear();
    const size_t dot   = name.find_last_of(L'.');
    const size_t slash = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || dot == 0) return false;   // 0：".lrc" 这种隐藏文件没有 stem
    if (slash != std::wstring::npos && dot < slash) return false;
    stem = name.substr(0, dot);
    ext  = name.substr(dot);
    return true;
}

std::wstring FileNameOfPath(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? path : path.substr(s + 1);
}

// 取目录，**保留末尾分隔符**（"D:\music\a.wav" -> "D:\music\"）。
// 保留而不是去掉，是因为拼候选路径时统一写 dir + leaf 就够了，
// 不必在每个调用点判断「这里该不该补一个反斜杠」。
std::wstring DirOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : path.substr(0, s + 1);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    if (IsSep(dir.back())) return dir + leaf;
    return dir + L"\\" + leaf;
}

// 到分隔符为止、去掉扩展名的文件名。
// 不用 playback_state.h 的 FileStemOf：那个不剥目录，这里传进来的可能是全路径。
std::wstring StemOfPath(const std::wstring& path) {
    std::wstring stem, ext;
    SplitExt(FileNameOfPath(path), stem, ext);
    return stem;
}

// 是不是「歌词文件」。用 _wcsicmp 而不是 std::tolower：
// 扩展名比较是纯 ASCII 语义，locale 相关的折叠在这里只会添乱。
bool HasLyricExt(const std::wstring& name) {
    std::wstring stem, ext;
    if (!SplitExt(name, stem, ext)) return false;
    return _wcsicmp(ext.c_str(), L".lrc") == 0 || _wcsicmp(ext.c_str(), L".txt") == 0;
}

// ---------------------------------------------------------------------------
// 真实文件判定
// ---------------------------------------------------------------------------

// 长路径要加 \\?\ 前缀，且前缀路径**只能用反斜杠**、不能有 . / ..
// （这两个限制来自 Win32 对 \\?\ 的约定，不是我们挑的）。
std::wstring MakeExtendedPath(const std::wstring& abs) {
    if (abs.size() >= 4 && abs.compare(0, 4, L"\\\\?\\") == 0) return abs;
    if (abs.size() >= 2 && IsSep(abs[0]) && IsSep(abs[1])) {
        return L"\\\\?\\UNC\\" + abs.substr(2);   // UNC 要写成 \\?\UNC\server\share
    }
    if (abs.size() >= 2 && abs[1] == L':') return L"\\\\?\\" + abs;
    return std::wstring();   // 相对路径没法安全扩展 —— 由调用方补全目录后再来
}

bool RealFileAttributes(const std::wstring& path, DWORD& attrs) {
    attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return true;

    // 失败可能只是「太长了」。GetFileAttributesW 在支持的 Windows 上
    // 内部会转 \\?\，转不了就报这个错 —— 我们自己再试一次带前缀的。
    const DWORD err = GetLastError();
    if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILENAME_EXCED_RANGE ||
        err == ERROR_INVALID_NAME) {
        const std::wstring ext = MakeExtendedPath(path);
        if (!ext.empty()) {
            attrs = GetFileAttributesW(ext.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) return true;
        }
    }
    return false;
}

// 「真实存在**且是文件**」。
//
// 排除项的取舍：
//   * 目录 —— 一个叫 "Song.lrc" 的**文件夹**会被「存在」骗过去，
//     真去读的时候再失败一次，所以在这里就挡住；
//   * 设备对象 —— 纯防御；
//   * 重解析点（符号链接 / junction）—— 本组件只需要「能读的普通文件」，
//     挡掉可以避免将来扫描整个库时被链接绕成死循环。
//     代价是「歌词目录本身是 junction」的用法不受支持，实测很少见。
bool IsRealFile(const std::wstring& path, bool* tooLong = nullptr) {
    if (path.empty()) return false;

    DWORD attrs = 0;
    if (!RealFileAttributes(path, attrs)) {
        if (tooLong != nullptr) {
            // 超长且系统不吃 \\?\ 时只能放弃 —— 但要说清楚是这个原因，
            // 免得后来的人以为匹配逻辑坏了（MAX_PATH 的坑当年就是这么找的）。
            *tooLong = !MakeExtendedPath(path).empty();
        }
        return false;
    }

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return false;

    const DWORD notAFile = FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT;
    return (attrs & notAFile) == 0;
}

// ---------------------------------------------------------------------------
// 归一化
//
// 目的只有一个：让「同一首歌的不同写法」落到同一个字符串上，
// 使相似度比较不再被排版噪音（全半角、空格、破折号、书名号）左右。
//
// 为什么保留 CJK 不转拼音：转拼音要么依赖词库（等于引第三方数据），
// 要么按单字硬转（"长" 读 chang 还是 zhang 无从判断），两者都会**制造**误配，
// 而我们最怕的就是误配。中文曲名的比较交给后面的字符二元组做，够用。
// ---------------------------------------------------------------------------

// 全角 ASCII（U+FF01..U+FF5E）折成半角；全角空格单独处理（它与 U+3000 同码）。
wchar_t FoldFullWidth(wchar_t c) {
    if (c >= 0xFF01 && c <= 0xFF5E) return static_cast<wchar_t>(c - 0xFEE0);
    if (c == 0x3000) return L' ';
    return c;
}

// 要丢掉的标点与符号。
//
// 有一点必须显式挡住：iswpunct 在 C locale 下对 '_'、'&'、'+'、'#'、'~' 也返回真，
// 光靠 default 分支会把它们一起删掉。这几个字符是有信息的 ——
// 它们常出现在团名和版本号里（"AC&DC"、"C++"、"Track_01"、"#1"），
// 删了会让不同的歌更容易撞到同一个归一化结果，正好和「宁可漏配不可误配」相反。
bool IsDroppablePunct(wchar_t c) {
    switch (c) {
        // 先豁免：这些不是「排版噪音」，是名字的一部分
        case L'_': case L'&': case L'+': case L'#': case L'~':
            return false;

        case L'-': case L'\'': case L'"': case L'`': case L'.': case L',':
        case L':': case L';': case L'!': case L'?': case L'/': case L'\\':
        case L'(': case L')': case L'[': case L']': case L'{': case L'}':
        case L'<': case L'>': case L'*': case L'|': case L'@':
        case L'^': case L'$': case L'%': case L'=':
            return true;
        // 中文全角标点与「有装饰性、无区分性」的符号
        case 0x3001: case 0x3002: case 0x300A: case 0x300B:   // 、。《》
        case 0x3008: case 0x3009: case 0x300C: case 0x300D:   // 〈〉「」
        case 0x3010: case 0x3011: case 0x2018: case 0x2019:   // 【】‘’
        case 0x201C: case 0x201D: case 0x2013: case 0x2014:   // “”–—
        case 0x2026: case 0x00B7: case 0x30FB:                // …·・
            return true;
        default:
            // 其余标点/符号一律丢掉（各语言的引号、破折号变体都落在这里）
            return iswpunct(c) != 0;
    }
}

} // namespace

std::wstring NormalizeLyricStem(const std::wstring& name) {
    std::wstring stem, ext;
    SplitExt(FileNameOfPath(name), stem, ext);

    std::wstring out;
    out.reserve(stem.size());
    bool pendingSpace = false;

    for (wchar_t raw : stem) {
        const wchar_t c = FoldFullWidth(raw);
        if (IsSpaceChar(c)) {
            // 空白只折叠成一个空格、并推迟到真的还有下一个字符时才落地，
            // 这样首尾空白自动消失。
            pendingSpace = !out.empty();
            continue;
        }
        if (IsDroppablePunct(c)) {
            // 标点整个删掉，但要保留「词与词之间的间隔」：
            // "Artist - Song" 必须变成 "artist song"，不能粘成 "artistsong"。
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) { out.push_back(L' '); pendingSpace = false; }
        out.push_back(static_cast<wchar_t>(towlower(c)));
    }
    return out;
}

namespace {

// ---------------------------------------------------------------------------
// 目录枚举（Win32，不用 std::filesystem —— 本工程全程没用它）
// ---------------------------------------------------------------------------

struct FileEntry {
    std::wstring leaf;       // 文件名，原样保留大小写：命中后要用它拼真实路径
    std::wstring normStem;   // 归一化后的 stem
    int          extRank = 0;// 0 = .lrc / 1 = .LRC / 2 = 其他（.txt 之类）
};

int LyricExtRank(const std::wstring& ext) {
    if (ext == L".lrc") return 0;
    if (ext == L".LRC") return 1;
    return 2;
}

// 枚举一个目录里的歌词文件。**每次搜索现枚举一次**，不做跨调用的缓存。
//
// 为什么不留缓存：目录内容会变（用户就是会一边听一边把 lrc 丢进去），
// 而函数级 static 缓存一旦建立就活到进程结束 —— 症状是「新放的歌词要重启
// 才认得」，这种 bug 现场极难联想到搜索模块。一次枚举的开销可以忽略，
// 正确性优先。
//
// 排序**必须**做：同目录下同时存在 Song.lrc 与 Song.txt 时，命中谁取决于
// FindNextFileW 的返回顺序 —— 那是文件系统的自由，换台机器就可能变。
// 同一首歌的搜索结果必须是可复现的，所以这里定死「.lrc 优先、其次 .LRC、
// 最后其余」，同档按文件名。
std::vector<FileEntry> ListLyricFiles(const std::wstring& dir) {
    std::vector<FileEntry> out;
    if (dir.empty()) return out;

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = JoinPath(dir, L"*");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // 目录不存在是常态（extraDir 没配、网络盘掉线），不刷日志
        return out;
    }

    do {
        const std::wstring leaf(fd.cFileName);
        if (leaf == L"." || leaf == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!HasLyricExt(leaf)) continue;

        FileEntry e;
        e.leaf = leaf;
        std::wstring stem, ext;
        SplitExt(leaf, stem, ext);
        e.normStem = NormalizeLyricStem(stem);
        e.extRank  = LyricExtRank(ext);
        out.push_back(std::move(e));

        // 上限是防呆：真有人把几千个 lrc 堆在音乐目录里。
        // 精确/标签/去前缀匹配与文件数量无关，只有模糊匹配会被这个上限影响。
        if (out.size() >= 8192) break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    std::stable_sort(out.begin(), out.end(),
                     [](const FileEntry& a, const FileEntry& b) {
                         if (a.extRank != b.extRank) return a.extRank < b.extRank;
                         return a.leaf < b.leaf;
                     });
    return out;
}

// 目录字符串要不要补 \\?\ 前缀
bool NeedsExtPrefix(const std::wstring& dir) {
    if (dir.size() >= 4 && dir.compare(0, 4, L"\\\\?\\") == 0) return false;
    if (dir.size() >= 2 && dir[1] == L':') return false;   // 盘符路径总是绝对路径
    return true;
}

// 用目录里**真实存在**的那份文件名拼出全路径，并最后确认一次存在性。
//
// 为什么不用「dir + 原候选名 + 扩展名」了事：候选名来自标签或切分，
// 大小写和标点都跟磁盘上的真实文件不一定一样。必须以磁盘上的名字为准。
bool BuildHitPath(const std::wstring& dir, const std::vector<FileEntry>& list,
                  size_t index, std::wstring& outPath) {
    if (index >= list.size()) return false;

    std::wstring stem, ext;
    SplitExt(list[index].leaf, stem, ext);   // ext 一定非空：HasLyricExt 已经筛过

    std::wstring dirForPath = dir;
    if (NeedsExtPrefix(dirForPath)) {
        const std::wstring extDir = MakeExtendedPath(dirForPath);
        if (!extDir.empty()) dirForPath = extDir;
    }

    outPath = JoinPath(dirForPath, stem + ext);
    return IsRealFile(outPath);   // 候选路径必须真的能打开，否则宁可不返回
}

// 当前最优结果。分数是唯一的取舍依据；同分时保留先到的 ——
// 调用顺序本身已经表达了策略优先级，不需要第二套比较规则。
struct Pick {
    bool         found = false;
    int          score = 0;
    std::wstring how;
    std::wstring path;
};

// 单次搜索的全部状态。
//
// 做成一个显式对象而不是散落各处的静态缓存/全局变量：搜索的生命周期就是
// 一次 FindLyricFile 调用，作用域一清二楚，也不会在两次搜索之间串味。
struct SearchContext {
    std::wstring audioDir;   // 末尾带分隔符
    std::wstring extraDir;   // 同上；空 = 不启用

    std::vector<FileEntry> same;    // audioDir 的歌词清单
    std::vector<FileEntry> extra;   // extraDir 的歌词清单
    bool extraScanned = false;      // extraDir 为空时不必枚举

    explicit SearchContext(const std::wstring& dir) : audioDir(dir) {
        same = ListLyricFiles(dir);
    }

    const std::vector<FileEntry>& ExtraList() {
        if (!extraScanned) {
            extraScanned = true;
            extra = ListLyricFiles(extraDir);
        }
        return extra;
    }

    // dirIndex: 0 = 同目录，1 = extraDir
    //
    // 在同档策略内**只认严格更高分**：调用方按可信度从高到低投候选名，
    // 先投的赢，不需要第二套比较规则。
    void Lookup(const std::vector<std::wstring>& names, int score,
                const wchar_t* how, int dirIndex, Pick& best) {
        const std::wstring& dir = (dirIndex == 0) ? audioDir : extraDir;
        if (dir.empty()) return;

        const std::vector<FileEntry>& list =
            (dirIndex == 0) ? same : ExtraList();

        for (const std::wstring& name : names) {
            const std::wstring key = NormalizeLyricStem(name);
            if (key.empty()) continue;

            size_t idx = list.size();
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].normStem == key) { idx = i; break; }
            }
            if (idx == list.size()) continue;

            // extraDir 是集中存放的备胎，不是原生位置：同类匹配一律低 5 分，
            // 让「同目录能找到」永远优先。
            const int finalScore = score - (dirIndex == 0 ? 0 : 5);
            if (finalScore <= best.score) continue;

            std::wstring path;
            if (!BuildHitPath(dir, list, idx, path)) continue;

            best.found = true;
            best.score = finalScore;
            best.how   = how;
            best.path  = std::move(path);
        }
    }

    // 每个策略都要往两个目录各投一次，所以封一层
    void LookupBoth(const std::vector<std::wstring>& names, int score,
                    const wchar_t* how, Pick& best) {
        Lookup(names, score, how, 0, best);
        Lookup(names, score, how, 1, best);
    }
};

// ---------------------------------------------------------------------------
// 候选名生成
// ---------------------------------------------------------------------------

// 音频 stem 形如 "X - Y" 时，把 Y 拿去当候选名 —— 实测最常见的一类摆放就是
// 「音频名 = 厂牌/艺术家 - 曲名，歌词名 = 曲名」。
//
// 【关键】` - ` 的左边既可能真是艺术家，也可能只是曲名的一部分，
// 所以两边都要试。右边比左边分高：右侧通常是曲名，左侧常常是厂牌。
//
// 有多个 ` - ` 时（"A - B - C"）**所有切分点都要试**：
// 曲名里带破折号一点都不稀奇（"Love - Reprise"），
// 只在第一个切分点动手的话，这种曲子永远匹配不上。
void CollectStrippedNames(const std::wstring& stem,
                          std::vector<std::wstring>& rightSide,
                          std::vector<std::wstring>& leftSide) {
    const std::wstring sep = L" - ";
    size_t pos = stem.find(sep);
    while (pos != std::wstring::npos) {
        const std::wstring right = Trim(stem.substr(pos + sep.size()));
        const std::wstring left  = Trim(stem.substr(0, pos));
        if (!right.empty()) rightSide.push_back(right);
        if (!left.empty())  leftSide.push_back(left);
        pos = stem.find(sep, pos + 1);   // +1 而不是 +sep.size()：让 "A - - B" 这种也能切开
    }
}

// 音频名常带**音轨号前缀**："02 遗忘山丘"、"02. 遗忘山丘"、"02 - 遗忘山丘"。
// 歌词文件通常只写曲名，所以这一段必须切掉再去找。
//
// 【为什么必须显式切一刀，不能指望模糊匹配兜住】
// 以 "02 遗忘山丘" vs "遗忘山丘" 为例：
//     前者二元组 {02, 2␣, ␣遗, 遗忘, 忘山, 山丘} 共 6 个
//     后者二元组 {遗忘, 忘山, 山丘}               共 3 个
//     交集 3  ->  Dice = 2*3/(6+3) = 0.667  <  阈值 0.80   ->  被拒
// 多出来的 "02 " 只占两个字符，却让二元组总数翻了一倍，把 Dice 稀释掉了。
// 实测用户的曲库整个是「NN 曲名.wav」这个形态，所以这条不是锦上添花。
//
// 分隔符必须是空格 / 点 / 横线 / 下划线 / 顿号之一，且数字最多 3 位。
// 否则 "24K Magic"、"7 Years" 这类**以数字开头的曲名**会被切坏。
void CollectTrackNumberStripped(const std::wstring& stem,
                                std::vector<std::wstring>& out) {
    size_t i = 0;
    while (i < stem.size() && i < 3 && iswdigit(stem[i]) != 0) ++i;
    if (i == 0 || i >= stem.size()) return;   // 开头没数字，或整串就是数字

    const wchar_t sep = stem[i];
    if (sep != L' ' && sep != L'.' && sep != L'-' && sep != L'_' && sep != L'、') return;

    const std::wstring rest = Trim(stem.substr(i + 1));
    if (!rest.empty()) out.push_back(rest);
}

// 归一化后重复的候选名没有意义，只会白扫一遍目录
void PushUnique(std::vector<std::wstring>& v, const std::wstring& candidate) {
    const std::wstring c = Trim(candidate);
    if (c.empty()) return;
    const std::wstring key = NormalizeLyricStem(c);
    if (key.empty()) return;   // 全是标点的名字，匹配不出任何东西
    for (const std::wstring& e : v) {
        if (NormalizeLyricStem(e) == key) return;
    }
    v.push_back(c);
}

// 标签构造的候选名，**按可信度排列**：
// artist - title 最可信（这正是各家歌词库的命名习惯）；单 title 次之；
// title - artist 最后（少见，但确实存在）。
void CollectTaggedNames(const std::wstring& artist, const std::wstring& title,
                        std::vector<std::wstring>& out) {
    if (!artist.empty() && !title.empty()) {
        PushUnique(out, artist + L" - " + title);
    }
    if (!title.empty()) PushUnique(out, title);
    if (!artist.empty() && !title.empty()) {
        PushUnique(out, title + L" - " + artist);
    }
}

// ---------------------------------------------------------------------------
// 模糊匹配
//
// 【算法选择】字符二元组上的 Dice 系数，外加一道「包含率」闸门。
//
// 为什么是它：
//   * 不依赖分词。中文曲名没有空格，token 方案必须退化到按字切，
//     而在单字层面 Dice 与二元组 Dice 的信息量差一个量级 —— 二元组
//     顺带编码了字序，「上海」与「海上」不会因为字面相同而被判为一样。
//   * 对插入/删除/替换都只有局部影响，不需要像编辑距离那样做全局对齐；
//     代价是 O(n) 的集合统计，而编辑距离是 O(n*m)。
//   * 长标题的相对惩罚天然较小，符合直觉：名字越长，多一个词越不重要。
//
// 为什么还要「包含率」那道闸门：
//   二元组 Dice 对**短串**系统性偏高。"Love" 与 "Love Me" 这类
//   「一边是另一边真前缀」的组合，光靠 Dice 阈值挡不住，
//   所以再加一条：短串的二元组必须有 85% 以上出现在长串里。
//   两条都过才接受。
//
// 阈值 0.80：0.7 会把 "Live" 与 "Live at Budokan" 放进来；
// 0.9 在很多正经的同曲异名（"Song (Remastered)" vs "Song"）上会漏。
// 之所以偏保守 —— 误配一首歌是持续可见的错误，漏配只是没显示，
// 这个不对称决定了「宁可返回空」。
// ---------------------------------------------------------------------------

constexpr double kFuzzyDiceThreshold = 0.80;
constexpr double kFuzzyContainGuard  = 0.85;

// 二元组的打包表示：高 16 位 = 前一个字符，低 16 位 = 后一个。
//
// 【为什么不用 std::wstring 存二元组】原来每个二元组是一个独立的 std::wstring ——
// 一个 20 字的曲名就是 19 次堆分配，之后还要排序一遍字符串。
// 打包成整数后整个相似度计算**一次堆分配都不需要**，排序也从比字符串变成比整数。
// （排序顺序变了不影响结果：set_intersection 只要求两侧用同一个序。）
using Bigram = uint32_t;

Bigram PackBigram(wchar_t a, wchar_t b) {
    return (static_cast<Bigram>(static_cast<uint16_t>(a)) << 16) |
            static_cast<Bigram>(static_cast<uint16_t>(b));
}

void CharBigrams(const std::wstring& s, std::vector<Bigram>& out) {
    out.clear();
    if (s.size() < 2) return;
    out.reserve(s.size() - 1);
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        out.push_back(PackBigram(s[i], s[i + 1]));
    }
}

// 两个**已排序**二元组序列的交集大小（多重集语义，与 std::set_intersection 一致）。
//
// 手写双指针而不是 set_intersection + back_inserter：后者还要再分配一个结果
// vector，而我们只要个数。
size_t IntersectionSize(const std::vector<Bigram>& a, const std::vector<Bigram>& b) {
    size_t i = 0, j = 0, n = 0;
    while (i < a.size() && j < b.size()) {
        if      (a[i] < b[j]) ++i;
        else if (b[j] < a[i]) ++j;
        else                  { ++n; ++i; ++j; }
    }
    return n;
}

// 一个查询串的模糊匹配器。
//
// 【为什么要做成对象】原来 FuzzySimilarity(query, candidate) 对每个候选文件都：
//   1. 把 query 的二元组重算一遍 —— 而 query 在整个目录扫描里根本不变，
//      等于一半计算量是纯重复；
//   2. 给每个二元组分配一个独立 std::wstring（20 字曲名 = 19 次堆分配），
//      再排序、再做一次 set_intersection 并再分配一个结果 vector。
// 实测（tests/harness/bench_search.cpp）模糊匹配占一次目录搜索的 **70%**：
// 3000 个歌词文件要 30ms、500 个要 7ms —— 而这些全发生在**换曲的主线程**上。
//
// 改法：二元组打包成 uint32（零分配），query 的二元组构造时算一次并排好序，
// 候选的二元组复用成员缓冲，交集只数个数。
// 相似度公式与阈值一字未改，所以命中结果与改前**逐条一致**。
class FuzzyMatcher {
public:
    explicit FuzzyMatcher(const std::wstring& query) : m_query(query) {
        CharBigrams(m_query, m_queryBigrams);
        std::sort(m_queryBigrams.begin(), m_queryBigrams.end());
    }

    // 返回 0..1 的相似度；没达标返回 0（也就是「不接受」）。
    double Match(const std::wstring& b) {
        if (m_query.empty() || b.empty()) return 0.0;
        if (m_query == b) return 1.0;

        if (m_query.size() < 2 || b.size() < 2) return 0.0;

        CharBigrams(b, m_candBigrams);
        std::sort(m_candBigrams.begin(), m_candBigrams.end());

        const double interSize = static_cast<double>(
            IntersectionSize(m_queryBigrams, m_candBigrams));
        if (interSize == 0.0) return 0.0;   // 一个二元组都不共享：肯定不是同一首

        const double dice = 2.0 * interSize /
            static_cast<double>(m_queryBigrams.size() + m_candBigrams.size());

    // ⚠️ 这里的括号**不是多余的**，别删。
    //    windef.h 把 min/max 定义成了宏，不加括号的话
    //    `std::min(a, b)` 会被预处理器拆成 `std::(((a)<(b))?(a):(b))`，
    //    报 error C2589「"(":"::"右边的非法标记」。
    //    本工程已经栽过两次（另一次在 lyrics_view.cpp），所以整个工程
    //    倾向手写比较；这里保留 std::min 语义，用括号挡住宏展开。
    const double shortSize = static_cast<double>(
        (std::min)(m_queryBigrams.size(), m_candBigrams.size()));
    const double contain   = interSize / shortSize;

    // 加 1e-9 是给浮点留个明确的容差意图，不是靠运气过阈值
    const bool passDice    = dice    + 1e-9 >= kFuzzyDiceThreshold;
    const bool passContain = contain + 1e-9 >= kFuzzyContainGuard;
    return (passDice && passContain) ? dice : 0.0;
    }

private:
    std::wstring        m_query;
    std::vector<Bigram> m_queryBigrams;   // 构造时算一次、排好序，全程复用
    std::vector<Bigram> m_candBigrams;    // 复用缓冲，避免每个候选文件重新分配
};

struct FuzzyPick {
    bool         found = false;
    int          score = 0;
    std::wstring path;
};

// 在一个目录里扫所有歌词文件做模糊匹配，返回分数最高且达标的一条。
FuzzyPick FuzzySearchDir(const std::wstring& dir, const std::vector<FileEntry>& list,
                         const std::wstring& query) {
    FuzzyPick best;
    if (dir.empty() || query.empty()) return best;

    FuzzyMatcher matcher(query);   // query 的二元组只在这里算一次

    double bestRatio = 0.0;
    for (size_t i = 0; i < list.size(); ++i) {
        const double r = matcher.Match(list[i].normStem);
        if (r <= bestRatio) continue;   // <= 让并列时更早（即排序更前）的那条胜出

        std::wstring path;
        if (!BuildHitPath(dir, list, i, path)) continue;

        bestRatio  = r;
        best.found = true;
        best.path  = std::move(path);
    }

    if (!best.found) return best;

    // 相似度只映射到 76..95，**刻意到不了 100**：
    // 模糊匹配再像，也不能跟精确命中平起平坐 —— 分数会写进日志，
    // 将来也可能展示给用户，两个档次混在一起就失去意义了。
    // 76 由阈值 0.80 * 95 得出，是这一档的实际下限。
    best.score = static_cast<int>(std::lround(bestRatio * 95.0));
    return best;
}

// 日志里要打完整路径，但 DebugLog 的正文缓冲只有 1024 字节，
// 两条 260 字符的路径在 UTF-8 下（最多 3 倍）就能把它撑爆，
// 而 vsnprintf 是**静默截断**的 —— 结果是最关键的那段路径看不见。
// 所以按长度裁一刀，保证「命中方式、分数、路径头部」一定打得出来；
// 保留头部而不是尾部：音乐库通常按艺术家/专辑分层，头部更有辨识度。
constexpr size_t kLogPathLimit = 160;

std::wstring ShortenForLog(const std::wstring& path) {
    if (path.size() <= kLogPathLimit) return path;
    return path.substr(0, kLogPathLimit) + L"...";
}

} // namespace

// ---------------------------------------------------------------------------

// 见 lyric_search.h 的说明。
//
// 直接复用上面匿名的 CollectStrippedNames / PushUnique，**不另写一套切法**：
// 两处要是切得不一样，就会出现「搜索按 A 切法找到了、核对按 B 切法又把它否掉」
// 这种自相矛盾，而且极难排查。
std::vector<std::wstring> MakeTitleCandidates(const std::wstring& name) {
    std::vector<std::wstring> out;

    const std::wstring base = Trim(name);
    if (base.empty()) return out;

    PushUnique(out, base);

    std::vector<std::wstring> right, left;
    CollectStrippedNames(base, right, left);
    for (const std::wstring& s : right) PushUnique(out, s);
    for (const std::wstring& s : left)  PushUnique(out, s);

    // 音轨号前缀也要切 —— 在线歌词的核对走的是同一份候选集合，
    // 漏了这一条，「02 遗忘山丘.wav」就核对不上「遗忘山丘」。
    std::vector<std::wstring> trackStripped;
    CollectTrackNumberStripped(base, trackStripped);
    for (const std::wstring& s : trackStripped) PushUnique(out, s);

    return out;
}

// ---------------------------------------------------------------------------

LyricSearchHit FindLyricFile(const std::wstring& audioPath,
                             const std::wstring& artist,
                             const std::wstring& title,
                             const std::wstring& album,
                             const LyricSearchConfig& cfg) {
    LyricSearchHit hit;   // path 为空 = 没找到

    // album 目前不参与构造候选名，但保留这个参数：歌词文件确实偶尔按
    // 「专辑 - 曲序 - 曲名」摆放，将来要加策略时不必再改接口、改所有调用点。
    (void)album;

    const std::wstring audioDir = DirOf(audioPath);
    const std::wstring stem     = StemOfPath(audioPath);

    if (audioDir.empty() || stem.empty()) {
        DebugLog("歌词搜索: 音频路径解析不出目录或文件名: %s",
                 WideToUtf8(ShortenForLog(audioPath)).c_str());
        return hit;
    }

    SearchContext ctx(audioDir);
    ctx.extraDir = Trim(cfg.extraDir);

    DebugLog("歌词搜索开始: 曲目=%s  标签=[%s] - [%s]  同目录=%s",
             WideToUtf8(ShortenForLog(stem)).c_str(),
             WideToUtf8(ShortenForLog(artist)).c_str(),
             WideToUtf8(ShortenForLog(title)).c_str(),
             WideToUtf8(ShortenForLog(audioDir)).c_str());

    Pick best;

    // --- 策略 1：精确（归一化后 stem 相等）-------------------------------
    // 用归一化而不是逐字节比较，是为了吃掉「空格/全半角/破折号写法」的差异；
    // 但这一步仍然是 100 分，因为它对的是同一串字符，不存在猜的成分 ——
    // 也正是本题那个失败案例之外、最该先命中的情况。
    {
        std::vector<std::wstring> names;
        names.push_back(stem);
        ctx.LookupBoth(names, 100, L"exact", best);
    }

    // --- 策略 2：去前缀 --------------------------------------------------
    // 音频名带「厂牌 - 」/「艺术家 - 」前缀，歌词名却是干净的曲名。
    {
        std::vector<std::wstring> rightSide, leftSide;
        CollectStrippedNames(stem, rightSide, leftSide);
        if (!rightSide.empty()) ctx.LookupBoth(rightSide, 90, L"prefix-strip", best);
        if (!leftSide.empty())  ctx.LookupBoth(leftSide,  85, L"prefix-strip", best);
    }

    // --- 策略 2.5：去音轨号前缀 ------------------------------------------
    // 「02 遗忘山丘.wav」配「遗忘山丘.lrc」。
    // 分数夹在「去前缀」的 90/85 之间：它和去前缀是同一类确定性操作
    //（都是"用户起的名字多了一截"，不是猜），但比「厂牌 - 曲名」少见一点。
    {
        std::vector<std::wstring> trackStripped;
        CollectTrackNumberStripped(stem, trackStripped);
        if (!trackStripped.empty()) ctx.LookupBoth(trackStripped, 88, L"tracknum-strip", best);
    }

    // --- 策略 3：标签构造（cfg.useTags 为假时整段跳过）-------------------
    if (cfg.useTags) {
        std::vector<std::wstring> names;
        CollectTaggedNames(Trim(artist), Trim(title), names);
        if (!names.empty()) ctx.LookupBoth(names, 80, L"tagged", best);
    }

    // --- 策略 4：模糊（只在前面全落空时才做，且必须达标）-----------------
    //
    // 已经命中精确/去前缀/标签时不再模糊：那些结果的依据是「名字对得上」，
    // 模糊的依据只是「看起来像」，没有理由用它去覆盖前者。
    if (!best.found && cfg.fuzzy) {
        const std::wstring query = NormalizeLyricStem(stem);

        const FuzzyPick inSame  = FuzzySearchDir(ctx.audioDir, ctx.same, query);
        const FuzzyPick inExtra = FuzzySearchDir(ctx.extraDir, ctx.ExtraList(), query);

        // 同目录与 extraDir 各取一条再比大小；同分时同目录赢，
        // 因为 extraDir 是备胎目录，不是原生位置。
        const FuzzyPick* winner = nullptr;
        int winnerScore = 0;
        if (inSame.found) {
            winner = &inSame;
            winnerScore = inSame.score;
        }
        if (inExtra.found && inExtra.score - 5 > winnerScore) {
            winner = &inExtra;
            winnerScore = inExtra.score - 5;
        }
        if (winner != nullptr) {
            best.found = true;
            best.score = winnerScore;
            best.how   = L"fuzzy";
            best.path  = winner->path;
        }
    }

    if (best.found) {
        hit.path  = best.path;
        hit.score = best.score;
        hit.how   = best.how;
        DebugLog("歌词搜索命中: 方式=%s  分数=%d  路径=%s",
                 WideToUtf8(hit.how).c_str(), hit.score,
                 WideToUtf8(ShortenForLog(hit.path)).c_str());
    } else {
        DebugLog("歌词搜索未命中: %s", WideToUtf8(ShortenForLog(stem)).c_str());
    }
    return hit;
}

namespace {

// 按单个分隔符切开，**保留原始字符不做归一化**。
// 归一化会连分隔符一起剔掉，所以顺序必须是"先切后归一化"。
std::vector<std::wstring> SplitRaw(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    for (;;) {
        const size_t at = s.find(sep, pos);
        if (at == std::wstring::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, at - pos));
        pos = at + 1;
    }
    return out;
}

} // namespace

// 见 lyric_search.h 的说明。
bool IsPlaceholderTag(const std::wstring& s) {
    const std::wstring t = Trim(s);
    if (t.empty()) return true;   // 空值同样等于"没有信息"

    static const wchar_t* kPlaceholders[] = {
        L"?", L"??", L"-", L"—",
        L"未知艺术家", L"未知", L"佚名", L"群星",
        L"Unknown Artist", L"Unknown", L"Various Artists",
    };
    for (const wchar_t* p : kPlaceholders) {
        if (_wcsicmp(t.c_str(), p) == 0) return true;
    }
    return false;
}

// 见 lyric_search.h 的说明。
//
// 直接复用匿名的 CollectTrackNumberStripped，**不另写一套切法** ——
// 两处切得不一样就会出现「搜索按 A 切、核验按 B 切」的自相矛盾。
// 那个函数自带「数字最多 3 位 + 必须跟分隔符」的保护，
// 所以 "24K Magic" / "7 Years" 这类以数字开头的曲名不会被切坏。
std::wstring StripLeadingTrackNumber(const std::wstring& name) {
    std::vector<std::wstring> stripped;
    CollectTrackNumberStripped(Trim(name), stripped);
    return stripped.empty() ? name : stripped.front();
}

// 见 lyric_search.h 的说明。
bool ArtistNamesOverlap(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = NormalizeLyricStem(a);
    const std::wstring nb = NormalizeLyricStem(b);
    if (na.empty() || nb.empty()) return false;

    // 整体有一方包含另一方（`Soda纯白` vs `純白P` 这种繁简差异兜不住，
    // 但 `阿良良木健` 落在 `苍十三阿良良木健` 里是靠这个抓到的）
    if (na.find(nb) != std::wstring::npos) return true;
    if (nb.find(na) != std::wstring::npos) return true;

    // 两边都可能写多个人，按常见分隔符切开逐个比。
    // ⚠️ 必须**先按原始串切、再归一化** —— 归一化会把标点（含分隔符）剔掉，
    //    反过来的话切分就成了空操作（这个坑在 D-033 踩过一次）。
    static const wchar_t kSeps[] = { L'/', L'、', L',', L'，', L'&', L';', L'；' };
    std::vector<std::wstring> pa, pb;
    for (wchar_t sep : kSeps) {
        for (const std::wstring& s : SplitRaw(a, sep)) {
            const std::wstring n = NormalizeLyricStem(s);
            if (!n.empty()) pa.push_back(n);
        }
        for (const std::wstring& s : SplitRaw(b, sep)) {
            const std::wstring n = NormalizeLyricStem(s);
            if (!n.empty()) pb.push_back(n);
        }
    }

    for (const std::wstring& x : pa) {
        for (const std::wstring& y : pb) {
            if (x.find(y) != std::wstring::npos || y.find(x) != std::wstring::npos) return true;
        }
    }
    return false;
}

} // namespace lyricus
