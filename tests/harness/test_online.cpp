// ---------------------------------------------------------------------------
// 在线歌词模块 —— 离线单测（重点：手写 JSON 解析器）
//
// 【为什么敢 #include 一个 .cpp】
// online_lyric.cpp 里所有解析函数都在**匿名命名空间**里，外部链接不到。
// 而测试台是把源码**逐字节拷到临时目录**再编译的，所以这里直接
// `#include "online_lyric.cpp"` 就等于把它们并进同一个翻译单元，
// 匿名命名空间的名字立刻可见 —— 测的还是**同一份源码**，不是副本。
//
// 【为什么只测这一块】
// 整个文件只有 6 个 SDK 符号（core_api 三个 + fb2k 两个 + PFC_ASSERT），
// 其余全是标准库和 Win32。shim 把那 6 个挡掉之后，
// JSON 解析 / 百分号编码 / 缓存键这些**纯逻辑**就能离线跑，
// 不必联网、不必起线程。
//
// 手写 JSON 解析是全工程风险最高的手写代码 —— 它要正确处理
// \n 转义、转义引号、\uXXXX 代理对、null 字面量。任何一处判错，
// 结果都是"歌词看起来拿到了、其实是一堆 \n 字面量"或"整屏乱码"。
// ---------------------------------------------------------------------------

#include "online_lyric.cpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// online_lyric.cpp 与 lyric.cpp 都要用（后者提供 Utf8ToWide / WideToUtf8 的真实实现）
namespace lyricus {
void DebugLog(const char* fmt, ...) { (void)fmt; }
} // namespace lyricus

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}

int CountChar(const std::string& s, char c) {
    int n = 0;
    for (char x : s) if (x == c) ++n;
    return n;
}

// 解析一个字面量形式的 JSON 字符串（入参必须自带首尾引号）
std::string Unescape(const std::string& literal) {
    std::string out;
    size_t end = 0;
    if (!lyricus::ParseJsonString(literal, 0, out, end)) return "<FAILED>";
    return out;
}

// ---------------------------------------------------------------------------

void TestJsonString() {
    std::printf("\n== JSON 字符串反转义 ==\n");

    // \n —— 最关键的一条。LRC 靠换行分行，而 JSON 里的换行是字面两字符。
    // 不反转义的话，拿到的是一整行挤在一起、中间夹着 "\n" 字面量的废文本。
    {
        const std::string out = Unescape("\"a\\nb\"");
        Check(out == "a\nb", "\\n -> 真换行（不是字面两字符）");
        Check(CountChar(out, '\n') == 1, "且只有一个换行");
    }

    // \" —— 歌词里出现英文双引号时必然遇到。
    // 找结束引号如果只是 find('"')，会在这里提前截断。
    {
        const std::string out = Unescape("\"say \\\"hi\\\"\"");
        Check(out == "say \"hi\"", "\\\" 不会提前结束字符串");
    }

    // \\ 与 \/
    Check(Unescape("\"a\\\\b\"") == "a\\b", "\\\\ -> 单个反斜杠");
    Check(Unescape("\"a\\/b\"") == "a/b",   "\\/ -> 斜杠");

    // 制表 / 回车
    Check(Unescape("\"a\\tb\"") == "a\tb", "\\t -> 制表符");
    Check(Unescape("\"a\\rb\"") == "a\rb", "\\r -> 回车");

    // \uXXXX 基本平面
    {
        const std::string out = Unescape("\"\\u4e2d\"");
        Check(out == "\xE4\xB8\xAD", "\\u4e2d -> UTF-8 的「中」");
    }

    // \uXXXX 代理对 —— 非 BMP 字符（emoji）在 JSON 里是两个 4 位码元，
    // 必须合成一个码点再编码。各自单独编码会得到非法 UTF-8。
    {
        const std::string out = Unescape("\"\\uD83D\\uDE00\"");
        Check(out == "\xF0\x9F\x98\x80", "代理对 \\uD83D\\uDE00 -> UTF-8 的 U+1F600");
        Check(out.size() == 4, "代理对编码成 4 字节（不是两个 3 字节）");
    }

    // 中文原样发 UTF-8（LRCLIB 现在的实际做法）
    {
        const std::string out = Unescape("\"晴天\"");
        Check(out == "晴天", "未转义的中文原样保留");
    }

    // 截断的输入不能崩，也不能谎报成功
    Check(Unescape("\"abc") == "<FAILED>", "没有收尾引号 -> 解析失败");
    Check(Unescape("\"abc\\") == "<FAILED>", "反斜杠结尾（响应被截断）-> 失败");
    Check(Unescape("\"") == "<FAILED>", "只有一个引号 -> 失败");
}

void TestParseEntryObject() {
    std::printf("\n== /api/get 的对象响应 ==\n");

    // 真实形状：plainLyrics 为 null，syncedLyrics 有值
    const std::string body =
        R"({"id":17788,"name":"晴天","trackName":"晴天","artistName":"周杰伦",)"
        R"("albumName":"叶惠美","duration":270.0,"instrumental":false,)"
        R"("plainLyrics":null,"syncedLyrics":"[00:29.36] 故事的小黃花\n[00:32.77] 從出生那年就飄著\n"})";

    lyricus::LrclibEntry e;
    Check(lyricus::ParseEntryRoot(body, e), "解析成功");

    Check(e.hasId && std::fabs(e.id - 17788.0) < 0.5, "id 解析为数字");
    Check(e.hasDuration && std::fabs(e.duration - 270.0) < 0.01, "duration 解析为数字");
    Check(e.artistName == "周杰伦", "artistName（中文原样）");
    Check(e.trackName == "晴天",   "trackName");
    Check(e.albumName == "叶惠美", "albumName");

    Check(e.hasSynced, "syncedLyrics 被标记为存在");
    Check(!e.hasPlain, "plainLyrics 为 null -> **不算**存在");
    Check(CountChar(e.synced, '\n') == 2, "syncedLyrics 里是两个**真换行**");
    Check(e.synced.find("[00:29.36]") == 0, "首行时间戳完好");
    Check(e.synced.find("故事的小黃花") != std::string::npos, "中文歌词完好");

    Check(!e.fromSearch, "真实响应里没有 fromSearch 键 -> 恒为 false");
}

