#pragma once

#include <SDK/cfg_var.h>

#include <windows.h>   // COLORREF

#include <string>

// ---------------------------------------------------------------------------
// Lyricus 配置项
//
// 每个 cfg_var 都必须有一个全局唯一的 GUID —— 撞了会读到别的组件的配置。
// 命名空间用 cfg_var_modern（SDK/config/cfg_var.h 里的现代实现，
// 而不是 cfg_var_legacy 那套）。
// ---------------------------------------------------------------------------

namespace lyricus {

// 背景材质模式
enum class BackdropMode : int {
    None        = 0,   // 不使用系统背景（自绘不透明底色，最稳的兜底）
    Mica        = 1,   // 采样桌面壁纸，不透明
    Acrylic     = 2,   // 毛玻璃，透出后面的窗口
    MicaAlt     = 3,   // Mica 的强着色变体
    Translucent = 4,   // 分层窗口整体半透明（自绘，不依赖 DWM 材质）
};

const char* BackdropModeName(BackdropMode mode);

// 独立操作面板：位置与尺寸。-1 表示"从未保存过"，此时用默认值。
extern cfg_var_modern::cfg_int  cfg_panel_x;
extern cfg_var_modern::cfg_int  cfg_panel_y;
extern cfg_var_modern::cfg_int  cfg_panel_w;
extern cfg_var_modern::cfg_int  cfg_panel_h;

// 关闭时是否记住"面板是开着的"，下次启动自动恢复。
extern cfg_var_modern::cfg_bool cfg_panel_visible;

// 背景材质模式（存 BackdropMode 的整数值）
extern cfg_var_modern::cfg_int  cfg_backdrop_mode;

// 手动指定的歌词，**按曲目记住**。
//
// 存储格式：每条一行，「曲目 URL <TAB> 歌词路径」。
// 特例：URL 为空的那条表示「选文件时没在播放」，会被下一首播放的曲目收养。
//
// 之所以做成一张表而不是单个键值对：早期的实现只存一对，一旦播放别的歌
// 就把绑定整个丢弃 —— 结果是「给 A 指定的歌词，换到 B 再换回 A 就忘了」。
extern cfg_var_modern::cfg_string cfg_manual_lyric_map;

// 按文件夹指定的歌词线索（歌手 / 专辑）。见 folder_hint.h 的说明。
//
// 格式：每条一行，`文件夹键 \t 歌手 \t 专辑`。
// 用户那批专辑一个标签都没打，只能靠人工给一句线索 ——
// 实测"歌手"是决定性的那一个词（`查「阿良良木健 哀歌」` 第 1 条就是答案，
// 而 `查「哀歌」` 连前 10 都进不去）。
extern cfg_var_modern::cfg_string cfg_folder_hints;

// 「当前行位置」基准的一次性迁移标记（0 = 还没迁，1 = 已迁到"整个面板"基准）。
//
// 【为什么必须有这个标记】见 D-043：currentRatio 的基准从"歌词区"改成了
// "整个面板"。老用户存的 60 是按歌词区调的，直接按新基准读会让画面往下掉
// 十几个像素 —— 用户明确说过那个位置"刚好"，不能动。所以要迁一次。
// 迁完置 1，之后用户自己怎么调都不再碰。
extern cfg_var_modern::cfg_int cfg_ratio_base_ver;

// 逐曲目的歌词**时间偏移**，按曲目 URL 记住。
//
// 存储格式：每条一行，`曲目 URL \t 毫秒偏移`（正数 = 歌词提前显示）。
//
// 【为什么需要它】用户 2026-09-25 报「有一些歌词整句漂移 —— 声音唱到下一句了，
// 面板还停在上一句」。日志里查到两条放行记录，都是**时长差 7.0 秒**
//（本地 240.0s vs 在线 247.058s，见 D-048）—— 在线歌词是按另一个剪辑版本
// 打的时间轴。这不是 bug，是数据本身对不上；而"这 7 秒在前奏还是尾奏"
// 从时长差**看不出来**，只能靠耳朵判断。所以给一个用户入口，按曲目记住。
extern cfg_var_modern::cfg_string cfg_lyric_offset_map;

// ---------------------------------------------------------------------------
// 设置项：走 foobar2000 的 Advanced 首选项（SDK/advconfig_impl.h）
//
// 为什么这些**不用** cfg_var_modern：
//   advconfig 工厂的存储是 fb2k::configStore，按**字符串名**索引
//   （advconfig_impl.h:174），工厂实例本身就是唯一真相。
//   如果再另开一套 cfg_int，就有两份互不同步的数据 ——
//   用户在首选项里改了，我们读到的还是旧值。
//
// 所以这些项只从下面几个 Get 函数读，**不要另建 cfg_var**。
//
// 另外**不要**改用 advconfig_*_factory_cached：它的 fb2k::configIntCache
// 内部只有 atomic + once_flag（configCache.h:22,39），初始化一次后永不失效。
// 用户在首选项里改值走的是 impl 的 set_state，不经过那个缓存，
// 结果是「设置改了但插件没反应」，直到重启 foobar2000。实测确认过，别踩。
// ---------------------------------------------------------------------------

// 歌词排版。
//
// 每个宿主在自己的刷新周期里取一次快照，和上次的比一下决定要不要重绘 ——
// advconfig 的改动**没有任何通知机制**，轮询是唯一可靠的办法。
// 三个 int 的比较，代价可以忽略。
struct LyricDisplayConfig {
    int fontPct      = 100;   // 字号百分比，100 = 默认（有效范围 50..300）
    int span         = 0;     // 当前行上下各显示几行；0 = 自适应
    int currentRatio = 50;    // 当前行在歌词区里的垂直位置（%），50 = 正中

