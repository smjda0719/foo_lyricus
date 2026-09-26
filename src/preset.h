#pragma once

#include <windows.h>

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

    // ---- 来自 cfg_backdrop_mode ----
    // 刻意用 int 而不是 BackdropMode 枚举：那个枚举属于窗口层，
    // 而这一层要能脱离 SDK 单独编译进单测台。
    // 取值和 BackdropMode 一致：0=不透明 1=毛玻璃 2=半透明。
    int backdropMode = 2;
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

} // namespace lyricus