void TestParseSearchArray() {
    std::printf("\n== /api/search 的数组响应 ==\n");

    // 三个候选：第一个无歌词、第二个只有纯文本、第三个有 syncedLyrics
    const std::string body =
        R"([{"id":1,"trackName":"A","artistName":"X","albumName":"","duration":100.0,)"
        R"("instrumental":true,"plainLyrics":null,"syncedLyrics":null},)"
        R"({"id":2,"trackName":"B","artistName":"Y","albumName":"","duration":200.0,)"
        R"("instrumental":false,"plainLyrics":"没有时间轴","syncedLyrics":null},)"
        R"({"id":3,"trackName":"C","artistName":"Z","albumName":"","duration":300.0,)"
        R"("instrumental":false,"plainLyrics":null,"syncedLyrics":"[00:01.00] c\n"}])";

    std::vector<lyricus::LrclibEntry> v;
    Check(lyricus::ParseSearchRoot(body, v), "解析成功");
    Check(v.size() == 3, "三个候选都解析出来");

    if (v.size() == 3) {
        Check(v[0].instrumental, "instrumental 布尔值解析正确");
        Check(!v[0].hasSynced && !v[0].hasPlain, "第一项两个歌词字段都是 null");

        Check(v[1].hasPlain && !v[1].hasSynced, "第二项只有纯文本");
        Check(v[2].hasSynced, "第三项有 syncedLyrics");

        const lyricus::LrclibEntry* best = lyricus::PickBest(v);
        Check(best != nullptr && std::fabs(best->duration - 300.0) < 0.01,
              "PickBest 挑中有 syncedLyrics 的那一项");
    }

    // 空数组：LRCLIB 查不到东西时就是 `[]` + 200，不是 404
    {
        std::vector<lyricus::LrclibEntry> empty;
        Check(lyricus::ParseSearchRoot("[]", empty), "空数组解析成功");
        Check(empty.empty(), "空数组 -> 零个候选");
        Check(lyricus::PickBest(empty) == nullptr, "空候选集 -> PickBest 返回 nullptr");
    }
}

void TestOddJson() {
    std::printf("\n== 畸形 JSON 不能崩、也不能谎报成功 ==\n");

    lyricus::LrclibEntry e;
    Check(!lyricus::ParseEntryRoot("", e),        "空响应 -> 失败");
    Check(!lyricus::ParseEntryRoot("{", e),       "只有左花括号 -> 失败");
    Check(!lyricus::ParseEntryRoot("null", e),    "字面量 null -> 失败");
    Check(!lyricus::ParseEntryRoot("[]", e),      "数组喂给对象解析 -> 失败");

    std::vector<lyricus::LrclibEntry> v;
    Check(!lyricus::ParseSearchRoot("{}", v),     "对象喂给数组解析 -> 失败");
    Check(!lyricus::ParseSearchRoot("", v),       "空响应 -> 失败");

    // 未知键要能跳过，而不是卡住
    const std::string weird =
        R"({"unknownKey":{"nested":[1,2,{"deep":true}]},"trackName":"T",)"
        R"("duration":5.0,"syncedLyrics":"x"})";
    lyricus::LrclibEntry e2;
    Check(lyricus::ParseEntryRoot(weird, e2), "含未知嵌套键仍能解析");
    Check(e2.trackName == "T", "未知键被正确跳过，后面的字段照常读到");
}

void TestPercentEncode() {
    std::printf("\n== 百分号编码（中文不编码会直接 400）==\n");

    Check(lyricus::PercentEncode(L"abc") == L"abc", "字母原样");
    Check(lyricus::PercentEncode(L"a b") == L"a%20b", "空格 -> %20");
    Check(lyricus::PercentEncode(L"a/b") == L"a%2Fb", "斜杠必须编码（否则会跑出查询串）");
    Check(lyricus::PercentEncode(L"a&b") == L"a%26b", "& 必须编码（否则会被当成参数分隔符）");

    // 周杰伦 = E5 91 A8 E6 9D B0 E4 BC A6
    Check(lyricus::PercentEncode(L"周杰伦") == L"%E5%91%A8%E6%9D%B0%E4%BC%A6",
          "中文按 UTF-8 逐字节编码");
}

void TestCacheKey() {
    std::printf("\n== 缓存键 ==\n");

    lyricus::OnlineLyricRequest a;
    a.artist = L"周杰伦"; a.title = L"晴天"; a.durationSec = 270.4;

    lyricus::OnlineLyricRequest b = a;
    b.durationSec = 270.2;    // 同一秒内 -> 应当算出同一个键

    Check(lyricus::MakeCacheKey(a) == lyricus::MakeCacheKey(b),
          "时长四舍五入到秒：270.4 与 270.2 同键");

    lyricus::OnlineLyricRequest c = a;
    c.durationSec = 271.9;    // 不同秒 -> 不同键
    Check(lyricus::MakeCacheKey(a) != lyricus::MakeCacheKey(c),
          "不同秒 -> 不同键");

    // 大小写/空白差异应当归一到同一个键
    lyricus::OnlineLyricRequest d;
    d.artist = L" 周杰伦 "; d.title = L"晴天"; d.durationSec = 270.4;
    Check(lyricus::MakeCacheKey(a) == lyricus::MakeCacheKey(d),
          "首尾空白被归一化 -> 同键");

    // 时长未知时也要有稳定键，不能和"时长为 0 秒"混起来
    lyricus::OnlineLyricRequest e;
    e.artist = L"周杰伦"; e.title = L"晴天"; e.durationSec = 0.0;
    Check(!lyricus::MakeCacheKey(e).empty(), "时长未知也能算出键");
    Check(lyricus::MakeCacheKey(e) != lyricus::MakeCacheKey(a), "时长未知与 270 秒不同键");
}

// ---------------------------------------------------------------------------
// 网易云（第二个在线源）
//
// 这里测的三块都是**会静默出错**的地方：
//   * 搜索结果解析 —— 形状比 LRCLIB 复杂（artists 是数组、album 是嵌套对象），
//     读错位就会拿到别的字段，而且看起来「有结果」；
//   * 候选核验     —— 实测搜「周杰伦 晴天」首条是**翻唱**，闸门失效就会配错词；
//   * 名单剥离     —— **最危险**：多剥一行就是吃掉一句真歌词。
//     所以既有"必须剥掉"的用例，也有"绝不能剥"的用例。
// ---------------------------------------------------------------------------

