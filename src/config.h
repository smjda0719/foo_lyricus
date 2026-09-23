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

    bool operator!=(const LyricDisplayConfig& o) const {
        return fontPct != o.fontPct || span != o.span || currentRatio != o.currentRatio;
    }
    bool operator==(const LyricDisplayConfig& o) const { return !(*this != o); }
};

LyricDisplayConfig GetLyricDisplayConfig();

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
