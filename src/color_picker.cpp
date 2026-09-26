#include "stdafx.h"

#include "resource.h"
#include "color_wheel.h"
#include "color_picker.h"
#include "dpi_util.h"
#include "debug_log.h"
#include "ui_draw.h"

#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// 色环取色器 —— 窗口与绘制。
//
// 用户 2026-09-26 看过系统取色对话框之后说「虽然稍微好点，但我还是想要类似色环的」。
//
// 【分工】坐标 <-> 颜色的换算、色环几何、命中测试全在 color_wheel.cpp 里，
// 那是纯函数、能离线单测（wheel 组 50 项）。这个文件只做三件事：
// 生成位图、把位图贴到窗口上、处理鼠标。**换算一个都不在这里写** ——
// 这类"看起来只是差了半格"的偏差，靠肉眼在窗口里找基本找不出来。
//
// 【位图缓存】色相环对所有颜色都一样，生成一次就一直用；
// 饱和度/明度方块依赖当前色相，色相变了才重画（拖色环时每帧都在变，
// 但 120x120 也就一万多个像素，够快）。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// 位图生成
// ---------------------------------------------------------------------------

// 造一张 32bpp 自上而下的 DIB section，返回位图指针并把像素首地址写进 *bits。
HBITMAP CreateDib(int w, int h, void** bits) {
    if (w <= 0 || h <= 0) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;    // 负数 = 自上而下，省得每行算 y 翻转
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* mem = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &mem, nullptr, 0);
    if (bmp == nullptr || mem == nullptr) {
        if (bmp != nullptr) DeleteObject(bmp);
        return nullptr;
    }
    *bits = mem;
    return bmp;
}

