#include "stdafx.h"
#include "control_window.h"
#include "playback_state.h"
#include "debug_log.h"

#include <algorithm>
#include <dwmapi.h>

// ===========================================================================
// DWM 相关常量
//
// 属性 ID 在较新的 dwmapi.h 里已有定义；这里做兜底，保证在旧 SDK 上也能编译。
// 数值来自官方文档：
//   DWMWA_USE_IMMERSIVE_DARK_MODE  = 20
//   DWMWA_WINDOW_CORNER_PREFERENCE = 33
//   DWMWA_SYSTEMBACKDROP_TYPE      = 38   (Windows 11 Build 22621+)
// ===========================================================================
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace lyricus {
namespace {

constexpr wchar_t kWindowClass[] = L"LyricusControlPanel";
constexpr wchar_t kWindowTitle[] = L"Lyricus";

// DWM_SYSTEMBACKDROP_TYPE 取值。
// 刻意用字面量而不是 SDK 的枚举名 —— 枚举常量不是宏，用 #ifdef 兜底会撞名。
constexpr int kBdNone    = 1; // DWMSBT_NONE
constexpr int kBdMica    = 2; // DWMSBT_MAINWINDOW
constexpr int kBdAcrylic = 3; // DWMSBT_TRANSIENTWINDOW —— Desktop Acrylic
constexpr int kBdMicaAlt = 4; // DWMSBT_TABBEDWINDOW

constexpr int kCornerRound = 2; // DWMWCP_ROUND

// 系统背景材质要求 Windows 11 Build 22621+
constexpr DWORD kMinBuildForBackdrop = 22621;

// 刷新定时器：驱动播放位置查询与「当前歌词行」更新。
// 250ms 对行级歌词足够；将来做逐字歌词需要更高频率或高精度计时器插值。
constexpr UINT_PTR kRefreshTimerId  = 1;
constexpr UINT     kRefreshInterval = 250;

// 默认窗口尺寸，定义在 96 dpi 下，实际创建时按系统 DPI 缩放
constexpr int kDefaultW96 = 460;
constexpr int kDefaultH96 = 150;

bool g_classRegistered = false;

DWORD GetWindowsBuildNumber() {
    // GetVersionEx 会被应用程序清单里的兼容性设置欺骗，RtlGetVersion 才是真实值。
    using fnRtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static fnRtlGetVersion pRtlGetVersion = reinterpret_cast<fnRtlGetVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));

    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (pRtlGetVersion && pRtlGetVersion(&vi) == 0) {
        return vi.dwBuildNumber;
    }
    return 0;
}

bool SupportsSystemBackdrop() {
    static const bool supported = GetWindowsBuildNumber() >= kMinBuildForBackdrop;
    return supported;
}

int BackdropValueFor(BackdropMode mode) {
    switch (mode) {
        case BackdropMode::Mica:    return kBdMica;
        case BackdropMode::Acrylic: return kBdAcrylic;
        case BackdropMode::MicaAlt: return kBdMicaAlt;
        case BackdropMode::Translucent:     // 分层窗口自绘，不用系统材质
        case BackdropMode::None:
        default:                    return kBdNone;
    }
}

// 整体半透明的程度（0-255）。太小会看不清字，太大就看不出透。
constexpr BYTE kTranslucentAlpha = 215;

// 面板底色（BGRA 顺序里用到的三个分量）。分层渲染的 alpha 修正要靠它做比对。
constexpr BYTE kPanelB = 30;
constexpr BYTE kPanelG = 28;
constexpr BYTE kPanelR = 28;

const wchar_t* ModeDisplayName(BackdropMode mode) {
    switch (mode) {
        case BackdropMode::None:        return L"无（不透明）";
        case BackdropMode::Mica:        return L"Mica";
        case BackdropMode::Acrylic:     return L"Acrylic（毛玻璃）";
        case BackdropMode::MicaAlt:     return L"Mica Alt";
        case BackdropMode::Translucent: return L"半透明（自绘）";
    }
    return L"未知";
}

// GetDpiForWindow / GetDpiForSystem 在 winuser.h 里受 WINVER 宏保护，
// 当前编译环境没暴露出来。改成运行时从 user32.dll 取地址 —— 顺便兼容老系统。
UINT GetDpiForWindowSafe(HWND hwnd) {
    using fn_t = UINT(WINAPI*)(HWND);
    static const auto p = reinterpret_cast<fn_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (p != nullptr) return p(hwnd);

    HDC dc = GetDC(hwnd);
    const UINT dpi = (dc != nullptr) ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96u;
    if (dc != nullptr) ReleaseDC(hwnd, dc);
    return dpi;
}

