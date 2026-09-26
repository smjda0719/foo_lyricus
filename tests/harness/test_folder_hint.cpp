// 歌词线索表 —— 纯逻辑，离线可测。
//
// 【为什么值得单独测】这是 plan 第 10 项一直挂着的待办
//（「存储层还没进离线单测台，依赖 SDK 的 cfg_var」）。原先整个 folder_hint.cpp
// 因为 include 了 config.h / online_lyric.h，一行都测不了 —— 而它里面真正
// 容易出错的恰恰是这些纯逻辑：文本格式容错、空字段、重复键、512 上限、
// UTF-8 往返、以及**"这次编辑到底有没有改动表"**。
//
// 最后那条最要紧：它决定调用方要不要作废「没有歌词」的缓存。
// 判错了的代价是用户白白多查一轮网络，而且完全看不出原因。

#include "folder_hint_table.h"
#include "lyric.h"        // WideToUtf8 / Utf8ToWide

#include <cstdio>
#include <string>
#include <vector>

// lyric.cpp 用到的外部符号。单测不需要真日志。
// （run.ps1 的注释里提过：各组都要自己提供 lyricus::DebugLog，不能合成一个 exe。）
namespace lyricus {
void DebugLog(const char* fmt, ...) { (void)fmt; }
} // namespace lyricus

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

using lyricus::FolderHint;
using lyricus::FolderKeyOf;

FolderHint Make(const wchar_t* artist, const wchar_t* album) {
    FolderHint h;
    h.artist = artist ? artist : L"";
    h.album  = album  ? album  : L"";
    return h;
}

// 查一条并画成可读文本，便于断言
std::wstring Show(const std::string& table, const wchar_t* key) {
    const FolderHint h = lyricus::LookupFolderHint(table, key);
    if (h.Empty()) return L"<无>";
    return h.artist + L"|" + h.album;
}

size_t Count(const std::string& table) {
    return lyricus::ParseFolderHints(table).size();
}

// ---------------------------------------------------------------------------
void TestFolderKey() {
    std::printf("\n== 文件夹键的归一化 ==\n");

    Check(FolderKeyOf(L"E:\\a\\b\\song.wav")      == L"e:\\a\\b", "取到文件所在目录");
    Check(FolderKeyOf(L"E:\\a\\b\\song.wav")      == FolderKeyOf(L"e:\\A\\B\\song.mp3"),
          "★ 大小写不敏感（同一个文件夹的不同写法必须折成同一个键）");
    Check(FolderKeyOf(L"E:/a/b/song.wav")         == L"e:/a/b",  "正斜杠也认");
    Check(FolderKeyOf(L"E:\\a\\b\\\\song.wav")    == L"e:\\a\\b", "★ 结尾多余的斜杠被去掉");

    Check(FolderKeyOf(L"").empty(),               "空路径 -> 空键");
    Check(FolderKeyOf(L"song.wav").empty(),       "★ 没有分隔符（裸文件名）-> 空键");
    Check(FolderKeyOf(L"E:\\song.wav")            == L"e:",       "盘根 -> 就是盘符");

    // 中文目录
    const std::wstring zh = FolderKeyOf(L"E:\\奇爱人生·终焉版\\本体\\01 哀歌.wav");
    Check(zh == L"e:\\奇爱人生·终焉版\\本体", "中文路径照样折对");
}

// ---------------------------------------------------------------------------
void TestRoundTrip() {
    std::printf("\n== 文本表 <-> 内存表 ==\n");

    std::vector<std::pair<std::wstring, FolderHint>> entries;
    entries.emplace_back(L"e:\\a", Make(L"阿良良木健", L"奇爱人生"));
    entries.emplace_back(L"e:\\b", Make(L"某歌手", L""));       // 专辑空
    entries.emplace_back(L"e:\\c", Make(L"", L"某专辑"));       // 歌手空

    const std::string text = lyricus::FormatFolderHints(entries);
    const auto back = lyricus::ParseFolderHints(text);

    Check(back.size() == 3, "三条进、三条出");
    Check(back[0].first == L"e:\\a" && back[0].second == Make(L"阿良良木健", L"奇爱人生"),
          "中文歌手/专辑往返一致");
    Check(back[1].second.artist == L"某歌手" && back[1].second.album.empty(),
          "★ 专辑留空时不会被下一个字段顶位（靠 TAB 个数判断）");
    Check(back[2].second.artist.empty() && back[2].second.album == L"某专辑",
          "★ 歌手留空同理");
}

