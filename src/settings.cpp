#include "stdafx.h"
#include "config.h"
#include "lyric.h"
#include "debug_log.h"

#include <SDK/advconfig_impl.h>

#include <cmath>

// ---------------------------------------------------------------------------
// 设置项：注册到 foobar2000 的「首选项 -> 高级 -> Lyricus」。
//
// 这里**只放工厂定义和读取函数**，不写业务逻辑。
// 显示的三个参数由各宿主轮询（见 config.h 的说明），
// 搜索 / 在线相关的参数由 lyric_search.cpp 与 online_lyric.cpp 在用到时读取。
//
// 选 advconfig 而不是自建首选项页的理由：
//   * 高级页自带搜索框，用户搜 "lyric" 就能找到全部项；
//   * 不用写一个 preferences_page 子类，也不用管页面的创建/销毁；
//   * 缺点是没有中文标签和分组美化 —— 个人自用够了，
//     真要做漂亮界面时，把这里换成 preferences_page 读写同样的 configStore 键即可，
//     存储层不用动。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

// GUID 段沿用 1A7C3E90-2B41-4C58-9D6E-0F1A2B3C4Dxx，末字节手工分配。
//
// ⚠️ 加新 GUID 之前**务必先 grep 全工程的 `0x4d,0x`**。
//    撞了不会报错、不会警告，编译链接一路绿灯 —— 已经踩过一次：
//    这里原本用 0x10 / 0x11，正好和 dui_element.cpp 的元素 GUID、
//    cui_panel.cpp 的面板 GUID 撞上，是 grep 出来才发现的。
//
// 当前分段（末字节）：
//   0x01-0x06  config.cpp    面板位置 / 尺寸 / 可见性 / 背景材质
//   0x07-0x0C  menu.cpp      菜单项
//   0x0D       config.cpp    手动歌词映射表
//   0x0F       menu.cpp      「恢复自动匹配歌词」
//   0x10-0x1F  UI 相关       dui_element(0x10) / cui_panel(0x11) / menu(0x12-0x14)
//   0x20-0x27  本文件         设置项
//   0x28       本文件         正文=翻译
//   0x38       config.cpp    按文件夹的歌词线索
//   0x39       config.cpp    「当前行位置」基准迁移标记
const GUID kBranchGuid       = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x20}};
const GUID kFontPctGuid      = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x21}};
const GUID kSpanGuid         = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x22}};
const GUID kCurrentRatioGuid = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x23}};
const GUID kExtraDirGuid     = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x24}};
const GUID kFuzzyGuid        = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x25}};
const GUID kUseTagsGuid      = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x26}};
const GUID kOnlineGuid       = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x27}};
// 0x28 起是新段：0x20-0x27 已经用满（原来是照"最多 8 项"排的）
const GUID kTlPrimaryGuid    = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x28}};
const GUID kFontFaceGuid     = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x29}};

// 挂在高级首选项树的根下。Lyricus 只有一个分支，优先级取 0 就行。
advconfig_branch_factory g_branch("Lyricus", kBranchGuid, advconfig_branch::guid_root, 0);

// 参数顺序：(显示名, configStore 变量名, 自身 GUID, 父 GUID, 排序优先级,
//             初值, 最小值, 最大值)
//
// 必须用带 varName 的那个重载。不带 varName 的重载内部会调
// fb2k::advconfig_autoName(guid)，把变量名变成一串 GUID ——
// 首选项里看着莫名其妙，将来想手工改配置或做迁移也认不出来。
//
// 最小值必须**严格小于**最大值：advconfig_impl.h:163 有 PFC_ASSERT(min < max)，
// Debug 构建下相等就会断言失败。
advconfig_integer_factory g_fontPct(
    "歌词字号（百分比，100 = 默认）", "lyricus.fontPct",
    kFontPctGuid, kBranchGuid, 0, 100, 50, 300);

advconfig_integer_factory g_span(
    "歌词显示行数（当前行上下各显示几行，0 = 自适应）", "lyricus.span",
    kSpanGuid, kBranchGuid, 1, 0, 0, 30);

advconfig_integer_factory g_currentRatio(
    "当前行的垂直位置（占**整个面板**高度的百分比，50 = 正中，越小越靠上）",
    "lyricus.currentRatio",
    kCurrentRatioGuid, kBranchGuid, 2, 50, 0, 100);