UINT GetDpiForSystemSafe() {
    using fn_t = UINT(WINAPI*)();
    static const auto p = reinterpret_cast<fn_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (p != nullptr) return p();

    HDC dc = GetDC(nullptr);
    const UINT dpi = (dc != nullptr) ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96u;
    if (dc != nullptr) ReleaseDC(nullptr, dc);
    return dpi;
}

// 画一行文字并把「实际占用的高度 + 行距」返回，供调用方推进光标。
// 用 DT_CALCRECT 量出真实高度 —— 而不是写死像素，这是 DPI 无关的关键。
int DrawMeasuredLine(HDC dc, const wchar_t* text, const RECT& area,
                     HFONT font, COLORREF color, int gapAfter) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);

    RECT calc = area;
    DrawTextW(dc, text, -1, &calc,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    const int height = calc.bottom - calc.top;

    RECT draw = area;
    draw.bottom = draw.top + height;
    DrawTextW(dc, text, -1, &draw, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, oldFont);
    return height + gapAfter;
}

// 只量高度，不画。用于先算总高再垂直居中。
int MeasureLine(HDC dc, const wchar_t* text, HFONT font, int maxWidth) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    RECT r{ 0, 0, maxWidth, 0 };
    DrawTextW(dc, text, -1, &r,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, oldFont);
    return r.bottom - r.top;
}

// ---------------------------------------------------------------------------
// 控制条绘制小工具
// ---------------------------------------------------------------------------

void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = CreateSolidBrush(color);
    HPEN   p = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ ob = SelectObject(dc, b);
    const HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// kind: 0=上一首 1=播放 2=暂停 3=下一首
// 用几何图形而不是字体里的符号 —— 各系统上的符号字体不一定齐全。
void DrawTransportGlyph(HDC dc, const RECT& r, int kind, COLORREF color) {
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    const int s  = (r.bottom - r.top) / 3;

    HBRUSH br = CreateSolidBrush(color);
    HPEN   pn = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ ob = SelectObject(dc, br);
    const HGDIOBJ op = SelectObject(dc, pn);

    auto bar = [&](int x) {
        RECT t{ x, cy - s, x + s / 3, cy + s };
        FillRect(dc, &t, br);
    };

    switch (kind) {
    case 0: {   // 上一首：竖条 + 左三角
        bar(cx - s);
        POINT pts[3] = { { cx + s, cy - s }, { cx + s, cy + s }, { cx - s / 3, cy } };
        Polygon(dc, pts, 3);
        break;
    }
    case 1: {   // 播放：右三角
        POINT pts[3] = { { cx - s + s / 3, cy - s }, { cx - s + s / 3, cy + s }, { cx + s, cy } };
        Polygon(dc, pts, 3);
        break;
    }
    case 2:     // 暂停：两条竖条
        bar(cx - s / 2 - s / 6);
        bar(cx + s / 6);
        break;
    case 3: {   // 下一首：右三角 + 竖条
        POINT pts[3] = { { cx - s, cy - s }, { cx - s, cy + s }, { cx + s / 3, cy } };
        Polygon(dc, pts, 3);
        bar(cx + s * 2 / 3);
        break;
    }
    default: break;
    }

    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

void DrawSlider(HDC dc, const RECT& r, double ratio, int thickness,
                COLORREF bg, COLORREF fill) {
    if (r.right <= r.left) return;
    const int cy = (r.top + r.bottom) / 2;

    RECT track{ r.left, cy - thickness / 2, r.right, cy + thickness / 2 };
    FillRoundRect(dc, track, thickness, bg);

    const int fillRight = r.left + static_cast<int>((r.right - r.left) * ratio);
    if (fillRight > r.left + 1) {
        RECT fr{ r.left, track.top, fillRight, track.bottom };
        FillRoundRect(dc, fr, thickness, fill);
    }

    const int knobR = (r.bottom - r.top) / 4;
    HBRUSH kb = CreateSolidBrush(RGB(246, 246, 250));
    HPEN   kp = CreatePen(PS_SOLID, 1, RGB(246, 246, 250));
    const HGDIOBJ ob = SelectObject(dc, kb);
    const HGDIOBJ op = SelectObject(dc, kp);
    Ellipse(dc, fillRight - knobR, cy - knobR, fillRight + knobR, cy + knobR);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(kb);
    DeleteObject(kp);
}

std::wstring FormatTime(double sec) {
    if (!(sec > 0.0)) sec = 0.0;          // 同时挡住 NaN
    const int total = static_cast<int>(sec);
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", total / 60, total % 60);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------

ControlWindow& ControlWindow::Get() {
    static ControlWindow instance;
    return instance;
}

bool ControlWindow::IsVisible() const {
    return m_hwnd != nullptr && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd) != FALSE;
}

void ControlWindow::Toggle() {
    if (IsVisible()) Hide();
    else             Show();
}

RECT ControlWindow::ComputeInitialRect() const {
    const int sx = static_cast<int>(cfg_panel_x.get());
    const int sy = static_cast<int>(cfg_panel_y.get());

    if (sx >= 0 && sy >= 0) {
        const int sw = static_cast<int>(cfg_panel_w.get());
        const int sh = static_cast<int>(cfg_panel_h.get());
        RECT saved{ sx, sy, sx + sw, sy + sh };
        if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL) != nullptr) {
            DebugLog("ComputeInitialRect: 沿用已保存位置 %d,%d %dx%d", sx, sy, sw, sh);
            return saved;
        }
        DebugLog("ComputeInitialRect: 已保存位置 %d,%d %dx%d 不在任何显示器上，回退居中",
                 sx, sy, sw, sh);
    }

    // 默认尺寸定义在 96 dpi，按系统 DPI 缩放后再用
    const UINT dpi = GetDpiForSystemSafe();
    const int w = MulDiv(kDefaultW96, static_cast<int>(dpi), 96);
    const int h = MulDiv(kDefaultH96, static_cast<int>(dpi), 96);

    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    const int cx = wa.left + ((wa.right - wa.left) - w) / 2;
    const int cy = wa.top + ((wa.bottom - wa.top) - h) / 2;

    DebugLog("ComputeInitialRect: 无保存位置，系统DPI=%u，默认 %dx%d，居中于 %d,%d",
             dpi, w, h, cx, cy);
    return RECT{ cx, cy, cx + w, cy + h };
}

