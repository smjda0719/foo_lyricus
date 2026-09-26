#include "stdafx.h"
#include "control_window.h"
#include "playback_state.h"
#include "lyrics_view.h"
#include "svg_icon.h"
#include "debug_log.h"
#include "control_bar_layout.h"   // 控制条的布局数学（纯函数，可离线单测）

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

// 动画拍的定时器。**和逻辑拍分开**是有意的：
//   * 逻辑拍要轮询配置（每拍 3 次 configStore 查表），不能提到 25fps
//   * 动画拍除了"把下一帧画出来"什么都不做，而且只在真的在动时才存在
// id 取 3：1 是逻辑拍，2 留给 DUI/CUI（见 dui_element.cpp 的说明）。
constexpr UINT_PTR kAnimTimerId  = 3;
constexpr UINT     kAnimInterval = 40;   // 25fps
// 位置变化的节流间隔 kPositionRepaintMs 定义在 playback_state.h ——
// 三个宿主共用同一个值。

// 默认窗口尺寸，定义在 96 dpi 下，实际创建时按系统 DPI 缩放
constexpr int kDefaultW96 = 460;
constexpr int kDefaultH96 = 150;

// 缩放手柄的命中宽度（96 dpi 下的逻辑像素）。
//
// 角落给得比边宽：用户的原话是「拖拽窗口四角缩放」—— 角落是主入口，
// 大一点好抓；四条边窄一些，免得把控制条两端的按钮吃掉。
//
// ⚠️ 这两个值和 WM_NCHITTEST 里的判定**顺序**是一套的：边界判定必须排在
//    控件之前。反过来的话，贴着底边的控制条会把底下那几像素永远挡住。
constexpr int kResizeCorner96 = 12;
constexpr int kResizeEdge96   = 6;

// 面板能被拖到多小（96 dpi 下的逻辑像素）。
//
// 【为什么要设下限】缩到极小之后曲名、歌词、控制条会互相压在一起，
// 而那是**布局数学兜不住**的局面 —— LayoutControls 是按"宽度够放下一整条
// 控制条"写的。320x120 大致是"还看得清一行歌词 + 控制条完整"的下限。
constexpr int kMinPanelW96 = 320;
constexpr int kMinPanelH96 = 120;

// 无边框窗口的「抓边框」判定：这个客户区坐标是不是落在缩放边上？
// 是就返回对应的 HTxxx，不是返回 HTNOWHERE。
//
// 【为什么需要它】面板是 WS_POPUP，没有系统边框可抓 —— 于是整个客户区
// 只能拖动、不能缩放。把边缘那几像素认出来并返回 HTxxx 之后，系统会进入
// 它**自己的**缩放循环：拖动、贴边吸附、跨 DPI 显示器时的度量换算全都免费，
// 我们一行 SetWindowPos 都不用写。
//
// 角落要在四边**之前**判：角落区域同时满足两条边，先判边就永远轮不到角落。
LRESULT HitTestResizeBorder(const POINT& c, const RECT& rc, int corner, int edge) {
    const bool atL = (c.x < edge);
    const bool atR = (c.x >= rc.right - edge);
    const bool atT = (c.y < edge);
    const bool atB = (c.y >= rc.bottom - edge);

    const bool inL = (c.x < corner);
    const bool inR = (c.x >= rc.right - corner);
    const bool inT = (c.y < corner);
    const bool inB = (c.y >= rc.bottom - corner);

    if (inT && inL) return HTTOPLEFT;
    if (inT && inR) return HTTOPRIGHT;
    if (inB && inL) return HTBOTTOMLEFT;
    if (inB && inR) return HTBOTTOMRIGHT;

    if (atL) return HTLEFT;
    if (atR) return HTRIGHT;
    if (atT) return HTTOP;
    if (atB) return HTBOTTOM;
    return HTNOWHERE;
}

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

// 底色与整体不透明度**曾经**是这里的常量（kPanelB/G/R + kTranslucentAlpha）。
// 现在它们搬到了 config.h 的 PanelAppearance —— 因为浮动面板的外观做成了
// 可配置的（首选项 → 显示 → Lyricus，带取色器），默认值就写在那个结构体上。
// 这里不再留常量：留着会变成"改了不生效"的第二份真相。

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

// 垂直滑块：**底部 = 0，顶部 = 1**。
//
// 和上面的 DrawSlider 是镜像关系（那个按宽度算、左边 = 0）。刻意没合并成一个
// 带方向参数的函数 —— 两个方向的比例换算、轨道构造、填充起点全都不一样，
// 硬合并只会让两边都难读，而它们各自只有十几行。
void DrawVerticalSlider(HDC dc, const RECT& r, double ratio, int thickness,
                        COLORREF bg, COLORREF fill) {
    if (r.bottom <= r.top) return;
    const int cx = (r.left + r.right) / 2;

    RECT track{ cx - thickness / 2, r.top, cx + thickness / 2, r.bottom };
    FillRoundRect(dc, track, thickness, bg);

    // 从底部往上填
    const int fillTop = r.bottom - static_cast<int>((r.bottom - r.top) * ratio);
    if (fillTop < r.bottom - 1) {
        RECT fr{ track.left, fillTop, track.right, r.bottom };
        FillRoundRect(dc, fr, thickness, fill);
    }

    const int knobR = (r.right - r.left) / 4;
    HBRUSH kb = CreateSolidBrush(RGB(246, 246, 250));
    HPEN   kp = CreatePen(PS_SOLID, 1, RGB(246, 246, 250));
    const HGDIOBJ ob = SelectObject(dc, kb);
    const HGDIOBJ op = SelectObject(dc, kp);
    Ellipse(dc, cx - knobR, fillTop - knobR, cx + knobR, fillTop + knobR);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(kb);
    DeleteObject(kp);
}

// 标签挂在锚点的哪一侧。
enum class LabelSide {
    Above,    // 水平居中于锚点、底边贴着锚点往上长 —— 给横向滑块用
    LeftOf,   // 右边缘贴着锚点、垂直居中 —— 给贴着面板右沿的垂直浮层用
};

