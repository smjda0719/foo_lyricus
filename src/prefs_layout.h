#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Lyricus 首选项页的**布局数学** —— 纯函数：给一块矩形和 DPI，算出一组控件矩形。
//
// 【为什么要重做这一页】用户 2026-09-26 看过取色崩溃修复后的界面说
// 「这个界面有点老旧，在 lyricus 那个二级界面实现一个更现代化的面板」。
// 旧版是标准 Win32 控件拼的：GROUPBOX 分组框 + 六个 62x14 的小色块按钮 +
// 系统 trackbar。控件本身画成什么样由系统主题决定，改不动 ——
// 所以这次整页改成**全自绘**，那些控件一个都不留。
//
// 【为什么布局要单独抽出来】它和 control_bar_layout 是同一类东西：
// 「给定尺寸 -> 一组矩形」，正是单测台最擅长钉的。全自绘界面一旦布局算错，
// 表现是控件叠在一起或者跑到看不见的地方，靠肉眼在截图里找很费劲。
//
// 本文件**不依赖 SDK**（只要 windows.h 的 RECT / MulDiv），所以能进
// tests/harness 那套 shim 单测台（run.ps1 的 prefs 组）。
// ---------------------------------------------------------------------------

namespace lyricus {

// 颜色项的数量。布局和绘制都依赖它，写两处迟早不一致。
constexpr int kPrefsColorCount = 6;

// 控件配色的基色数量（D-093）。
//
// ⚠️ 刻意**只有 4 个**：控制条上一共有 11 类颜色（按钮的悬停/按下、图标的
// 普通/主操作/悬停/按下、滑块的轨道/填充、时间文字、音量图标、浮层底板）。
// 全暴露给用户太多了 —— 挑色本身就是负担，何况还得保证它们互相搭配。
// 每组一个基色、组内其余由程序推导，是这个模式能用的前提。
constexpr int kPrefsCtrlColorCount = 4;

// 页面的**建议尺寸**（逻辑像素）。对话框资源按它设，宿主给多大就画多大 ——
// 布局全部按实际客户区算，不假设固定尺寸。
constexpr int kPrefsWidth96  = 380;
// 330 -> 424（外观预设区）-> 596（控件配色区，D-093）。
constexpr int kPrefsHeight96 = 596;

// 首选项页上所有需要定位的元素。
//
// 全部按**物理像素**返回（已经乘过 dpi/96）。空矩形（right <= left）
// 表示"这块地方不够，这一项没画" —— 和 control_bar_layout 同一个约定。
struct PrefsLayout {
    // ⚠️ 这两个数组**必须**带 `{}` 默认初始化。
    //
    // 写成 `RECT cards[kPrefsColorCount];`（不带花括号）时，`PrefsLayout out;`
    // 走的是**默认初始化** —— 数组元素是未初始化的**垃圾值**，而不是全零。
    // 后果不只是测试失败：ComputePrefsLayout 有好几条"提前返回空布局"的路径
    // （尺寸太小、dpi 非法），那些路径会返回一堆**看起来完全有效**的随机矩形，
    // 绘制侧照着画就会画到莫名其妙的地方去。
    //
    // 这个坑是 test_prefs_layout.cpp 抓出来的 —— 两条"应该返回空"的断言失败。
    RECT cards[kPrefsColorCount]{};
    RECT cardLabels[kPrefsColorCount]{};
    RECT titleColors{};                 // 小标题：浮动面板配色
    RECT titleAlpha{};                  // 小标题：整体不透明度
    RECT alphaValue{};                  // 标题右侧的 "%d / 255"
    RECT slider{};                      // 不透明度滑块的完整区域
    RECT hint{};                        // 底部说明文字
    RECT reset{};                       // "恢复默认"按钮
    RECT fontBtn{};                     // "字体..."按钮（按钮上显示当前字体名）

    // ---- 外观预设（D-088）----
    //
    // 排布：标题一行，下面两行控件 ——
    //     [当前预设名                    v] [保存]
    //     [删除] [导入] [导出]
    // 下拉**不用自绘列表**，点击直接弹系统 TrackPopupMenu：自绘列表要么被
    // 对话框边界裁掉、要么得再开一个弹窗，而菜单是系统给的、位置自己会算。
    RECT titlePreset{};                 // 小标题：外观预设
    RECT presetCombo{};                 // 「当前预设名 v」框（点击弹菜单）
    RECT presetSave{};                  // 保存
    RECT presetDelete{};                // 删除
    RECT presetImport{};                // 导入
    RECT presetExport{};                // 导出

    // ---- 控件配色（D-093）----
    //
    // 排布：标题和模式开关同一行，下面 2x2 的 4 个基色块。
    //     ◆ 控件配色                    [自动 / 自定义]
    //     [按钮] [图标]
    //     [滑块] [文字]
    RECT titleCtrl{};                   // 小标题：控件配色
    RECT ctrlModeBtn{};                 // 两态开关（点一下切换「自动 / 自定义」）
    RECT ctrlCards[kPrefsCtrlColorCount]{};        // 4 个基色块
    RECT ctrlCardLabels[kPrefsCtrlColorCount]{};

    int  dpi = 96;                      // 回传给绘制侧，省得它再查一次
    int  padX = 0;                      // 左右内边距（提示文字要用）
};

// 算出整页的元素位置。width / height 是**客户区**的物理像素尺寸。
//
// 【降级】页面很小的时候（宿主给的区域窄）按顺序丢东西：色块从三列变两列、
// 再变一列；实在放不下才让某些项变空矩形。宁可少画几个也不要叠在一起。
PrefsLayout ComputePrefsLayout(int width, int height, int dpi);

// 由宿主主题推导出这一页要用的一组颜色。
//
// 【为什么要单独一个函数】自绘页面最怕"颜色写死" —— 用户切到暗色模式后
// 一片惨白或一片漆黑。背景与文字直接问 foobar2000 要（见 .cpp 里的说明），
// 其余（卡片底、悬停、边框、轨道）从这两色按亮度推导，于是**任何主题下都自洽**。
struct PrefsTheme {
    COLORREF pageBg   = RGB(255, 255, 255);  // 页面底
    COLORREF text     = RGB(0, 0, 0);        // 主文字
    COLORREF textDim  = RGB(110, 110, 118);  // 次要文字（提示、十六进制之外的说明）
    COLORREF cardBg   = RGB(245, 245, 247);  // 卡片/滑块轨道的底
    COLORREF cardHot  = RGB(232, 232, 236);  // 悬停
    COLORREF border   = RGB(214, 214, 220);  // 分隔线与卡片描边
    COLORREF accent   = RGB(0, 120, 212);    // 强调（滑块已填充部分、按下态）
};

// dark = true 走深色一套。bg / fg 由调用方从 ui_config_manager 取，
// 这样页面底色能和首选项窗口严丝合缝地接上。
PrefsTheme MakePrefsTheme(bool dark, COLORREF bg, COLORREF fg);

} // namespace lyricus