bool ControlWindow::EnsureCreated() {
    if (m_hwnd != nullptr && IsWindow(m_hwnd)) return true;

    HINSTANCE instance = core_api::get_my_instance();

    if (!g_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = &ControlWindow::StaticWndProc;
        wc.hInstance     = instance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // 关键：不设背景刷，否则会盖住 DWM 材质
        wc.lpszClassName = kWindowClass;
        if (RegisterClassExW(&wc) == 0) {
            DebugLog("EnsureCreated: RegisterClassExW 失败，GetLastError=%lu", GetLastError());
            return false;
        }
        g_classRegistered = true;
        DebugLog("EnsureCreated: 窗口类注册成功");
    }

    const RECT r = ComputeInitialRect();
    const int reqW = r.right - r.left;
    const int reqH = r.bottom - r.top;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,   // 置顶；不出现在任务栏和 Alt-Tab
        kWindowClass,
        kWindowTitle,
        WS_POPUP,                            // 无边框，外观完全自绘
        r.left, r.top, reqW, reqH,
        nullptr, nullptr, instance, this);

    if (m_hwnd == nullptr) {
        DebugLog("EnsureCreated: CreateWindowExW 失败，GetLastError=%lu", GetLastError());
        return false;
    }

    // 记录「请求尺寸 vs 实际尺寸」—— 排查 DPI 虚拟化时这是关键证据
    RECT actual{};
    GetWindowRect(m_hwnd, &actual);
    const UINT winDpi  = GetDpiForWindowSafe(m_hwnd);
    const UINT sysDpi  = GetDpiForSystemSafe();
    DebugLog("EnsureCreated: 请求 %d,%d %dx%d -> 实际 %d,%d %dx%d  winDpi=%u sysDpi=%u",
             r.left, r.top, reqW, reqH,
             actual.left, actual.top, actual.right - actual.left, actual.bottom - actual.top,
             winDpi, sysDpi);

    ApplyBackdrop();

    // 定时刷新播放位置与当前歌词行
    SetTimer(m_hwnd, kRefreshTimerId, kRefreshInterval, nullptr);

    return true;
}

