// 外观预设 —— 纯逻辑，离线可测。
//
// 【为什么值得单独测】解析一个"人能手工编辑、也能从别人那儿粘贴进来"的文本格式，
// 真正的难点全在**容错**上：缺字段、多字段、坏值、BOM、CRLF、前后空行。
// 这些在界面上试不出来（试到了也只会看到"导入失败"四个字），
// 但每一颗都能让用户对着一个明明没问题的文件干瞪眼。
//
// 另外这一组还钉着**设计意图**：内置的「高对比」为什么必须不透明、
// 为什么每套预设都得是完整值而不是"只覆盖几个字段"。那种断言
// 防的是将来有人"顺手优化"掉它们。

#include "preset.h"
#include "lyric.h"

#include <cstdio>
#include <string>
#include <vector>

// lyric.cpp 用到的外部符号。单测不需要真日志。
namespace lyricus {
void DebugLog(const char* fmt, ...) { (void)fmt; }
} // namespace lyricus

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("    [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("    [FAIL] %s\n", what); }
}

using lyricus::AppearancePreset;
using lyricus::ApplyPresetEdit;
using lyricus::ParsePresets;
using lyricus::FormatPresets;
using lyricus::FindPreset;
using lyricus::ExportPreset;
using lyricus::ImportPreset;
using lyricus::BuiltinPresets;

// ---------------------------------------------------------------------------
void TestBuiltins() {
    std::printf("\n== 内置预设 ==\n");

    const auto b = BuiltinPresets();
    Check(b.size() == 4, "内置 4 套");
    Check(b.size() == 4 && b[0].name == L"默认", "★ 第一套是「默认」（它是\"什么都没改\"的样子，放第一个）");

    // 名字各不相同 —— 同名会让"按名字查"失去意义
    bool dup = false;
    for (size_t i = 0; i < b.size(); ++i)
        for (size_t j = i + 1; j < b.size(); ++j)
            if (b[i].name == b[j].name) dup = true;
    Check(!dup, "四套名字互不相同（否则按名字查会歧义）");

    // ★ 每套都必须是**完整**的外观，不能"只覆盖几个字段"。
    //   否则切过去再切回来时，中间态会污染别的预设。
    int incomplete = 0;
    for (const auto& p : b) {
        if (p.name.empty()) ++incomplete;
        if (p.alpha < 0 || p.alpha > 255) ++incomplete;
        if (p.fontPct < 50 || p.fontPct > 300) ++incomplete;
        if (p.backdropMode < 0 || p.backdropMode > 2) ++incomplete;
    }
    Check(incomplete == 0, "★ 每套都是完整的外观（不依赖当前配置，切换不会互相污染）");

    // ★「高对比」的三个特殊之处，每一条都有理由，别被"顺手统一"掉
    const AppearancePreset* hc = FindPreset(b, L"高对比");
    Check(hc != nullptr, "查得到「高对比」");
    if (hc != nullptr) {
        Check(hc->alpha == 255,
              "★ 高对比：alpha = 255（半透明会透出后面的内容，正好毁掉对比度）");
        Check(hc->backdropMode == 0,
              "★ 高对比：通透度也选「不透明」—— 同上，这一套的全部意义就是对比度");
        Check(hc->fontPct > 100,
              "★ 高对比：字号大于 100%（选它的人本来就是要\"更容易看清\"）");
        Check(GetRValue(hc->bg) < 20 && GetGValue(hc->bg) < 20 && GetBValue(hc->bg) < 20,
              "高对比的底色接近纯黑");
    }

    // 亮色那套必须是浅底 —— 否则名字就是骗人的
    const AppearancePreset* light = FindPreset(b, L"亮色");
    Check(light != nullptr && GetRValue(light->bg) > 200,
          "★「亮色」的底色确实是浅的（名字和内容不能对不上）");
}

// ---------------------------------------------------------------------------
void TestRoundTrip() {
    std::printf("\n== 文本表 <-> 内存表 ==\n");

    std::vector<AppearancePreset> v = BuiltinPresets();
    const std::string text = FormatPresets(v);

    const auto back = ParsePresets(text);
    Check(back.size() == v.size(), "四套进、四套出");

    int mismatch = 0;
    for (size_t i = 0; i < back.size() && i < v.size(); ++i) {
        const auto& a = v[i];
        const auto& b = back[i];
        if (a.name != b.name) ++mismatch;
        if (a.header != b.header || a.current != b.current || a.normal != b.normal ||
            a.dim != b.dim || a.warn != b.warn || a.bg != b.bg) ++mismatch;
        if (a.alpha != b.alpha || a.fontPct != b.fontPct) ++mismatch;
        if (a.backdropMode != b.backdropMode) ++mismatch;
        if (a.fontFace != b.fontFace) ++mismatch;
    }
    Check(mismatch == 0, "★ 四套预设逐字段往返一致（颜色 / alpha / 字号 / 通透度 / 字体）");
}

