#pragma once

#include "config.h"
#include "lyrics_view.h"

// ---------------------------------------------------------------------------
// 独立操作面板（顶层窗口）
//
// 设计要点：
//  * 无边框 (WS_POPUP) + 置顶 (WS_EX_TOPMOST) + 不进任务栏 (WS_EX_TOOLWINDOW)
//  * 背景由 DWM 系统材质提供（Mica / Acrylic），见 ApplyBackdrop()
//  * 位置与尺寸持久化到 cfg_var，重启后恢复
//  * 整个客户区可拖动（WM_NCHITTEST 返回 HTCAPTION）
//
// 注意：所有方法都必须在**主线程**调用。foobar2000 的 playback_control 之类
// 接口只在主线程有效，UI 也应当如此。
// ---------------------------------------------------------------------------

namespace lyricus {

class ControlWindow {
public:
    static ControlWindow& Get();

    void Toggle();
    void Show();
    void Hide();

    bool IsVisible() const;

    // 重新套用当前配置里的背景材质。切换模式后调用。
    void ApplyBackdrop();

    // 立即把窗口位置写回配置。
    void SavePosition();

    // 销毁并按当前配置重建窗口。改了位置/尺寸/材质后需要它才能生效，
    // 因为窗口创建后这些属性不会自己跟随配置变化。
    void Recreate();

    // 把位置/尺寸/材质恢复成默认值（尺寸按系统 DPI 缩放后写入）。
    void ResetToDefaults();

    // 播放器退出时调用：保存位置并销毁窗口。
    void Shutdown();

private:
    ControlWindow() = default;
    ~ControlWindow() = default;
    ControlWindow(const ControlWindow&) = delete;
    ControlWindow& operator=(const ControlWindow&) = delete;

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool EnsureCreated();
    void PaintContent(HDC dc);
    // 真正落笔的那个（PaintContent 只负责给它套一层内存 DC 做双缓冲）
    void PaintContentRaw(HDC dc, const RECT& rc);

    // 触发一次重绘（分层模式下走 RenderLayered，否则走 WM_PAINT）
    void RequestRepaint();

    // ---- 控制条（M2）----
    //
    // VolumePopup 是「横向音量条被降级掉之后，鼠标悬停在音量图标上」展开的
    // **垂直**滑块。它和 VolumeIcon 是两个不同的命中目标：
    //   * 悬停图标 -> 展开浮层（不改变任何状态）
    //   * 点击图标 -> 静音开关（原有行为，用户 2026-09-26 明确要保留）
    // 悬停与点击用同一个图标但不打架，这是当初选悬停而非点击展开的原因。
    enum class CtrlId { None, Prev, PlayPause, Next, Progress, VolumeIcon, VolumeBar, VolumePopup };

    void   EnsureLayout();                       // 按当前客户区尺寸重算控件矩形
    void   LayoutControls(const RECT& rc, int dpi);
    void   DrawControls(HDC dc, int dpi);
    CtrlId HitTestControls(POINT clientPt, double* ratioOut) const;
    void   ActivateControl(CtrlId id, double ratio);

    // SVG 图标叠层。**必须在分层渲染的 alpha 修正之后调用** —— 那一步会把
    // 非背景像素的 alpha 拉到 255，先混进去的图标边缘会被毁掉。
    void   DrawIconOverlay(unsigned char* dst, int w, int h, int stride, int dpi);
    std::wstring IconPath(const wchar_t* name) const;

    RECT   m_rcPrev{}, m_rcPlayPause{}, m_rcNext{}, m_rcProgress{}, m_rcTime{},
           m_rcVolumeIcon{}, m_rcVolumeBar{}, m_rcVolumePopup{};
    CtrlId m_hot    = CtrlId::None;   // 鼠标悬停
    CtrlId m_active = CtrlId::None;   // 鼠标按下
    bool   m_draggingProgress = false;
    bool   m_draggingVolume   = false;
    // 这一轮音量拖拽是在**浮层**上进行的（垂直方向）还是横向条上（水平方向）。
    // 两者的 ratio 换算方向相反 —— 用矩形反推容易出错，索性显式记一笔。
    bool   m_dragFromPopup    = false;
    // 浮层当前是否展开。悬停展开、移开收起，状态由 WM_MOUSEMOVE 维护。
    bool   m_volumePopupOpen  = false;
    double m_dragRatio        = 0.0;
    // 鼠标**悬停**在滑块上时指向的值。悬停也要显示数值标签
    //（用户 2026-09-26：「鼠标悬停也应该显示，这样方便用户调节」），
    // 而标签要画的是**鼠标指向的那个值**（"点这里会是多少"）——
    // m_hot 只存了控件 id，存不下位置，所以另开一个。
    double m_hotRatio         = 0.0;
    int    m_ctrlBarTop       = 0;    // 控制条上沿 = 歌词区的下界

    // 只负责把文字画到给定 DC 上（不碰背景），供普通绘制和分层渲染共用。
    void DrawTextContent(HDC dc, const RECT& rc);

    // 分层窗口渲染路径：自建 32bpp DIB，手工写 alpha，再 UpdateLayeredWindow 提交。
    // 「半透明」模式走这条路 —— 不依赖 DWM 材质，GDI 文字也不会出现黑底方块。
    void RenderLayered();

    // 计算窗口初始位置：优先用已保存的位置；若那块区域已经不在任何显示器上
    // （换了显示器 / 拔了外接屏），退回到主显示器居中。
    RECT ComputeInitialRect() const;