advconfig_string_factory g_extraDir(
    "额外的歌词目录（集中放歌词时用，留空则只在音频同目录里找）", "lyricus.extraLyricDir",
    kExtraDirGuid, kBranchGuid, 3, "");

advconfig_checkbox_factory g_fuzzy(
    "搜索歌词时启用模糊匹配（文件名不完全一致也能找到）", "lyricus.fuzzyMatch",
    kFuzzyGuid, kBranchGuid, 4, true);

advconfig_checkbox_factory g_useTags(
    "搜索歌词时用标签辅助匹配（读取 artist / title）", "lyricus.matchByTags",
    kUseTagsGuid, kBranchGuid, 5, true);

advconfig_checkbox_factory g_online(
    "本地找不到时联网查询歌词（LRCLIB）", "lyricus.onlineLookup",
    kOnlineGuid, kBranchGuid, 6, true);


// 有翻译时哪个当正文。
//
// 【为什么要给这一档】听日语/同人曲的人很多只看得懂译文，原文对他们反而是参照。
// 用户 2026-09-24 提：「有一些用户可能喜欢把翻译当成主要的歌词」。
// 这不是"高级选项"，是两种正当的读法。
advconfig_checkbox_factory g_tlPrimary(
    "歌词有翻译时，把**翻译**当正文显示（原文降为小字参照行）", "lyricus.translationPrimary",
    kTlPrimaryGuid, kBranchGuid, 7, false);

// 用户自己指定的歌词字体族。留空 = 跟随宿主界面字体（DUI/CUI），
// 宿主也没给（浮动面板）就用渲染层的默认字体。
//
// 【为什么放这里而不是只做首选项页的一个按钮】三个宿主共用 GetLyricDisplayConfig()
// 的快照，放进来之后"用户改了字体 -> 宿主轮询发现变了 -> 重绘"这条链路是**免费**的，
// 不需要任何额外通知机制 —— 这正是当初选轮询换来的好处。
advconfig_string_factory g_fontFace(
    "歌词字体（留空 = 跟随宿主界面字体；填字体名，如「微软雅黑」「Consolas」）",
    "lyricus.fontFace",
    kFontFaceGuid, kBranchGuid, 8, "");

} // namespace

LyricDisplayConfig GetLyricDisplayConfig() {
    LyricDisplayConfig c;

    // 这三个 get() 每次都会走一遍 fb2k::configStore 查表。
    // 不用 advconfig_*_factory_cached 的原因见 config.h 的注释：
    // 那个缓存初始化一次之后永不失效，用户在首选项里改了值我们看不到。
    // 打开播放器后 250ms 一次、每次三次查表，代价可以忽略。
    c.fontPct      = static_cast<int>(g_fontPct.get());
    c.span         = static_cast<int>(g_span.get());
    c.currentRatio = static_cast<int>(g_currentRatio.get());
    c.tlPrimary    = g_tlPrimary.get();

    // 字体族：pfc::string8 内部就是 UTF-8，直接搬进 std::string。
    // 空串是**正常值**（= 跟随宿主），不是"没读到"。
    {
        pfc::string8 face;
        g_fontFace.get(face);
        c.fontFace.assign(face.get_ptr(), face.length());
    }

    // 兜底夹取。advconfig 自己会 clip，但配置文件是文本的，
    // 手工编辑或跨版本残留都可能塞进超范围的值。
    // 取回来是 uint64_t，上界要夹；下界只有字号需要夹
    // （范围从 50 起，配置里写 0 会让字缩到看不见）。
    if (c.fontPct < 50)       c.fontPct = 50;
    if (c.fontPct > 300)      c.fontPct = 300;
    if (c.span > 30)          c.span = 30;
    if (c.currentRatio > 100) c.currentRatio = 100;

    return c;
}

void SetLyricFontPct(int pct) {
    // set() 内部走 set_state_int()，会按注册时的 min/max 夹取，
    // 所以这里不用自己判边界。
    g_fontPct.set(static_cast<uint64_t>(pct));
}

void SetLyricSpan(int span) {
    g_span.set(static_cast<uint64_t>(span));
}

void SetLyricCurrentRatio(int ratio) {
    g_currentRatio.set(static_cast<uint64_t>(ratio));
}

