#pragma once

#include <SDK/cfg_var.h>

#include <windows.h>   // COLORREF

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Lyricus 配置项
//
// 每个 cfg_var 都必须有一个全局唯一的 GUID —— 撞了会读到别的组件的配置。
// 命名空间用 cfg_var_modern（SDK/config/cfg_var.h 里的现代实现，
// 而不是 cfg_var_legacy 那套）。
// ---------------------------------------------------------------------------

#include "preset.h"   // AppearancePreset（预设系统，见 D-088）

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

// 控件配色（D-093）。放这里是因为 PanelAppearance 是浮动面板外观的完整快照 ——
// 面板轮询它就能发现控件色变了，不需要第二条通知路径。
// GUID 末字节 0x40~0x44（0x02~0x0d 和 0x30~0x3c 已被占用）。
extern cfg_var_modern::cfg_int  cfg_app_ctrl_mode;
extern cfg_var_modern::cfg_int  cfg_app_ctrl_btn;
extern cfg_var_modern::cfg_int  cfg_app_ctrl_icon;
extern cfg_var_modern::cfg_int  cfg_app_ctrl_slider;
extern cfg_var_modern::cfg_int  cfg_app_ctrl_text;

// 背景图（D-098）。GUID 末字节 0x50~0x54。
//
// ⚠️ 这几个是加背景图那轮漏掉的 —— 当时只有 config.cpp 自己在读写，
//    没声明也能编译，于是没人发现。等有了第二个使用者（预览控件）才会
//    报"未声明的标识符"，而那时离改动已经隔了几轮。
//    加字段时顺手把 extern 也写上，别等编译器替你发现。
extern cfg_var_modern::cfg_string cfg_app_bg_image;
extern cfg_var_modern::cfg_int  cfg_app_bg_fit;
extern cfg_var_modern::cfg_int  cfg_app_bg_blur;
extern cfg_var_modern::cfg_int  cfg_app_bg_dim;
extern cfg_var_modern::cfg_int  cfg_app_click_through;
extern cfg_var_modern::cfg_int  cfg_app_bg_opacity;
// 手动构图（D-103）。GUID 末字节 0x55~0x57。
extern cfg_var_modern::cfg_int  cfg_app_bg_zoom;
extern cfg_var_modern::cfg_int  cfg_app_bg_offx;
extern cfg_var_modern::cfg_int  cfg_app_bg_offy;

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