// ---------------------------------------------------------------------------
void TestParseTolerance() {
    std::printf("\n== 解析容错（用户会手工改，也会粘贴别人的）==\n");

    Check(ParsePresets("").empty(),                              "空文本 -> 空表");
    Check(ParsePresets("\n\n\n").empty(),                        "只有空行 -> 空表");
    Check(ParsePresets("没有TAB的行\n").empty(),                  "★ 缺 TAB 的行被跳过，不是崩");
    Check(ParsePresets("\tbg=000000\n").empty(),                 "★ 空名字的行被跳过（空名字没法引用）");
    // ⚠️ 这条最初写成了 "A\r\nbg=112233\r\n" —— 那**两行都没有 TAB**，
    //    本来就不是合法的预设，被跳过是对的。要验的是"行尾的 \r 不影响解析"，
    //    所以必须用**合法的一行**再加 CRLF。
    Check(ParsePresets("A\tbg=112233\r\n").size() == 1,
          "★ CRLF 行尾也认（编辑器改过的文件会带 \\r）");
    Check(ParsePresets("A\tbg=112233\r\nB\tbg=445566\r\n").size() == 2,
          "★ 多行 CRLF 也认");
    {
        // \r 不能混进名字里 —— 它是行尾标记，不是内容
        const auto v = ParsePresets("A\tbg=112233\r\n");
        Check(v.size() == 1 && v[0].name == L"A",
              "★ 名字里不会残留 \\r（先剥行尾再找 TAB）");
    }

    // ★ 未知 key 忽略 —— 这是**向前兼容**的核心：
    //   将来加字段时，老版本读新文件不该整条丢掉。
    {
        const auto v = ParsePresets("A\tbg=112233;futureField=whatever\n");
        Check(v.size() == 1 && v[0].bg == RGB(0x11, 0x22, 0x33),
              "★ 不认识的 key 被忽略，其余字段照常解析（向前兼容）");
    }

    // ★ 缺 key 用默认值 —— 同上，老预设文件缺新字段时该能读
    {
        const auto v = ParsePresets("A\tbg=112233\n");
        Check(v.size() == 1 && v[0].alpha == 215 && v[0].fontPct == 100,
              "★ 缺的 key 保持默认值（不因为字段少就丢整条）");
    }

    // 坏值：颜色格式不对 / 数字有尾巴 —— 都该退回默认，而不是算出一个离谱的值
    {
        const auto v = ParsePresets("A\tbg=ZZZZZZ;alpha=abc\n");
        Check(v.size() == 1 && v[0].bg == RGB(28, 28, 30),
              "★ 坏的颜色值 -> 保持默认（不是变成黑色或 0）");
        Check(v.size() == 1 && v[0].alpha == 215, "★ 坏的数字 -> 保持默认");
    }

    // 带 '#' 的颜色也认（用户可能从网页复制）
    {
        const auto v = ParsePresets("A\tbg=#112233\n");
        Check(v.size() == 1 && v[0].bg == RGB(0x11, 0x22, 0x33), "颜色带 '#' 前缀也认");
    }

    // 夹取：手改配置写了个 999
    {
        const auto v = ParsePresets("A\talpha=999;fontPct=1;backdrop=9\n");
        Check(v.size() == 1 && v[0].alpha == 255,    "★ alpha 越界被夹到 255");
        Check(v.size() == 1 && v[0].fontPct == 50,   "★ fontPct 越界被夹到 50");
        Check(v.size() == 1 && v[0].backdropMode == 2, "★ backdrop 越界被夹到 2");
    }

    // 名字里的 TAB 会把表撑坏 —— 它本来就分割不开，直接跳过整行
    Check(ParsePresets("A\tB\tbg=000000\n").size() == 1,
          "★ 名字里若混进 TAB，只按第一个 TAB 分段（后面的进了字段区）");
}

