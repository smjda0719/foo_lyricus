#pragma once

#include <windows.h>

#include "scroll_anim.h"   // LyricAnimFrame

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

    // 当前行的垂直位置：占**整个面板**高度的百分比，50 = 正中。
    //
    // 【基准是整个面板，不是歌词区】2026-09-25 改的，见 D-043。
    // 原来按"歌词区"算，而歌词区在三种宿主里定义不同（浮动面板要扣掉底部
    // 控制条，DUI/CUI 是整个元素），于是同一个百分比在两边落点不一样 ——
    // 用户报「内嵌的歌词没有居中」。改成整个面板之后：
    //   * 同一个值在三种宿主里含义完全一致；
    //   * 50 永远是面板正中；
    //   * 面板**缩放**时百分比自动跟着变，不用另立规则。
    // 文字仍然只画在 rc 与 clipBottom 之间，不会压到控制条上。
    int currentRatio = 50;

    // 有翻译时哪个当正文：false = 原文（翻译作小字参照行），true = 反过来。
    //
    // 放在最后是为了让既有的 `{fontPct, span, ratio}` 聚合初始化照旧编译 ——
    // 三个宿主都那么写，加在中间会一次性把它们全打断。
    bool tlPrimary   = false;

    // 歌词**画到哪儿为止**（屏幕无关的 y 坐标）。
    //
    // 只有浮动面板用得上：它的 rc 是整个面板（给 currentRatio 当基准），
    // 但底部约 37.5 逻辑像素是控制条，歌词不能压上去。
    // 0（默认）= 不裁剪，画到 rc.bottom 为止 —— DUI/CUI 就是这种情况，
    // 它们整个元素都是歌词区。
    int clipBottom   = 0;
};

// 画「曲名 + 歌词」的返回值。
//
// 【为什么不再是裸 int】宿主需要知道两件只有渲染层才知道的事，
// 才能驱动下一步的动画（见 scroll_anim.h）：
//   * 当前行**宽出去多少** —— 才能判断要不要滚、滚多远
//   * 上滑的**步距** —— 换行时整块歌词该从低多少像素处升上来
// 这两件事都依赖 padX / 字体 / 行距，宿主自己算就会**和渲染层跑偏**。
struct LyricsViewResult {
    int bottom = 0;   // 内容实际占用的下边界（y），从前的返回值

    // 当前行正文比可用宽度宽出多少像素。0 = 放得下，不用滚。
    //
    // ⚠️ 是"宽出多少"，不是"文本有多宽" —— 可用宽度是渲染层内部
    //    （左右各 padX）算出来的，交给宿主去减迟早会不一致。
    int currentOverflow = 0;

    // 换行上滑的起始位移（像素）。
    //
    // 取"上一行作为上下文行时的步距" = 它的行高 + 常规行距。
    // 不是精确的视觉位移量（上一行原本用当前行字体、更大更粗），
    // 200ms 的过渡里看不出来 —— 精确求解要留着上一帧的整个排版，不值得。
    int currentStepH = 0;
};

// 画「曲名 + 歌词」。
//
// anim 是这一帧的动画状态（横滚偏移 / 上滑位移）。默认值 = 都不动，
// 所以宿主不接动画时行为和不传一样。
LyricsViewResult DrawLyricsView(HDC dc, const RECT& rc, const LyricsViewTheme& theme,
                                const LyricsViewLayout& layout = LyricsViewLayout{},
                                const LyricAnimFrame& anim = LyricAnimFrame{});

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
