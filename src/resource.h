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