void ControlWindow::ApplyBackdrop() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;

    const BackdropMode mode = static_cast<BackdropMode>(cfg_backdrop_mode.get());

    BOOL dark = TRUE;
    const HRESULT hrDark = DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                                 &dark, sizeof(dark));

    HRESULT hrCorner = E_NOTIMPL;
    HRESULT hrBackdrop = E_NOTIMPL;
    HRESULT hrFrame = E_NOTIMPL;
    int backdropValue = -1;

    // 分层窗口只服务于「半透明」模式：整体按 alpha 混合，不依赖 DWM 材质。
    bool needLayeredRender = false;
    const LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (mode == BackdropMode::Translucent) {
        if ((exStyle & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        }
        // 这条路径用 UpdateLayeredWindow 提交画面，
        // 因此*不能*再调 SetLayeredWindowAttributes —— 两者互斥。
        needLayeredRender = true;
    } else if ((exStyle & WS_EX_LAYERED) != 0) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        InvalidateRect(m_hwnd, nullptr, TRUE);   // 退出分层后恢复正常重绘
    }

    if (SupportsSystemBackdrop()) {
        const int corner = kCornerRound;
        hrCorner = DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                         &corner, sizeof(corner));

        backdropValue = BackdropValueFor(mode);
        hrBackdrop = DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                           &backdropValue, sizeof(backdropValue));

        // 【重要】绝不要把边框延伸到客户区。
        // 实测 DwmExtendFrameIntoClientArea(-1) 会让客户区里的 GDI 绘制内容
        // 不被 DWM 呈现到屏幕上 —— 症状是「面板空白，一按 PrintScreen 文字就出现，
        // 取消后又消失」，因为截屏动作会强制重新合成。详见 docs/decisions.md D-009。
        // 这里显式清零边距，确保不残留旧设置。
        const MARGINS margins{ 0, 0, 0, 0 };
        hrFrame = DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    }

    m_diagBackdropValue = backdropValue;
    m_diagBackdropHr    = static_cast<unsigned>(hrBackdrop);
    m_diagFrameHr       = static_cast<unsigned>(hrFrame);

    DebugLog("ApplyBackdrop: mode=%d(%s) build=%lu supported=%d | dark=0x%08X corner=0x%08X backdrop=%d hr=0x%08X frame=0x%08X",
             static_cast<int>(mode), BackdropModeName(mode),
             GetWindowsBuildNumber(), SupportsSystemBackdrop() ? 1 : 0,
             static_cast<unsigned>(hrDark), static_cast<unsigned>(hrCorner),
             backdropValue, static_cast<unsigned>(hrBackdrop),
             static_cast<unsigned>(hrFrame));

    RedrawWindow(m_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);

    // 分层模式不走 WM_PAINT，得主动提交一次画面
    if (needLayeredRender) RenderLayered();
}

void ControlWindow::Show() {
    if (!EnsureCreated()) return;

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // 显式要求重绘。窗口刚显示、或刚被重建时，客户区未必会自动收到 WM_PAINT，
    // 那就会出现「只有背景、没有文字」的空面板。
    if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) == BackdropMode::Translucent) {
        RenderLayered();          // 分层模式不经过 WM_PAINT
    } else {
        InvalidateRect(m_hwnd, nullptr, TRUE);
        UpdateWindow(m_hwnd);
    }

    cfg_panel_visible = true;
    DebugLog("Show: 面板已显示");
}