// 拖拽时浮在滑块旁边的**数值标签**。
//
// 【为什么必须有】用户 2026-09-26：「拖拽的时候出现一个数值标签，这样方便调节，
// 更何况 foobar2000 用的是 dB，这样更不容易调节」。
// 音量在 playback_control 里的单位是 dB（-40..0），那是对数刻度 ——
// 光看滑块停在哪，根本分不出当前是 -3 还是 -12；进度条同理，
// 拖到哪儿了不给秒数就只能凭感觉。
//
// 它是**拖拽期间才出现**的临时提示，松手即消失（调用方按 m_draggingXxx 判断）。
void DrawValueLabel(HDC dc, POINT anchor, LabelSide side, const wchar_t* text,
                    int dpi, const RECT& bounds) {
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };

    const HFONT font = GetCachedUiFont(dpi, 9, false);

    // 先量文字，标签尺寸由它决定
    SIZE sz{};
    {
        const HGDIOBJ of = SelectObject(dc, font);
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcsnlen(text, 63)), &sz);
        SelectObject(dc, of);
    }

    const int padX = S(8);
    const int padY = S(4);
    const int w    = sz.cx + padX * 2;
    const int h    = sz.cy + padY * 2;

    int left, top;
    if (side == LabelSide::Above) {
        left = anchor.x - w / 2;
        top  = anchor.y - h;
    } else {
        left = anchor.x - w;
        top  = anchor.y - h / 2;
    }

    // 贴住面板边界 —— 拖到两端时标签最容易探出去
    if (left < bounds.left)             left = bounds.left;
    if (left + w > bounds.right)        left = bounds.right - w;
    if (top  < bounds.top)              top  = bounds.top;
    if (top + h > bounds.bottom)        top  = bounds.bottom - h;

    const RECT box{ left, top, left + w, top + h };
    FillRoundRect(dc, box, S(6), RGB(26, 28, 34));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 246));
    const HGDIOBJ of = SelectObject(dc, font);
    RECT tr = box;
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
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

    // -1 是「从未保存过」的哨兵值（见 config.h 里 cfg_panel_x/y 的说明）。
    //
    // ⚠️ 这里**不能**写成 `sx >= 0 && sy >= 0`。
    //    主显示器**左边或上边**的显示器坐标是负的 —— 那样写会把
    //    「面板放在左侧副屏」这种完全合法的位置当成「没保存过」，
    //    于是每次启动都跳回主屏居中。
    //
    //    实测踩到：副屏在主屏左侧（X 从 -1463 起），面板拖到 x = -929，
    //    重启后日志里出现"无保存位置"，面板跑到主屏中央。
    //    这个 bug 一直存在，只是之前面板恰好在正坐标（1901,67）才没暴露。
    if (sx != -1 && sy != -1) {
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

    // 先把外观读进来。不能指望定时器 —— 首帧绘制发生在第一次定时器之前，
    // 那之前用默认值画出来会闪一下「出厂配色」。
    m_appearance = GetPanelAppearance();

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
        // WS_POPUP：无边框，外观完全自绘。
        // WS_THICKFRAME：**只为了拿到系统的缩放行为**。没有它的话，
        // WM_NCHITTEST 里返回 HTBOTTOMRIGHT 系统也不会真的去 resize。
        // 它带来的那一圈非客户区边框由下面的 WM_NCCALCSIZE 抹掉，
        // 所以视觉上仍然是一块无边框面板。
        WS_POPUP | WS_THICKFRAME,
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

    // ⚠️ 补一次**显式定位**。
    //
    // 【为什么必须补】窗口带了 WS_THICKFRAME 才拿得到系统的缩放行为，而它的
    // 边框会被算进 CreateWindowExW 的尺寸里。实测：请求 920x300、创建出来也
    // 确实是 920x300（EnsureCreated 那条日志），但**第一次重绘之后**窗口变成了
    // 872x252 —— 正好少掉两圈 24 物理像素的边框。原因是那一次 GetClientRect
    // 拿到的还是"含边框的客户区"，而 RenderLayered 会拿它当 psize 去调
    // UpdateLayeredWindow，等于**用客户区尺寸反过来改了窗口尺寸**。
    //
    // 后果是每次启动面板都比配置里的尺寸小一圈 —— 用户拖好的尺寸存了也白存。
    //
    // 这一句把窗口钉回请求尺寸，不去赌 WM_NCCALCSIZE 的到达时机。
    SetWindowPos(m_hwnd, nullptr, r.left, r.top, reqW, reqH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // 定时刷新播放位置与当前歌词行
    SetTimer(m_hwnd, kRefreshTimerId, kRefreshInterval, nullptr);

    return true;
}

void ControlWindow::ApplyBackdrop() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;

    // ---- 兜底：DWM 材质一律降级成半透明自绘 ----------------------------------
    //
    // 理由见 D-009：DWMWA_SYSTEMBACKDROP_TYPE 那几个材质与普通 GDI 绘制**不兼容** ——
    // 相关 API 全部返回 S_OK，但客户区里的绘制内容不会被呈现到屏幕上
    // （表现是面板一片空白，只有截图时能看见）。
    //
    // 万一配置里存着 Mica / Acrylic / Mica Alt（早期版本留下的，或有人手工改过），
    // 面板**每次启动都会空白**，而用户完全看不出原因 —— 菜单里还显示"设置成功"。
    // 与其留着这种状态，不如在这里直接改掉并记一笔。
    {
        const BackdropMode raw = static_cast<BackdropMode>(cfg_backdrop_mode.get());
        if (raw == BackdropMode::Mica || raw == BackdropMode::Acrylic ||
            raw == BackdropMode::MicaAlt) {
            DebugLog("ApplyBackdrop: 配置里是「%s」—— 该材质与 GDI 绘制不兼容（D-009），"
                     "已自动降级为「半透明（自绘）」",
                     BackdropModeName(raw));
            cfg_backdrop_mode = static_cast<int>(BackdropMode::Translucent);
        }
    }

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

    case WM_NCCALCSIZE:
        // 【为什么必须有这一条】窗口加 WS_THICKFRAME 才拿得到系统的缩放行为，
        // 但它同时会留出一圈非客户区边框。返回 0 表示「客户区 = 整个窗口」——
        // 边框就此消失，缩放能力保留，外观上仍然是一块完整的自绘面板。
        //
        // ⚠️ wParam == TRUE 才是"重新计算客户区"的那一次调用，此时 lp 指向
        //    NCCALCSIZE_PARAMS；FALSE 时不该动 —— 那条路上根本没有参数可改。
        if (wp) return 0;
        break;

    case WM_GETMINMAXINFO: {
        // 最小尺寸。不设的话能拖成一条缝，那时曲名、歌词、控制条会叠在一起，
        // 看起来像面板坏了（而且 WM_EXITSIZEMOVE 会把这个尺寸存进配置）。
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const int dpi = static_cast<int>(GetDpiForWindowSafe(hwnd));
        mmi->ptMinTrackSize.x = MulDiv(kMinPanelW96, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(kMinPanelH96, dpi, 96);
        return 0;
    }

    case WM_NCHITTEST: {
        const POINTS sp = MAKEPOINTS(lp);
        POINT c{ sp.x, sp.y };
        ScreenToClient(hwnd, &c);

        // ---- 先判「抓边框」 ----
        //
        // ⚠️ 必须排在控件判定**前面**。控制条贴着底边，控件要是先判，
        //    底下那几像素就永远轮不到缩放。
        //    代价是控制条两端和底部各让出去几像素 —— 那几像素本来也不是按钮。
        //
        // 判出来的 HTxxx 交给系统，由它跑自己的缩放循环（拖动 / 吸附 / DPI 换算）。
        RECT client{};
        if (GetClientRect(hwnd, &client)) {
            const int dpi = static_cast<int>(GetDpiForWindowSafe(hwnd));
            const LRESULT edge = HitTestResizeBorder(
                c, client,
                MulDiv(kResizeCorner96, dpi, 96),
                MulDiv(kResizeEdge96,  dpi, 96));
            if (edge != HTNOWHERE) return edge;
        }

        // 控制条上的控件要吃掉鼠标事件；其余客户区仍然交给拖动窗口。
        // （M1 时整个客户区都返回 HTCAPTION，加按钮后不改的话点按钮会变成拖窗口。）
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
            // ⚠️ 浮层是**垂直**的（底部 = 0、顶部 = 1），横向那两样是左 0 右 1 ——
            //    方向正好相反，所以按来源分开算，不从矩形形状去反推。
            const RECT& r = m_draggingProgress ? m_rcProgress
                          : (m_dragFromPopup ? m_rcVolumePopup : m_rcVolumeBar);
            double v = 0.0;
            if (m_draggingVolume && m_dragFromPopup) {
                const int h = r.bottom - r.top;
                if (h > 0) v = static_cast<double>(r.bottom - pt.y) / static_cast<double>(h);
            } else {
                const int w = r.right - r.left;
                if (w > 0) v = static_cast<double>(pt.x - r.left) / static_cast<double>(w);
            }
            m_dragRatio = (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
            RequestRepaint();
            return 0;
        }

        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);

        EnsureLayout();

        // 顺手记下"鼠标指向的值" —— 悬停也要显示数值标签，而标签画的是
        // **鼠标指向的那个值**（"点这里会是多少"）。HitTestControls 只在
        // 进度条 / 音量条 / 音量浮层上会填 ratioOut，其它位置给 0。
        double hoverRatio = 0.0;
        const CtrlId id = HitTestControls(pt, &hoverRatio);
        m_hotRatio = hoverRatio;

        // ---- 音量浮层的展开 / 收起 ----
        //
        // 【为什么是悬停而不是点击】点击音量图标已经是静音开关了
        //（用户 2026-09-26 明确要保留那个入口），悬停展开两者就不打架。
        //
        // 【收起条件】鼠标既不压在图标上、也不在浮层里。拖动中永远不收。
        // ⚠️ 浮层与图标是**零间隙**的（见 control_bar_layout.cpp）——
        //    正因为那条缝不存在，从图标往上滑才是连续的，
        //    不会中途收掉再展开（那样会抖得没法用）。
        const bool overIcon  = (id == CtrlId::VolumeIcon);
        const bool overPopup = (id == CtrlId::VolumePopup);
        const bool wantOpen  = overIcon || overPopup || m_draggingVolume;
        if (wantOpen != m_volumePopupOpen) {
            m_volumePopupOpen = wantOpen;
            RequestRepaint();
        }

        // 鼠标进了浮层之后让图标保持"热" —— 否则图标一熄，看着就像这浮层
        // 跟它没关系。m_hot 只参与绘制，改它没有副作用。
        const CtrlId hotId = overPopup ? CtrlId::VolumeIcon : id;
        SetCursor(LoadCursorW(nullptr, (id == CtrlId::None) ? IDC_ARROW : IDC_HAND));
        if (hotId != m_hot) { m_hot = hotId; RequestRepaint(); }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (m_hot != CtrlId::None) { m_hot = CtrlId::None; RequestRepaint(); }
        // 鼠标离开整个窗口了，浮层没有理由继续开着
        if (m_volumePopupOpen) { m_volumePopupOpen = false; RequestRepaint(); }
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT pt{ static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
        EnsureLayout();
        double ratio = 0.0;
        const CtrlId id = HitTestControls(pt, &ratio);
        if (id == CtrlId::None) break;   // 空白处：交给默认处理，走拖动窗口

        m_active = id;
        if (id == CtrlId::Progress)    { m_draggingProgress = true; m_dragFromPopup = false; m_dragRatio = ratio; }
        if (id == CtrlId::VolumeBar)   { m_draggingVolume   = true; m_dragFromPopup = false; m_dragRatio = ratio; }
        if (id == CtrlId::VolumePopup) { m_draggingVolume   = true; m_dragFromPopup = true;  m_dragRatio = ratio; }
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
        const bool   fromPop = m_dragFromPopup;
        const double ratio = m_dragRatio;

        m_active           = CtrlId::None;
        m_draggingProgress = false;
        m_draggingVolume   = false;
        m_dragFromPopup    = false;

        // 松手才真正落音量 —— 拖动期间只改 m_dragRatio（和进度条同一个套路）
        if (wasP)                    ActivateControl(CtrlId::Progress,  ratio);
        else if (wasV && fromPop)    ActivateControl(CtrlId::VolumePopup, ratio);
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
            // 每 250ms 一次的热路径。阈值取 5ms —— 超过就说明这一拍太重了，
            // 连续几拍偏重就是肉眼可见的卡顿。
            ScopedTimer tick("面板定时器一拍", 5.0);

            auto& st = PlaybackState::Get();
            const TickChange change = st.RefreshPosition();

            // ⚠️ 这里**只能**判 Line，不能写 `change != TickChange::None`。
            //
            // 写宽了的话 `lineChanged` 对"位置在走"那一拍也成立，
            // 而播放时位置**每拍都在变** —— 于是下面那个 positionDue 节流
            // 永远轮不到它生效，每 250ms 都全量重绘一次，
            // D-031 那一整套节流等于白写。
            //
            // 这不是推理出来的：加心跳之后日志直接写着「本段重绘=40」（40 拍里
            // 重绘 40 次），实测确认。修好之后应该降到 ~10（每秒一次）。
            const bool lineChanged  = (change == TickChange::Line);
            const bool stateChanged = (st.Revision() != m_lastRevision);

            // 用户改了「高级首选项」里的字号 / 行数 / 当前位置 —— 那套配置
            // **没有变更通知**，只能每帧轮询比对（三个 int，代价可忽略）。
            const LyricDisplayConfig cfg = GetLyricDisplayConfig();
            const bool cfgChanged = (cfg != m_displayCfg);
            if (cfgChanged) {
                m_displayCfg = cfg;
                m_layout = { cfg.fontPct, cfg.span, cfg.currentRatio, cfg.tlPrimary };
                DebugLog("显示设置变更 -> %s", DescribeDisplayConfig(cfg).c_str());
            }

            // 首选项页改的配色 / 不透明度，同样靠轮询（7 个值，代价可忽略）。
            // 这样首选项页和面板之间没有任何回调耦合 ——
            // 页面关掉、销毁都不会影响面板，反过来也一样。
            const PanelAppearance ap = GetPanelAppearance();
            const bool apChanged = (ap != m_appearance);
            if (apChanged) {
                m_appearance = ap;
                DebugLog("外观变更 -> 曲名=#%02X%02X%02X 当前行=#%02X%02X%02X 底色=#%02X%02X%02X alpha=%d",
                         GetRValue(ap.header), GetGValue(ap.header), GetBValue(ap.header),
                         GetRValue(ap.current), GetGValue(ap.current), GetBValue(ap.current),
                         GetRValue(ap.bg), GetGValue(ap.bg), GetBValue(ap.bg),
                         ap.alpha);
            }

            // 换曲 / 改显示设置 -> 动画状态清零。
            //
            // 不清的话，上一首滚到一半的横向偏移会**带到新歌上**
            // （新歌第一行一出来就少了一截），上滑也会凭空滑一次。
            if (stateChanged || cfgChanged) {
                m_animator.Reset();
                m_animFrame = LyricAnimFrame{};
            }

            // 只有「位置在走、歌词行没变」时才受节流限制，
            // 而且这一拍还会被下面更"有意思"的变化再次覆盖 —— 见 kPositionRepaintMs。
            const ULONGLONG now = GetTickCount64();
            const bool positionDue =
                (change == TickChange::Position) &&
                (now - m_lastPositionRepaint >= kPositionRepaintMs);

            // 动画状态**先推进**，再决定要不要重绘 —— 顺序不能反。
            //
            // 反过来的话，换行那一拍会先用**上一行的滚动偏移**把新行画一遍，
            // 下一句才纠正过来：白多一次整帧渲染，而且那一帧是错的（看着闪一下）。
            const bool animChanged = AdvanceAnimation(now);

            if (lineChanged || stateChanged || cfgChanged || apChanged ||
                positionDue || animChanged) {
                m_lastPositionRepaint = now;
                m_lastRevision = st.Revision();
                RequestRepaint();
            }

            // 心跳：每 40 拍（10 秒）记一行。
            //
            // 【为什么要它】用户 2026-09-24 报「偶尔歌词会停止更新，点击暂停重新播放
            // 或者调节音量才会继续更新」，**而且只有浮动面板会**（内嵌的 DUI/CUI 正常）。
            //
            // "内嵌的正常"这一条直接排掉了两种可能：位置在走、当前行在换 ——
            // 那些是三个宿主共用的。所以只剩两种，而它们光看现象分不出来：
            //   1. 定时器死了        -> 「拍」不再增长
            //   2. 状态在变但重绘没生效 -> 「拍」在涨、「本段重绘」也在涨，画面却不动
            //     （对应 RenderLayered 里 UpdateLayeredWindow 失败 —— 那个另有一条日志）
            ++m_diagTickCount;
            if (m_diagTickCount % 40 == 0) {
                // 动画拍单列一栏：**它就是实际帧率**。
                //
                // 【为什么要单列】用户 2026-09-25 报「帧数确实不高」。
                // 想回答"到底几帧"只有一个可靠办法：数动画定时器真打了几拍。
                // 「本段重绘」做不到这件事 —— 它统计的是绘制次数，
                // 而一次动画拍不一定重绘（帧没变就不重绘），
                // 一次逻辑拍也可能重绘好几次。两个数混在一起谁也说明不了。
                DebugLog("面板心跳: 拍=%u  位置=%.1fs  行=%zu/%zu  rev=%u  "
                         "本段重绘=%d  动画拍=%u（=%.1f fps）  本拍=%s",
                         m_diagTickCount, st.PositionSec(),
                         st.CurrentLine(), st.Lyrics().Count(), st.Revision(),
                         m_diagRepaintCount,
                         m_diagAnimTicks, m_diagAnimTicks / 10.0,
                         change == TickChange::None ? "无变化" :
                         (change == TickChange::Line ? "换行" : "仅位置"));
                m_diagRepaintCount = 0;
                m_diagAnimTicks    = 0;
            }

            return 0;
        }

        // 动画拍：推进时间线，变了就重绘。
        //
        // ⚠️ 绝不能把逻辑拍那套（RefreshPosition / 轮询配置 / 心跳）搬过来。
        //    逻辑拍每拍要查 3 次 fb2k::configStore，25fps 就是每秒 75 次查表 ——
        //    纯浪费。这里唯一的职责就是"把动画的下一帧画出来"。
        if (wp == kAnimTimerId) {
            ++m_diagAnimTicks;
            TickAnimation(GetTickCount64());
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
        if (m_animTimerOn) { KillTimer(hwnd, kAnimTimerId); m_animTimerOn = false; }
        ReleaseLayeredCache();
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
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);

    // 先算出控制条位置。歌词区限定在「顶边到控制条上沿」，两者各占各的位置、
    // 互不侵占 —— 之前诊断行插进歌词块，就是没把区域划分干净。
    LayoutControls(rc, dpi);

    // 曲名 + 歌词交给公共渲染层。
    // 抽出去的理由：DUI 元素和 CUI 面板要共用同一份绘制，否则三套实现会慢慢跑偏。
    //
    // 配色是**浮动面板专有**的：它没有宿主可跟随，所以读自己的配置
    // （首选项 → 显示 → Lyricus）。DUI / CUI 面板走各自的宿主主题，不读这里。
    LyricsViewTheme theme;
    theme.dpi = dpi;
    theme.headerText  = m_appearance.header;
    theme.currentText = m_appearance.current;
    theme.normalText  = m_appearance.normal;
    theme.dimText     = m_appearance.dim;
    theme.warnText    = m_appearance.warn;

    // ⚠️ 这里传的是**整个面板** rc，不是"歌词区"。
    //
    // currentRatio（当前行的垂直位置）的基准是整个面板 —— 三种宿主统一，
    // 见 lyrics_view.h 与 D-043。从前这里把 rc.bottom 砍到控制条上沿再传进去，
    // 结果同一个百分比在浮动面板和 DUI 里落点不同，用户报「内嵌的歌词没有居中」。
    //
    // 底部的控制条改用 clipBottom 排除：它只决定"画到哪儿为止"，
    // 不影响居中基准。这样面板以后支持缩放时，百分比也是跟着面板走的。
    m_layout.clipBottom = m_ctrlBarTop;

    // ---- 面板宽度 -> 字号缩放（#11 / D-066）----
    //
    // 【为什么算在这里】它依赖**窗口尺寸**，而尺寸随时会变（用户拖四角）。
    // 放在"配置变更"那处不行 —— 那只在用户改设置时触发，拖窗口不经过它。
    // 和上面 clipBottom 一样：绘制前按当前状态刷一下，代价是两次 MulDiv。
    //
    // 用**宽度**而不是高度：面板高只决定能放几行（span=0 已经自适应了），
    // 而"字显得小"这件事来自宽度 —— 拉宽之后一行能塞更多字，字却没变大。
    {
        const int dpiSafe  = (dpi > 0) ? dpi : 96;
        const int logicalW = MulDiv(rc.right - rc.left, 96, dpiSafe);
        m_layout.panelScalePct = PanelFontScalePct(logicalW, kDefaultW96);
    }

    // 返回值存下来：宽出量和上滑步距要靠它，下一拍喂给动画时间线
    m_lastResult = DrawLyricsView(dc, rc, theme, m_layout, m_animFrame);
    // 控制条
    DrawControls(dc, dpi);
}

void ControlWindow::ReleaseLayeredCache() {
    if (m_layeredDC != nullptr) {
        if (m_layeredOldBmp != nullptr) SelectObject(m_layeredDC, m_layeredOldBmp);
        DeleteDC(m_layeredDC);
        m_layeredDC = nullptr;
    }
    if (m_layeredDib != nullptr) {
        DeleteObject(m_layeredDib);
        m_layeredDib = nullptr;
    }
    m_layeredOldBmp = nullptr;
    m_layeredBits   = nullptr;
    m_layeredW      = 0;
    m_layeredH      = 0;
}

void ControlWindow::RenderLayered() {
    ScopedTimer timer("RenderLayered（重绘 + 提交）", 5.0);

    // 分段计时。**只在整帧偏慢时才写一行**（借用 ScopedTimer 的 5ms 阈值），
    // 免得平时把日志刷爆。
    //
    // 【为什么要它】用户 2026-09-25 报「现在能动了，不过帧数确实不高」。
    // 日志里能看出动画期间每帧 8.4~13.9ms、静态 5.2~6.0ms，
    // 但**差在哪一段**看不出来 —— 而对策完全取决于这个：
    //   * 铺底/预乘占大头 -> 上"带状重绘"（只重算变化的那一条）
    //   * 画文字占大头    -> 缓存整行的栅格（一行只栅格化一次）
    //   * 提交占大头      -> 省 CPU 没用，只能降帧率或改脏区提交
    LARGE_INTEGER qpf{}, qpc0{}, qpc1{}, qpc2{}, qpc3{}, qpc4{}, qpc5{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc0);
    auto msBetween = [&qpf](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
        return (b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(qpf.QuadPart);
    };

    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;
    if (static_cast<BackdropMode>(cfg_backdrop_mode.get()) != BackdropMode::Translucent) return;

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC screenDC = GetDC(nullptr);
    if (screenDC == nullptr) return;

    // 尺寸没变就复用上一帧的 DIB 和内存 DC —— 见 control_window.h 里缓存成员的说明。
    // 尺寸变了（改字号、拖窗口）才重建，重建前先把旧的拆干净。
    if (m_layeredDC == nullptr || m_layeredDib == nullptr ||
        m_layeredW != w || m_layeredH != h) {
        ReleaseLayeredCache();

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

        HDC dc = CreateCompatibleDC(screenDC);
        if (dc == nullptr) {
            DeleteObject(dib);
            ReleaseDC(nullptr, screenDC);
            DebugLog("RenderLayered: CreateCompatibleDC 失败");
            return;
        }

        m_layeredDC     = dc;
        m_layeredDib    = dib;
        m_layeredOldBmp = static_cast<HBITMAP>(SelectObject(dc, dib));
        m_layeredBits   = bits;
        m_layeredW      = w;
        m_layeredH      = h;
    }

    void* const bits = m_layeredBits;

    // 底色与不透明度来自配置（首选项 → 显示 → Lyricus）。
    //
    // 先取一份**局部快照**，整个渲染过程都用同一组值 ——
    // 否则万一配置在渲染途中被改（比如用户正在拖不透明度滑块），
    // 铺底和下面的 alpha 修正会用到不同的值，画面会花掉。
    const PanelAppearance ap = m_appearance;
    const BYTE bgB   = GetBValue(ap.bg);
    const BYTE bgG   = GetGValue(ap.bg);
    const BYTE bgR   = GetRValue(ap.bg);
    const BYTE alpha = static_cast<BYTE>(ClampAlpha(ap.alpha));

    // 1) 手工铺底。GDI 完全不管理 alpha 通道，底色只能自己按 BGRA 写进去。
    //
    // 复用位图意味着底色可能和上一帧不同（用户调了透明度），
    // 所以这一遍**每次都要写**，不能省 —— 省掉就会留下上一帧的 alpha。
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    {
        BYTE* p = static_cast<BYTE*>(bits);
        for (size_t i = 0; i < pixelCount; ++i) {
            p[0] = bgB;
            p[1] = bgG;
            p[2] = bgR;
            p[3] = alpha;
            p += 4;
        }
    }

    QueryPerformanceCounter(&qpc1);

    HDC memDC = m_layeredDC;

    // 2) 文字照常交给 GDI。GDI 只改 RGB，不会破坏上面写好的 alpha 值。
    DrawTextContent(memDC, rc);
    QueryPerformanceCounter(&qpc2);

    // 2.5) 修正 alpha 并预乘。
    //   (a) GDI 不写 alpha —— 文字像素的 alpha 仍是底色的那个值，表现为「字也是透明的」。
    //       凡是与底色不同的像素就是文字（含抗锯齿边缘），把它提到完全不透明。
    //   (b) UpdateLayeredWindow(ULW_ALPHA) 要求源位图是**预乘 alpha** 的，
    //       即 RGB 必须已经乘过 alpha/255，否则文字会偏暗偏糊。
    {
        BYTE* p = static_cast<BYTE*>(bits);
        for (size_t i = 0; i < pixelCount; ++i, p += 4) {
            if (p[0] != bgB || p[1] != bgG || p[2] != bgR) {
                p[3] = 255;
            }
            p[0] = static_cast<BYTE>(p[0] * p[3] / 255);
            p[1] = static_cast<BYTE>(p[1] * p[3] / 255);
            p[2] = static_cast<BYTE>(p[2] * p[3] / 255);
        }
    }
    QueryPerformanceCounter(&qpc3);

    // 2.6) SVG 图标。**必须在这之后混** —— 上面的 alpha 修正会把所有非背景
    //      像素的 alpha 拉到 255，先混进来的图标抗锯齿边缘会被毁成硬边。
    DrawIconOverlay(static_cast<unsigned char*>(bits), w, h, w * 4,
                    static_cast<int>(GetDpiForWindowSafe(m_hwnd)));
    QueryPerformanceCounter(&qpc4);

    // 3) 提交
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    POINT dst{ wr.left, wr.top };
    POINT src{ 0, 0 };
    SIZE size{ w, h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const BOOL uwlOk = UpdateLayeredWindow(m_hwnd, screenDC, &dst, &size, memDC, &src, 0, &blend, ULW_ALPHA);
    QueryPerformanceCounter(&qpc5);

    // 整帧偏慢就写一行分解。阈值同 ScopedTimer（5ms）——
    // 平时不写，动画期间才看得到，正好是我们要诊断的场景。
    {
        const double total = msBetween(qpc0, qpc5);
        if (total >= 5.0) {
            DebugLog("RenderLayered 分解: 铺底=%.1f 画=%.1f 预乘=%.1f 图标=%.1f 提交=%.1f 共=%.1f ms",
                     msBetween(qpc0, qpc1), msBetween(qpc1, qpc2),
                     msBetween(qpc2, qpc3), msBetween(qpc3, qpc4),
                     msBetween(qpc4, qpc5), total);
        }
    }

    // 提交失败**必须留痕**，而且只在"好->坏"翻转时记一条。
    //
    // 【为什么】这是"状态在变、重绘也发了，但画面就是不动"的唯一解释 ——
    // 用户 2026-09-24 报的「偶尔歌词停止更新、只有浮动面板会」正好是这个形状：
    // 内嵌面板走 WM_PAINT，不受这条路径影响，所以它们一直正常。
    // 不判返回值的话，失败是**完全静默**的：循环照跑、日志照写、画面冻住。
    if (!uwlOk != !m_lastUwlOk) {
        m_lastUwlOk = (uwlOk != FALSE);
        if (uwlOk) {
            DebugLog("RenderLayered: UpdateLayeredWindow 恢复正常");
        } else {
            DebugLog("RenderLayered: UpdateLayeredWindow 失败（GetLastError=%lu）"
                     "—— 画面从此刻起不会更新", GetLastError());
        }
    }

    ReleaseDC(nullptr, screenDC);
}

// ---------------------------------------------------------------------------
// 控制条（M2）
// ---------------------------------------------------------------------------

void ControlWindow::TickAnimation(ULONGLONG now) {
    if (AdvanceAnimation(now)) RequestRepaint();
}

// 推进动画时间线。返回 true = 这一帧和上一帧不一样，调用方要重绘。
//
// 【为什么宽出量/步距要晚一拍】它们只有渲染层知道（要量文本宽度，
// 还要减去渲染层内部算的左右内边距），而渲染发生在重绘里。
// 差一拍无所谓：animator 的重置条件是**行号变化**，而换行那一拍
// 它正处在起步前的静止期（900ms），足够下一帧把新值量出来。
bool ControlWindow::AdvanceAnimation(ULONGLONG now) {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return false;

    const auto& st = PlaybackState::Get();
    const LyricAnimFrame f = m_animator.Update(
        now, st.DisplayLine(), m_lastResult.currentOverflow, m_lastResult.currentStepH);

    const bool frameChanged = (f.scrollX != m_animFrame.scrollX) ||
                              (f.slideY  != m_animFrame.slideY);
    m_animFrame = f;

    // 定时器跟着 animating 走：起步前的静止期和滚完之后都不开，
    // 于是长歌词只在那几秒里烧 25fps，其余时间这个定时器根本不存在。
    if (f.animating && !m_animTimerOn) {
        SetTimer(m_hwnd, kAnimTimerId, kAnimInterval, nullptr);
        m_animTimerOn = true;
    } else if (!f.animating && m_animTimerOn) {
        KillTimer(m_hwnd, kAnimTimerId);
        m_animTimerOn = false;
    }

    // ⚠️ 返回的是**帧变了没有**，不是 animating。
    //    上滑结束那一帧 slideY 从 >0 变成 0：内容变了（必须重绘），
    //    但它同时把 animating 置回了 false。只看 animating 的话
    //    最后那一帧会被吞掉，画面停在偏移位置上（见 scroll_anim.h）。
    return frameChanged;
}

void ControlWindow::RequestRepaint() {
    if (m_hwnd == nullptr || !IsWindow(m_hwnd)) return;

    // 计数放在这里 —— **唯一的绘制出口**。
    //
    // 【踩过的坑】原先是在逻辑拍里 `++m_diagRepaintCount`，而动画拍走的是
    // TickAnimation -> RequestRepaint()，**完全绕过**了那个计数器。
    // 于是心跳里那个"本段重绘"只统计逻辑拍的重绘，动画帧数一个都没算进去 ——
    // 我拿它去判断"动画定时器没跑"，判断反了（见 D-047）。
    // 计数必须在出口，不在入口。
    ++m_diagRepaintCount;

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
    // 布局数学全部搬进了 control_bar_layout.cpp —— 那边是**纯函数**，
    // 能进离线单测台遍历各种宽度（见 tests/harness/run.ps1 的 cbar 组）。
    //
    // 【为什么值得抽】这段逻辑 2026-09-26 一天之内改了两轮：先是横向拖窄时
    // 进度条被挤没（判据卡在 `340 > 340` 为假），接着改成按优先级降级、
    // 又要保证空矩形不被画成鬼影。两轮都只能靠"把数算一遍 + 截图看"验证。
    // 抽出来之后，"进度条在任何宽度下都不该消失"这类断言才钉得住。
    //
    // 本函数从此只做一件事：把结果存进成员，供绘制与命中测试取用。
    const ControlBarRects r = ComputeControlBarLayout(rc, dpi);

    m_rcPrev       = r.prev;
    m_rcPlayPause  = r.playPause;
    m_rcNext       = r.next;
    m_rcProgress   = r.progress;
    m_rcTime       = r.time;
    m_rcVolumeIcon = r.volumeIcon;
    m_rcVolumeBar  = r.volumeBar;
    m_ctrlBarTop   = r.barTop;

    // 音量浮层（悬停下拉的垂直滑块）。同时维护展开状态：
    // 浮层没了（面板被拉宽、或变得太矮）就顺手收起，否则会留下一个
    // "状态是开着、却画不出东西"的悬空值。
    m_rcVolumePopup = r.volumePopup;
    if (m_rcVolumePopup.right <= m_rcVolumePopup.left) m_volumePopupOpen = false;
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

    // ---- 音量浮层最先判 ----
    //
    // 它**盖在**歌词和其它控件上面，所以必须排在所有命中之前；
    // 而且它的 ratio 是**垂直**的（底部 = 0，顶部 = 1），
    // 和横向那套「左边 = 0」相反 —— 这里单独算，不复用 ratioOf。
    if (m_volumePopupOpen && inside(m_rcVolumePopup)) {
        if (ratioOut != nullptr) {
            const int h = m_rcVolumePopup.bottom - m_rcVolumePopup.top;
            double v = (h > 0)
                     ? static_cast<double>(m_rcVolumePopup.bottom - pt.y) / static_cast<double>(h)
                     : 0.0;
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            *ratioOut = v;
        }
        return CtrlId::VolumePopup;
    }

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
    case CtrlId::VolumePopup:
        // 【注意】playback_control 的音量单位是 dB，0 表示满音量（不是 0..100）。
        // 把滑条 0..1 映射到 [-40dB, 0dB]，-40dB 近似当静音用。
        //
        // 横向条和悬停浮层共用这一条映射 —— 两者给的都是 0..1 的**音量比例**，
        // 区别只在拖动方向与画法，换算到 dB 这一步没有任何不同。
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
    buttonBg(CtrlId::PlayPause, m_rcPlayPause);
    buttonBg(CtrlId::Next, m_rcNext);
    buttonBg(CtrlId::VolumeIcon, m_rcVolumeIcon);   // 之前漏了，音量按钮一直没有悬停底板

    // 分层模式下图标走 DrawIconOverlay（在 alpha 修正之后混合），
    // 这里只在非分层路径上画几何图形兜底。
    const bool layered =
        (static_cast<BackdropMode>(cfg_backdrop_mode.get()) == BackdropMode::Translucent);

    if (!layered) {
        DrawTransportGlyph(dc, m_rcPrev, 0, RGB(212, 212, 220));
        DrawTransportGlyph(dc, m_rcPlayPause, (st.IsPlaying() && !st.IsPaused()) ? 2 : 1,
                           RGB(255, 255, 255));
        DrawTransportGlyph(dc, m_rcNext, 3, RGB(212, 212, 220));
    }

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
    // 时间。字体走缓存 —— 这行每帧都要画（时间每秒都在变），
    // 原来每帧 CreateFontW 一个再删掉，是白扔的开销。
    //
    // ⚠️ 面板窄时 LayoutControls 会把它降级掉（m_rcTime 是空矩形），这里必须跳过。
    HFONT fSmall = GetCachedUiFont(dpi, 10, false);
    if (m_rcTime.right > m_rcTime.left) {
        const std::wstring text = FormatTime(st.PositionSec()) + L" / " + FormatTime(st.LengthSec());
        const HGDIOBJ oldFont = SelectObject(dc, fSmall);
        SetTextColor(dc, RGB(190, 190, 198));
        RECT tr = m_rcTime;
        DrawTextW(dc, text.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    // 音量图标（几何图形，不依赖符号字体）；分层模式下同样由图标叠层负责
    //
    // ⚠️ 也要判空：它是从右往左摆的，面板极窄时它会越到 progress 左边去。
    if (!layered && m_rcVolumeIcon.right > m_rcVolumeIcon.left) {
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
    //
    // ⚠️ 它是控制条上优先级最低的一个 —— 面板窄到一定程度 LayoutControls 会
    //    把它整个牺牲掉（滚轮仍能调音量），那时这里是空矩形，跳过。
    if (m_rcVolumeBar.right > m_rcVolumeBar.left) {
        double v = 1.0 + static_cast<double>(st.VolumeDb()) / 40.0;   // 0dB -> 1.0
        if (m_draggingVolume) v = m_dragRatio;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        DrawSlider(dc, m_rcVolumeBar, v, S(4), RGB(74, 74, 82), RGB(206, 210, 220));
    }

    // ---- 音量浮层（悬停展开的垂直滑块）----
    //
    // 画在**最后**：它盖在歌词之上（面板里除了控制条，就属它最靠前）。
    //
    // 它的存在条件是"横向条被降级掉了" —— 两者不会同时出现，
    // 所以用户在任何一个宽度下都恰好有一个音量控件可用。
    if (m_volumePopupOpen && m_rcVolumePopup.right > m_rcVolumePopup.left) {
        // 底板：比面板深一档的圆角块，让它从歌词背景里浮出来
        FillRoundRect(dc, m_rcVolumePopup, S(10), RGB(44, 46, 54));

        // 轨道两侧留内边距，别贴着底板边缘
        RECT track = m_rcVolumePopup;
        InflateRect(&track, -S(9), -S(10));

        double v = 1.0 + static_cast<double>(st.VolumeDb()) / 40.0;
        if (m_draggingVolume && m_dragFromPopup) v = m_dragRatio;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        DrawVerticalSlider(dc, track, v, S(4), RGB(74, 74, 82), RGB(206, 210, 220));
    }

    // ---- 数值标签（悬停**或**拖拽时显示）----
    //
    // 【为什么要它】用户 2026-09-26：「拖拽的时候出现一个数值标签，这样方便调节，
    // 更何况 foobar2000 用的是 dB，这样更不容易调节」。音量在 playback_control
    // 里是 dB（-40..0）的**对数**刻度 —— 光看滑块停在哪，分不出是 -3 还是 -12。
    //
    // 【悬停也显示】用户随后补了一句：「鼠标悬停也应该显示，这样方便用户调节」。
    // 于是它给出的其实是**预览**：「点这里会是多少」。常驻则不做 ——
    // 那会和进度条右侧那个 `当前 / 总长` 重复。
    //
    // 画在**最后**：它要浮在所有东西之上（含音量浮层）。
    const bool hoverProgress = (m_hot == CtrlId::Progress);
    const bool hoverVolume   = (m_hot == CtrlId::VolumeBar ||
                                m_hot == CtrlId::VolumePopup);

    // 拖拽优先于悬停：拖着的时候鼠标可能已经滑出滑块了，那时位置仍以拖拽为准
    const bool showProgress = m_draggingProgress || (!m_draggingVolume && hoverProgress);
    const bool showVolume   = m_draggingVolume   || (!m_draggingProgress && hoverVolume);

    if (showProgress || showVolume) {
        RECT client{};
        if (GetClientRect(m_hwnd, &client)) {
            if (showProgress) {
                const double r = m_draggingProgress ? m_dragRatio : m_hotRatio;
                const RECT& bar = m_rcProgress;
                const std::wstring label = FormatTime(st.LengthSec() * r);
                const int cx = bar.left + static_cast<int>((bar.right - bar.left) * r);
                DrawValueLabel(dc, POINT{ cx, bar.top - S(6) }, LabelSide::Above,
                               label.c_str(), dpi, client);
            } else {
                const double r = m_draggingVolume ? m_dragRatio : m_hotRatio;

                // 显示的是**将要设置成**的那个 dB 值，不是播放器当前值 ——
                // 拖动期间并没有真去改播放器（松手才落，见 WM_LBUTTONUP），
                // 显示当前值会和滑块位置对不上。
                wchar_t buf[32];
                swprintf_s(buf, L"%.1f dB", -40.0 * (1.0 - r));

                // 浮层贴着面板右沿，标签只能往**左**让；横向条则挂在正上方
                const bool fromPopup = m_draggingVolume ? m_dragFromPopup
                                                        : (m_hot == CtrlId::VolumePopup);
                if (fromPopup) {
                    const RECT& bar = m_rcVolumePopup;
                    const int cy = bar.bottom -
                        static_cast<int>((bar.bottom - bar.top) * r);
                    DrawValueLabel(dc, POINT{ bar.left - S(8), cy }, LabelSide::LeftOf,
                                   buf, dpi, client);
                } else {
                    const RECT& bar = m_rcVolumeBar;
                    const int cx = bar.left + static_cast<int>((bar.right - bar.left) * r);
                    DrawValueLabel(dc, POINT{ cx, bar.top - S(6) }, LabelSide::Above,
                                   buf, dpi, client);
                }
            }
        }
    }

    // fSmall 归字体缓存所有，这里不删 —— 见 GetCachedUiFont 的说明。
}

std::wstring ControlWindow::IconPath(const wchar_t* name) const {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(core_api::get_my_instance(), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();

    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    p += L"resources\\";
    p += name;
    p += L".svg";
    return p;
}

void ControlWindow::DrawIconOverlay(unsigned char* dst, int w, int h, int stride, int dpi) {
    if (dst == nullptr || w <= 0 || h <= 0) return;

    const int iconH = MulDiv(14, dpi, 96);   // 图标高度，按 DPI 缩放
    const auto& st = PlaybackState::Get();

    // 悬停/按下的反馈 = 底板（DrawControls 里画）+ 图标变亮，两者一起给。
    // active 优先于 hot —— 按住时鼠标必然还在上面，不能只显示悬停态。
    auto tintFor = [this](CtrlId id, unsigned normal) -> unsigned {
        if (m_active == id) return 0x9CCBFFu;   // 按下：淡蓝
        if (m_hot == id)    return 0xFFFFFFu;   // 悬停：纯白
        return normal;
    };

    auto drawIn = [&](const wchar_t* name, const RECT& r, unsigned rgb) {
        // 空矩形 = LayoutControls 把这个元素降级掉了。**必须判**：
        // 不判的话 x/y 会算成负数，在客户区左上角画出一个只露一半的图标。
        if (r.right <= r.left || r.bottom <= r.top) return;
        const RasterIcon* icon = GetSvgIcon(IconPath(name), iconH);
        if (icon == nullptr) return;   // 文件缺失就静默跳过，不阻塞其它绘制
        const int x = r.left + ((r.right - r.left) - icon->width) / 2;
        const int y = r.top + ((r.bottom - r.top) - icon->height) / 2;
        BlendIcon(dst, w, h, stride, x, y, *icon, rgb);
    };

    drawIn(L"prev",   m_rcPrev,       tintFor(CtrlId::Prev, 0xC8C8D2u));
    // 播放/暂停是主操作，常态就给亮色；悬停再提到纯白
    drawIn((st.IsPlaying() && !st.IsPaused()) ? L"pause" : L"play",
           m_rcPlayPause, tintFor(CtrlId::PlayPause, 0xF2F2F8u));
    drawIn(L"next",   m_rcNext,       tintFor(CtrlId::Next, 0xC8C8D2u));
    drawIn(L"volume", m_rcVolumeIcon, tintFor(CtrlId::VolumeIcon, 0xC8C8D2u));
}

} // namespace lyricus
