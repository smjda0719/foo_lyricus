// ---------------------------------------------------------------------------
// LRC 解析器 —— 离线单测
//
// 直接跑真实的 src/lyric.cpp（同样靠 shim 挡掉 SDK 依赖）。
//
// 为什么值得测：解析器是**整个插件的地基** —— 搜索对了、在线拿到了，
// 最后都要经过它才能变成能滚动的行。而其中的**编码识别**是最容易
// 静默出错的一环：UTF-8 / UTF-16 BOM / GB18030 三选一判错，
// 表现是一屏乱码或"没有歌词"，但日志里一句话都不会说。
//
// 全部用字节数组直接喂 Parse()，不碰文件系统。
// ---------------------------------------------------------------------------

#include "lyric.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// lyric.cpp 用到的外部符号。单测不需要真日志。
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

lyricus::LyricDocument Parse(const std::string& s) {
    std::vector<unsigned char> b(s.begin(), s.end());
    return lyricus::LyricDocument::Parse(b);
}

bool Near(double a, double b) { return std::fabs(a - b) < 0.002; }

void ShowDoc(const char* label, const lyricus::LyricDocument& d) {
    std::printf("         %s -> %zu 行", label, d.Count());
    if (d.Count() > 0) {
        std::printf("  首行 t=%.3f  尾行 t=%.3f", d.At(0).timeSec, d.At(d.Count()-1).timeSec);
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------

void TestBasics() {
    std::printf("\n== 基本解析 ==\n");

    auto d = Parse("[00:12.34]hello\n[00:15.00]world\n");
    ShowDoc("基本", d);
    Check(d.Count() == 2, "两行");
    Check(d.Count() == 2 && Near(d.At(0).timeSec, 12.34), "第一行时间 12.34s");
    Check(d.Count() == 2 && d.At(1).text == L"world", "第二行文本正确");

    // 末行没有换行符 —— 很常见，不能丢
    auto d2 = Parse("[00:01.00]a\n[00:02.00]b");
    Check(d2.Count() == 2, "末行没有换行符时也要保留");

    // CRLF
    auto d3 = Parse("[00:01.00]a\r\n[00:02.00]b\r\n");
    Check(d3.Count() == 2, "CRLF 换行");
    Check(d3.Count() == 2 && d3.At(0).text == L"a", "CRLF 不残留 \\r 在文本里");

    // 空输入
    Check(Parse("").IsEmpty(), "空输入 -> 空文档");
    Check(Parse("\n\n\n").IsEmpty(), "只有空行 -> 空文档");
}

void TestFractionDigits() {
    std::printf("\n== 小数位数（1/2/3 位都要正确缩放）==\n");

    // LRC 里这三位数含义完全不同：.5 = 半秒，.55 = 0.55 秒，.555 = 0.555 秒。
    // 一律当毫秒会把前者缩小 100 倍，歌词就会整体乱掉。
    auto d = Parse("[00:01.5]a\n[00:02.55]b\n[00:03.555]c\n");
    ShowDoc("小数位", d);
    Check(d.Count() == 3, "三行都解析出来");
    Check(d.Count() == 3 && Near(d.At(0).timeSec, 1.5),   "1 位小数：.5  = 0.5 秒");
    Check(d.Count() == 3 && Near(d.At(1).timeSec, 2.55),  "2 位小数：.55 = 0.55 秒");
    Check(d.Count() == 3 && Near(d.At(2).timeSec, 3.555), "3 位小数：.555 = 0.555 秒");
}

void TestMultiTimestamp() {
    std::printf("\n== 一行多时间戳 ==\n");

    // 副歌复用同一句歌词，写成 [00:01.00][00:05.00]同一句
    auto d = Parse("[00:01.00][00:05.00]same\n");
    ShowDoc("多时间戳", d);
    Check(d.Count() == 2, "展开成两行");
    Check(d.Count() == 2 && d.At(0).text == L"same" && d.At(1).text == L"same",
          "两行文本相同");
    Check(d.Count() == 2 && Near(d.At(1).timeSec, 5.0), "第二份时间正确");
}

void TestOffset() {
    std::printf("\n== [offset:] ==\n");

    // offset 单位是毫秒，正数表示歌词整体延后
    auto d = Parse("[offset:-500]\n[00:10.00]a\n");
    ShowDoc("offset -500", d);
    Check(d.Count() == 1, "offset 行本身不是歌词");
    Check(d.Count() == 1 && Near(d.At(0).timeSec, 9.5), "[offset:-500] 把 10.000 提前到 9.500");

    auto d2 = Parse("[offset:500]\n[00:10.00]a\n");
    Check(d2.Count() == 1 && Near(d2.At(0).timeSec, 10.5), "[offset:500] 把 10.000 延后到 10.500");
}

void TestEncodings() {
    std::printf("\n== 编码识别（判错就是整屏乱码，且日志不报）==\n");

    // UTF-8 无 BOM
    {
        auto d = Parse("[00:01.00]\xE6\xAD\x8C\xE8\xAF\x8D\n");   // 歌词
        Check(d.Count() == 1 && d.At(0).text == L"歌词", "UTF-8 无 BOM");
    }

    // UTF-8 带 BOM —— BOM 三个字节必须被吃掉，不能混进第一行
    {
        auto d = Parse("\xEF\xBB\xBF[00:01.00]\xE6\xAD\x8C\xE8\xAF\x8D\n");
        Check(d.Count() == 1, "UTF-8 BOM：行数正确");
        Check(d.Count() == 1 && d.At(0).text == L"歌词", "UTF-8 BOM：BOM 没有混进文本");
    }

    // UTF-16 LE 带 BOM
    {
        std::string s;
        s += '\xFF'; s += '\xFE';                     // BOM
        const wchar_t* text = L"[00:01.00]歌词\n";
        for (const wchar_t* q = text; *q; ++q) {
            s += static_cast<char>(static_cast<unsigned>(*q) & 0xFF);
            s += static_cast<char>((static_cast<unsigned>(*q) >> 8) & 0xFF);
        }
        auto d = Parse(s);
        Check(d.Count() == 1, "UTF-16LE BOM：行数正确");
        Check(d.Count() == 1 && d.At(0).text == L"歌词", "UTF-16LE BOM：文本正确");
    }

    // GB18030(GBK) 兜底 —— 没有 BOM，也不是合法 UTF-8
    {
        std::string s = "[00:01.00]";
        s += '\xB8'; s += '\xE8';                     // 歌
        s += '\xB4'; s += '\xCA';                     // 词
        s += '\n';
        auto d = Parse(s);
        Check(d.Count() == 1, "GBK：行数正确");
        Check(d.Count() == 1 && d.At(0).text == L"歌词", "GBK 兜底解码正确");
    }
}

void TestLineIndexAt() {
    std::printf("\n== LineIndexAt（当前行查找）==\n");

    auto d = Parse("[00:00.00]a\n[00:10.00]b\n[00:20.00]c\n");
    if (d.Count() != 3) { Check(false, "前置条件：三行"); return; }

    Check(d.LineIndexAt(-5.0)  == 0, "早于第一行 -> 第 0 行（不返回 npos）");
    Check(d.LineIndexAt(0.0)   == 0, "正好在第一行 -> 第 0 行");
    Check(d.LineIndexAt(5.0)   == 0, "第一、二行之间 -> 第 0 行");
    Check(d.LineIndexAt(10.0)  == 1, "正好在第二行 -> 第 1 行");
    Check(d.LineIndexAt(19.99) == 1, "第二、三行之间 -> 第 1 行");
    Check(d.LineIndexAt(20.0)  == 2, "正好在最后一行 -> 第 2 行");
    Check(d.LineIndexAt(999.0) == 2, "远晚于最后一行 -> 停在最后一行");

    lyricus::LyricDocument empty;
    Check(empty.LineIndexAt(5.0) == lyricus::LyricDocument::npos, "空文档 -> npos");
}

// 显示序号 —— 换行上滑的判据就是靠它。
//
// 出处：用户 2026-09-25「好像没有做出歌词逐行上移，上一行歌词是突然消失的」。
// 根因是双语歌词"同时间戳、原文在前、翻译在后"两行，而 LineIndexAt 会退到组首，
// 于是**原始行号每推进一个时间戳就跳 2**（真机日志实测 34→35→37→38→41…）。
// 上滑的判据是"顺序 +1"，拿原始行号去比几乎永远不成立 —— 上滑等于没做。
//
// 换算成显示序号之后"下一句"恒为 +1，而 seek 仍然是 +N 或负数。
void TestDisplayIndex() {
    std::printf("\n== DisplayIndex（显示序号）==\n");

    // 双语：每个时间戳一组两行（原文 + 翻译）
    auto d = Parse("[00:00.00]a\n[00:00.00]A\n"
                   "[00:10.00]b\n[00:10.00]B\n"
                   "[00:20.00]c\n[00:20.00]C\n");
    if (d.Count() != 6) { Check(false, "前置条件：六行（三组双语）"); return; }

    Check(!d.IsSubLine(0) && d.IsSubLine(1), "组内第一行是正文、第二行是参照行");
    Check(d.DisplayIndex(0) == 0, "第 0 行 -> 显示序号 0");
    Check(d.DisplayIndex(2) == 1, "★ 原始行号 2（第二组正文）-> 显示序号 **1**");
    Check(d.DisplayIndex(4) == 2, "★ 原始行号 4 -> 显示序号 **2**");

    // ★ 这条就是上滑判据能成立的原因
    Check(d.DisplayIndex(2) == d.DisplayIndex(0) + 1 &&
          d.DisplayIndex(4) == d.DisplayIndex(2) + 1,
          "★ 相邻两句的显示序号差**恒为 1**（原始行号差是 2）");

    // 参照行不占序号：它和它上面那行是同一句
    Check(d.DisplayIndex(1) == d.DisplayIndex(0),
          "参照行的显示序号和它上面那行相同（同一句歌词）");

    // 没有翻译的普通 LRC：显示序号 == 原始行号（这条改动对它必须是无操作）
    auto plain = Parse("[00:00.00]a\n[00:10.00]b\n[00:20.00]c\n");
    if (plain.Count() == 3) {
        bool identity = true;
        for (size_t i = 0; i < 3; ++i)
            if (plain.DisplayIndex(i) != i) identity = false;
        Check(identity, "★ 普通 LRC 下显示序号 == 原始行号（对既有行为无操作）");
    }

    Check(d.DisplayIndex(lyricus::LyricDocument::npos) == lyricus::LyricDocument::npos,
          "npos -> npos（没在播放时不能瞎给一个序号）");
    Check(d.DisplayIndex(999) == lyricus::LyricDocument::npos, "越界 -> npos");

    // 真机上那个形状：一段里有的时间戳有翻译、有的没有 -> 步长 1 和 2 混着来
    auto mixed = Parse("[00:00.00]a\n[00:00.00]A\n[00:10.00]b\n[00:20.00]c\n[00:20.00]C\n");
    if (mixed.Count() == 5) {
        Check(mixed.DisplayIndex(0) == 0 && mixed.DisplayIndex(2) == 1 &&
              mixed.DisplayIndex(3) == 2,
              "★ 混着来也成立：0 -> 1 -> 2（原始行号是 0 -> 2 -> 3）");
    }
}

void TestOddInput() {
    std::printf("\n== 畸形输入不能崩 ==\n");

    // 时间戳不完整的行
    auto d1 = Parse("[00:01.00]正常\n[00:xx]坏时间戳\n[00:02.00]也正常\n");
    ShowDoc("坏时间戳", d1);
    Check(d1.Count() >= 2, "坏时间戳的行被跳过，其余照常解析");

    // 方括号但不是时间戳（元数据）
    auto d2 = Parse("[ti:标题]\n[ar:艺术家]\n[00:01.00]正文\n");
    ShowDoc("元数据", d2);
    Check(d2.Count() == 1, "非时间戳的 [xx:yy] 被当作元数据丢掉");
    Check(d2.Count() == 1 && d2.At(0).text == L"正文", "正文不受影响");

    // 极长/极短
    Check(Parse("[").IsEmpty(), "单个 '[' 不崩");
    Check(Parse("[00:01.00]").Count() <= 1, "有时间戳但没文本：不崩即可");

    // 冒号多、数字大
    auto d3 = Parse("[999:59.99]a\n");
    Check(d3.Count() == 1, "分钟数很大也能解析（999 分钟）");
}

} // namespace

int wmain() {
    std::printf("LRC 解析器离线单测 —— 直接跑 src/lyric.cpp 的真实实现\n");

    TestBasics();
    TestFractionDigits();
    TestMultiTimestamp();
    TestOffset();
    TestEncodings();
    TestLineIndexAt();
    TestDisplayIndex();
    TestOddInput();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