void ControlWindow::Hide() {
    SavePosition();
    if (m_hwnd != nullptr && IsWindow(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    cfg_panel_visible = false;
    DebugLog("Hide: 面板已隐藏");
}

void ControlWindow::SavePosition() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;

    RECT r{};
    if (!GetWindowRect(m_hwnd, &r)) return;
    if (r.left <= -30000 || r.top <= -30000) return;  // 最小化

    cfg_panel_x = r.left;
    cfg_panel_y = r.top;
    cfg_panel_w = r.right - r.left;
    cfg_panel_h = r.bottom - r.top;

    DebugLog("SavePosition: %d,%d %dx%d", r.left, r.top, r.right - r.left, r.bottom - r.top);
}

void ControlWindow::Recreate() {
    const bool wasVisible = IsVisible();

    if (m_hwnd != nullptr && IsWindow(m_hwnd)) {
        m_skipSaveOnDestroy = true;   // 别让 WM_DESTROY 把旧位置写回去
        DestroyWindow(m_hwnd);
        m_skipSaveOnDestroy = false;
    }
    m_hwnd = nullptr;

    DebugLog("Recreate: 销毁完成，wasVisible=%d", wasVisible ? 1 : 0);
    if (wasVisible) Show();
}

void ControlWindow::ResetToDefaults() {
    const UINT dpi = GetDpiForSystemSafe();

    cfg_panel_x = -1;
    cfg_panel_y = -1;
    cfg_panel_w = MulDiv(kDefaultW96, static_cast<int>(dpi), 96);
    cfg_panel_h = MulDiv(kDefaultH96, static_cast<int>(dpi), 96);
    cfg_backdrop_mode = static_cast<int>(BackdropMode::Translucent);

    DebugLog("ResetToDefaults: dpi=%u 尺寸=%lldx%lld 材质=半透明",
             dpi,
             static_cast<long long>(cfg_panel_w.get()),
             static_cast<long long>(cfg_panel_h.get()));

    Recreate();
}

void ControlWindow::Shutdown() {
    SavePosition();
    if (m_hwnd != nullptr && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
    DebugLog("Shutdown: 窗口已销毁");
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK ControlWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ControlWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ControlWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<ControlWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ControlWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        // 不擦背景。让 DWM 材质透出来；自己铺底色会把它盖死。
        return 1;

    case WM_NCHITTEST: {
        // 控制条上的控件要吃掉鼠标事件；其余客户区仍然交给拖动窗口。
        // （M1 时整个客户区都返回 HTCAPTION，加按钮后不改的话点按钮会变成拖窗口。）
        const POINTS sp = MAKEPOINTS(lp);
        POINT c{ sp.x, sp.y };
        ScreenToClient(hwnd, &c);
        EnsureLayout();
        if (HitTestControls(c, nullptr) != CtrlId::None) return HTCLIENT;

        const LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) return HTCAPTION;
        return hit;
    }

    case WM_MOUSEMOVE: {
        const POINT pt{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };

        // 拖拽中：只更新比例，松手才真正 seek / 设音量
        if (m_draggingProgress || m_draggingVolume) {
            const RECT& r = m_draggingProgress ? m_rcProgress : m_rcVolumeBar;
            const int w = r.right - r.left;
            if (w > 0) {
                const double v = static_cast<double>(pt.x - r.left) / static_cast<double>(w);
                m_dragRatio = (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
                RequestRepaint();
            }
            return 0;
        }

        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);

        EnsureLayout();
        const CtrlId id = HitTestControls(pt, nullptr);
        SetCursor(LoadCursorW(nullptr, id == CtrlId::None ? IDC_ARROW : IDC_HAND));
        if (id != m_hot) { m_hot = id; RequestRepaint(); }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (m_hot != CtrlId::None) { m_hot = CtrlId::None; RequestRepaint(); }
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT pt{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
        EnsureLayout();
        double ratio = 0.0;
        const CtrlId id = HitTestControls(pt, &ratio);
        if (id == CtrlId::None) break;   // 空白处：交给默认处理，走拖动窗口

        m_active = id;
        if (id == CtrlId::Progress)  { m_draggingProgress = true; m_dragRatio = ratio; }
        if (id == CtrlId::VolumeBar) { m_draggingVolume   = true; m_dragRatio = ratio; }
        SetCapture(hwnd);
        RequestRepaint();
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_active == CtrlId::None && !m_draggingProgress && !m_draggingVolume) break;

        ReleaseCapture();
        const CtrlId id    = m_active;
        const bool   wasP  = m_draggingProgress;
        const bool   wasV  = m_draggingVolume;
        const double ratio = m_dragRatio;

        m_active           = CtrlId::None;
        m_draggingProgress = false;
        m_draggingVolume   = false;

        if (wasP)                    ActivateControl(CtrlId::Progress,  ratio);
        else if (wasV)               ActivateControl(CtrlId::VolumeBar, ratio);
        else if (id != CtrlId::None) ActivateControl(id, ratio);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        // 滚轮调音量。
        // 【注意】playback_control 的音量单位是 dB、0 为满音量 —— 所以上限是 0，
        // 不是 100。写成 0..100 的话会把音量顶到 +100dB。
        const int delta = static_cast<short>(HIWORD(wp));
        auto pc = playback_control::get();
        if (pc.is_valid()) {
            float db = pc->get_volume() + ((delta > 0) ? 1.0f : -1.0f);
            if (db > 0.0f) db = 0.0f;
            pc->set_volume(db);
            PlaybackState::Get().RefreshPosition();
            RequestRepaint();
        }
        return 0;
    }

    case WM_DPICHANGED: {
        // 窗口被拖到不同缩放的显示器上时，按系统建议的新矩形调整，
        // 否则窗口会保持旧尺寸、内容错位。
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested != nullptr) {
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            DebugLog("WM_DPICHANGED: 新 DPI=%u，调整到 %d,%d %dx%d",
                     HIWORD(wp), suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) == BackdropMode::Translucent) {
            RenderLayered();      // 分层窗口的画面由 UpdateLayeredWindow 提供
        } else if (dc != nullptr) {
            PaintContent(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOVE:
    case WM_SIZE:
        // 分层窗口不会自动跟随位置/尺寸，必须重新提交位图
        if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) == BackdropMode::Translucent) {
            RenderLayered();
        }
        break;

    case WM_TIMER:
        if (wp == kRefreshTimerId) {
            auto& st = PlaybackState::Get();
            const bool lineChanged  = st.RefreshPosition();
            const bool stateChanged = (st.Revision() != m_lastRevision);

            if (lineChanged || stateChanged) {
                m_lastRevision = st.Revision();
                if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) == BackdropMode::Translucent) {
                    RenderLayered();
                } else {
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }
        break;

    case WM_EXITSIZEMOVE:
        SavePosition();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Hide(); return 0; }
        break;

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimerId);
        if (!m_skipSaveOnDestroy) SavePosition();
        m_hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ControlWindow::PaintContent(HDC dc) {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);

    const BackdropMode mode = static_cast<BackdropMode>(cfg_backdrop_mode.get());

    // 只有靠 DWM 材质提供背景的模式才「什么都不画」。
    // None 与 Translucent 都是自绘底色。
    const bool dwmProvidesBackground =
        SupportsSystemBackdrop() &&
        (mode == BackdropMode::Mica ||
         mode == BackdropMode::Acrylic ||
         mode == BackdropMode::MicaAlt);

    if (!dwmProvidesBackground) {
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
    }

    DrawTextContent(dc, rc);
}

