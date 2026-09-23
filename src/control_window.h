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

    // 触发一次重绘（分层模式下走 RenderLayered，否则走 WM_PAINT）
    void RequestRepaint();

    // ---- 控制条（M2）----
    enum class CtrlId { None, Prev, PlayPause, Next, Progress, VolumeIcon, VolumeBar };

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
           m_rcVolumeIcon{}, m_rcVolumeBar{};
    CtrlId m_hot    = CtrlId::None;   // 鼠标悬停
    CtrlId m_active = CtrlId::None;   // 鼠标按下
    bool   m_draggingProgress = false;
    bool   m_draggingVolume   = false;
    double m_dragRatio        = 0.0;
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

    // 上一次**因为播放位置变化**而重绘的时刻（GetTickCount64）。
    // 用来把「位置在走」那种重绘节流到每秒一次 —— 见 control_window.cpp 里
    // kPositionRepaintMs 的说明。初值 0 让第一次位置变化就能通过。
    ULONGLONG m_lastPositionRepaint = 0;

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