// ---------------------------------------------------------------------------
void TestApplyEdit() {
    std::printf("\n== 增删改 + 「改没改」的判定 ==\n");

    bool changed = false;

    // 新增
    AppearancePreset p;
    p.name = L"我的";
    p.bg   = RGB(10, 20, 30);
    std::string t = ApplyPresetEdit("", L"我的", &p, &changed);
    Check(changed, "空表上新增 -> changed=true");
    Check(ParsePresets(t).size() == 1 && ParsePresets(t)[0].bg == RGB(10, 20, 30), "新增的内容对");

    // 覆盖
    p.bg = RGB(40, 50, 60);
    t = ApplyPresetEdit(t, L"我的", &p, &changed);
    Check(changed, "同名 -> 覆盖而不是追加");
    Check(ParsePresets(t).size() == 1, "★ 覆盖后仍然只有一条");
    Check(ParsePresets(t)[0].bg == RGB(40, 50, 60), "覆盖后内容对");

    // 删除
    t = ApplyPresetEdit(t, L"我的", nullptr, &changed);
    Check(changed, "删除 -> changed=true");
    Check(ParsePresets(t).empty(), "删除后表空了");

    // 删不存在的：不算改动
    t = ApplyPresetEdit(t, L"不存在", nullptr, &changed);
    Check(!changed, "★ 删除不存在的名字 -> changed=false（不该白写一次盘）");

    // 空名字：一律不动
    t = ApplyPresetEdit("A\tbg=000000\n", L"", &p, &changed);
    Check(!changed, "★ 空名字 -> changed=false");

    // 名字以传入的为准，不信任调用方在结构里填的
    {
        AppearancePreset q;
        q.name = L"结构里写的";
        q.bg   = RGB(1, 2, 3);
        const std::string r = ApplyPresetEdit("", L"参数里的", &q, nullptr);
        const auto v = ParsePresets(r);
        Check(v.size() == 1 && v[0].name == L"参数里的",
              "★ 以参数里的 name 为准（结构里的被忽略，免得两处不一致）");
    }

    // 改中间一条不碰坏前后两条 —— 钉的是"用下标 erase"那个决定
    {
        std::string m = "A\tbg=010101\nB\tbg=020202\nC\tbg=030303\n";
        AppearancePreset b2;
        b2.bg = RGB(0x22, 0x22, 0x22);
        m = ApplyPresetEdit(m, L"B", &b2, &changed);
        Check(changed && ParsePresets(m).size() == 3, "改中间一条：仍是三条");
        const auto v = ParsePresets(m);
        Check(v[0].bg == RGB(1, 1, 1) && v[2].bg == RGB(3, 3, 3),
              "★ 改中间一条不会碰坏前后两条");

        m = ApplyPresetEdit(m, L"B", nullptr, &changed);
        const auto v2 = ParsePresets(m);
        Check(changed && v2.size() == 2, "删中间一条 -> 剩两条");
        Check(v2[0].name == L"A" && v2[1].name == L"C",
              "★ 删中间一条后，前后两条都还在");
    }

    // 名字里的换行会被 Sanitize（否则整张表会被撑坏）
    {
        AppearancePreset q;
        q.bg = RGB(5, 5, 5);
        const std::string r = ApplyPresetEdit("", L"带\n换行\t的名字", &q, nullptr);
        Check(ParsePresets(r).size() == 1,
              "★ 名字里的换行/TAB 被换成空格，表没被撑坏");
    }
}

// ---------------------------------------------------------------------------
void TestImportExport() {
    std::printf("\n== 单条导出 / 导入（分享那条路）==\n");

    AppearancePreset src;
    src.name    = L"朋友的配色";
    src.bg      = RGB(0x12, 0x34, 0x56);
    src.current = RGB(0xAB, 0xCD, 0xEF);
    src.alpha   = 200;
    src.fontFace = "Consolas";
    src.fontPct  = 130;

    const std::string dumped = ExportPreset(src);
    Check(!dumped.empty(), "导出非空");
    Check(dumped.back() == '\n', "导出以换行结尾（粘进文件就是一行）");

    AppearancePreset back;
    Check(ImportPreset(dumped, back), "导出的能导入回来");
    Check(back.name == src.name && back.bg == src.bg && back.current == src.current,
          "往返后名字与颜色一致");
    Check(back.alpha == 200 && back.fontPct == 130, "往返后 alpha 与字号一致");
    Check(back.fontFace == "Consolas", "★ 字体族也往返一致");

    // ---- 导入的容错：用户的粘贴板里什么都有 ----

    Check(ImportPreset("\xEF\xBB\xBF" + dumped, back),
          "★ 容忍 UTF-8 BOM（记事本另存为会加）");
    Check(ImportPreset("\r\n\r\n" + dumped + "\r\n\r\n", back),
          "★ 容忍前后空行（从聊天窗口粘贴常见）");
    Check(ImportPreset("   " + dumped + "   ", back), "容忍前后空白");

    // CRLF 版本：把 \n 换成 \r\n
    {
        std::string crlf;
        for (char c : dumped) { if (c == '\n') crlf += '\r'; crlf += c; }
        Check(ImportPreset(crlf, back), "★ 容忍 CRLF 行尾");
    }

    Check(!ImportPreset("", back),        "空文本 -> 导入失败");
    Check(!ImportPreset("   \n  \n", back), "只有空白 -> 导入失败");
    Check(!ImportPreset("根本不是预设", back), "没有 TAB 的文本 -> 导入失败");

    // ★ 导入失败时**不能改 out** —— 调用方可能正拿着一个有效的预设
    {
        AppearancePreset keep;
        keep.name = L"原来的";
        keep.bg   = RGB(9, 9, 9);
        Check(!ImportPreset("垃圾", keep), "非法内容 -> 返回 false");
        Check(keep.name == L"原来的" && keep.bg == RGB(9, 9, 9),
              "★ 导入失败时 out 保持原样（不半途改状态）");
    }
}

} // namespace

int main() {
    std::printf("======== 外观预设（纯逻辑）========\n");
    TestBuiltins();
    TestRoundTrip();
    TestParseTolerance();
    TestApplyEdit();
    TestImportExport();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
