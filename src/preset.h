#pragma once

#include <windows.h>

#include "bg_math.h"   // BgFit 的取值范围常量（kPresetClamp 要用）

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 外观预设 —— **纯逻辑层**，不碰 SDK，能进离线单测台（preset 组）。
//
// 【为什么要预设】用户 2026-09-26 提的：「其实应该加一个预设系统」。
// 动机接在前面两件事上 —— 他说过这个插件的界面「像千千静听的皮肤系统」，
// 也提过想加 QQ 那种背景自定义。预设就是那个方向的第一步：
// 把一整套外观存成有名字的快照，一键换。
//
// 【★ 预设里刻意**只装外观**，不装另外两类东西】
//
//   装：6 个颜色、整体不透明度、字体族、字号百分比、通透度模式。
//
//   不装 ①：**窗口位置与尺寸**。
//     那是"窗口状态"，不是外观。切个配色就把面板挪到别处、变个大小，
//     属于纯粹的惊吓 —— 而且用户好不容易摆好的位置会丢。
//
//   不装 ②：**阅读偏好**（显示行数 span / 当前行位置 currentRatio / 正文字向）。
//     那三个是"我怎么读歌词"，和"它长什么样"是两回事。
//     换皮肤时把"当前行在 30% 还是 50%"也一起改掉，用户会以为界面坏了。
//
// 【格式】一条预设一行，`名字 \t key=value;key=value;...`
//   * 用 `key=value` 而不是固定列数：将来加字段时老预设照样能读（缺的用默认值），
//     而固定列数一改就全废；
//   * 文本表天生可读可改，用户能手工编辑，也便于导入导出时人工检查。
// ---------------------------------------------------------------------------

namespace lyricus {

// 控件配色的两种模式（AppearancePreset::ctrlMode）。
//
// ⚠️ 定义在 AppearancePreset **之前** —— 它的成员初值要用 kCtrlAuto，
//    而 C++ 的成员初值表达式是在类定义处求值的，常量必须先可见。
constexpr int  kCtrlAuto   = 0;   // 从面板底色推导（默认，永远不会配出看不见的组合）
constexpr int  kCtrlCustom = 1;   // 用预设里那 4 个基色
constexpr int  kCtrlMin    = 0;
constexpr int  kCtrlMax    = 1;

// 一套外观预设。
//
// 字段刻意**和 PanelAppearance / LyricDisplayConfig 一一对应**，不在这里
// 另立一套命名 —— 落盘时多一层映射就多一处会写错的地方。
struct AppearancePreset {
    std::wstring name;

    // ---- 来自 PanelAppearance ----
    COLORREF header  = RGB(235, 235, 240);   // 曲名
    COLORREF current = RGB(255, 255, 255);   // 当前歌词行
    COLORREF normal  = RGB(172, 172, 180);   // 其它歌词行
    COLORREF dim     = RGB(150, 150, 158);   // 次要文字
    COLORREF warn    = RGB(205, 165, 165);   // 警告文字
    COLORREF bg      = RGB(28, 28, 30);      // 面板底色
    int      alpha   = 215;                  // 整体不透明度 0..255

    // ---- 来自 LyricDisplayConfig ----
    // 空串 = **跟随宿主界面字体**（DUI / CUI），这是出厂行为。
    std::string fontFace;
    int         fontPct = 100;               // 字号百分比（50..300）

    // ---- 来自 cfg_backdrop_mode ----    // 刻意用 int 而不是 BackdropMode 枚举：那个枚举属于窗口层，
    // 而这一层要能脱离 SDK 单独编译进单测台。
    //
    // ⚠️ 取值是 **BackdropMode 的原始值**，照抄 config.h 里那份定义：
    //     0 = None        自绘不透明底色（最稳的兜底）
    //     1 = Mica        采样桌面壁纸，不透明
    //     2 = Acrylic     毛玻璃
    //     3 = MicaAlt     Mica 的强着色变体
    //     4 = Translucent 分层窗口整体半透明（出厂默认）
    //   **默认值是 4**（= 出厂那套）。这里一开始写了 2 并注释成"半透明"，
    //   是照着自己想当然的顺序编的 —— 而枚举里有 5 个值、顺序也不同。
    int backdropMode = 4;

    // ---- 控件配色（D-093）----
    //
    // 【为什么要有模式开关】用户 2026-09-26：
    //   「提供两个选项，自动推导和自定义。前者系统会自己调节，后者让用户自己更改。」
    //
    // 自动 = 从面板底色推导 —— **任何预设下都看得见**（那是 D-092 修的东西）；
    // 自定义 = 用户指定。两者并存而不是二选一：自动保证配不出"看不见"的组合，
    // 自定义给想要个性的人留口子。
    int ctrlMode = kCtrlAuto;