void TestNetEaseSearchParse() {
    std::printf("\n== 网易云搜索响应解析 ==\n");

    // 真实形状（照 2026-09-24 实测的响应裁剪，字段顺序保持原样）
    //
    // ⚠️ 原始字符串必须用**自定义分隔符** R"J(...)J"。
    //    默认的 )" 会被曲名里的「(Instrumental)」撞上 ——
    //    `(Instrumental)",` 中间的 `)"` 就是终止符，字符串会在那儿截断，
    //    报的错是 C2001「字符串字面量中的换行符」，看不出真正原因。
    const std::string body =
        R"J({"result":{"songs":[{"id":3397872556,"name":"Battleplan Obliteration",)J"
        R"J("artists":[{"id":1,"name":"塞壬唱片-MSR"},{"id":2,"name":"VANTA万塔"}],)J"
        R"J("album":{"id":9,"name":"危机合约涤墨作战OST","picId":0},"duration":244800,)J"
        R"J("fee":0},{"id":3397872558,"name":"Battleplan Obliteration (Instrumental)",)J"
        R"J("artists":[{"id":1,"name":"塞壬唱片-MSR"}],"album":{"id":9,"name":"危机合约涤墨作战OST"},)J"
        R"J("duration":244800}],"songCount":2},"code":200})J";

    std::vector<lyricus::NetEaseSong> songs;
    Check(lyricus::ParseNetEaseSearchRoot(body, songs), "解析成功");
    Check(songs.size() == 2, "拿到 2 个候选");

    if (songs.size() == 2) {
        Check(songs[0].hasId && songs[0].id == 3397872556.0, "id 正确");
        Check(songs[0].name == "Battleplan Obliteration", "曲名正确");
        Check(songs[0].artist == "塞壬唱片-MSR/VANTA万塔",
              "artists 数组拼成 A/B");
        Check(songs[0].album == "危机合约涤墨作战OST", "album.name 取到（嵌套对象）");
        Check(songs[0].durationMs == 244800.0, "duration 是毫秒原值");

        // 第二个候选的 name 不能被第一个的污染 —— 嵌套对象处理错就会串味
        Check(songs[1].name == "Battleplan Obliteration (Instrumental)",
              "第二个候选曲名没串味");
        Check(songs[1].artist == "塞壬唱片-MSR", "第二个候选演唱者没串味");
    }

    // 无结果时网易云**根本不带 songs 键** —— 必须返回 false（= 确定的"没有"），
    // 而不是崩掉或者返回一堆空条目。
    std::vector<lyricus::NetEaseSong> none;
    Check(!lyricus::ParseNetEaseSearchRoot(R"({"result":{},"code":200})", none),
          "没有 songs 键 -> 返回 false（确定性未命中）");
    Check(none.empty(), "没有 songs 键 -> 结果为空");

    Check(!lyricus::ParseNetEaseSearchRoot("", none), "空响应体不崩");
    Check(!lyricus::ParseNetEaseSearchRoot("{", none), "截断的 JSON 不崩");
}

void TestNetEaseLyricParse() {
    std::printf("\n== 网易云歌词响应解析 ==\n");

    const std::string body =
        R"({"sgc":false,"sfy":false,"qfy":false,)"
        R"("lrc":{"version":32,"lyric":"[00:29.260]故事的小黄花\n[00:33.000]从出生那年就飘着"},)"
        R"("klyric":{"version":0,"lyric":null},)"
        R"("tlyric":{"version":0,"lyric":"[00:29.260]translated"},)"
        R"("code":200})";

    std::string lrc;
    Check(lyricus::ParseNetEaseLyricRoot(body, lrc), "解析成功");
    Check(lrc.find("[00:29.260]故事的小黄花") != std::string::npos, "取到正文");

    // \n 必须被反转义成真换行 —— 不然后面按行解析只有一行
    Check(lrc.find('\n') != std::string::npos, "\\n 已反转义成真换行");

    // tlyric 不能被误当成 lrc
    Check(lrc.find("translated") == std::string::npos, "没有误取 tlyric");

    // "lrc" 这个键名出现在 tlyric/klyric 的值里也不该带偏
    std::string empty2;
    Check(!lyricus::ParseNetEaseLyricRoot(R"({"code":200})", empty2),
          "没有 lrc 键 -> false");

    // 纯音乐：lrc.lyric 是 null。
    //
    // 【这里断言的是「解出来是空串」，不是「返回 false」】
    // 和 LRCLIB 那个解析器同一套约定：裸 null 走 SkipValue，
    // 解析本身**成功**，只是那个字段没有值。
    // 「没有词」这个判断由调用方做（TryNetEase 里查 lrc.empty()），
    // 分两层是有意的 —— 解析器只管形状对不对。
    std::string nullLrc = "不该被保留的内容";
    Check(lyricus::ParseNetEaseLyricRoot(
              R"({"lrc":{"version":0,"lyric":null},"code":200})", nullLrc),
          "lyric 为 null：形状合法，解析成功");
    Check(nullLrc.empty(), "lyric 为 null -> 解出空串（调用方据此判定纯音乐）");
}

