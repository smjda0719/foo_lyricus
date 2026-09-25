// ---------------------------------------------------------------------------
// 歌词搜索算法 —— 离线单测
//
// 直接跑**真实的 src/lyric_search.cpp**（用 shim 挡掉 SDK 依赖），
// 在临时目录里摆出真实文件，然后调 FindLyricFile() 验证各条策略。
//
// 为什么不靠播放器测：FindLyricFile 是个对文件系统的纯函数，
// 造静音 WAV 夹具再让播放器去播，既慢又会打断用户听歌。
// 这里毫秒级跑完全部策略，而且失败时能直接指出是哪一条。
// ---------------------------------------------------------------------------

#include "lyric_search.h"

#include <windows.h>   // FindLyricFile 本身不带它，测试要自己建目录/文件

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// lyric_search.cpp 用到的两个外部符号。单测不需要真日志，也不需要真转码。
//
// ⚠️ 必须定义在 lyricus 命名空间**里面** —— 它们在头文件里就是那么声明的。
// 放到全局会链接不上，报一长串修饰名看不懂的 LNK2019。
namespace lyricus {
void DebugLog(const char* fmt, ...) { (void)fmt; }
std::string WideToUtf8(const std::wstring&) { return std::string(); }
} // namespace lyricus

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}

bool HasCandidate(const std::vector<std::wstring>& v, const wchar_t* want) {
    return std::find(v.begin(), v.end(), std::wstring(want)) != v.end();
}

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