// ---------------------------------------------------------------------------
void TestParseTolerance() {
    std::printf("\n== 文本表的容错 ==\n");

    Check(Count("") == 0,                              "空文本 -> 空表");
    Check(Count("\n\n\n") == 0,                        "只有空行 -> 空表");
    Check(Count("没有TAB的行\n") == 0,                  "★ 缺 TAB 的行被跳过，不是崩");
    Check(Count("a\tb\n") == 0,                        "只有两个字段（缺专辑）-> 跳过");
    Check(Count("\tb\tc\n") == 0,                      "★ 空键的行被跳过");
    Check(Count("a\tb\tc\n") == 1,                     "三个字段 -> 一条");
    Check(Count("a\tb\tc") == 1,                       "★ 最后一行没有换行也要认（不丢数据）");

    // 专辑里再出现 TAB：按"第一个和第二个 TAB 分段"，剩下的全归专辑
    const auto extraTab = lyricus::ParseFolderHints("a\tb\tc\td\n");
    Check(extraTab.size() == 1 && extraTab[0].second.album == L"c\td",
          "★ 专辑里多余的 TAB 原样保留");

    Check(Count("a\tb\tc\n没有TAB\nx\ty\tz\n") == 2,    "好坏行混在一起时只取好的");
}

// ---------------------------------------------------------------------------
void TestLookup() {
    std::printf("\n== 查表 ==\n");

    const std::string t =
        "e:\\a\t阿良良木健\t奇爱人生\n"
        "e:\\b\t某歌手\t\n";

    Check(Show(t, L"e:\\a") == L"阿良良木健|奇爱人生", "查得到");
    Check(Show(t, L"e:\\b") == L"某歌手|",             "专辑空的那条也查得到");
    Check(Show(t, L"e:\\zzz") == L"<无>",              "查不到 -> 空");
    Check(Show(t, L"") == L"<无>",                     "★ 空键直接返回空，不去扫表");
}

// ---------------------------------------------------------------------------
// ★ 本文件最要紧的一组：编辑 + "到底改没改"
// ---------------------------------------------------------------------------
void TestApplyEdit() {
    std::printf("\n== 编辑表 + 「改没改」的判定 ==\n");

    using lyricus::ApplyFolderHintEdit;

    // ---- 新增 ----
    bool changed = false;
    std::string t = ApplyFolderHintEdit("", L"e:\\a", Make(L"歌手A", L"专辑A"), &changed);
    Check(changed, "空表上新增 -> changed=true");
    Check(Count(t) == 1 && Show(t, L"e:\\a") == L"歌手A|专辑A", "新增的内容对");

    // ---- 更新 ----
    t = ApplyFolderHintEdit(t, L"e:\\a", Make(L"歌手B", L"专辑A"), &changed);
    Check(changed, "改歌手 -> changed=true");
    Check(Show(t, L"e:\\a") == L"歌手B|专辑A", "改完内容对");
    Check(Count(t) == 1, "★ 更新不会变成两条");

    // ---- ★ 无变化：这条驱动"要不要作废未命中缓存" ----
    const std::string before = t;
    t = ApplyFolderHintEdit(t, L"e:\\a", Make(L"歌手B", L"专辑A"), &changed);
    Check(!changed, "★ 写入完全相同的内容 -> changed=false");
    Check(t == before, "★ 且返回的文本与输入逐字节相同（调用方据此跳过写盘）");

    // 只有前后空白不同也算"没变化"吗？—— 不算，空格的差异是真实差异
    t = ApplyFolderHintEdit(t, L"e:\\a", Make(L"歌手B ", L"专辑A"), &changed);
    Check(changed, "（对照）尾部多一个空格算真改动");

    // ---- 删除 ----
    t = ApplyFolderHintEdit(t, L"e:\\a", FolderHint{}, &changed);
    Check(changed, "清空 -> changed=true");
    Check(Count(t) == 0, "★ 清空 = 删掉这一条，不是留一条空记录");

    // 删不存在的：不算改动
    t = ApplyFolderHintEdit(t, L"e:\\nonexistent", FolderHint{}, &changed);
    Check(!changed, "★ 删除不存在的键 -> changed=false（不该白写一次盘）");

    // ---- 空键：一律不动 ----
    const std::string base = "e:\\a\tx\ty\n";
    t = ApplyFolderHintEdit(base, L"", Make(L"z", L"w"), &changed);
    Check(!changed && t == base, "★ 空键 -> 原样返回、changed=false");

    // ---- 字段里的 TAB / 换行必须被清掉，否则整张表会被撑坏 ----
    t = ApplyFolderHintEdit("", L"e:\\a", Make(L"歌手\t带TAB", L"专辑\n带换行"), &changed);
    const auto parsed = lyricus::ParseFolderHints(t);
    Check(parsed.size() == 1, "★ 字段里的 TAB/换行被换成空格，表没被撑坏");
    Check(parsed.size() == 1 && parsed[0].second.artist == L"歌手 带TAB" &&
          parsed[0].second.album == L"专辑 带换行",
          "★ 换成了空格而不是直接删（文件夹名里的空格有意义）");
}