void TestNetEaseCreditStrip() {
    std::printf("\n== 制作人员名单剥离（最危险的一块）==\n");

    // ---- 必须剥掉：照实测的《晴天》缓存条目 ----
    const std::string withCredits =
        "[00:00.000] 作词 : 方文山\n"
        "[00:20.250]鼓：陈柏州\n"
        "[00:22.510]录音助理：刘勇志\n"
        "[00:24.760]录音工程：杨瑞代（Alfa Studio）\n"
        "[00:27.010]混音工程：杨大纬（杨大纬录音工作室）\n"
        "[00:29.260]故事的小黄花\n"
        "[00:33.000]从出生那年就飘着\n";

    const std::string stripped = lyricus::StripNetEaseCredits(withCredits);
    Check(stripped.find("鼓：陈柏州") == std::string::npos, "剥掉了「鼓：」");
    Check(stripped.find("混音工程") == std::string::npos, "剥掉了「混音工程：」");
    Check(stripped.find("作词 : 方文山") == std::string::npos,
          "剥掉了「作词 : X」（全角冒号 + 冒号旁有空格）");

    // ★ 分隔线**不在这里**补 —— 挪到取词的统一出口 EnsureLeadInSeparator 了。
    //   放这里只能覆盖"本次新取到的词"，缓存里已存下的 .lrc 根本不会再走这段代码
    //   （用户实测：「缓存到本地的歌词还没有第一行的分隔线」）。
    Check(stripped.find("———") == std::string::npos,
          "★ 剥名单这一步不再补分隔线（交给 EnsureLeadInSeparator）");

    // ★ 最重要的一条：真歌词一行都不能少
    Check(stripped.find("[00:29.260]故事的小黄花") != std::string::npos,
          "★ 第一句真歌词保留");
    Check(stripped.find("[00:33.000]从出生那年就飘着") != std::string::npos,
          "★ 后面的真歌词全部保留");

    // ---- 绝不能剥：第一行就是歌词 ----
    const std::string pureLyric =
        "[00:12.000]作词的人是我自己\n"      // 含「作词」但冒号在很后面
        "[00:16.000]曲终人散\n";
    const std::string kept = lyricus::StripNetEaseCredits(pureLyric);
    Check(kept.find("作词的人是我自己") != std::string::npos,
          "★ 冒号位置靠后的歌词行不剥");
    Check(kept.find("曲终人散") != std::string::npos, "★ 后续行原样保留");

    // ---- 中间夹一行非名单 -> 立刻停手，后面即使像名单也不再剥 ----
    const std::string mixed =
        "[00:00.000]作词：某人\n"
        "[00:05.000]这是真歌词\n"
        "[00:09.000]混音：另一个人\n";       // 这句在真歌词之后，必须留着
    const std::string m = lyricus::StripNetEaseCredits(mixed);
    Check(m.find("这是真歌词") != std::string::npos, "遇到真歌词后停手");
    Check(m.find("混音：另一个人") != std::string::npos,
          "★ 停手之后即使像名单也不再剥（宁可漏剥，不能吃歌词）");

    // ---- 整篇都是名单 -> 视为没有歌词（纯音乐条目）----
    //
    // ★ 这一条卡的是"分隔线"方案的边界：分隔线必须在**判空之后**才补。
    //   顺序反了的话，纯音乐条目剥完是空的、本该返回空让面板显示「（无歌词）」，
    //   却因为多了一条分隔线而被当成"有歌词"，整首歌显示一条横线 ——
    //   比一片空白更让人困惑。（这个 bug 是这条断言抓出来的。）
    const std::string allCredits =
        "[00:00.000]作曲：某人\n"
        "[00:02.000]编曲：另一人\n";
    Check(lyricus::StripNetEaseCredits(allCredits).empty(),
          "整篇都是名单 -> 返回空（当作没歌词，不能只剩一条分隔线）");

    // ---- 没剥过任何东西 -> 不该多出一行 ----
    const std::string noCredits =
        "[00:00.000]第一句就是歌词\n"
        "[00:05.000]第二句\n";
    const std::string untouched = lyricus::StripNetEaseCredits(noCredits);
    Check(untouched.find("———") == std::string::npos,
          "★ 没有名单可剥时，不补分隔线");
    Check(untouched.rfind("[00:00.000]第一句就是歌词", 0) == 0, "原样返回");

    // ---- 单行判定 ----
    Check(lyricus::IsNetEaseCreditLine("[00:01.000]Producer: Someone"),
          "英文名单也认（Producer:）");
    Check(!lyricus::IsNetEaseCreditLine("[00:01.000]故事的小黄花"),
          "没有冒号 -> 不是名单");
    Check(!lyricus::IsNetEaseCreditLine("混音：某人"),
          "没有时间戳 -> 不是名单（只认带时间戳的行）");
}