// 每个用例一个干净目录，互不干扰
std::wstring FreshDir(const wchar_t* name) {
    std::wstring d = ExeDir() + L"\\cases\\" + name;
    std::wstring parent = ExeDir() + L"\\cases";
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

lyricus::LyricSearchConfig DefaultCfg() {
    lyricus::LyricSearchConfig c;
    c.fuzzy   = true;
    c.useTags = true;
    return c;
}

void ShowHit(const wchar_t* label, const lyricus::LyricSearchHit& h) {
    std::printf("         %ls -> how=%ls score=%d\n", label,
                h.how.empty() ? L"(空)" : h.how.c_str(), h.score);
}

// ---------------------------------------------------------------------------

void TestCandidates() {
    std::printf("\n== MakeTitleCandidates（候选集合）==\n");

    // 真实案例：用户曲库里到处都是这个形态
    auto c1 = lyricus::MakeTitleCandidates(L"02 遗忘山丘");
    Check(HasCandidate(c1, L"遗忘山丘"), "「02 遗忘山丘」切出「遗忘山丘」");

    // 本题的原始案例
    auto c2 = lyricus::MakeTitleCandidates(L"塞壬唱片-MSR - Battleplan Obliteration");
    Check(HasCandidate(c2, L"Battleplan Obliteration"), "厂牌前缀案例：切出右侧曲名");
    Check(HasCandidate(c2, L"塞壬唱片-MSR"),            "厂牌前缀案例：也保留左侧");

    // D-027 的修复：在线核对要靠它把「周杰伦 - 晴天」对上「晴天」
    auto c3 = lyricus::MakeTitleCandidates(L"周杰伦 - 晴天");
    Check(HasCandidate(c3, L"晴天"), "「周杰伦 - 晴天」切出「晴天」（D-027）");

    // 数字护栏：以数字开头的曲名不能被切坏
    auto c4 = lyricus::MakeTitleCandidates(L"24K Magic");
    Check(!HasCandidate(c4, L"K Magic"), "「24K Magic」不被切成「K Magic」");

    // 【一个无法消除的歧义，如实记下来】
    // "7 Years" 这种**以数字开头的曲名**，和"第 7 轨，曲名 Years"在字面上
    // 完全一样，没有任何办法区分。所以这里刻意**两边都留成候选**。
    //
    // 为什么这样是安全的：精确匹配（100 分）永远压过去音轨号（88 分），
    // 所以磁盘上真有 '7 Years.lrc' 时不会错配。
    // 只有当磁盘上**真的存在**一个叫 'Years.lrc'（另一首歌的歌词）时才会误命中 ——
    // 这是这个策略的代价，可接受。
    auto c5 = lyricus::MakeTitleCandidates(L"7 Years");
    Check(HasCandidate(c5, L"7 Years"), "「7 Years」原样保留（优先）");
    Check(HasCandidate(c5, L"Years"),   "「7 Years」同时给出「Years」候选（歧义无法消除，两边都留）");

    // 各种音轨号写法
    Check(HasCandidate(lyricus::MakeTitleCandidates(L"02. 遗忘山丘"), L"遗忘山丘"), "「02. 遗忘山丘」");
    Check(HasCandidate(lyricus::MakeTitleCandidates(L"02-遗忘山丘"),  L"遗忘山丘"), "「02-遗忘山丘」");
    Check(HasCandidate(lyricus::MakeTitleCandidates(L"102 歌名"),     L"歌名"),     "「102 歌名」（三位数）");
}

void TestNormalize() {
    std::printf("\n== NormalizeLyricStem（归一化）==\n");

    Check(lyricus::NormalizeLyricStem(L"Song Title") != lyricus::NormalizeLyricStem(L"SongTitle"),
          "空白是折叠不是删除：'Song Title' != 'SongTitle'");

    Check(lyricus::NormalizeLyricStem(L"Ａ Ｂ") == lyricus::NormalizeLyricStem(L"a b"),
          "全角折叠 + 转小写");

    Check(lyricus::NormalizeLyricStem(L"Artist - Song") == L"artist song",
          "标点变空格而不是删掉：'Artist - Song' -> 'artist song'");
}

// 繁简折叠 —— D-034 的根因，以及 2026-09-25 标定时抓到的活例子。
//
// 出处：用户标签写繁体「純白P」、网易云写简体，归一化不折繁简 ->
// 演唱者闸一条都过不了。当时只把演唱者降级成软闸，症状绕开了、病根还在：
// 曲名硬闸 / 演唱者闸 / 本地文件名比对，三处都还在繁简上失明。
//
// 这个测试是**唯一能证明 LCMAP_SIMPLIFIED_CHINESE 在这台机器上真的可用**的地方 ——
// 要是哪天系统映射表没了，FoldToSimplified 会静默退化成"原样返回"，
// 匹配悄悄变回老样子，而没有任何报错。
void TestFoldSimplified() {
    std::printf("\n== 繁简折叠（NormalizeLyricStem 里做）==\n");

    // ---- 基本映射 ----
    Check(lyricus::NormalizeLyricStem(L"純白P") == lyricus::NormalizeLyricStem(L"纯白P"),
          "★「純白P」和「纯白P」归一化后相等");
    Check(lyricus::NormalizeLyricStem(L"愛") == lyricus::NormalizeLyricStem(L"爱"),
          "愛 -> 爱");
    Check(lyricus::NormalizeLyricStem(L"鋼琴") == lyricus::NormalizeLyricStem(L"钢琴"),
          "鋼琴 -> 钢琴");

    // ---- 折叠真的发生了（不是"两边都被折成同一个空串"这种假相等）----
    Check(lyricus::NormalizeLyricStem(L"純白P") == L"纯白p",
          "★ 折叠后的字面就是简体（防止「两边都变成空串」式的假通过）");

    // ---- 该动的地方动了，不该动的地方别动 ----
    Check(lyricus::NormalizeLyricStem(L"abc XYZ") == L"abc xyz",
          "ASCII 不受影响（只有大小写折叠）");
    Check(lyricus::NormalizeLyricStem(L"") == L"", "空串仍然是空串");
    Check(lyricus::NormalizeLyricStem(L"晴天") == L"晴天",
          "本来就是简体 -> 原样");

    // ---- 演唱者闸：这才是 D-034 那条实际被打回来的路径 ----
    Check(lyricus::ArtistNamesOverlap(L"純白P", L"纯白P"),
          "★ 演唱者闸现在认得出繁简写法是同一个人");
    Check(!lyricus::ArtistNamesOverlap(L"純白P", L"周杰伦"),
          "不相干的演唱者仍然不认（别折过头）");

    // ⚠️ **已知边界，别把这条当 bug 去"修"**：
    //    D-034 原案的两种写法是繁体「純白P」与简体「Soda纯白」——
    //    它们差的不只是繁简：一个多前缀 Soda、一个多后缀 P。
    //    折完是 `纯白p` vs `soda纯白`，谁也不包含谁，所以这里的答案仍然是 false。
    //
    //    这正是当初把演唱者**降级成软闸**而不是硬闸的原因：
    //    它本来就认不全，强求它认全只会误杀（D-034 就是这么把一整张专辑打死的）。
    //    真正挡翻唱的是**曲名硬闸**；演唱者只是"二者其一对上即可"里的那一个。
    //    要让它认全就得上别名表或模糊匹配，收益不大、误配风险不小，不做。
    Check(!lyricus::ArtistNamesOverlap(L"純白P", L"Soda纯白"),
          "★ 已知边界：繁简之外的差异（多前缀/后缀）仍然认不出 —— 靠曲名闸兜底");
}

void TestExtensions() {
    std::printf("\n== 多扩展名（目标里明确要求的一项）==\n");
    const auto cfg = DefaultCfg();

    // 大写 .LRC —— Windows 上文件名不区分大小写，但字典序/后缀判断很容易写死小写
    {
        auto d = FreshDir(L"ext_upper");
        Touch(d + L"\\song.wav");
        Touch(d + L"\\song.LRC");
        auto h = lyricus::FindLyricFile(d + L"\\song.wav", L"", L"", L"", cfg);
        ShowHit(L".LRC", h);
        Check(!h.path.empty(), "能找到大写 .LRC");
    }

    // .txt —— 很多人把歌词存成纯文本
    {
        auto d = FreshDir(L"ext_txt");
        Touch(d + L"\\song.wav");
        Touch(d + L"\\song.txt");
        auto h = lyricus::FindLyricFile(d + L"\\song.wav", L"", L"", L"", cfg);
        ShowHit(L".txt", h);
        Check(!h.path.empty(), "能找到 .txt");
    }

    // 同时存在时优先 .lrc —— 这条最容易写漏：
    // .txt 常常是**别的用途**的文本（说明、翻译稿），
    // 而 .lrc 才是带时间轴的歌词。两者都在时必须选 .lrc。
    {
        auto d = FreshDir(L"ext_priority");
        Touch(d + L"\\song.wav");
        Touch(d + L"\\song.lrc");
        Touch(d + L"\\song.txt");
        auto h = lyricus::FindLyricFile(d + L"\\song.wav", L"", L"", L"", cfg);
        ShowHit(L"优先级", h);
        Check(!h.path.empty() && h.path.find(L".lrc") != std::wstring::npos,
              ".lrc 与 .txt 同时存在时选 .lrc");
    }

    // 大小写混写
    {
        auto d = FreshDir(L"ext_mixed");
        Touch(d + L"\\song.wav");
        Touch(d + L"\\song.Lrc");
        auto h = lyricus::FindLyricFile(d + L"\\song.wav", L"", L"", L"", cfg);
        Check(!h.path.empty(), "能找到混写大小写的 .Lrc");
    }

    // 不支持的扩展名不能被当成歌词
    {
        auto d = FreshDir(L"ext_unsupported");
        Touch(d + L"\\song.wav");
        Touch(d + L"\\song.doc");
        Touch(d + L"\\song.pdf");
        auto h = lyricus::FindLyricFile(d + L"\\song.wav", L"", L"", L"", cfg);
        Check(h.path.empty(), ".doc/.pdf 不被当成歌词");
    }
}

void TestStrategies() {
    std::printf("\n== FindLyricFile（各条策略，真实文件）==\n");
    const auto cfg = DefaultCfg();

    // --- 精确 ---
    {
        auto d = FreshDir(L"exact");
        Touch(d + L"\\song.wav");
        Touch(d + L"\\song.lrc");
        auto h = lyricus::FindLyricFile(d + L"\\song.wav", L"", L"", L"", cfg);
        ShowHit(L"exact", h);
        Check(!h.path.empty() && h.how == L"exact" && h.score == 100, "精确命中 100 分");
    }

    // --- 去前缀（本题原始案例）---
    {
        auto d = FreshDir(L"prefix");
        Touch(d + L"\\塞壬唱片-MSR - Battleplan Obliteration.wav");
        Touch(d + L"\\Battleplan Obliteration.lrc");
        auto h = lyricus::FindLyricFile(d + L"\\塞壬唱片-MSR - Battleplan Obliteration.wav",
                                        L"", L"", L"", cfg);
        ShowHit(L"prefix-strip", h);
        Check(!h.path.empty() && h.how == L"prefix-strip" && h.score == 90, "去前缀命中 90 分");
    }

    // --- 去音轨号前缀（★ 本轮新增）---
    {
        auto d = FreshDir(L"tracknum");
        Touch(d + L"\\02 遗忘山丘.wav");
        Touch(d + L"\\遗忘山丘.lrc");
        auto h = lyricus::FindLyricFile(d + L"\\02 遗忘山丘.wav", L"", L"", L"", cfg);
        ShowHit(L"tracknum-strip", h);
        Check(!h.path.empty() && h.how == L"tracknum-strip" && h.score == 88,
              "去音轨号命中 88 分（用户曲库的通用形态）");
    }

    // --- 标签构造 ---
    {
        auto d = FreshDir(L"tagged");
        Touch(d + L"\\track01.wav");
        Touch(d + L"\\My Song.lrc");
        auto h = lyricus::FindLyricFile(d + L"\\track01.wav", L"The Band", L"My Song", L"", cfg);
        ShowHit(L"tagged", h);
        Check(!h.path.empty() && h.how == L"tagged" && h.score == 80, "标签构造命中 80 分");
    }

    // --- 关闭标签策略后应落空 ---
    {
        auto d = FreshDir(L"tagged_off");
        Touch(d + L"\\track01.wav");
        Touch(d + L"\\My Song.lrc");
        auto c2 = cfg;
        c2.useTags = false;
        auto h = lyricus::FindLyricFile(d + L"\\track01.wav", L"The Band", L"My Song", L"", c2);
        Check(h.path.empty(), "useTags=false 时标签策略整段跳过");
    }

    // --- 模糊 ---
    {
        auto d = FreshDir(L"fuzzy");
        Touch(d + L"\\Some Long Song Name.wav");
        Touch(d + L"\\Some Long Song Nam.lrc");   // 少一个字母
        auto h = lyricus::FindLyricFile(d + L"\\Some Long Song Name.wav", L"", L"", L"", cfg);
        ShowHit(L"fuzzy", h);
        Check(!h.path.empty() && h.how == L"fuzzy", "模糊命中（差一个字符）");
        Check(h.score >= 76 && h.score <= 95, "模糊分数落在 76..95（不与精确平权）");
    }

    // --- 关闭模糊后应落空 ---
    {
        auto d = FreshDir(L"fuzzy_off");
        Touch(d + L"\\Some Long Song Name.wav");
        Touch(d + L"\\Some Long Song Nam.lrc");
        auto c2 = cfg;
        c2.fuzzy = false;
        auto h = lyricus::FindLyricFile(d + L"\\Some Long Song Name.wav", L"", L"", L"", c2);
        Check(h.path.empty(), "fuzzy=false 时不做模糊");
    }

    // --- 找不到就返回空，不能瞎配 ---
    {
        auto d = FreshDir(L"negative");
        Touch(d + L"\\lonely.wav");
        Touch(d + L"\\完全不相干的文件.lrc");
        auto h = lyricus::FindLyricFile(d + L"\\lonely.wav", L"", L"", L"", cfg);
        Check(h.path.empty(), "毫无关系的歌词**不能**被配上（宁缺勿错）");
    }

    // --- extraDir 优先级低于同目录 ---
    {
        // 注意变量名不能叫 near / far —— 那是 <windows.h> 里的遗留宏，
        // 一包含 windows.h 就会被展开成空，报出莫名其妙的 "auto: 在=前没有声明变量"。
        auto nearDir = FreshDir(L"extra_near");
        auto farDir  = FreshDir(L"extra_far");
        Touch(nearDir + L"\\song.wav");
        Touch(nearDir + L"\\song.lrc");       // 同目录有一份
        Touch(farDir  + L"\\song.lrc");       // extraDir 也有一份
        auto c2 = cfg;
        c2.extraDir = farDir;
        auto h = lyricus::FindLyricFile(nearDir + L"\\song.wav", L"", L"", L"", c2);
        Check(h.path.find(L"extra_near") != std::wstring::npos,
              "同目录与 extraDir 都有时，优先同目录");
    }

    // --- extraDir 单独命中 ---
    {
        auto nearDir = FreshDir(L"extra_only_near");
        auto farDir  = FreshDir(L"extra_only_far");
        Touch(nearDir + L"\\song.wav");
        Touch(farDir  + L"\\song.lrc");       // 只有 extraDir 有
        auto c2 = cfg;
        c2.extraDir = farDir;
        auto h = lyricus::FindLyricFile(nearDir + L"\\song.wav", L"", L"", L"", c2);
        ShowHit(L"extraDir", h);
        Check(!h.path.empty() && h.path.find(L"extra_only_far") != std::wstring::npos,
              "extraDir 里能找到（分数减 5）");
        Check(h.score == 95, "extraDir 命中比同目录低 5 分（100-5）");
    }
}

} // namespace

int wmain() {
    std::printf("歌词搜索离线单测 —— 直接跑 src/lyric_search.cpp 的真实实现\n");
    std::printf("用例目录: %ls\\cases\n", ExeDir().c_str());

    TestCandidates();
    TestNormalize();
    TestFoldSimplified();
    TestExtensions();
    TestStrategies();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