    // 有翻译时，**哪个当正文**。
    //
    // false（默认）= 原文当正文，翻译作小字参照行。
    // true         = 翻译当正文，原文作小字参照行。
    //
    // 【为什么要有 true 这一档】用户 2026-09-24 提：「有一些用户可能喜欢把翻译
    // 当成主要的歌词」。听日语/同人曲的人很多只看得懂译文，原文对他们反而是参照。
    // 所以这不是"高级选项"，是两种正当的读法。
    bool tlPrimary = false;

    bool operator!=(const LyricDisplayConfig& o) const {
        return fontPct != o.fontPct || span != o.span ||
               currentRatio != o.currentRatio || tlPrimary != o.tlPrimary;
    }
    bool operator==(const LyricDisplayConfig& o) const { return !(*this != o); }
};

LyricDisplayConfig GetLyricDisplayConfig();

// 把老配置里的 currentRatio 从"歌词区基准"换算成"整个面板基准"，只做一次。
//
// 由 initquit::on_init 调用（面板显示之前），不要在渲染热路径里调。
// 换算依据是浮动面板的实际几何，理由写在 settings.cpp 的实现里。
void MigrateCurrentRatioToPanelBase();

// 写入单个显示设置（供菜单里的快捷调整用）。
// 值会被 advconfig 自己夹到合法区间；写完各宿主下一帧轮询就能读到，
// 不需要额外通知 —— 这正是"轮询"这个选择带来的好处。
void SetLyricFontPct(int pct);
void SetLyricSpan(int span);
void SetLyricCurrentRatio(int ratio);

// 把显示设置格式化成一行，供日志用。
//
// 单独抽出来是因为三种宿主（独立面板 / DUI / CUI）都要打这条 ——
// 各写一遍迟早就跑偏。有了它，「改了设置但画面没动」这类问题
// 能直接从日志判断是断在哪一环：是设置没写进去，还是宿主没轮询到，
// 还是画面刷了但显示不对。
std::string DescribeDisplayConfig(const LyricDisplayConfig& c);

// 歌词搜索策略
struct LyricSearchConfig {
    std::wstring extraDir;    // 集中存放歌词的目录，空 = 不启用
    bool         fuzzy   = true;
    bool         useTags = true;
};

LyricSearchConfig GetLyricSearchConfig();

// 本地找不到时是否联网查（LRCLIB）
bool OnlineLyricEnabled();

// ---------------------------------------------------------------------------
// 浮动面板的外观
//
// 【只给独立面板用】DUI 元素和 CUI 面板刻意**不读这里** ——
// 它们跟随宿主主题（用户在 foobar2000 / Columns UI 里改配色，面板就跟着变），
// 那是嵌入面板该有的行为。
//
// 浮动面板没有宿主，所以给它一套自己的配色，并做成可配置的。
//
// 为什么不做成 advconfig 项：色值写成 "#RRGGBB" 手敲太难受，
// 而这里有一个真正的首选项页（带取色器）。存储层还是 fb2k::configStore，
// 两者并不冲突 —— advconfig 只是「把 configStore 暴露到高级页」的一种包装。
// ---------------------------------------------------------------------------
struct PanelAppearance {
    COLORREF header  = RGB(235, 235, 240);   // 曲名
    COLORREF current = RGB(255, 255, 255);   // 当前歌词行
    COLORREF normal  = RGB(172, 172, 180);   // 其它歌词行
    COLORREF dim     = RGB(150, 150, 158);   // 次要文字（无歌词提示等）
    COLORREF warn    = RGB(205, 165, 165);   // 警告文字
    COLORREF bg      = RGB(28, 28, 30);      // 面板底色
    int      alpha   = 215;                  // 整体不透明度 0..255

    bool operator==(const PanelAppearance& o) const {
        return header == o.header && current == o.current && normal  == o.normal &&
               dim    == o.dim    && warn    == o.warn    && bg      == o.bg &&
               alpha  == o.alpha;
    }
    bool operator!=(const PanelAppearance& o) const { return !(*this == o); }
};

PanelAppearance GetPanelAppearance();
void            SetPanelAppearance(const PanelAppearance& a);

// 把整体不透明度限制在「看得见又不至于完全糊住」的范围内。
// 0 会让面板彻底消失、255 就是完全不透明，两头都没有实际意义。
constexpr int kMinAlpha = 60;
constexpr int kMaxAlpha = 255;
int ClampAlpha(int a);

} // namespace lyricus