void TestNetEaseCandidatePick() {
    std::printf("\n== 候选核验（防翻唱 / 防错配）==\n");

    lyricus::OnlineLyricRequest req;
    req.artist = L"周杰伦";
    req.title  = L"晴天";
    req.durationSec = 269.0;

    // 实测：搜「周杰伦 晴天」返回的**首条是翻唱**
    std::vector<lyricus::NetEaseSong> cover;
    lyricus::NetEaseSong c;
    c.hasId = true; c.id = 2668397359.0; c.durationMs = 270700.0;
    c.name = "晴天 (原唱 周杰伦)"; c.artist = "RyaVocal";
    cover.push_back(c);

    lyricus::NetEaseSong picked;
    Check(!lyricus::PickNetEaseCandidate(cover, req, picked),
          "★ 翻唱被拒绝（曲名归一化后不等）");

    // 正主应当被接受
    std::vector<lyricus::NetEaseSong> good = cover;
    lyricus::NetEaseSong g;
    g.hasId = true; g.id = 186016.0; g.durationMs = 269000.0;
    g.name = "晴天"; g.artist = "周杰伦"; g.album = "叶惠美";
    good.push_back(g);

    Check(lyricus::PickNetEaseCandidate(good, req, picked), "正主被接受");
    Check(picked.id == 186016.0, "选中的是正主那条");

    // ---- 时长差很多 + 演唱者对上 -> **接受**（用户 2026-09-24 拍板）----
    //
    // 出处：《恋爱理论》remaster 版「心加心」本地 308.0s、网易云原版 273.9s。
    // 同一个歌手的同一首曲子，差的是编曲长度。配上原版的词时间轴会整体偏，
    // 但用户明确选了"宁可偏，也要有词"。
    std::vector<lyricus::NetEaseSong> wrongDur;
    lyricus::NetEaseSong w;
    w.hasId = true; w.id = 1.0; w.durationMs = 400000.0;   // 400s vs 269s
    w.name = "晴天"; w.artist = "周杰伦";
    wrongDur.push_back(w);
    Check(lyricus::PickNetEaseCandidate(wrongDur, req, picked),
          "时长差 131 秒但演唱者对得上 -> 接受（宁可偏也要有词）");

    // ---- ★ 但两道软闸不能一起失效：时长差很多 + 演唱者也对不上 -> 拒绝 ----
    //
    // 这是 D-024 那道保护的残余：搜 "Best Wishes" 曾经回来一首完全不相干的歌
    // （328s vs 240s）。那种情况下演唱者也是对不上的，所以仍然拦得住。
    std::vector<lyricus::NetEaseSong> bothBad;
    lyricus::NetEaseSong bb;
    bb.hasId = true; bb.id = 9.0; bb.durationMs = 400000.0;   // 差 131 秒
    bb.name = "晴天"; bb.artist = "完全不相干的人";            // 而且演唱者对不上
    bothBad.push_back(bb);
    Check(!lyricus::PickNetEaseCandidate(bothBad, req, picked),
          "★ 时长差很多 **且** 演唱者对不上 -> 拒绝（防同名不同歌）");

    // 演唱者对不上 —— **现在要放行**，这是刻意的行为变更。
    //
    // 初版这里是硬否决，结果把用户「再见，碳酸海」整张专辑误杀
    // （详见 PickNetEaseCandidate 的说明）。真正挡翻唱的是**曲名闸**：
    // 下面这条的曲名归一化后是相等的，所以它本来就该被接受。
    std::vector<lyricus::NetEaseSong> wrongArtist;
    lyricus::NetEaseSong a2;
    a2.hasId = true; a2.id = 2.0; a2.durationMs = 269000.0;
    a2.name = "晴天"; a2.artist = "完全不相干的人";
    wrongArtist.push_back(a2);
    Check(lyricus::PickNetEaseCandidate(wrongArtist, req, picked),
          "演唱者对不上但曲名+时长对上 -> 仍然接受（演唱者只影响排序）");
    Check(picked.id == 2.0, "选中了那一条");

    // ★ 真实回归：繁体/简体写法差异绝不能否决
    //
    // 用户标签是繁体「純白P」，网易云写的是简体「Soda纯白」。
    // NormalizeLyricStem 折全半角和大小写，但**不做繁简转换**。
    // 这三条照 2026-09-24 从网易云打回来的真实候选写。
    {
        lyricus::OnlineLyricRequest ne;
        ne.artist = L"純白P"; ne.title = L"Best Wishes"; ne.durationSec = 216.4;

        std::vector<lyricus::NetEaseSong> cands;
        lyricus::NetEaseSong s;
        s.hasId = true; s.id = 111.0; s.durationMs = 216400.0;   // 差 0 秒
        s.name = "Best Wishes"; s.artist = "Soda纯白/洛天依/乐正绫";
        cands.push_back(s);

        Check(lyricus::PickNetEaseCandidate(cands, ne, picked),
              "★ 繁简写法不同（純白P vs Soda纯白）不该否决");
        Check(picked.id == 111.0, "★ 选中的就是那条正确候选");
    }
    {
        lyricus::OnlineLyricRequest ne;
        ne.artist = L"純白P"; ne.title = L"世界第一可爱"; ne.durationSec = 225.0;

        std::vector<lyricus::NetEaseSong> cands;
        lyricus::NetEaseSong s;
        s.hasId = true; s.id = 222.0; s.durationMs = 225000.0;
        s.name = "世界第一可爱"; s.artist = "Soda纯白";
        cands.push_back(s);

        Check(lyricus::PickNetEaseCandidate(cands, ne, picked),
              "★ 同上（世界第一可爱）");
    }

    // 演唱者对得上时应当**优先**选中它，即使它排在后面
    {
        lyricus::OnlineLyricRequest ne;
        ne.artist = L"周杰伦"; ne.title = L"晴天"; ne.durationSec = 269.0;

        std::vector<lyricus::NetEaseSong> cands;
        lyricus::NetEaseSong bad;      // 先放一条演唱者对不上的
        bad.hasId = true; bad.id = 10.0; bad.durationMs = 269000.0;
        bad.name = "晴天"; bad.artist = "翻唱歌手";
        cands.push_back(bad);

        lyricus::NetEaseSong good2;    // 再放正主
        good2.hasId = true; good2.id = 20.0; good2.durationMs = 269000.0;
        good2.name = "晴天"; good2.artist = "周杰伦";
        cands.push_back(good2);

        Check(lyricus::PickNetEaseCandidate(cands, ne, picked),
              "两条都过闸 -> 有结果");
        Check(picked.id == 20.0,
              "★ 演唱者对得上的那条优先（排序起作用了，不是简单取第一条）");
    }

    // 多人演唱（A/B/C）里有一个对上就该拿到最高分
    std::vector<lyricus::NetEaseSong> multi;
    lyricus::NetEaseSong m;
    m.hasId = true; m.id = 3.0; m.durationMs = 269000.0;
    m.name = "晴天"; m.artist = "某合唱团/周杰伦/另一个人";
    multi.push_back(m);
    Check(lyricus::PickNetEaseCandidate(multi, req, picked),
          "多人演唱里含目标歌手 -> 放行");

    // 时长未知时不拦（无标签曲目查不到时长）
    lyricus::OnlineLyricRequest noDur = req;
    noDur.durationSec = 0.0;
    Check(lyricus::PickNetEaseCandidate(wrongDur, noDur, picked),
          "本地时长未知时不按时长拦");
}

void TestEditionMarker() {
    std::printf("\n== 版本标记剥离（剥错就会配错版本）==\n");

    // ---- 必须剥：换的是母带/编码，内容与时间轴都没变 ----
    Check(lyricus::StripEditionMarker(L"白夜梦 [Remastered]") == L"白夜梦",
          "剥掉 [Remastered]");
    Check(lyricus::StripEditionMarker(L"白夜梦（Remastered）") == L"白夜梦",
          "全角括号也认");
    Check(lyricus::StripEditionMarker(L"Song (Remaster)") == L"Song", "Remaster 也认");
    Check(lyricus::StripEditionMarker(L"Song (2011 Remastered)") == L"Song",
          "带年份的 Remastered（按包含判定）");
    Check(lyricus::StripEditionMarker(L"Song [Remastered] [Hi-Res]") == L"Song",
          "多个标记叠加，循环剥干净");
    Check(lyricus::StripEditionMarker(L"Song (REMASTERED)") == L"Song", "大小写无关");

    // ---- ★ 绝不能剥：这些版本的内容真的不一样 ----
    //
    // 剥了会把带词的版本配到伴奏/现场/翻唱上 —— 那比没有歌词更糟。
    Check(lyricus::StripEditionMarker(L"Song (Instrumental)") == L"Song (Instrumental)",
          "★ (Instrumental) 不剥：伴奏版是没词的");
    Check(lyricus::StripEditionMarker(L"Song (inst.)") == L"Song (inst.)",
          "★ (inst.) 不剥");
    Check(lyricus::StripEditionMarker(L"Song (Live)") == L"Song (Live)",
          "★ (Live) 不剥：现场版时间轴不同");
    Check(lyricus::StripEditionMarker(L"Song (Cover)") == L"Song (Cover)",
          "★ (Cover) 不剥");
    Check(lyricus::StripEditionMarker(L"Song (TV Size)") == L"Song (TV Size)",
          "★ (TV Size) 不剥：只有一段");
    Check(lyricus::StripEditionMarker(L"Song (Love Theory Ver.)") == L"Song (Love Theory Ver.)",
          "★ (…Ver.) 不剥");

    // ---- 中间的括号是曲名的一部分，不能碰 ----
    Check(lyricus::StripEditionMarker(L"Song (Live) [Remastered]") == L"Song (Live)",
          "只剥末尾那个，中间的 (Live) 保留");
    Check(lyricus::StripEditionMarker(L"无括号的曲名") == L"无括号的曲名", "没有标记时原样返回");
    Check(lyricus::StripEditionMarker(L"") == L"", "空串不崩");

    // ---- 与真实候选的联合验证：remaster 本地 vs 原版网易云 ----
    {
        lyricus::OnlineLyricRequest ne;
        ne.artist = L"叶秋池、阿良良木健";
        ne.title  = L"白夜梦 [Remastered]";
        ne.durationSec = 271.2;

        std::vector<lyricus::NetEaseSong> cands;
        lyricus::NetEaseSong s;
        s.hasId = true; s.id = 333.0; s.durationMs = 271200.0;   // 差 0 秒
        s.name = "白夜梦"; s.artist = "阿良良木健/洛天依/乐正绫";
        cands.push_back(s);

        lyricus::NetEaseSong picked;
        Check(lyricus::PickNetEaseCandidate(cands, ne, picked),
              "★ 真实回归：本地 [Remastered] 能配上网易云原版");
        Check(picked.id == 333.0, "★ 选中的是那条原版");
    }

    // ---- ★ 搜索词也必须剥版本标记 ----
    //
    // 实测（2026-09-24）带着 [Remastered] 发出去召回的全是垃圾：
    //     「阿良良木健 远恋 [Remastered]」            -> 过曲名闸 0 条
    //     「皓月、阿良良木健 依存症（…）[Remastered]」 -> 过曲名闸 0 条
    // 而那两首在网易云上都有。剥掉之后搜索词才干净。
    {
        lyricus::OnlineLyricRequest req;
        req.artist = L"阿良良木健";
        req.title  = L"远恋 [Remastered]";

        const std::wstring path = lyricus::BuildNetEaseSearchPath(req);
        // 「远恋」的 UTF-8 百分号编码
        Check(path.find(L"%E8%BF%9C%E6%81%8B") != std::wstring::npos,
              "★ 搜索词里有「远恋」");
        Check(path.find(L"Remastered") == std::wstring::npos &&
              path.find(L"remastered") == std::wstring::npos,
              "★ 搜索词里**没有** [Remastered]");
    }
}