void ControlWindow::DrawTextContent(HDC dc, const RECT& rc) {
    SetBkMode(dc, TRANSPARENT);

    const BackdropMode mode = static_cast<BackdropMode>(cfg_backdrop_mode.get());

    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    auto makeFont = [dpi](int pt, bool bold) {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0,
                           bold ? FW_SEMIBOLD : FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY,   // 透明底上用 ANTIALIASED，ClearType 依赖不透明背景
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };

    RECT area = rc;
    area.left  += scale(20);
    area.right -= scale(20);
    area.top   += scale(14);

    // 先算出控制条位置。歌词区限定在「曲名之下、控制条之上」，两者各占各的位置、
    // 互不侵占 —— 之前诊断行插进歌词块，就是没把区域划分干净。
    LayoutControls(rc, dpi);
    const int bottomLimit = m_ctrlBarTop;

    HFONT fHeader  = makeFont(11, false);   // 顶部曲名（刻意比歌词小，别抢戏）
    HFONT fCurrent = makeFont(15, true);    // 当前歌词行
    HFONT fBody    = makeFont(11, false);   // 其它歌词行

    // 用「量出来的高度」推进，而不是写死像素 —— DPI 变化时不会挤在一起
    int y = area.top;
    RECT line = area;

    const auto& st = PlaybackState::Get();

    line.top = y;
    const std::wstring header = st.HasTrack()
                              ? FileStemOf(st.TrackPath())
                              : std::wstring(L"Lyricus（未播放）");
    y += DrawMeasuredLine(dc, header.c_str(), line, fHeader, RGB(235, 235, 240), scale(6));

    std::wstring source;
    if (!st.Lyrics().IsEmpty())     source = L"歌词：" + FileNameOf(st.LyricPath());
    else if (st.HasTrack())         source = L"未找到同名 .lrc";

    // 来源信息不在这儿画 —— 挪到底部那条，把纵向空间让给歌词
    y += scale(6);

    // 歌词正文：以当前行为中心显示若干行，当前行用大号加粗白字突出
    const LyricDocument& doc = st.Lyrics();
    if (doc.IsEmpty()) {
        if (st.HasTrack()) {
            line.top = y;
            y += DrawMeasuredLine(dc, L"（无歌词）菜单 View → Lyricus → 重新加载歌词 / 选择歌词文件",
                                  line, fBody, RGB(205, 165, 165), scale(4));
            if (!source.empty()) {
                line.top = y;
                y += DrawMeasuredLine(dc, source.c_str(), line, fBody, RGB(150, 150, 158), 0);
            }
        }
    } else {
        const size_t cur   = st.CurrentLine();
        const size_t total = doc.Count();
        constexpr size_t kSpan = 2;          // 当前行前后各显示几行
        size_t first = 0;
        if (cur != LyricDocument::npos && cur > kSpan) first = cur - kSpan;

        const int gapCurrent = scale(10);
        const int gapNormal  = scale(6);
        const int bottom     = bottomLimit;

        // 先量出总高、剔除放不下的行，再**垂直居中**。
        // 直接从上往下画会让歌词块贴着顶部排，最后一行被窗口底边切掉。
        std::vector<size_t> show;
        int blockHeight = 0;
        for (size_t i = first; i < total && show.size() < kSpan * 2 + 1; ++i) {
            const bool isCurrent = (i == cur);
            HFONT f = isCurrent ? fCurrent : fBody;
            const int h   = MeasureLine(dc, doc.At(i).text.c_str(), f, area.right - area.left);
            const int gap = isCurrent ? gapCurrent : gapNormal;
            if (y + blockHeight + h > bottom) break;
            blockHeight += h + gap;
            show.push_back(i);
        }

        int drawY = y + ((bottom - y) - blockHeight) / 2;
        if (drawY < y) drawY = y;

        for (size_t idx : show) {
            const bool isCurrent = (idx == cur);
            line.top = drawY;
            drawY += DrawMeasuredLine(dc, doc.At(idx).text.c_str(), line,
                                      isCurrent ? fCurrent : fBody,
                                      isCurrent ? RGB(255, 255, 255) : RGB(172, 172, 180),
                                      isCurrent ? gapCurrent : gapNormal);
        }
    }

    // 控制条（占用了原先底部预留条的位置）
    DrawControls(dc, dpi);

    DeleteObject(fHeader);
    DeleteObject(fCurrent);
    DeleteObject(fBody);
}

