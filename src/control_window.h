#pragma once

#include "config.h"

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

    HWND m_hwnd = nullptr;

    // 重建窗口时置位，避免 WM_DESTROY 里把刚重置的配置又写回旧值
    bool m_skipSaveOnDestroy = false;

    // 上一次重绘时看到的 PlaybackState 代次。
    // 换曲 / 歌词变更后它一定会变，靠它触发重绘 —— 见 playback_state.h 的说明。
    unsigned m_lastRevision = 0;

    // 上一次 ApplyBackdrop 的结果，直接画在面板上 ——
    // 这样即使日志写不出来，一张截图也能告诉我全部状态。
    int      m_diagBackdropValue = -1;
    unsigned m_diagBackdropHr    = 0xFFFFFFFF;
    unsigned m_diagFrameHr       = 0xFFFFFFFF;
    int      m_diagPaintCount    = 0;
};

} // namespace lyricus