// 在线歌词源的**顺序与启用状态**，格式 `netease:1,kugou:0,lrclib:1`。
//
// 用户在菜单的「歌词源顺序...」面板里调，见 source_order.h。
// 空串 = 没配置过 = 出厂顺序且全部启用。
extern cfg_var_modern::cfg_string cfg_lyric_source_order;

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

    // 用户自己指定的字体族（UTF-8）。**空串 = 没设过**，此时按下面的优先级回落：
    //
    //     用户指定的  >  宿主界面字体（DUI / CUI 会给）  >  渲染层默认（Segoe UI）
    //
    // 【为什么要有它】用户 2026-09-26 看过"接上宿主字体"的效果之后说
    // 「字体确实变了，也许可以给用户自定义字体的权限」。
    // 前两级各解决了一半：跟宿主让 DUI/CUI 里的歌词和界面协调，
    // 但用户可能就是想用别的字体（宋体配古风歌词、等宽体配代码注释）。
    //
    // ⚠️ **只存字体族，不存字号** —— 字号归 fontPct，两个维度独立。
    //    混在一起的话，用户挑一次字体就会把辛苦调好的字号一并覆盖。
    std::string fontFace;

    bool operator!=(const LyricDisplayConfig& o) const {
        return fontPct != o.fontPct || span != o.span ||
               currentRatio != o.currentRatio || tlPrimary != o.tlPrimary ||
               fontFace != o.fontFace;
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

// 设置用户指定的字体族。传空串 = 恢复"跟随宿主 / 默认"。
// 和上面几个一样：写完各宿主下一帧轮询就能读到，不需要额外通知。
void SetLyricFontFace(const std::string& faceUtf8);

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

    // ---- 鼠标穿透（D-130）----
    //
    // true = 面板不接收鼠标，点击落到**下面的窗口**上。
    //
    // 【为什么需要】面板是置顶的，挡住底下窗口时用户想点下面的东西就得先移开它 ——
    // 移开又得拖回来，一来一回。开了这个就"当它不存在"。
    //
    // ⚠️ **逃生舱是 Ctrl**：穿透时按住 Ctrl 就不穿透（见 control_window 的
    //    WM_NCHITTEST）。没有它的话这个开关就是个单向门 —— 打开之后面板
    //    完全点不动，只能从 foobar2000 主窗口去开首选项改回来，
    //    而主窗口要是也被挡着就更麻烦。
    bool     clickThrough = false;

    // ---- 控件配色（D-093）----
    //
    // ⚠️ 控制条上那 11 类颜色**不在这里**，只放 4 个**基色** + 一个模式标记：
    //     自动   -> 这些都不用，全部从 bg 推导（见 DrawControls）
    //     自定义 -> 用这四个基色，组内其余（悬停 / 按下 / 轨道 / 主操作图标）
    //               由它们推出来
    //   把 11 类颜色全暴露给用户太多了 —— 挑色本身就是负担，
    //   何况还得保证它们互相搭配。
    //
    // 【为什么放在 PanelAppearance 里】它本来就是"浮动面板外观"的**完整快照**。
    //   放这儿之后，面板靠轮询它就能发现控件色变了，不需要第二条通知路径 ——
    //   和配色、字体共用同一条链路。
    //   ⚠️ 所以 operator== **必须**带上这几个字段，漏一个就等于"改了不生效"。
    int      ctrlMode   = kCtrlAuto;
    COLORREF ctrlButton = RGB(58, 62, 72);     // 按钮底
    COLORREF ctrlIcon   = RGB(200, 200, 208);  // 图标
    COLORREF ctrlSlider = RGB(206, 210, 220);  // 滑块填充
    COLORREF ctrlText   = RGB(190, 190, 198);  // 时间文字

    // ---- 背景图（D-098）----
    //
    // 空路径 = 没有背景图，回到纯色底（也就是从前一直的行为）。
    //
    // ⚠️ 存的是**机器相关的绝对路径**，所以预设分享给别人之后这个字段
    //    多半是无效的。刻意不做"把图片嵌进预设"那件事 —— 一张图几 MB，
    //    塞进那个纯文本格式里会让预设文件完全没法看、也没法手工编辑。
    //    读不到就当作"没有背景图"，并在日志里留一行原因。
    std::string bgImage;
    int bgFit     = 0;     // BgFit：0=填充 1=适应 2=拉伸 3=平铺
    int bgBlur    = 0;     // 磨砂半径（96dpi 逻辑像素）0..40
    int bgDim     = 0;     // 压暗 % 0..90 —— 保证歌词能读清
    int bgOpacity = 100;   // 图片不透明度 % 0..100

    // 手动构图（D-103）。只在 bgFit == Manual(4) 时参与绘制。
    //
    // ⚠️ 存**百分比**而不是像素：面板尺寸会变（拖窗口、换 dpi、换预设里的
    //    面板大小），存像素的话用户拖一次窗口构图就整体偏掉，
    //    而且偏得没规律 —— 看起来像"图的定位坏了"。见 bg_math.h 的 BgManual。
    int bgZoomPct    = 100;   // 100 = 刚好铺满（下限）
    int bgOffsetXPct = 0;     // -100..100，0 = 居中
    int bgOffsetYPct = 0;

    bool operator==(const PanelAppearance& o) const {
        return header == o.header && current == o.current && normal  == o.normal &&
               dim    == o.dim    && warn    == o.warn    && bg      == o.bg &&
               alpha  == o.alpha  &&
               ctrlMode   == o.ctrlMode   &&
               ctrlButton == o.ctrlButton && ctrlIcon   == o.ctrlIcon &&
               ctrlSlider == o.ctrlSlider && ctrlText   == o.ctrlText &&
               bgImage  == o.bgImage  && bgFit     == o.bgFit &&
               bgBlur   == o.bgBlur   && bgDim     == o.bgDim &&
               bgOpacity == o.bgOpacity &&
               bgZoomPct == o.bgZoomPct &&
               bgOffsetXPct == o.bgOffsetXPct && bgOffsetYPct == o.bgOffsetYPct &&
               clickThrough == o.clickThrough;
    }
    bool operator!=(const PanelAppearance& o) const { return !(*this == o); }
};

PanelAppearance GetPanelAppearance();
void            SetPanelAppearance(const PanelAppearance& a);

// ---------------------------------------------------------------------------
// 外观预设
// ---------------------------------------------------------------------------

// 预设表存哪。**只存用户自己存的那些** —— 内置 4 套是代码里生成的，不落盘。
// 这样升级版本时内置预设能跟着更新，而用户改过的同名条目会覆盖内置那份。
extern cfg_var_modern::cfg_string cfg_appearance_presets;

// 完整的预设列表 = **内置的 + 用户存的**，同名的以用户那份为准。
//
// 【为什么同名的用户那份赢】用户改了「暗色」之后要能存下来。
// 存成"暗色 2"之类的新名字是另一种做法，但那样想恢复内置的「暗色」
// 就变得很别扭（得先自己删掉那条）。
std::vector<AppearancePreset> GetAppearancePresets();

// 存一条（新增，或覆盖同名）。空名字忽略。
void SaveAppearancePreset(const std::wstring& name, const AppearancePreset& preset);

// 删掉**用户存的**那一条，返回"有没有真的删掉东西"。
//
// ⚠️ 内置的删不掉（它们没落盘）—— 但如果用户存过同名覆盖，
//    删它就是"**恢复内置默认**"，那正是这个函数最有用的地方。
//    对没覆盖过的内置名字，它返回 false，UI 该据此提示而不是假装删掉了。
bool DeleteAppearancePreset(const std::wstring& name);

// 这个名字是不是**内置的**。UI 用它区分"删除"的两种语义。
bool IsBuiltinPresetName(const std::wstring& name);

// 把一套预设**应用到实际外观**：6 色 + 不透明度 + 字体族 + 字号 + 通透度。
//
// ⚠️ 刻意**不碰**窗口位置尺寸和阅读偏好（行数 / 当前行位置 / 正文字向）——
//    理由写在 preset.h 开头。
void ApplyAppearancePreset(const AppearancePreset& preset);

// 把整体不透明度限制在「看得见又不至于完全糊住」的范围内。
// 0 会让面板彻底消失、255 就是完全不透明，两头都没有实际意义。
constexpr int kMinAlpha = 60;
constexpr int kMaxAlpha = 255;
int ClampAlpha(int a);

} // namespace lyricus