void ControlWindow::RenderLayered() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;
    if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) != BackdropMode::Translucent) return;

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr) return;

    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = w;
    bi.bV5Height      = -h;          // 负值 = 自上而下，与 GDI 坐标一致
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, reinterpret_cast<BITMAPINFO*>(&bi),
                                   DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) DeleteObject(dib);
        ReleaseDC(nullptr, screenDC);
        DebugLog("RenderLayered: CreateDIBSection 失败");
        return;
    }

    // 1) 手工铺底。GDI 完全不管理 alpha 通道，底色只能自己按 BGRA 写进去。
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    {
        BYTE* p = static_cast<BYTE*>(bits);
        for (size_t i = 0; i < pixelCount; ++i) {
            p[0] = kPanelB;
            p[1] = kPanelG;
            p[2] = kPanelR;
            p[3] = kTranslucentAlpha;
            p += 4;
        }
    }

    HDC memDC = CreateCompatibleDC(screenDC);
    const HGDIOBJ oldBmp = SelectObject(memDC, dib);

    // 2) 文字照常交给 GDI。GDI 只改 RGB，不会破坏上面写好的 alpha 值。
    DrawTextContent(memDC, rc);

    // 2.5) 修正 alpha 并预乘。
    //   (a) GDI 不写 alpha —— 文字像素的 alpha 仍是底色的 215，表现为「字也是透明的」。
    //       凡是与底色不同的像素就是文字（含抗锯齿边缘），把它提到完全不透明。
    //   (b) UpdateLayeredWindow(ULW_ALPHA) 要求源位图是**预乘 alpha** 的，
    //       即 RGB 必须已经乘过 alpha/255，否则文字会偏暗偏糊。
    {
        BYTE* p = static_cast<BYTE*>(bits);
        for (size_t i = 0; i < pixelCount; ++i, p += 4) {
            if (p[0] != kPanelB || p[1] != kPanelG || p[2] != kPanelR) {
                p[3] = 255;
            }
            p[0] = static_cast<BYTE>(p[0] * p[3] / 255);
            p[1] = static_cast<BYTE>(p[1] * p[3] / 255);
            p[2] = static_cast<BYTE>(p[2] * p[3] / 255);
        }
    }

    // 3) 提交
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    POINT dst{ wr.left, wr.top };
    POINT src{ 0, 0 };
    SIZE size{ w, h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(m_hwnd, screenDC, &dst, &size, memDC, &src, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// ---------------------------------------------------------------------------
// 控制条（M2）
// ---------------------------------------------------------------------------

void ControlWindow::RequestRepaint() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;
    if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) == BackdropMode::Translucent) {
        RenderLayered();
    } else {
        InvalidateRect(m_hwnd, nullptr, TRUE);
        UpdateWindow(m_hwnd);
    }
}

void ControlWindow::EnsureLayout() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    LayoutControls(rc, static_cast<int>(GetDpiForWindowSafe(m_hwnd)));
}

void ControlWindow::LayoutControls(const RECT& rc, int dpi) {
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };

    const int btn = S(26);
    const int gap = S(8);
    const int pad = S(20);
    const int top = rc.bottom - S(16) - btn;

    int x = rc.left + pad;
    m_rcPrev      = RECT{ x, top, x + btn, top + btn };  x += btn + gap;
    m_rcPlayPause = RECT{ x, top, x + btn, top + btn };  x += btn + gap;
    m_rcNext      = RECT{ x, top, x + btn, top + btn };  x += btn + gap;

    const int iconW = S(20);
    const int volW  = S(70);
    m_rcVolumeBar  = RECT{ rc.right - pad - volW, top + S(9), rc.right - pad, top + btn - S(9) };
    m_rcVolumeIcon = RECT{ m_rcVolumeBar.left - gap - iconW, top,
                           m_rcVolumeBar.left - gap, top + btn };

    const int timeW = S(96);
    m_rcTime = RECT{ m_rcVolumeIcon.left - gap * 2 - timeW, top,
                     m_rcVolumeIcon.left - gap * 2, top + btn };

    // 中间剩下的空间给进度条；窄到放不下就不画（避免和两边重叠）
    const int progL = x + gap;
    const int progR = m_rcTime.left - gap * 2;
    m_rcProgress = (progR > progL + S(40))
                 ? RECT{ progL, top + S(9), progR, top + btn - S(9) }
                 : RECT{ 0, 0, 0, 0 };

    m_ctrlBarTop = top - S(6);   // 控制条上沿 = 歌词区的下界
}