// 色相环。内外缘各做一条软边，否则边缘是一圈锯齿。
//
// ⚠️ AlphaBlend 配上 AC_SRC_ALPHA 时要求源是**预乘**的 ——
//    颜色分量必须先乘 alpha，否则半透明的边缘会发白。
HBITMAP MakeHueRingBitmap(int size, int innerR, int outerR) {
    if (size <= 0 || outerR <= innerR) return nullptr;

    void* mem = nullptr;
    HBITMAP bmp = CreateDib(size, size, &mem);
    if (bmp == nullptr) return nullptr;

    auto* px = static_cast<unsigned*>(mem);
    const double cx = size / 2.0;
    const double cy = size / 2.0;
    const double aa = 1.2;   // 软边宽度（像素）

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - cx;
            const double dy = y + 0.5 - cy;
            const double d  = std::sqrt(dx * dx + dy * dy);

            double a = 1.0;
            if (d > outerR - aa) a = (outerR - d) / aa;
            if (d < innerR + aa) a = (a < (d - innerR) / aa) ? a : (d - innerR) / aa;
            if (a <= 0.0) { px[y * size + x] = 0u; continue; }
            if (a > 1.0) a = 1.0;

            // 与 color_wheel.cpp 里的口径必须一致：0 度在正上方、顺时针。
            double ang = std::atan2(dx, -dy) * 180.0 / kPi;
            if (ang < 0.0) ang += 360.0;

            const COLORREF c = HsvToRgb(HsvColor{ ang, 1.0, 1.0 });
            const unsigned al = static_cast<unsigned>(a * 255.0 + 0.5);
            const unsigned r  = static_cast<unsigned>(GetRValue(c) * a + 0.5);
            const unsigned g  = static_cast<unsigned>(GetGValue(c) * a + 0.5);
            const unsigned b  = static_cast<unsigned>(GetBValue(c) * a + 0.5);

            px[y * size + x] = (al << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return bmp;
}

// 饱和度/明度方块：横轴 S（左 0 右 1）、纵轴 V（上 1 下 0）。
// 完全由 hue 决定，所以色相一变就得重画。
HBITMAP MakeSvBitmap(int size, double hue) {
    if (size <= 0) return nullptr;

    void* mem = nullptr;
    HBITMAP bmp = CreateDib(size, size, &mem);
    if (bmp == nullptr) return nullptr;

    auto* px = static_cast<unsigned*>(mem);
    for (int y = 0; y < size; ++y) {
        const double v = 1.0 - (y + 0.5) / size;
        for (int x = 0; x < size; ++x) {
            const double s = (x + 0.5) / size;
            const COLORREF c = HsvToRgb(HsvColor{ hue, s, v });
            px[y * size + x] = 0xFF000000u |
                               (static_cast<unsigned>(GetRValue(c)) << 16) |
                               (static_cast<unsigned>(GetGValue(c)) << 8) |
                                static_cast<unsigned>(GetBValue(c));
        }
    }
    return bmp;
}

// ---------------------------------------------------------------------------

class CColorWheelDlg : public CDialogImpl<CColorWheelDlg> {
public:
    enum { IDD = IDD_LYRICUS_COLORWHEEL };

    CColorWheelDlg(COLORREF initial, const wchar_t* title)
        : m_hsv(RgbToHsv(initial)), m_result(initial), m_title(title) {}

    ~CColorWheelDlg() { FreeBitmaps(); }

    COLORREF Result()   const { return m_result; }
    bool     Accepted() const { return m_accepted; }

    BEGIN_MSG_MAP(CColorWheelDlg)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_ERASEBKGND(OnEraseBkgnd)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSELEAVE(OnMouseLeave)
        MSG_WM_SETCURSOR(OnSetCursor)
    END_MSG_MAP()

private:
    // 位图与窗口尺寸/色相绑定，所以只在必要时重造
    void EnsureBitmaps(const ColorWheelLayout& L);
    void FreeBitmaps();

    ColorWheelLayout CurrentLayout() const;
    void  Repaint();
    void  ApplyPick(const WheelPick& pick);   // 只吸收被碰到的那个分量

    BOOL OnInitDialog(HWND, LPARAM);
    void OnDestroy();
    BOOL OnEraseBkgnd(CDCHandle);
    void OnPaint(CDCHandle);
    void OnMouseMove(UINT flags, CPoint pt);
    void OnLButtonDown(UINT flags, CPoint pt);
    void OnLButtonUp(UINT flags, CPoint pt);
    void OnMouseLeave();
    BOOL OnSetCursor(CWindow, UINT, UINT);

    void DrawRing(HDC dc, const ColorWheelLayout& L);
    void DrawSvBox(HDC dc, const ColorWheelLayout& L);
    void DrawMarkers(HDC dc, const ColorWheelLayout& L);
    void DrawPreview(HDC dc, const ColorWheelLayout& L, const HostTheme& T);
    void DrawButtons(HDC dc, const ColorWheelLayout& L, const HostTheme& T);
    void DrawMarkerRing(HDC dc, POINT at, int r);

    HsvColor         m_hsv;
    COLORREF         m_result;
    const wchar_t*   m_title = nullptr;
    bool             m_accepted = false;

    HBITMAP m_ringBmp = nullptr;
    int     m_ringSize = 0, m_ringInner = 0, m_ringOuter = 0;

    HBITMAP m_svBmp = nullptr;
    int     m_svSize = 0;
    double  m_svHue = -1.0;

    HFONT   m_font = nullptr;
    HFONT   m_fontSmall = nullptr;

    WheelHit m_drag = WheelHit::None;
    WheelHit m_hot  = WheelHit::None;
    bool     m_tracking = false;
};

// ---------------------------------------------------------------------------

ColorWheelLayout CColorWheelDlg::CurrentLayout() const {
    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    return ComputeColorWheelLayout(rc.right - rc.left, rc.bottom - rc.top,
                                   static_cast<int>(GetDpiForWindowSafe(m_hWnd)));
}

void CColorWheelDlg::Repaint() {
    if (::IsWindow(m_hWnd)) ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

BOOL CColorWheelDlg::OnInitDialog(HWND, LPARAM) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    m_font      = MakeUiFont(dpi, 9, false);
    m_fontSmall = MakeUiFont(dpi, 8, false);

    // ⚠️ 必须加 :: —— 不加会被 ATL 的 CWindow::SetWindowTextW 抢走，
    //    而那是成员函数、只接受一个参数（工程里已经踩过同一个坑）。
    if (m_title != nullptr && *m_title != L'\0') ::SetWindowTextW(m_hWnd, m_title);
    return TRUE;
}

void CColorWheelDlg::OnDestroy() {
    FreeBitmaps();
    if (m_font      != nullptr) { DeleteObject(m_font);      m_font      = nullptr; }
    if (m_fontSmall != nullptr) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
}

void CColorWheelDlg::FreeBitmaps() {
    if (m_ringBmp != nullptr) { DeleteObject(m_ringBmp); m_ringBmp = nullptr; }
    if (m_svBmp   != nullptr) { DeleteObject(m_svBmp);   m_svBmp   = nullptr; }
    m_ringSize = m_ringInner = m_ringOuter = 0;
    m_svSize = 0;
    m_svHue = -1.0;
}

void CColorWheelDlg::EnsureBitmaps(const ColorWheelLayout& L) {
    // 色相环只跟几何有关，与当前颜色无关 —— 尺寸没变就一直用同一张
    if (m_ringBmp == nullptr || m_ringSize  != L.outerR * 2 ||
        m_ringInner != L.innerR || m_ringOuter != L.outerR) {
        if (m_ringBmp != nullptr) { DeleteObject(m_ringBmp); m_ringBmp = nullptr; }
        m_ringSize  = L.outerR * 2;
        m_ringInner = L.innerR;
        m_ringOuter = L.outerR;
        m_ringBmp   = MakeHueRingBitmap(m_ringSize, m_ringInner, m_ringOuter);
    }

    // SV 方块依赖色相：色相变了必须重画，否则方块里的颜色和环上的marker对不上
    const int svSide = L.svBox.right - L.svBox.left;
    if (m_svBmp == nullptr || m_svSize != svSide || m_svHue != m_hsv.h) {
        if (m_svBmp != nullptr) { DeleteObject(m_svBmp); m_svBmp = nullptr; }
        m_svSize = svSide;
        m_svHue  = m_hsv.h;
        m_svBmp  = MakeSvBitmap(m_svSize, m_svHue);
    }
}

BOOL CColorWheelDlg::OnEraseBkgnd(CDCHandle) {
    return TRUE;   // 配 OnPaint 的双缓冲，拖色环时不闪
}

void CColorWheelDlg::OnPaint(CDCHandle) {
    PAINTSTRUCT ps{};
    const HDC dc = BeginPaint(&ps);
    if (dc == nullptr) return;

    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    const HDC mem = CreateCompatibleDC(dc);
    const HBITMAP back = CreateCompatibleBitmap(dc, w, h);
    const HGDIOBJ oldBmp = SelectObject(mem, back);

    const HostTheme T = QueryHostTheme();
    const ColorWheelLayout L = CurrentLayout();

    // 底色
    HBRUSH bg = CreateSolidBrush(T.bg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    if (L.outerR > 0) {
        EnsureBitmaps(L);
        DrawRing(mem, L);
        DrawSvBox(mem, L);
        DrawMarkers(mem, L);
        DrawPreview(mem, L, T);
        DrawButtons(mem, L, T);
    }

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(back);
    DeleteDC(mem);
    EndPaint(&ps);
}

void CColorWheelDlg::DrawRing(HDC dc, const ColorWheelLayout& L) {
    if (m_ringBmp == nullptr) return;

    const HDC tmp = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(tmp, m_ringBmp);

    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;   // 源是预乘的

    AlphaBlend(dc, L.cx - L.outerR, L.cy - L.outerR, m_ringSize, m_ringSize,
               tmp, 0, 0, m_ringSize, m_ringSize, bf);

    SelectObject(tmp, old);
    DeleteDC(tmp);
}

void CColorWheelDlg::DrawSvBox(HDC dc, const ColorWheelLayout& L) {
    if (m_svBmp == nullptr) return;

    const HDC tmp = CreateCompatibleDC(dc);
    const HGDIOBJ old = SelectObject(tmp, m_svBmp);
    // SV 方块是不透明的，且尺寸与几何一致 —— 直接拷，省下 AlphaBlend 的开销
    BitBlt(dc, L.svBox.left, L.svBox.top,
           m_svSize, m_svSize, tmp, 0, 0, SRCCOPY);
    SelectObject(tmp, old);
    DeleteDC(tmp);

    // 描边，免得浅色区块和窗口底色糊在一起
    const HostTheme T = QueryHostTheme();
    StrokeRoundRect(dc, L.svBox, MulDiv(2, L.dpi, 96), 1, T.fg);
}

void CColorWheelDlg::DrawMarkerRing(HDC dc, POINT at, int r) {
    // 两圈：里面白、外面黑。色环上什么颜色都有，单色标记在某些色相上会看不见。
    const int w = MulDiv(2, 96, 96);
    HBRUSH nb = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));

    HPEN outer = CreatePen(PS_SOLID, w + 2, RGB(0, 0, 0));
    HGDIOBJ op = SelectObject(dc, outer);
    HGDIOBJ ob = SelectObject(dc, nb);
    Ellipse(dc, at.x - r, at.y - r, at.x + r, at.y + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(outer);

    HPEN inner = CreatePen(PS_SOLID, w, RGB(255, 255, 255));
    op = SelectObject(dc, inner);
    ob = SelectObject(dc, nb);
    Ellipse(dc, at.x - r, at.y - r, at.x + r, at.y + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(inner);
}

void CColorWheelDlg::DrawMarkers(HDC dc, const ColorWheelLayout& L) {
    const int radius = MulDiv(6, L.dpi, 96);

    // 色环上的标记：画在环的中线上
    const int rMid = (L.outerR + L.innerR) / 2;
    DrawMarkerRing(dc, HuePointOnRing(L, m_hsv.h, rMid), radius);

    // SV 方块里的标记
    DrawMarkerRing(dc, SvPointInBox(L.svBox, m_hsv.s, m_hsv.v), radius);
}

void CColorWheelDlg::DrawPreview(HDC dc, const ColorWheelLayout& L, const HostTheme& T) {
    const COLORREF cur = HsvToRgb(m_hsv);
    FillRoundRect(dc, L.preview, MulDiv(6, L.dpi, 96), cur);
    StrokeRoundRect(dc, L.preview, MulDiv(6, L.dpi, 96), 1, T.fg);

    wchar_t buf[16];
    swprintf_s(buf, L"#%02X%02X%02X", GetRValue(cur), GetGValue(cur), GetBValue(cur));
    DrawTextIn(dc, L.hexLabel, buf, T.fg, m_font,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void CColorWheelDlg::DrawButtons(HDC dc, const ColorWheelLayout& L, const HostTheme& T) {
    auto one = [&](const RECT& r, const wchar_t* text, WheelHit id, bool primary) {
        const bool hot    = (m_hot  == id);
        const bool active = (m_drag == id);
        RECT box = r;
        if (active) OffsetRect(&box, 0, MulDiv(1, L.dpi, 96));

        const int radius = MulDiv(6, L.dpi, 96);
        COLORREF fill;
        if (active)      fill = T.dark ? RGB(60, 110, 170) : RGB(0, 100, 180);
        else if (hot)    fill = T.dark ? RGB(70, 74, 82)  : RGB(228, 228, 232);
        else if (primary) fill = T.dark ? RGB(46, 90, 140) : RGB(0, 120, 212);
        else             fill = T.dark ? RGB(52, 54, 60)  : RGB(240, 240, 242);

        const COLORREF fg = (primary && !hot && !active) ? RGB(255, 255, 255)
                          : (active ? RGB(255, 255, 255) : T.fg);

        FillRoundRect(dc, box, radius, fill);
        if (!primary) StrokeRoundRect(dc, box, radius, 1,
                                      T.dark ? RGB(90, 92, 98) : RGB(200, 200, 206));
        DrawTextIn(dc, box, text, fg, m_font,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    one(L.cancel, L"取消", WheelHit::Cancel, false);
    one(L.ok,     L"确定", WheelHit::Ok,     true);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void CColorWheelDlg::ApplyPick(const WheelPick& pick) {
    // ⚠️ 只吸收**被碰到的**那个分量。
    //    WheelPick 已经把没碰到的分量原样带回来了，这里整份赋值即可 ——
    //    千万不要在这边另算一遍，那就是两处口径，迟早对不上。
    switch (pick.hit) {
    case WheelHit::Ring:  m_hsv.h = pick.hsv.h; break;
    case WheelHit::SvBox: m_hsv.s = pick.hsv.s; m_hsv.v = pick.hsv.v; break;
    default: return;
    }
    Repaint();
}

void CColorWheelDlg::OnMouseMove(UINT, CPoint pt) {
    if (m_drag == WheelHit::Ring || m_drag == WheelHit::SvBox) {
        ApplyPick(HitTestColorWheel(CurrentLayout(), pt, m_hsv));
        return;
    }

    if (!m_tracking) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = m_hWnd;
        if (TrackMouseEvent(&tme)) m_tracking = true;
    }

    const WheelHit hit = HitTestColorWheel(CurrentLayout(), pt, m_hsv).hit;
    SetCursor(LoadCursorW(nullptr, hit == WheelHit::None ? IDC_ARROW : IDC_HAND));
    if (hit != m_hot) { m_hot = hit; Repaint(); }
}

void CColorWheelDlg::OnMouseLeave() {
    m_tracking = false;
    if (m_hot != WheelHit::None) { m_hot = WheelHit::None; Repaint(); }
}

void CColorWheelDlg::OnLButtonDown(UINT, CPoint pt) {
    const WheelPick pick = HitTestColorWheel(CurrentLayout(), pt, m_hsv);
    if (pick.hit == WheelHit::None) return;

    m_drag = pick.hit;
    if (pick.hit == WheelHit::Ring || pick.hit == WheelHit::SvBox) {
        ::SetCapture(m_hWnd);
        ApplyPick(pick);   // 点哪儿跳哪儿，而不是只响应拖动
    } else {
        Repaint();
    }
}

void CColorWheelDlg::OnLButtonUp(UINT, CPoint pt) {
    const WheelHit was = m_drag;
    m_drag = WheelHit::None;
    if (::GetCapture() == m_hWnd) ::ReleaseCapture();

    // 松手时以**松开的位置**为准：按下滑到别处再抬起来，不该算点中最初那个键
    const WheelPick pick = HitTestColorWheel(CurrentLayout(), pt, m_hsv);

    if (was == WheelHit::Ok || was == WheelHit::Cancel) {
        if (pick.hit != was) { Repaint(); return; }   // 移开了就不算
        if (was == WheelHit::Ok) {
            m_result   = HsvToRgb(m_hsv);
            m_accepted = true;
            EndDialog(IDOK);
        } else {
            EndDialog(IDCANCEL);
        }
        return;
    }
    Repaint();
}

BOOL CColorWheelDlg::OnSetCursor(CWindow, UINT, UINT) {
    return TRUE;   // 光标在 OnMouseMove 里按命中目标设过了
}

} // namespace

// ---------------------------------------------------------------------------

namespace lyricus {

bool PromptColorWheel(HWND parent, COLORREF& inOut, const wchar_t* title) {
    CColorWheelDlg dlg(inOut, title);
    if (dlg.DoModal(parent) != IDOK || !dlg.Accepted()) return false;

    inOut = dlg.Result();
    DebugLog("色环取色：-> #%02X%02X%02X",
             GetRValue(inOut), GetGValue(inOut), GetBValue(inOut));
    return true;
}

} // namespace lyricus