void SetLyricFontFace(const std::string& faceUtf8) {
    // 传空串 = 恢复"跟随宿主 / 默认"。这是合法的设置值，不是清空错误。
    g_fontFace.set(faceUtf8.c_str());
}

// ---------------------------------------------------------------------------
// 一次性迁移：currentRatio 的基准从"歌词区"改成"整个面板"。
//
// 【为什么必须迁】用户把浮动面板的当前行调到了"刚好"（旧基准下的 60）。
// 基准一换，同一个 60 在新基准里含义变了，画面会直接掉下去 —— 那是
// 用户明确认可过的位置，不能动。所以换算成"在面板里的同一个位置"。
//
// 【换算系数怎么来的】取自浮动面板 460x150（逻辑像素）的实际几何，
// 由 captures/lyricus-20260925-004755.png 在 200% 缩放下量得（物理像素）：
//     面板   = [0, 300]
//     歌词区 = [99, 204]     （上方 S(14)+曲名行+间距；下方控制条，LayoutControls）
// 即：顶部装饰占面板 33%，歌词区占 35%，底部控制条占 32%。于是
//     新 = 33 + 旧 * 0.35           （端点自洽：旧 0->33，旧 100->68）
//
// 60 -> 54，和用户现在看到的位置（锚点 ≈ 面板 165/300 = 55%）差不到 2 个逻辑像素。
//
// 【50 为什么不参与换算】设置项的文档契约是"50 = 正中"。旧基准下歌词区
// 上下装饰几乎对称（33% / 32%），50 本来就在面板正中附近，换算成 51 反而
// 破坏了这条契约 —— 没亲手调过这一项的用户（存的就是默认 50）不该被迁走。
// ---------------------------------------------------------------------------
void MigrateCurrentRatioToPanelBase() {
    if (cfg_ratio_base_ver.get() >= 1) return;
    cfg_ratio_base_ver.set(1);          // 先落标记：换算中途出错也不该反复迁

    const int oldRatio = static_cast<int>(g_currentRatio.get());
    if (oldRatio == 50) {
        DebugLog("迁移: 当前行位置保持 50（正中，基准换了也一样）");
        return;
    }

    constexpr double kTopPct  = 33.0;   // 顶部装饰占面板高度的百分比
    constexpr double kSpanPct = 35.0;   // 歌词区占面板高度的百分比

    int newRatio = static_cast<int>(std::lround(kTopPct + oldRatio * kSpanPct / 100.0));
    if (newRatio < 0)   newRatio = 0;
    if (newRatio > 100) newRatio = 100;

    g_currentRatio.set(static_cast<uint64_t>(newRatio));

    DebugLog("迁移: 当前行位置 %d%% -> %d%%（基准改成了整个面板，画面位置不变）",
             oldRatio, newRatio);
}

std::string DescribeDisplayConfig(const LyricDisplayConfig& c) {
    std::string s = "字号=";
    s += std::to_string(c.fontPct);
    s += "%  行数=";
    s += (c.span > 0 ? std::to_string(c.span) : std::string("自适应"));
    s += "  当前行位置=";
    s += std::to_string(c.currentRatio);
    s += "%";
    // 只在**开着**时才写出来：默认值不刷日志，免得每次换曲都多出一截
    if (c.tlPrimary) s += "  正文=翻译";
    // 字体同理：空 = 跟随宿主（常态），写出来只是噪音
    if (!c.fontFace.empty()) {
        s += "  字体=";
        s += c.fontFace;
    }
    return s;
}

LyricSearchConfig GetLyricSearchConfig() {
    LyricSearchConfig c;

    pfc::string8 tmp;
    g_extraDir.get(tmp);
    // pfc::string8 内部就是 UTF-8，直接转宽。
    c.extraDir = Utf8ToWide(tmp.get_ptr());

    // 结尾的斜杠先去掉，调用方拼路径时统一自己加，避免出现双斜杠。
    while (!c.extraDir.empty() &&
           (c.extraDir.back() == L'\\' || c.extraDir.back() == L'/')) {
        c.extraDir.pop_back();
    }

    c.fuzzy   = g_fuzzy.get();
    c.useTags = g_useTags.get();
    return c;
}

bool OnlineLyricEnabled() {
    return g_online.get();
}

} // namespace lyricus