// 无标签文件：曲名带音轨号、歌手是占位符。
//
// 出处：用户那批没打标的 WAV，日志里长这样
//     换曲: E:\奇爱人生·终焉版\本体\02 遗忘山丘.wav
//     标签=[?] - [02 遗忘山丘]
// 而网易云上是干净的「遗忘山丘」by 阿良良木健/洛天依（242.8s，本地 242.1s）。
void TestUntaggedTracks() {
    std::printf("\n== 无标签文件（音轨号 + 占位符歌手）==\n");

    // 占位符判定
    Check(lyricus::IsPlaceholderTag(L"?"),        "「?」是占位符");
    Check(lyricus::IsPlaceholderTag(L"未知艺术家"), "「未知艺术家」是占位符");
    Check(lyricus::IsPlaceholderTag(L"Unknown Artist"), "英文占位符也认");
    Check(lyricus::IsPlaceholderTag(L""),         "空串当占位符（等于没有信息）");
    Check(!lyricus::IsPlaceholderTag(L"周杰伦"),   "★ 真歌手名不能被当成占位符");
    Check(!lyricus::IsPlaceholderTag(L"純白P"),   "★ 同上（繁体的）");

    // 演唱者比对
    Check(lyricus::ArtistNamesOverlap(L"苍十三、阿良良木健", L"阿良良木健/洛天依"),
          "本地用「、」在线用「/」，其中一人对得上即可");
    Check(lyricus::ArtistNamesOverlap(L"叶秋池、阿良良木健", L"阿良良木健/洛天依/乐正绫"),
          "多人名单里有交集即可");

    // ★ 但**繁简差异兜不住** —— 这条断言记录的是能力的边界，不是缺陷。
    //
    // 「純白P」（繁体純）vs「Soda纯白」（简体纯）归一化后是 `純白p` 和 `soda纯白`，
    // 没有任何公共子串。那种情况靠的是**时长闸**（实测差 0 秒），不是这里。
    // 我一开始在注释里写了"包含能兜住繁简"，是个不成立的说法 —— 被这条抓出来了。
    Check(!lyricus::ArtistNamesOverlap(L"純白P", L"Soda纯白"),
          "★ 繁简差异**不**能靠包含兜住（那条路走的是时长闸）");

    Check(!lyricus::ArtistNamesOverlap(L"周杰伦", L"完全不相干的人"), "对不上就是 false");
    Check(!lyricus::ArtistNamesOverlap(L"?", L"阿良良木健"), "占位符不参与比对");
    Check(!lyricus::ArtistNamesOverlap(L"", L"阿良良木健"), "空值不与任何人对上");

    // ---- ★ 真实回归：02 遗忘山丘 ----
    //
    // 曲名闸用 MakeTitleCandidates，它会切掉音轨号前缀；
    // 时长差 0.7 秒（在 ±5 秒内）—— 这条必须过。
    {
        lyricus::OnlineLyricRequest ne;
        ne.artist = L"?";                 // 无标签 -> 字面的占位符
        ne.title  = L"02 遗忘山丘";        // %title% 退化成文件名
        ne.durationSec = 242.1;

        std::vector<lyricus::NetEaseSong> cands;
        lyricus::NetEaseSong s;
        s.hasId = true; s.id = 444.0; s.durationMs = 242800.0;   // 差 0.7 秒
        s.name = "遗忘山丘"; s.artist = "阿良良木健/洛天依";
        cands.push_back(s);

        lyricus::NetEaseSong picked;
        Check(lyricus::PickNetEaseCandidate(cands, ne, picked),
              "★ 真实回归：无标签「02 遗忘山丘.wav」能配上「遗忘山丘」");
        Check(picked.id == 444.0, "★ 选中的是那条");
    }

    // ---- ★ 真实回归：心加心（remaster vs 原版，差 34 秒）----
    //
    // 曲名全等、演唱者（阿良良木健）对得上，但时长差 34 秒。
    // 用户选了"宁可偏，也要有词"，所以必须接受。
    {
        lyricus::OnlineLyricRequest ne;
        ne.artist = L"苍十三、阿良良木健";
        ne.title  = L"心加心 [Remastered]";
        ne.durationSec = 308.0;

        std::vector<lyricus::NetEaseSong> cands;
        lyricus::NetEaseSong s;
        s.hasId = true; s.id = 555.0; s.durationMs = 273900.0;   // 差 34.1 秒
        s.name = "心加心"; s.artist = "阿良良木健/洛天依";
        cands.push_back(s);

        lyricus::NetEaseSong picked;
        Check(lyricus::PickNetEaseCandidate(cands, ne, picked),
              "★ 真实回归：remaster「心加心」差 34 秒也能配上（演唱者对上）");
        Check(picked.id == 555.0, "★ 选中的是那条");
    }
}

