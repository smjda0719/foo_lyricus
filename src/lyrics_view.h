#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// 歌词面板的公共渲染层。
//
// 抽出来的目的：同一套绘制要服务三种宿主 ——
//   1. 独立顶层窗口（分层窗口半透明渲染，见 control_window.cpp）
//   2. DUI 元素（嵌入 foobar2000 默认界面）
//   3. CUI 面板（嵌入 Columns UI）
//
// 这里只负责「拿 DC 和矩形，把曲名与歌词画上去」，
// **不碰窗口、不碰背景填充** —— 底色由调用方决定（半透明宿主需要环境 alpha，
// 不透明宿主则自己铺底）。
// ---------------------------------------------------------------------------

namespace lyricus {

// 配色与字体度量。由各宿主各自填充，渲染代码不关心来源：
//   * 独立面板用固定深色配色
//   * DUI/CUI 从宿主主题取（query_color / cui::colours）
struct LyricsViewTheme {
    COLORREF headerText  = RGB(235, 235, 240);
    COLORREF currentText = RGB(255, 255, 255);
    COLORREF normalText  = RGB(172, 172, 180);
    COLORREF dimText     = RGB(150, 150, 158);
    COLORREF warnText    = RGB(205, 165, 165);
    int      dpi         = 96;
};

// 排版参数。
//
// 和主题分开是因为**来源不同**：主题来自宿主（独立面板写死深色 / DUI 主题 /
// CUI 配色），而排版来自用户设置 —— 三种宿主共用同一份 GetLyricDisplayConfig()。
// 混在一个结构里会让人误以为配色也能从设置里调。
struct LyricsViewLayout {
    int fontPct      = 100;   // 字号百分比，100 = 默认
    int span         = 0;     // 当前行上下各显示几行；0 = 按可用高度自适应
    int currentRatio = 50;    // 当前行在歌词区里的垂直位置（%），50 = 正中
};

// 画「曲名 + 歌词」。
// 返回内容实际占用的下边界（y），方便调用方接着画控制条之类。
int DrawLyricsView(HDC dc, const RECT& rc, const LyricsViewTheme& theme,
                   const LyricsViewLayout& layout = LyricsViewLayout{});

// 取一个**进程内缓存的** UI 字体。
//
// 给渲染层以外的绘制代码（控制条的时间文本、图标叠层的标签等）共用，
// 免得它们各自 CreateFontW 一个用完就删 —— 面板每 250ms 重绘一次，
// 「每帧现造字体」是实打实的持续开销。
//
// 【调用方注意】返回的句柄归缓存所有，**不要 DeleteObject**，
// 也不要 SelectObject 进去之后不还回来。
HFONT GetCachedUiFont(int dpi, int pt, bool bold);

} // namespace lyricus