// ---------------------------------------------------------------------------
void TestApplyEditEdgeCases() {
    std::printf("\n== 编辑的边角 ==\n");

    using lyricus::ApplyFolderHintEdit;
    bool changed = false;

    // 不关心 changed 时可以不传
    const std::string t = ApplyFolderHintEdit("", L"e:\\a", Make(L"x", L"y"));
    Check(Count(t) == 1, "changedOut 传 nullptr 也能用");

    // 多条目：改中间那条不影响别的
    std::string multi =
        "e:\\a\tA1\tA2\n"
        "e:\\b\tB1\tB2\n"
        "e:\\c\tC1\tC2\n";
    multi = ApplyFolderHintEdit(multi, L"e:\\b", Make(L"B1x", L"B2"), &changed);
    Check(changed && Count(multi) == 3, "改中间一条：仍是三条");
    Check(Show(multi, L"e:\\a") == L"A1|A2" && Show(multi, L"e:\\c") == L"C1|C2",
          "★ 改中间一条不会碰坏前后两条");
    Check(Show(multi, L"e:\\b") == L"B1x|B2", "中间那条确实改了");

    // 删除中间那条
    multi = ApplyFolderHintEdit(multi, L"e:\\b", FolderHint{}, &changed);
    Check(changed && Count(multi) == 2, "删中间一条 -> 剩两条");
    Check(Show(multi, L"e:\\a") == L"A1|A2" && Show(multi, L"e:\\c") == L"C1|C2",
          "★ 删中间一条后，前后两条都还在（这一条钉的是原先那个"
          "「erase 里传引用给 remove」的隐患）");

    // ---- 512 条上限 ----
    std::string big;
    for (int i = 0; i < 600; ++i) {
        wchar_t key[64];
        swprintf_s(key, L"e:\\d%03d", i);
        big = ApplyFolderHintEdit(big, key, Make(L"歌手", L"专辑"));
    }
    Check(Count(big) == 512, "★ 超过 512 条 -> 只留 512 条");
    Check(Show(big, L"e:\\d599") == L"歌手|专辑",
          "★ 留下的含**最新**那条");
    Check(Show(big, L"e:\\d000") == L"<无>",
          "★ 丢掉的是**最旧的**（从头部丢）");
}

} // namespace

int main() {
    std::printf("======== 歌词线索表（纯逻辑）========\n");
    TestFolderKey();
    TestRoundTrip();
    TestParseTolerance();
    TestLookup();
    TestApplyEdit();
    TestApplyEditEdgeCases();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