// 网易云的**业务码**（HTTP 200 + 响应体里的 code）。
//
// 这是 D-036 的病根：限流时网易云返回
//     HTTP/1.1 200 OK
//     {"msg":"操作频繁，请稍候再试","code":405,...}
// 而原来只看 HTTP 状态码 —— 于是一次限流被当成"这首歌确实没有"，
// 转去 LRCLIB 也没查到之后，**写下了 7 天有效的假"没有"**。
void TestNetEaseBizCode() {
    std::printf("\n== 网易云业务码（HTTP 200 里藏着的错误）==\n");

    // 实测的限流响应，原样抄下来
    Check(lyricus::ParseNetEaseCode(
              R"({"msg":"操作频繁，请稍候再试","code":405,"message":"操作频繁，请稍候再试"})") == 405,
          "★ 限流响应解出 405");

    Check(lyricus::ParseNetEaseCode(R"({"result":{},"code":200})") == 200, "正常响应解出 200");
    Check(lyricus::ParseNetEaseCode(R"({"sgc":false,"lrc":{},"code":200})") == 200,
          "歌词响应也解得出");
    Check(lyricus::ParseNetEaseCode("") == -1, "空响应体 -> -1（形状不对）");
    Check(lyricus::ParseNetEaseCode(R"({"code":"405"})") == -1,
          "字符串形式的码解不出 -> -1（宁可当失败，也别当成正常）");

    // 搜索响应里嵌套对象也可能有同名键 —— 顶层那个在最后，必须取到它
    Check(lyricus::ParseNetEaseCode(R"({"result":{"code":1},"code":405})") == 405,
          "★ 嵌套对象里的 code 不能抢走顶层的（rfind 从后往前找）");

    // 值不值得重试
    Check(lyricus::IsNetEaseRetryableCode(405), "限流要重试");
    Check(lyricus::IsNetEaseRetryableCode(503), "5xx 要重试");
    Check(!lyricus::IsNetEaseRetryableCode(400), "★ 400 参数错不重试（重试多少次都一样）");
    Check(!lyricus::IsNetEaseRetryableCode(200), "200 不是错误");
}

// 搜索词必须干净 —— 光在闸门里剥音轨号是不够的。
//
// 实测对比（2026-09-24）：
//     查「02 遗忘山丘」 -> 青山不改与君携 / 讨好 / 遗憾 …（正确答案连前 6 都进不去）
//     查「遗忘山丘」    -> 遗忘山丘 by 阿良良木健（242.8s，本地 242.1s，差 0.7 秒）
// 原来只在候选核验里剥、搜索词没剥，于是**第一关就搜错了东西**，
// 后面两道闸门再准也救不回来 —— 日志里表现成「过曲名闸 0 条」，
// 看起来像"网易云没有这首歌"，其实是"我们搜错词了"。
void TestNetEaseSearchQuery() {
    std::printf("\n== 网易云搜索词（音轨号必须剥掉）==\n");

    // 先单测切法本身
    Check(lyricus::StripLeadingTrackNumber(L"02 遗忘山丘") == L"遗忘山丘", "剥掉「02 」");
    Check(lyricus::StripLeadingTrackNumber(L"11 心加心") == L"心加心", "剥掉「11 」");
    Check(lyricus::StripLeadingTrackNumber(L"07.哀歌") == L"哀歌", "点号分隔也认");
    Check(lyricus::StripLeadingTrackNumber(L"003 某曲") == L"某曲", "三位数也认");
    Check(lyricus::StripLeadingTrackNumber(L"无音轨号") == L"无音轨号", "没有就原样返回");

    // ★ 以数字开头的真曲名 —— 会被误剥，所以线上必须留后路
    //
    // 剥法的判据是「≤3 位数字 + 分隔符」，挡不住这两个：
    // 「7 Years」会被剥成「Years」，「99 Problems」会被剥成「Problems」。
    // 这在**本地搜索**里无害（那里只是多一个候选，原样的还在），
    // 但在**搜索词**里有害 —— 那是替换。
    // 所以 TryNetEase 会拿剥过的词搜一次，不行再用原词搜一次。
    //
    // ⚠️ 我一开始把「7 Years 不切」写成了断言，结果被单测打回来 ——
    //    这条记录的是**能力的边界**，不是缺陷。
    Check(lyricus::StripLeadingTrackNumber(L"7 Years") == L"Years",
          "★ 「7 Years」会被误剥（线上靠「原词重搜」兜底）");
    Check(lyricus::StripLeadingTrackNumber(L"99 Problems") == L"Problems",
          "★ 「99 Problems」同样会被误剥");
    Check(lyricus::StripLeadingTrackNumber(L"1234 太多位了") == L"1234 太多位了",
          "★ 4 位数不切（超过 3 位上限）");

    // 端到端：搜索词里带的曲名必须是剥过的
    {
        lyricus::OnlineLyricRequest req;
        req.artist = L"?";                 // 占位符，会被上游当空
        req.title  = L"02 遗忘山丘";

        const std::wstring path = lyricus::BuildNetEaseSearchPath(req);
        Check(path.find(L"%E9%81%97%E5%BF%98%E5%B1%B1%E4%B8%98") != std::wstring::npos,
              "★ 搜索词里是「遗忘山丘」的 UTF-8 百分号编码");
        Check(path.find(L"02") == std::wstring::npos,
              "★ 搜索词里没有音轨号「02」");
        Check(path.find(L"/api/search/get/web?s=") == 0, "路径形状正确");
    }
}