    // 释放 RenderLayered 的位图缓存（窗口销毁时调用）。
    void ReleaseLayeredCache();

    HWND m_hwnd = nullptr;

    // 重建窗口时置位，避免 WM_DESTROY 里把刚重置的配置又写回旧值
    bool m_skipSaveOnDestroy = false;

    // 上一次重绘时看到的 PlaybackState 代次。
    // 换曲 / 歌词变更后它一定会变，靠它触发重绘 —— 见 playback_state.h 的说明。
    unsigned m_lastRevision = 0;

    // 用户设置（字号 / 行数 / 当前行位置）。
    // 高级首选项的改动**没有任何通知机制**，只能在定时器里轮询比对 ——
    // 三个 int 的比较，代价可以忽略。详见 settings.cpp 的说明。
    LyricDisplayConfig m_displayCfg;   // 上一次看到的原始设置，用来判断"变了没"

    // 上一次应用过的通透度模式。
    //
    // ★ 这个成员是 bug 修的一部分（2026-09-26）：ApplyBackdrop() 从前**只在
    //   EnsureCreated() 里调一次**，于是切预设把 backdrop 从 Translucent 改成
    //   None 之后，窗口还带着 WS_EX_LAYERED 而 WM_PAINT 已经改走 PaintContent ——
    //   画面就此冻住（用户报的「导入高对比时浮动面板卡死」）。
    //   现在定时器里轮询它，变了就重新应用。
    //
    // 初值 -1：和任何合法模式都不同，保证第一拍一定会走一次同步。
    int m_lastBackdropMode = -1;
    LyricsViewLayout   m_layout;       // m_displayCfg 的渲染视图，跟着它一起更新

    // 浮动面板外观（配色 + 不透明度），来自首选项页「显示 → Lyricus」。
    // 和显示设置一样每帧轮询比对 —— 用户在首选项里点完"应用"，
    // 面板这边 250ms 内就会跟上，不需要任何跨模块的回调耦合。
    PanelAppearance    m_appearance;

    // 上一次 ApplyBackdrop 的结果，直接画在面板上 ——
    // 这样即使日志写不出来，一张截图也能告诉我全部状态。
    int      m_diagBackdropValue = -1;
    unsigned m_diagBackdropHr    = 0xFFFFFFFF;
    unsigned m_diagFrameHr       = 0xFFFFFFFF;
    int      m_diagPaintCount    = 0;

    // ---- 「歌词偶尔停止更新」的诊断计数器 ----
    //
    // 只在心跳日志里用，不参与任何逻辑。见 control_window.cpp 的 WM_TIMER。
    // 用户 2026-09-24 报的现象**只有浮动面板会**（内嵌的 DUI/CUI 正常），
    // 所以问题只可能在这条路上：定时器死了，或者状态在变但重绘没生效。
    unsigned m_diagTickCount    = 0;   // 定时器打了多少拍
    int      m_diagRepaintCount = 0;   // 上一段心跳以来重绘了几次
    unsigned m_diagAnimTicks    = 0;   // 上一段心跳以来动画拍打了几次（= 帧率×10）

    // 上一次 UpdateLayeredWindow 成功没有。
    // 初值 true：这样第一次失败会走"好 -> 坏"的翻转，记下那条关键日志。
    bool     m_lastUwlOk        = true;

    // 上一次**因为播放位置变化**而重绘的时刻（GetTickCount64）。
    // 用来把「位置在走」那种重绘节流到每秒一次 —— 见 control_window.cpp 里
    // kPositionRepaintMs 的说明。初值 0 让第一次位置变化就能通过。
    ULONGLONG m_lastPositionRepaint = 0;

    // ---- 动画（长行横滚 / 换行上滑）----
    //
    // 时间线本身在 scroll_anim.cpp 里，是纯逻辑（注入时钟、可离线单测）；
    // 这里只负责"每拍问它要一帧、然后按需重绘"。
    LyricAnimator     m_animator;
    LyricAnimFrame    m_animFrame;    // 正在显示的那一帧
    LyricsViewResult  m_lastResult;   // 上一次绘制量出来的宽出量 / 步距
    bool              m_animTimerOn = false;

    // 推进动画时间线：更新 m_animFrame、按需开关动画定时器。
    // 返回 true = 帧变了（调用方要重绘）。**必须在重绘之前调**。
    bool AdvanceAnimation(ULONGLONG now);

    // 动画拍入口：推进 + 变了就重绘。
    void TickAnimation(ULONGLONG now);

    // ---- 分层渲染的位图缓存 ----
    //
    // 【为什么需要】面板每 250ms 重绘一次（进度条在动），而原来的写法
    // 每帧都 CreateDIBSection 一张 920x300 的位图再用完删掉 —— 约 1.1MB 的
    // 分配/释放，实测「面板定时器一拍」稳定 5~9ms，这是其中一块。
    // 尺寸不变就复用同一张位图、同一个内存 DC。
    //
    // m_layeredBits 直接指向位图数据，铺底和 alpha 修正都靠它，
    // 免掉每帧一次 GetObject / DIBSECTION 查询。
    HDC      m_layeredDC     = nullptr;
    HBITMAP  m_layeredDib    = nullptr;
    HBITMAP  m_layeredOldBmp = nullptr;   // 选进 DC 前的原位图，释放时要还回去
    void*    m_layeredBits   = nullptr;
    int      m_layeredW      = 0;
    int      m_layeredH      = 0;
};

} // namespace lyricus
