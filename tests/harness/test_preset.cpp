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
// 区间常量和实现共用同一份 —— 见 preset.h 里那段说明
using lyricus::kPresetMinAlpha;
using lyricus::kPresetMaxAlpha;
using lyricus::kPresetMinFontPct;
using lyricus::kPresetMaxFontPct;
using lyricus::kPresetMinBackdrop;
using lyricus::kPresetMaxBackdrop;
// 控件配色模式（D-093）
using lyricus::kCtrlAuto;
using lyricus::kCtrlCustom;
using lyricus::kCtrlMax;
// 背景图参数的取值范围（D-098）—— 断言直接用实现那份常量，
// 而不是再写一遍字面量：两处各写一份迟早不同步（这个坑踩过一次）。
using lyricus::kBgFitMax;
using lyricus::kBgBlurMax;
using lyricus::kBgDimMax;
// 手动构图（D-103）
using lyricus::kBgZoomMinPct;
using lyricus::kBgOffsetMinPct;
using lyricus::kBgOffsetMaxPct;

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
    //
    // ⚠️ 区间用 preset.h 里的常量，不写字面量 —— 这条断言想验的是
    //    "字段合法"，不是"上限恰好是几"。写死 0..2 时，实现把上限改成 4
    //    之后这条就红了，而它红得完全没有道理。
    int incomplete = 0;
    for (const auto& p : b) {
        if (p.name.empty()) ++incomplete;
        if (p.alpha < kPresetMinAlpha || p.alpha > kPresetMaxAlpha) ++incomplete;
        if (p.fontPct < kPresetMinFontPct || p.fontPct > kPresetMaxFontPct) ++incomplete;
        if (p.backdropMode < kPresetMinBackdrop || p.backdropMode > kPresetMaxBackdrop) ++incomplete;
    }
    Check(incomplete == 0, "★ 每套都是完整的外观（不依赖当前配置，切换不会互相污染）");

    // ★「高对比」的三个特殊之处，每一条都有理由，别被"顺手统一"掉
    const AppearancePreset* hc = FindPreset(b, L"高对比");
    Check(hc != nullptr, "查得到「高对比」");
    if (hc != nullptr) {
        Check(hc->alpha == 255,
              "★ 高对比：alpha = 255（半透明会透出后面的内容，正好毁掉对比度）");
        Check(hc->backdropMode == 0,
              "★ 高对比：通透度选 None(0) 自绘不透明 —— 同上，这一套的全部意义就是对比度");
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
        Check(v.size() == 1 && v[0].backdropMode == 4, "★ backdrop 越界被夹到 4（不是 2）");
    }

    // ★ 这一组是**踩过坑之后加的**：preset.h 最初照着想当然的顺序把 backdropMode
    //   注释成 "0=不透明 1=毛玻璃 2=半透明"、默认值和夹取上限都取了 2。
    //   而 config.h 里实际有 **5** 个值且顺序不同：
    //     0=None 1=Mica 2=Acrylic 3=MicaAlt 4=Translucent
    //   后果是出厂那套「半透明」(4) 一存一读就被夹成「毛玻璃」(2)。
    {
        const AppearancePreset d;            // 默认构造 = 出厂值
        Check(d.backdropMode == 4,
              "★ 默认预设的 backdropMode = 4（Translucent，和 cfg_backdrop_mode 的出厂默认一致）");

        const auto v = ParsePresets("A\tbackdrop=4\n");
        Check(v.size() == 1 && v[0].backdropMode == 4,
              "★ 合法的 4 不会被夹掉（夹取范围是 0..4）");

        const auto v3 = ParsePresets("A\tbackdrop=3\n");
        Check(v3.size() == 1 && v3[0].backdropMode == 3,
              "★ 合法的 3（MicaAlt）也保留 —— 范围错成 0..2 时它会变成 2");
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

// ---------------------------------------------------------------------------
void TestMaxPresets() {
    std::printf("\n== 条数上限 ==\n");

    // 一直存到超过上限。FormatPresets 的截断逻辑写的是"保留最后 kMaxPresets 条"，
    // 也就是**从最旧的开始丢** —— 和歌词线索表同一套策略。
    // ⚠️ 这条以前没测过：上限是 64，而"丢错了方向"（丢最新那批）在界面上
    //    表现得非常像"保存失败"，但原因完全不同。
    std::string t;
    const int total = static_cast<int>(lyricus::kMaxPresets) + 10;
    for (int i = 0; i < total; ++i) {
        AppearancePreset p;
        p.bg = RGB(i & 0xFF, 0, 0);
        wchar_t name[32];
        swprintf_s(name, L"P%d", i);
        t = ApplyPresetEdit(t, name, &p, nullptr);
    }

    const auto v = ParsePresets(t);
    Check(v.size() == lyricus::kMaxPresets,
          "★ 超过上限后表里正好留 kMaxPresets 条（不会无限长）");

    // 最后写进去的那条必须在
    {
        wchar_t last[32];
        swprintf_s(last, L"P%d", total - 1);
        Check(FindPreset(v, last) != nullptr, "★ 最新存的那条在表里");
    }
    Check(FindPreset(v, L"P0") == nullptr,
          "★ 最旧的那条被丢掉了（丢的是旧的那一端，不是新的）");
    Check(FindPreset(v, L"P5") == nullptr,
          "★ 靠前的都被丢干净了");

    // 表长到这里就稳定了：再加也只是替换，不会再涨
    {
        AppearancePreset p;
        p.bg = RGB(0xFF, 0xFF, 0xFF);
        t = ApplyPresetEdit(t, L"再来一条", &p, nullptr);
        Check(ParsePresets(t).size() == lyricus::kMaxPresets,
              "★ 到上限之后继续新增，表长保持不变（稳定，不是每加一条就涨）");
    }
}

// ---------------------------------------------------------------------------
void TestControlColors() {
    std::printf("\n== 控件配色模式（D-093）==\n");

    // 用户 2026-09-26 定的：自动推导与自定义**两个选项并存**，
    // 「前者系统会自己调节，后者让用户自己更改」。
    {
        const AppearancePreset p;
        Check(p.ctrlMode == kCtrlAuto, "★ 默认是自动模式（从面板底色推导）");
    }

    // 自动模式下**不写**那 5 个字段 —— 它们不参与绘制，
    // 写进文件只会让预设变长、还会让人以为它们生效了。
    {
        AppearancePreset p;
        p.name = L"自动的";
        const std::string line = ExportPreset(p);
        Check(line.find("ctrlMode") == std::string::npos,
              "★ 自动模式导出的预设里不含 ctrl* 字段（它们不参与绘制）");
    }

    // 自定义模式往返
    {
        AppearancePreset p;
        p.name       = L"我的配色";
        p.ctrlMode   = kCtrlCustom;
        p.ctrlButton = RGB(0x11, 0x22, 0x33);
        p.ctrlIcon   = RGB(0x44, 0x55, 0x66);
        p.ctrlSlider = RGB(0x77, 0x88, 0x99);
        p.ctrlText   = RGB(0xAA, 0xBB, 0xCC);

        AppearancePreset back;
        Check(ImportPreset(ExportPreset(p), back), "自定义模式的预设能导出再导入");
        Check(back.ctrlMode == kCtrlCustom, "★ 模式位往返一致");
        Check(back.ctrlButton == p.ctrlButton && back.ctrlIcon   == p.ctrlIcon &&
              back.ctrlSlider == p.ctrlSlider && back.ctrlText   == p.ctrlText,
              "★ 4 个基色逐字段往返一致");
    }

    // ★ 向前兼容：没有 ctrlMode 的老预设读进来必须是自动。
    //   这正是"只在自定义时才写出去"那个决定的另一半 ——
    //   两条合起来，老预设文件一个字都不用改。
    {
        const auto v = ParsePresets("老预设\tbg=1C1C1E;current=FFFFFF;alpha=215\n");
        Check(v.size() == 1 && v[0].ctrlMode == kCtrlAuto,
              "★ 没有 ctrlMode 的老预设读进来是自动模式（向前兼容）");
    }

    // 夹取：手改配置写了个 7，不能让它变成一个没定义的模式
    {
        const auto v = ParsePresets("A\tctrlMode=7\n");
        Check(v.size() == 1 && v[0].ctrlMode == kCtrlMax,
              "★ ctrlMode 越界被夹到 1（不会变成没定义的模式）");
    }

    // 自定义那 4 个基色也要挨夹（走的是和别的颜色同一条 HexToColor）
    {
        const auto v = ParsePresets("A\tctrlMode=1;ctrlBtn=ZZZZZZ\n");
        Check(v.size() == 1 && v[0].ctrlButton == RGB(58, 62, 72),
              "★ 坏的基色值 -> 保持默认（不是变成黑色）");
    }
}

// ---------------------------------------------------------------------------
void TestBackground() {
    std::printf("\n== 背景图字段（D-098）==\n");

    // 默认没有背景图 —— 也就是"回到纯色底"，和从前一直的行为一致
    {
        const AppearancePreset p;
        Check(p.bgImage.empty(), "★ 默认没有背景图（空路径 = 纯色底）");
        Check(p.bgFit == 0 && p.bgBlur == 0 && p.bgDim == 0 && p.bgOpacity == 100,
              "★ 默认参数：填充 / 不模糊 / 不压暗 / 不透明 100%");
    }

    // 没设图时不写那 5 个字段 —— 没有图是常态，写一堆 bg*=0 只是让文件变长
    {
        AppearancePreset p;
        p.name = L"没图";
        const std::string line = ExportPreset(p);
        Check(line.find("bgImage") == std::string::npos,
              "★ 没设图的预设里不含 bg* 字段（没图是常态）");
    }

    // 设了图就往返
    {
        AppearancePreset p;
        p.name      = L"带图";
        p.bgImage   = "C:/pics/bg.jpg";
        p.bgFit     = 1;
        p.bgBlur    = 12;
        p.bgDim     = 30;
        p.bgOpacity = 80;

        AppearancePreset back;
        Check(ImportPreset(ExportPreset(p), back), "带图的预设能导出再导入");
        Check(back.bgImage == p.bgImage, "★ 路径往返一致");
        Check(back.bgFit == 1 && back.bgBlur == 12 &&
              back.bgDim == 30 && back.bgOpacity == 80, "★ 四个参数往返一致");
    }

    // ★ 向前兼容：没有 bgImage 的老预设读进来 = 没有背景图
    {
        const auto v = ParsePresets("老预设\tbg=1C1C1E;current=FFFFFF\n");
        Check(v.size() == 1 && v[0].bgImage.empty(),
              "★ 老预设读进来没有背景图（向前兼容）");
    }

    // 夹取：模糊和压暗越界都会实打实地出问题（跑很久 / 图全黑）
    {
        const auto v = ParsePresets("A\tbgImage=x.jpg;bgBlur=9999;bgDim=9999;bgFit=99\n");
        Check(v.size() == 1, "带越界参数的预设仍能解析");
        Check(v[0].bgBlur == kBgBlurMax, "★ bgBlur 被夹到上限");
        Check(v[0].bgDim  == kBgDimMax,  "★ bgDim 被夹到上限（不是全黑）");
        Check(v[0].bgFit  == kBgFitMax,  "★ bgFit 被夹到上限");
    }

    // 路径里的分号会截断字段，导出时要被换掉
    {
        AppearancePreset p;
        p.name    = L"怪路径";
        p.bgImage = "C:/a;b/c.jpg";
        AppearancePreset back;
        Check(ImportPreset(ExportPreset(p), back), "含分号的路径不会让字段截断");
        Check(back.bgImage.find(';') == std::string::npos,
              "★ 路径里的分号被换掉了（否则后面的字段全丢）");
    }

    // ---- 手动构图（D-103）----

    // 三个值是一组：只在 Manual 模式下写出去。
    // 分开写的话读回来另外两个会退回默认，构图就和存的时候不是一回事了。
    {
        AppearancePreset p;
        p.name      = L"拖过的";
        p.bgImage   = "C:/pics/bg.jpg";
        p.bgFit     = 4;   // Manual
        p.bgZoomPct = 175;
        p.bgOffsetXPct = -40;
        p.bgOffsetYPct = 25;

        AppearancePreset back;
        Check(ImportPreset(ExportPreset(p), back), "手动构图的预设能导出再导入");
        Check(back.bgZoomPct == 175 && back.bgOffsetXPct == -40 && back.bgOffsetYPct == 25,
              "★ 手动构图三个值一起往返（一个都不能丢）");
    }

    // 非 Manual 模式不写那三个字段 —— 它们不参与绘制
    {
        AppearancePreset p;
        p.name    = L"自动的";
        p.bgImage = "C:/pics/bg.jpg";
        p.bgFit   = 0;   // Cover
        const std::string line = ExportPreset(p);
        Check(line.find("bgZoom") == std::string::npos,
              "★ 非 Manual 模式不写 bgZoom（那几个值不参与绘制）");
    }

    // ★ 向前兼容：老预设读进来 = 铺满且居中
    {
        const auto v = ParsePresets("老预设\tbgImage=x.jpg;bgFit=4\n");
        Check(v.size() == 1, "老预设仍能解析");
        Check(v[0].bgZoomPct == 100 && v[0].bgOffsetXPct == 0 && v[0].bgOffsetYPct == 0,
              "★ 没有 bgZoom/bgOffX/bgOffY 的老预设 -> 铺满且居中（向前兼容）");
    }

    // 夹取走 ClampBgManual：缩放下限 100、偏移 ±100
    {
        const auto v = ParsePresets("A\tbgImage=x.jpg;bgZoom=5;bgOffX=9999;bgOffY=-9999\n");
        Check(v.size() == 1, "越界的手动参数仍能解析");
        Check(v[0].bgZoomPct == kBgZoomMinPct,
              "★ bgZoom=5 被夹到 100（再小会露出没图盖住的边）");
        Check(v[0].bgOffsetXPct == kBgOffsetMaxPct && v[0].bgOffsetYPct == kBgOffsetMinPct,
              "★ 偏移被夹到 ±100");
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
    TestMaxPresets();
    TestControlColors();
    TestBackground();
    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