// 前奏补分隔线 —— 放在取词的**统一出口**，所以新取的词和缓存命中的词都覆盖。
//
// 出处：用户说「缓存到本地的歌词还没有第一行的分隔线」。
// 原来这条逻辑写在 StripNetEaseCredits 里，只覆盖"本次从网易云新取到的词"；
// 而缓存里的 .lrc 是**上一次写下的**，根本不会再走那段代码。
void TestLeadInSeparator() {
    std::printf("\n== 前奏分隔线（新取的词 + 缓存里的词，同一个出口）==\n");

    // 首行在 29 秒 -> 有前奏 -> 补
    const std::string late29 =
        "[00:29.260]故事的小黄花\n"
        "[00:33.000]从出生那年就飘着\n";
    const std::string fixed = lyricus::EnsureLeadInSeparator(late29);
    Check(fixed.rfind("[00:00.000]———", 0) == 0, "★ 首行在 29 秒 -> 开头补上分隔线");
    Check(fixed.find("故事的小黄花") != std::string::npos, "真歌词一行没少");
    Check(fixed.find("———") < fixed.find("故事的小黄花"), "分隔线在真歌词之前");

    // ★ 幂等：新写的缓存已经带了线，缓存命中时再走一遍不能叠出第二条
    const std::string twice = lyricus::EnsureLeadInSeparator(fixed);
    Check(twice == fixed, "★ 幂等：已经有线就不会再加一条");

    // 首行本来就在 0 附近 -> 没有前奏 -> 不动
    const std::string atZero =
        "[00:00.000]第一句就是歌词\n"
        "[00:05.000]第二句\n";
    Check(lyricus::EnsureLeadInSeparator(atZero) == atZero,
          "首行在 0 秒 -> 原样返回（不补）");

    // 首行在 1 秒多 -> 那个前奏短到没必要补？不 —— 只要 > 0.05 就补。
    // 补了也无害：那一秒里当前行是空的线，第一句显示成"下一行"。
    const std::string at1 = "[00:01.200]开口就唱\n";
    Check(lyricus::EnsureLeadInSeparator(at1).rfind("[00:00.000]———", 0) == 0,
          "首行在 1.2 秒也补");

    // 边界与畸形输入不能崩
    Check(lyricus::EnsureLeadInSeparator("").empty(), "空串不崩");
    Check(lyricus::EnsureLeadInSeparator("没有时间戳的纯文本") == "没有时间戳的纯文本",
          "没有时间戳 -> 原样返回");
    Check(lyricus::EnsureLeadInSeparator("[坏的时间戳]词") == "[坏的时间戳]词",
          "时间戳畸形 -> 原样返回（不猜）");
    Check(lyricus::EnsureLeadInSeparator("[00:0.5]词") == "[00:00.000]———\n[00:0.5]词",
          "一位小数的秒数也能解析");
}

// 双语参照行：把翻译按时间戳并进原文。
//
// 数据来源实测（2026-09-24）：网易云《夜に駆ける》原文 64 行、tlyric 60 行 ——
// **行数不一样**，所以只能按时间戳对齐，不能按行号。
void TestTranslationMerge() {
    std::printf("\n== 双语：翻译按时间戳合并 ==\n");

    const std::string lrc =
        "[00:46.116]初めて会った日から\n"
        "[00:48.797]僕の心の全てを奪った\n"
        "[00:56.848]寂しい目をしてたんだ\n";

    // 故意**少一行**（模拟实测的 64 vs 60），而且顺序也打乱
    const std::string tl =
        "[00:56.848]眼神却显得如此寂寞\n"
        "[00:48.797]夺走了我心中的一切\n";

    const std::string merged = lyricus::MergeTranslationLines(lrc, tl);

    Check(merged.find("[00:48.797]僕の心の全てを奪った\n[00:48.797]夺走了我心中的一切\n")
              != std::string::npos,
          "★ 翻译紧跟在同时间戳的原文之后，且时间戳文本一致");
    Check(merged.find("[00:56.848]寂しい目をしてたんだ\n[00:56.848]眼神却显得如此寂寞\n")
              != std::string::npos,
          "★ 第二条也对上了");
    // 没有翻译的那行保持单行
    const size_t p = merged.find("初めて会った日から");
    Check(p != std::string::npos, "原文没丢");
    Check(merged.find("从第一次") == std::string::npos, "没翻译的行不会凭空多出一行");

    // 时间戳对不上的翻译**必须丢掉** —— 宁可少一行，也不能配错原文
    const std::string stray = lyricus::MergeTranslationLines(lrc, "[01:23.456]对不上的翻译\n");
    Check(stray == lrc, "★ 时间戳对不上的翻译被丢弃（不猜、不错配）");

    Check(lyricus::MergeTranslationLines(lrc, "") == lrc, "没有翻译时原样返回");
    Check(lyricus::MergeTranslationLines("", tl).empty(), "没有原文时返回空");
    Check(lyricus::MergeTranslationLines("没有时间戳的文本", tl) == "没有时间戳的文本",
          "原文没有时间戳 -> 原样返回");

    // ---- 合并之后，当前行必须落在**原文**上，不是翻译 ----
    //
    // LyricDocument::LineIndexAt 会往回退到同时间戳组的第一行。
    // 不退的话面板会把小字翻译当成主行来高亮，原文反倒成了"上一句"。
    {
        std::vector<unsigned char> bytes(merged.begin(), merged.end());
        lyricus::LyricDocument doc = lyricus::LyricDocument::Parse(bytes);

        const size_t i = doc.LineIndexAt(50.0);   // 落在 [00:48.797] 这一组
        Check(i != lyricus::LyricDocument::npos, "能找到当前行");
        if (i != lyricus::LyricDocument::npos) {
            Check(doc.At(i).text == L"僕の心の全てを奪った",
                  "★ 当前行是**原文**，不是翻译");
            Check(i + 1 < doc.Count() && doc.At(i + 1).text == L"夺走了我心中的一切",
                  "★ 翻译就在它后面一行");
        }
    }

    // 普通 LRC 不受影响（没有重复时间戳时，LineIndexAt 行为完全不变）
    {
        const std::string plain = "[00:10.000]甲\n[00:20.000]乙\n[00:30.000]丙\n";
        std::vector<unsigned char> bytes(plain.begin(), plain.end());
        lyricus::LyricDocument doc = lyricus::LyricDocument::Parse(bytes);
        Check(doc.At(doc.LineIndexAt(20.0)).text == L"乙", "普通 LRC：行为不变");
        Check(doc.At(doc.LineIndexAt(25.0)).text == L"乙", "普通 LRC：区间内取前一行");
    }
}

} // namespace

int wmain() {
    std::printf("在线歌词模块离线单测 —— 直接跑 src/online_lyric.cpp 的真实实现\n");
    std::printf("（不联网、不起线程，只验纯逻辑）\n");

    TestJsonString();
    TestParseEntryObject();
    TestParseSearchArray();
    TestOddJson();
    TestPercentEncode();
    TestCacheKey();
    TestNetEaseSearchParse();
    TestNetEaseLyricParse();
    TestNetEaseCreditStrip();
    TestNetEaseCandidatePick();
    TestEditionMarker();
    TestUntaggedTracks();
    TestNetEaseBizCode();
    TestNetEaseSearchQuery();
    TestLeadInSeparator();
    TestTranslationMerge();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
