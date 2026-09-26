#pragma once

// ---------------------------------------------------------------------------
// 首选项页的资源 ID。
//
// 本工程原本没有 .rc，这是第一份 —— 因为 preferences_page_impl<TDialog>
// 要求 TDialog 能 Create(parent)，也就是**必须基于对话框资源**
//（见 helpers/atl-misc.h:271-280）。自己用 CreateWindowEx 拼控件也能绕开，
// 但那样布局、Tab 顺序、DPI 缩放都得手写，反而更容易出错。
//
// 编号从 1000 起：对话框自身用 101，控件用 10xx 分段，
// 和图标的 1xx 段分开，避免以后加图标时撞号。
// ---------------------------------------------------------------------------

#define IDD_LYRICUS_PREFS     101

// 颜色按钮（BS_OWNERDRAW，由 WM_DRAWITEM 画成色块）
#define IDC_BTN_HEADER       1001
#define IDC_BTN_CURRENT      1002
#define IDC_BTN_NORMAL       1003
#define IDC_BTN_DIM          1004
#define IDC_BTN_WARN         1005
#define IDC_BTN_BG           1006

// 不透明度
#define IDC_SLIDER_ALPHA     1010
#define IDC_LBL_ALPHA        1011

// 动作
#define IDC_BTN_RESET        1020
#define IDC_LBL_HINT         1021

// ---------------------------------------------------------------------------
// 「为这个文件夹指定歌词线索」对话框
//
// 和首选项页那个的关键区别：这是**独立弹窗**，所以样式里**不能**有 WS_CHILD
//（首选项页那边必须有，因为它是被嵌进首选项窗口的 —— 见 atl-misc.h:277 的断言）。
// ---------------------------------------------------------------------------

#define IDD_LYRICUS_HINT     102

#define IDC_EDIT_ARTIST      1101
#define IDC_EDIT_ALBUM       1102
#define IDC_LBL_FOLDER       1103
#define IDC_LBL_TITLE_HINT   1104

// ---------------------------------------------------------------------------
// 「调节面板」对话框
//
// 同样是**独立弹窗**（样式里没有 WS_CHILD）。
//
// 【为什么要它】第一版把这几个量做成了菜单里的循环档（字号 100/125/150…、
// 行位置 30/40/50/60/70…），歌词偏移更是「点一次提前 0.5 秒」。
// 用户 2026-09-25 连着否掉两次：
//   「改成打开一个面板，通过滑动条调整。这么点太麻烦了。」
//   「也把字号，行数，行位置，透明度也这么改。透明度我记得有另外一个面板，
//     但是太深了不好找。」
// 这几个量的共同点是**要一边看面板一边调**，而"打开首选项 -> 找到项 -> 改数字
// -> 关掉 -> 看效果 -> 再打开"这个循环根本没法用。
//
// 所以合成一个面板、五条滑动条、**拖动时实时生效**。
// ---------------------------------------------------------------------------

#define IDD_LYRICUS_ADJUST   103

#define IDC_SLIDER_OFFSET    1201
#define IDC_LBL_V_OFFSET     1202
#define IDC_SLIDER_FONT      1203
#define IDC_LBL_V_FONT       1204
#define IDC_SLIDER_SPAN      1205
#define IDC_LBL_V_SPAN       1206
#define IDC_SLIDER_RATIO     1207
#define IDC_LBL_V_RATIO      1208
#define IDC_SLIDER_ADJ_ALPHA 1209

// ---------------------------------------------------------------------------
// 「歌词源顺序」对话框
//
// 起因：网易云排第一位、命中就收工，于是备用源（酷狗 / LRCLIB）在正常使用中
// **几乎永远跑不到** —— 实测用户连放十几首，酷狗一次都没轮到。用户
// 2026-09-25 说「暂时把网易云源短接掉」，随后自己提了更好的办法：
// 「可以给用户自定义查找歌词顺序的面板」。
// 与其在代码里临时短接（迟早忘了恢复），不如把这个选择交给用户。
//
// 纯逻辑在 source_order.cpp（可离线单测），这里只负责界面。
// ---------------------------------------------------------------------------

#define IDD_LYRICUS_SOURCES  104

// 「色环取色器」—— 独立弹窗。
//
// 用户 2026-09-26 看过系统取色对话框之后说「虽然稍微好点，但我还是想要类似色环的」。
// 和首选项页一样是全自绘的，所以里面**没有任何控件 ID**：
// 色环、SV 方块、预览、两个按钮全是画出来的，命中靠 color_wheel.cpp 那套纯函数。
#define IDD_LYRICUS_COLORWHEEL 105

#define IDC_LIST_SOURCES     1301
#define IDC_BTN_SRC_UP       1302
#define IDC_BTN_SRC_DOWN     1303
#define IDC_BTN_SRC_TOGGLE   1304
#define IDC_BTN_SRC_RESET    1305
#define IDC_LBL_SRC_HINT     1306
#define IDC_LBL_V_ALPHA      1210
#define IDC_LBL_TRACK        1211
#define IDC_BTN_OFFSET_ZERO  1212
#define IDC_BTN_DISPLAY_DEF  1213

// 字号的**微调**按钮（−1 / +1）。
//
// 用户 2026-09-26：「可以给用户一个微调旋钮，调节字号大小，放到调节面板里」。
// 滑块能覆盖 50~300 的大范围，但想在 118 和 119 之间挑一个，
// 鼠标拖动基本靠运气 —— 而字号恰恰是最常微调的那一项。
#define IDC_BTN_FONT_DEC     1214
#define IDC_BTN_FONT_INC     1215