ControlWindow::CtrlId ControlWindow::HitTestControls(POINT pt, double* ratioOut) const {
    if (ratioOut != nullptr) *ratioOut = 0.0;

    auto inside = [&pt](const RECT& r) {
        return r.right > r.left && pt.x >= r.left && pt.x < r.right
                                && pt.y >= r.top  && pt.y < r.bottom;
    };
    auto ratioOf = [&pt](const RECT& r) {
        const int w = r.right - r.left;
        if (w <= 0) return 0.0;
        const double v = static_cast<double>(pt.x - r.left) / static_cast<double>(w);
        return (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
    };

    if (inside(m_rcPrev))       return CtrlId::Prev;
    if (inside(m_rcPlayPause))  return CtrlId::PlayPause;
    if (inside(m_rcNext))       return CtrlId::Next;
    if (inside(m_rcVolumeIcon)) return CtrlId::VolumeIcon;
    if (inside(m_rcVolumeBar))  { if (ratioOut) *ratioOut = ratioOf(m_rcVolumeBar); return CtrlId::VolumeBar; }
    if (inside(m_rcProgress))   { if (ratioOut) *ratioOut = ratioOf(m_rcProgress);  return CtrlId::Progress; }
    return CtrlId::None;
}

void ControlWindow::ActivateControl(CtrlId id, double ratio) {
    auto pc = playback_control::get();
    if (!pc.is_valid()) return;

    switch (id) {
    case CtrlId::Prev:       pc->start(playback_control::track_command_prev); break;
    case CtrlId::Next:       pc->start(playback_control::track_command_next); break;
    case CtrlId::PlayPause:  pc->play_or_pause(); break;
    case CtrlId::VolumeIcon: pc->volume_mute_toggle(); break;

    case CtrlId::VolumeBar:
        // 【注意】playback_control 的音量单位是 dB，0 表示满音量（不是 0..100）。
        // 把滑条 0..1 映射到 [-40dB, 0dB]，-40dB 近似当静音用。
        pc->set_volume(static_cast<float>(-40.0 * (1.0 - ratio)));
        break;

    case CtrlId::Progress: {
        const double len = PlaybackState::Get().LengthSec();
        if (len > 0.5 && pc->playback_can_seek()) {
            pc->playback_seek(ratio * len);
        }
        break;
    }

    default: break;
    }

    PlaybackState::Get().RefreshPosition();
    RequestRepaint();
}

void ControlWindow::DrawControls(HDC dc, int dpi) {
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    const auto& st = PlaybackState::Get();

    auto buttonBg = [&](CtrlId id, const RECT& r) {
        if (m_active == id)   FillRoundRect(dc, r, S(8), RGB(72, 82, 96));
        else if (m_hot == id) FillRoundRect(dc, r, S(8), RGB(58, 62, 72));
    };

    buttonBg(CtrlId::Prev, m_rcPrev);
    DrawTransportGlyph(dc, m_rcPrev, 0, RGB(212, 212, 220));

    buttonBg(CtrlId::PlayPause, m_rcPlayPause);
    DrawTransportGlyph(dc, m_rcPlayPause, (st.IsPlaying() && !st.IsPaused()) ? 2 : 1,
                       RGB(255, 255, 255));

    buttonBg(CtrlId::Next, m_rcNext);
    DrawTransportGlyph(dc, m_rcNext, 3, RGB(212, 212, 220));

    // 进度条
    if (m_rcProgress.right > m_rcProgress.left) {
        const double len = st.LengthSec();
        double ratio = (len > 0.5) ? (st.PositionSec() / len) : 0.0;
        if (m_draggingProgress) ratio = m_dragRatio;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        DrawSlider(dc, m_rcProgress, ratio, S(4), RGB(74, 74, 82), RGB(206, 210, 220));
    }

    // 时间
    HFONT fSmall = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    {
        const std::wstring text = FormatTime(st.PositionSec()) + L" / " + FormatTime(st.LengthSec());
        const HGDIOBJ oldFont = SelectObject(dc, fSmall);
        SetTextColor(dc, RGB(190, 190, 198));
        RECT tr = m_rcTime;
        DrawTextW(dc, text.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    // 音量图标（几何图形，不依赖符号字体）
    {
        const RECT& r = m_rcVolumeIcon;
        const int cy = (r.top + r.bottom) / 2;
        RECT body{ r.left + S(3), cy - S(3), r.left + S(3) + S(5), cy + S(3) };
        HBRUSH b = CreateSolidBrush(RGB(200, 200, 208));
        HPEN   p = CreatePen(PS_SOLID, 1, RGB(200, 200, 208));
        const HGDIOBJ ob = SelectObject(dc, b);
        const HGDIOBJ op = SelectObject(dc, p);
        FillRect(dc, &body, b);
        POINT pts[3] = { { body.right, cy - S(6) }, { body.right, cy + S(6) },
                         { body.right + S(6), cy } };
        Polygon(dc, pts, 3);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
    }

    // 音量条
    {
        double v = 1.0 + static_cast<double>(st.VolumeDb()) / 40.0;   // 0dB -> 1.0
        if (m_draggingVolume) v = m_dragRatio;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        DrawSlider(dc, m_rcVolumeBar, v, S(4), RGB(74, 74, 82), RGB(206, 210, 220));
    }

    DeleteObject(fSmall);
}

} // namespace lyricus