    // ⚠️ **只有 4 个基色**，而且仅在自定义模式下参与绘制。
    //
    // 控制条上一共有 11 类颜色（按钮的悬停/按下、图标的普通/主操作/悬停/按下、
    // 滑块的轨道/填充、时间文字、音量图标、浮层底板）。**全暴露给用户太多了** ——
    // 挑颜色本身就是负担，何况还得保证它们互相搭配。
    //
    // 所以每组只给一个基色，组内其余由它推导（见 control_window.cpp 的 ApplyControlColors）。
    // 这几个默认值是**照默认深底色的推导结果**填的，于是"切到自定义"的起点
    // 和"自动"看起来一样 —— 用户是在现有配色上微调，而不是从零开始配。
    COLORREF ctrlButton = RGB(58, 62, 72);     // 按钮底（悬停/按下由它推）
    COLORREF ctrlIcon   = RGB(200, 200, 208);  // 图标（其它状态由它推）
    COLORREF ctrlSlider = RGB(206, 210, 220);  // 滑块**填充**（轨道由它推）
    COLORREF ctrlText   = RGB(190, 190, 198);  // 时间文字

    // ---- 背景图（D-098）----
    //
    // 和 PanelAppearance 里的同名字段一一对应。空路径 = 没有背景图，
    // 也就是回到纯色底（从前一直的行为），所以老预设天然兼容。
    //
    // ⚠️ 这是**机器相关的绝对路径**：预设分享给别人之后多半无效。
    //    刻意不做"把图片嵌进预设"—— 一张图几 MB，塞进那个纯文本格式里
    //    会让它彻底没法看、也没法手工编辑，而那个格式的全部价值就在于
    //    "能打开看一眼、能手动改一行"。读不到图就当作没有背景图。
    std::string bgImage;
    int bgFit     = 0;     // BgFit：0=填充 1=适应 2=拉伸 3=平铺
    int bgBlur    = 0;     // 磨砂半径（96dpi 逻辑像素）
    int bgDim     = 0;     // 压暗 % —— 保证歌词能读清
    int bgOpacity = 100;   // 图片不透明度 %

    // 手动构图（D-103）。只在 bgFit == Manual(4) 时参与。
    // 存百分比而不是像素的理由见 bg_math.h 的 BgManual。
    int bgZoomPct    = 100;
    int bgOffsetXPct = 0;
    int bgOffsetYPct = 0;
};

// 内置的几套。**顺序就是它们在下拉里的顺序**（第一套是「默认」）。
//
// 每一套都是"完整的"外观（所有字段都有值），不依赖当前配置 ——
// 否则用户切过去再切回来，中间态会污染。
std::vector<AppearancePreset> BuiltinPresets();

// ---- 文本表 <-> 内存表 ----

std::string FormatPresets(const std::vector<AppearancePreset>& presets);
std::vector<AppearancePreset> ParsePresets(const std::string& text);

// 按名字查。查不到返回 nullptr。
// ⚠️ 返回的是**指向容器内部**的指针，容器一变就失效 —— 只在查完立刻用。
const AppearancePreset* FindPreset(const std::vector<AppearancePreset>& presets,
                                   const std::wstring& name);

// ---- 增删改 ----

// 在文本表上加/改/删一条，返回新表。
//
//   preset != nullptr  -> 按 name 新增或覆盖
//   preset == nullptr  -> 按 name 删除
//
// changedOut：这次编辑有没有真的改动表。调用方靠它决定要不要写盘
//（和 folder_hint_table 的 ApplyFolderHintEdit 同一个约定）。
//
// 名字里的 TAB / 换行会被替换成空格 —— 否则整张表会被撑坏。
std::string ApplyPresetEdit(const std::string& currentText,
                            const std::wstring& name,
                            const AppearancePreset* preset,
                            bool* changedOut = nullptr);

// ---- 单条导出 / 导入（这就是"能分享"的那部分）----

// 把一条预设导成可分享的文本（**就是它在表里的那一行**，末尾带换行）。
std::string ExportPreset(const AppearancePreset& preset);

// 从分享文本导入。能容忍前后空白、BOM、CRLF。
// 失败返回 false 并保持 out 不变 —— 导入别人的文件时不该崩，也不该半途改状态。
bool ImportPreset(const std::string& text, AppearancePreset& out);

// 表里最多留多少条。超了从**最旧的**开始丢。
constexpr size_t kMaxPresets = 64;

// 各字段的合法区间。
//
// ★ 提成常量是因为它们**被写在不止一个地方**（ClampPreset 和单测的断言）。
//   backdropMode 的上限刚因为两处不同步挂过一次断言：实现改成了 0..4，
//   测试里还留着 0..2，于是"每套都是完整的外观"这条直接红了 ——
//   而它想验的其实是"字段合法"，不是"上限是不是 2"。
//
// ⚠️ 名字都带 `kPreset` 前缀：`config.h` 里已经有一份 `kMinAlpha` / `kMaxAlpha`
//    （那是**配置层**的不透明度区间），两处语义不同、不能共用一个名字 ——
//    不加前缀直接撞车，实测 C2374 重定义。
constexpr int  kPresetMinAlpha    = 0;
constexpr int  kPresetMaxAlpha    = 255;
constexpr int  kPresetMinFontPct  = 50;
constexpr int  kPresetMaxFontPct  = 300;
// ⚠️ BackdropMode 有 **5** 个值（见 config.h）：None/Mica/Acrylic/MicaAlt/Translucent。
constexpr int  kPresetMinBackdrop = 0;
constexpr int  kPresetMaxBackdrop = 4;
// 控件配色的模式常量定义在文件开头（AppearancePreset 的成员初值要用它）。

} // namespace lyricus
