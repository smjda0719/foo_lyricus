// 在线歌词源的顺序/启用状态 —— 纯逻辑，离线可测。
//
// 【为什么这段值得单独测】它是**配置串的解析与容错**：写错了不会崩，
// 表现只是"我的设置没生效"或者"某个源再也不工作" —— 真机上极难发现。
// 而它又能纯函数地测，没有理由不测。
//
// 出厂顺序（也是"恢复默认"）是 网易云 -> 酷狗 -> LRCLIB。

#include "source_order.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

using lyricus::LyricSource;
using lyricus::SourcePref;

std::wstring Show(const std::vector<SourcePref>& v) {
    std::wstring s;
    for (const SourcePref& p : v) {
        if (!s.empty()) s += L", ";
        s += lyricus::SourceDisplayName(p.src);
        if (!p.enabled) s += L"(停)";
    }
    return s;
}

std::wstring ShowTry(const std::vector<LyricSource>& v) {
    std::wstring s;
    for (LyricSource x : v) {
        if (!s.empty()) s += L", ";
        s += lyricus::SourceDisplayName(x);
    }
    return s;
}

void TestDefaults() {
    std::printf("\n== 歌词源顺序：默认值 ==\n");

    const auto all = lyricus::AllSources();
    Check(all.size() == 3, "出厂一共三个源");

    // 空串 = 没配置过 = 全启用、按出厂顺序
    const auto d = lyricus::ParseSourcePrefs(L"");
    Check(d.size() == 3, "空配置 -> 三项");
    Check(Show(d) == L"网易云, 酷狗, LRCLIB", "★ 空配置 -> 出厂顺序、全部启用");

    const auto t = lyricus::SourcesToTry(L"");
    Check(ShowTry(t) == L"网易云, 酷狗, LRCLIB",
          "★ 空配置时该试的顺序就是 网易云 -> 酷狗 -> LRCLIB");
}

void TestRoundTrip() {
    std::printf("\n== 歌词源顺序：存取往返 ==\n");

    std::vector<SourcePref> v = {
        { LyricSource::Kugou,   true  },
        { LyricSource::NetEase, false },
        { LyricSource::Lrclib,  true  },
    };
    const std::wstring s = lyricus::FormatSourcePrefs(v);
    Check(s == L"kugou:1,netease:0,lrclib:1", "格式化成 kugou:1,netease:0,lrclib:1");

    const auto back = lyricus::ParseSourcePrefs(s);
    Check(Show(back) == Show(v), "★ 写出去再读回来是同一份（顺序和启用状态都没丢）");

    // 停用的源不出现在"该试"列表里，但**仍然留在配置里**（下次还能启用回来）
    const auto tryList = lyricus::SourcesToTry(s);
    Check(ShowTry(tryList) == L"酷狗, LRCLIB",
          "★ 停用的网易云不参与尝试，但它在配置里还在（不是被删掉）");
}

void TestTolerance() {
    std::printf("\n== 歌词源顺序：配置串容错 ==\n");

    // 认不出的 id 跳过 —— 配置里可能留着已经删掉的源的名字
    {
        const auto v = lyricus::ParseSourcePrefs(L"netease:1,qqmusic:1,kugou:1");
        Check(v.size() == 3, "★ 认不出的源被跳过，其余照常");
        Check(Show(v) == L"网易云, 酷狗, LRCLIB",
              "被跳过后 LRCLIB 追加到末尾（它没被提到）");
    }

    // 重复的 id 只认第一次
    {
        const auto v = lyricus::ParseSourcePrefs(L"kugou:0,kugou:1,netease:1,lrclib:1");
        Check(v.size() == 3, "★ 重复的 id 只留一项（不会出现两个酷狗）");
        Check(!v[0].enabled, "认的是**第一次**写的 :0");
    }

    // ★ 将来加第四个源：老配置里没有它，必须被追加**并启用**
    //   （不追加的话新源永远不被尝试，而现象只是"新功能没生效"）
    {
        const auto v = lyricus::ParseSourcePrefs(L"lrclib:1");
        Check(v.size() == 3, "★ 配置里只提了一个源，另外两个也被补上");
        Check(v[0].src == LyricSource::Lrclib && v[0].enabled, "提到的那个排在最前");
        Check(v[1].enabled && v[2].enabled,
              "★ 没提到的源被追加**并启用** —— 这是为了让以后新加的源能自动生效");
    }

    // 全停用：原样返回，**不许**偷偷加回来（用户可能就是不想联网）
    {
        const auto v = lyricus::ParseSourcePrefs(L"netease:0,kugou:0,lrclib:0");
        Check(v.size() == 3, "三项都在");
        Check(lyricus::SourcesToTry(L"netease:0,kugou:0,lrclib:0").empty(),
              "★ 全停用 -> 一个都不试（不偷偷加回来）");
    }

    // 脏输入
    {
        const auto v1 = lyricus::ParseSourcePrefs(L"  netease : 0 , kugou:1  ");
        Check(v1.size() == 3 && !v1[0].enabled && v1[0].src == LyricSource::NetEase,
              "★ 冒号两边的空白被容忍");

        const auto v2 = lyricus::ParseSourcePrefs(L",,netease:1,,");
        Check(v2.size() == 3, "★ 多余/空的分段被跳过");

        const auto v3 = lyricus::ParseSourcePrefs(L"netease");
        Check(v3.size() == 3 && v3[0].enabled,
              "★ 只写 id 不写 :N -> 当成启用（宁可多试一个源）");

        const auto v4 = lyricus::ParseSourcePrefs(L"netease:yes");
        Check(v4.size() == 3 && v4[0].enabled,
              "★ `:yes` 这种拼错的值 -> 当启用（拼错不该让源失效）");

        const auto v5 = lyricus::ParseSourcePrefs(L"!!!:1");
        Check(v5.size() == 3, "★ 全是垃圾的配置 -> 退回三项默认，不崩");
    }
}

void TestMove() {
    std::printf("\n== 歌词源顺序：上移 / 下移 ==\n");

    const auto base = lyricus::ParseSourcePrefs(L"");   // 网易云, 酷狗, LRCLIB

    Check(Show(lyricus::MoveSourcePref(base, 0, -1)) == Show(base),
          "★ 第一项再上移 -> 原样（到边界不动，也不越界）");
    Check(Show(lyricus::MoveSourcePref(base, 2, +1)) == Show(base),
          "★ 最后一项再下移 -> 原样");

    const auto up = lyricus::MoveSourcePref(base, 1, -1);   // 酷狗上移到第一位
    Check(Show(up) == L"酷狗, 网易云, LRCLIB", "★ 上移交换相邻两项");

    const auto down = lyricus::MoveSourcePref(base, 0, +1);
    Check(Show(down) == L"酷狗, 网易云, LRCLIB", "★ 下移与上移互为逆操作");

    Check(Show(lyricus::MoveSourcePref(base, 9, -1)) == Show(base),
          "★ 下标越界 -> 原样（没选中任何一项时点按钮不该动）");
    Check(Show(lyricus::MoveSourcePref(base, 0, 0)) == Show(base), "delta=0 -> 原样");
}

} // namespace

int main() {
    std::printf("======== 在线歌词源顺序（纯逻辑）========\n");
    TestDefaults();
    TestRoundTrip();
    TestTolerance();
    TestMove();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
