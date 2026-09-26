#include "stdafx.h"

#include "resource.h"
#include "config.h"
#include "control_window.h"
#include "debug_log.h"
#include "prefs_layout.h"
#include "prefs_page.h"         // OpenPrefsPage（View 菜单的入口）
#include "dpi_util.h"           // GetDpiForWindowSafe（与控制面板共用同一份）
#include "ui_draw.h"            // 圆角矩形/文字/字体/宿主主题（与色环取色器共用）
#include "color_picker.h"       // PromptColorWheel（取色走自绘色环）

#include <SDK/preferences_page.h>
#include <SDK/ui.h>             // ui_control::show_preferences（菜单"外观设置"要用）
#include <SDK/ui_element.h>     // ui_config_manager：宿主主题色 + 暗色模式
#include <helpers/atl-misc.h>   // preferences_page_impl

#include <cstdio>

// ---------------------------------------------------------------------------
// Lyricus 首选项页 —— **全自绘**
//
// 【为什么要重做】用户 2026-09-26 说「这个界面有点老旧，在 lyricus 那个
// 二级界面实现一个更现代化的面板」。旧版是 GROUPBOX 分组框 + 六个 62x14 的
// 小色块按钮 + 系统 trackbar，控件长什么样完全由系统主题决定，改不动。
// 所以这一版把控件**全部删掉**（见 lyricus.rc），内容全在这里画。
//
// 【布局与配色是纯函数】都在 prefs_layout.h/.cpp 里，能进离线单测台
// （run.ps1 的 prefs 组）。这个文件只负责"把算好的矩形画出来"和"处理鼠标"。
// 那条界线很重要：自绘界面最容易出的两类问题（算错位置、配色读不清）
// 都是纯计算，放进单测比在截图里找强得多。
//
// 【页面底色必须问宿主要】自绘页面最怕颜色写死 —— 用户切暗色模式后一片惨白。
// 这里用 ui_config_manager::getSysColor()：它先查 foobar2000 的主题配置，
// 查不到才回退系统色（见 SDK/ui_element.cpp:259）。于是页面底色和首选项
// 窗口是同一个来源，接缝处不会有色差。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

// GUID 段 0x37：首选项页。分配前已 grep 全工程（见 D-022）。
const GUID guid_prefs_page = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x37}};

// 颜色项的绑定表：结构体成员 + 显示名。
//
// ⚠️ **顺序必须和 prefs_layout 的卡片编号一致**（索引 0..5）。
// 布局是按索引算矩形的（两行三列，从左到右、从上到下），这里换了顺序，
// 界面上的位置就跟着换 —— 两边必须同步改。
struct ColorSlot {
    COLORREF       PanelAppearance::*member;
    const wchar_t*                    label;
};

const ColorSlot kColorSlots[kPrefsColorCount] = {
    { &PanelAppearance::header,  L"曲名"       },
    { &PanelAppearance::current, L"当前歌词行" },
    { &PanelAppearance::normal,  L"其它歌词行" },
    { &PanelAppearance::dim,     L"次要文字"   },
    { &PanelAppearance::warn,    L"警告文字"   },
    { &PanelAppearance::bg,      L"面板底色"   },
};

// 命中目标。色块直接用数组下标（0..5），其余用负值区分 ——
// 这样返回值能直接当数组下标用，少一层映射。
constexpr int kHitNone   = -1;
constexpr int kHitSlider = -2;
constexpr int kHitReset  = -3;

class CLyricusPrefsDlg : public CDialogImpl<CLyricusPrefsDlg>,
                         public preferences_page_instance {
public:
    enum { IDD = IDD_LYRICUS_PREFS };

    explicit CLyricusPrefsDlg(preferences_page_callback::ptr callback)
        : m_callback(callback),
          m_edited(GetPanelAppearance()),     // 界面上正在编辑的值
          m_applied(GetPanelAppearance()) {}  // 上次"应用"下去的值

    ~CLyricusPrefsDlg() { FreeFonts(); }

    // 字体在**两处**都释放：WM_DESTROY 和析构。两边都先把指针置空，
    // 所以谁先来都不会重复 DeleteObject。
    //
    // ⚠️ 别写成 `for (HFONT& f : { m_fontBody, ... })` ——
    //    initializer_list 的元素是 const，非 const 引用绑不上（编译不过）。
    void FreeFonts() {
        if (m_fontBody  != nullptr) { DeleteObject(m_fontBody);  m_fontBody  = nullptr; }
        if (m_fontBold  != nullptr) { DeleteObject(m_fontBold);  m_fontBold  = nullptr; }
        if (m_fontSmall != nullptr) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
    }

    BEGIN_MSG_MAP(CLyricusPrefsDlg)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_ERASEBKGND(OnEraseBkgnd)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSELEAVE(OnMouseLeave)
        MSG_WM_SETCURSOR(OnSetCursor)
        MSG_WM_GETDLGCODE(OnGetDlgCode)
        MSG_WM_KEYDOWN(OnKeyDown)
    END_MSG_MAP()

    // ---- preferences_page_instance 的契约 ----
    t_uint32 get_state() override;
    void     apply()     override;
    void     reset()     override;

private:
    // 当前客户区对应的布局与配色。两者都是纯函数，按需重算即可（很便宜）。
    PrefsLayout CurrentLayout() const;
    PrefsTheme  CurrentTheme() const;

    int  HitTest(POINT pt) const;              // 返回 kHit* 或色块下标
    void Repaint();
    void NotifyChanged();

    BOOL OnInitDialog(HWND hwndFocus, LPARAM lParam);
    void OnDestroy();
    BOOL OnEraseBkgnd(CDCHandle dc);
    void OnPaint(CDCHandle dc);
    void OnMouseMove(UINT flags, CPoint pt);
    void OnLButtonDown(UINT flags, CPoint pt);
    void OnLButtonUp(UINT flags, CPoint pt);
    void OnMouseLeave();
    BOOL OnSetCursor(CWindow wnd, UINT hitTest, UINT message);
    UINT OnGetDlgCode(LPMSG msg);
    void OnKeyDown(TCHAR key, UINT repeat, UINT flags);

    void DrawPage(HDC dc, const RECT& rc, const PrefsLayout& L, const PrefsTheme& T);
    void DrawColorCard(HDC dc, const RECT& card, int index);
    void DrawSlider(HDC dc, const RECT& r, int value);
    void DrawResetButton(HDC dc, const RECT& r);

    void SetAlphaFromSliderX(int x);

    // 弹一个模态取色器。
    //
    // ⚠️ 模态对话框会泵消息，期间**首选项窗口可能被关掉、页面随之被释放**。
    //    SDK 在 preferences_page.h:133 专门警告过这种情况。
    //    所以进来先拿一份自身引用把自己钉住，返回后碰成员才安全；
    //    返回后还要再确认一次窗口还在。
    bool PickColor(COLORREF& inOut);

    preferences_page_callback::ptr m_callback;
    PanelAppearance m_edited;
    PanelAppearance m_applied;

    // 交互状态
    int  m_hot       = kHitNone;   // 鼠标悬停在谁身上
    int  m_active    = kHitNone;   // 按下了谁
    bool m_dragAlpha = false;      // 正在拖不透明度滑块
    bool m_tracking  = false;      // 已登记 TME_LEAVE

    // 字体按 dpi 建一次就够（对话框存续期间不会变 dpi）
    HFONT m_fontBody  = nullptr;
    HFONT m_fontBold  = nullptr;
    HFONT m_fontSmall = nullptr;
};

// ---------------------------------------------------------------------------

PrefsLayout CLyricusPrefsDlg::CurrentLayout() const {
    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    return ComputePrefsLayout(rc.right - rc.left, rc.bottom - rc.top,
                              static_cast<int>(GetDpiForWindowSafe(m_hWnd)));
}

PrefsTheme CLyricusPrefsDlg::CurrentTheme() const {
    // 底色与文字问宿主要 —— 走 ui_draw 里的公共实现。
    // 它和色环取色器用的是同一份：两边各取各的，会出现"一个跟着 foobar2000
    // 的暗色主题走、另一个还是系统亮色"，两个窗口并排打开时非常刺眼。
    const HostTheme h = QueryHostTheme();
    return MakePrefsTheme(h.dark, h.bg, h.fg);
}

int CLyricusPrefsDlg::HitTest(POINT pt) const {
    const PrefsLayout L = CurrentLayout();

    auto inside = [&pt](const RECT& r) {
        return r.right > r.left && r.bottom > r.top &&
               pt.x >= r.left && pt.x < r.right &&
               pt.y >= r.top  && pt.y < r.bottom;
    };

    for (int i = 0; i < kPrefsColorCount; ++i) {
        // 名称也算命中区域 —— 它紧贴色块下方，只点色块的话手感很别扭。
        if (inside(L.cards[i]) || inside(L.cardLabels[i])) return i;
    }
    if (inside(L.slider)) return kHitSlider;
    if (inside(L.reset))  return kHitReset;
    return kHitNone;
}

void CLyricusPrefsDlg::Repaint() {
    if (::IsWindow(m_hWnd)) ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

BOOL CLyricusPrefsDlg::OnInitDialog(HWND, LPARAM) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    m_fontBody  = MakeUiFont(dpi, 9,  false);
    m_fontBold  = MakeUiFont(dpi, 9,  true);
    m_fontSmall = MakeUiFont(dpi, 8,  false);

    DebugLog("首选项页：已初始化（全自绘，alpha=%d）", m_edited.alpha);
    return TRUE;
}

void CLyricusPrefsDlg::OnDestroy() {
    FreeFonts();
}

BOOL CLyricusPrefsDlg::OnEraseBkgnd(CDCHandle) {
    // 返回 TRUE = "我已经处理了" —— 不让系统擦背景。
    // 配合 OnPaint 里的双缓冲，页面切换时不会闪白。
    return TRUE;
}

void CLyricusPrefsDlg::OnPaint(CDCHandle) {
    PAINTSTRUCT ps{};
    const HDC dc = BeginPaint(&ps);
    if (dc == nullptr) return;

    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // 双缓冲：自绘页面直接在窗口 DC 上画会闪（尤其是拖滑块时每帧重绘）
    const HDC mem = CreateCompatibleDC(dc);
    const HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    const HGDIOBJ oldBmp = SelectObject(mem, bmp);

    DrawPage(mem, rc, CurrentLayout(), CurrentTheme());

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(&ps);
}

void CLyricusPrefsDlg::DrawPage(HDC dc, const RECT& rc, const PrefsLayout& L,
                                const PrefsTheme& T) {
    // ---- 底色 ----
    HBRUSH bg = CreateSolidBrush(T.pageBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    // 布局判定"这块地方画不下"时返回的是空矩形，逐项判空后跳过即可。
    auto empty = [](const RECT& r) { return r.right <= r.left || r.bottom <= r.top; };

    // ---- 分组标题 ----
    DrawTextIn(dc, L.titleColors, L"浮动面板配色", T.text, m_fontBold,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, L.titleAlpha, L"整体不透明度", T.text, m_fontBold,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t buf[32];
    swprintf_s(buf, L"%d / 255", m_edited.alpha);
    DrawTextIn(dc, L.alphaValue, buf, T.textDim, m_fontBody,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // ---- 色块 ----
    for (int i = 0; i < kPrefsColorCount; ++i) {
        if (empty(L.cards[i])) continue;
        DrawColorCard(dc, L.cards[i], i);
        DrawTextIn(dc, L.cardLabels[i], kColorSlots[i].label, T.textDim, m_fontBody,
                   DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    // ---- 滑块 ----
    if (!empty(L.slider)) DrawSlider(dc, L.slider, m_edited.alpha);

    // ---- 底部说明 ----
    DrawTextIn(dc, L.hint,
               L"点色块选颜色；拖滑块调不透明度。\n"
               L"这些设置只影响浮动面板，DUI / CUI 面板跟随宿主主题。",
               T.textDim, m_fontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // ---- 按钮 ----
    if (!empty(L.reset)) DrawResetButton(dc, L.reset);
}

void CLyricusPrefsDlg::DrawColorCard(HDC dc, const RECT& card, int index) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int radius = MulDiv(8, dpi, 96);

    const COLORREF c = m_edited.*(kColorSlots[index].member);
    const bool hot    = (m_hot == index);
    const bool active = (m_active == index);

    RECT r = card;
    if (active) OffsetRect(&r, 0, MulDiv(1, dpi, 96));   // 按下时轻微下沉

    FillRoundRect(dc, r, radius, c);

    // 描边：色块可能和页面底色撞色。悬停时换成强调色，给出"可以点"的反馈。
    const PrefsTheme T = CurrentTheme();
    StrokeRoundRect(dc, r, radius, hot ? MulDiv(2, dpi, 96) : 1,
                    hot ? T.accent : T.border);

    // 十六进制值：按色块自身亮度选黑字还是白字，浅色和深色底上都读得清。
    wchar_t text[16];
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    DrawTextIn(dc, r, text, PrefsLuminance(c) > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255),
               m_fontBody, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void CLyricusPrefsDlg::DrawSlider(HDC dc, const RECT& r, int value) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const PrefsTheme T = CurrentTheme();

    const int trackH = MulDiv(6, dpi, 96);
    const int knobR  = MulDiv(9, dpi, 96);
    const int inset  = knobR;   // 手柄贴到两端时不至于被裁掉

    const int left  = r.left + inset;
    const int right = r.right - inset;
    if (right <= left) return;

    const int cy = (r.top + r.bottom) / 2;

    // 轨道
    RECT track{ left, cy - trackH / 2, right, cy + trackH / 2 };
    FillRoundRect(dc, track, trackH / 2, T.cardBg);

    // 已填充部分 + 手柄位置
    const int span = right - left;
    const int pos  = left + MulDiv(span, ClampAlpha(value) - kMinAlpha,
                                   kMaxAlpha - kMinAlpha);

    const bool hot = (m_hot == kHitSlider) || m_dragAlpha;

    if (pos > left) {
        RECT fill{ left, track.top, pos, track.bottom };
        FillRoundRect(dc, fill, trackH / 2, T.accent);
    }

    // 手柄：悬停/拖动时稍微放大一点，给出反馈
    const int kr = hot ? knobR + MulDiv(1, dpi, 96) : knobR;
    RECT knob{ pos - kr, cy - kr, pos + kr, cy + kr };
    FillRoundRect(dc, knob, kr, hot ? T.accent : T.text);

    // 手柄描边用页面底色，让它和轨道之间有干净的边界
    StrokeRoundRect(dc, knob, kr, MulDiv(2, dpi, 96), T.pageBg);
}

void CLyricusPrefsDlg::DrawResetButton(HDC dc, const RECT& r) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const PrefsTheme T = CurrentTheme();

    const bool hot    = (m_hot == kHitReset);
    const bool active = (m_active == kHitReset);

    RECT box = r;
    if (active) OffsetRect(&box, 0, MulDiv(1, dpi, 96));

    const int radius = MulDiv(6, dpi, 96);
    FillRoundRect(dc, box, radius, active ? T.accent : (hot ? T.cardHot : T.cardBg));
    StrokeRoundRect(dc, box, radius, 1, T.border);

    // 按下态底色是强调色（偏深），文字得换成能读清的一侧
    const COLORREF fg = active ? RGB(255, 255, 255) : T.text;
    DrawTextIn(dc, box, L"恢复默认", fg, m_fontBody,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void CLyricusPrefsDlg::OnMouseMove(UINT /*flags*/, CPoint pt) {
    if (m_dragAlpha) {
        SetAlphaFromSliderX(pt.x);
        return;
    }

    // 登记一次 TME_LEAVE —— 没有它收不到 WM_MOUSELEAVE，悬停态会一直留着。
    if (!m_tracking) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = m_hWnd;
        if (TrackMouseEvent(&tme)) m_tracking = true;
    }

    const int hit = HitTest(pt);
    SetCursor(LoadCursorW(nullptr, hit == kHitNone ? IDC_ARROW : IDC_HAND));
    if (hit != m_hot) { m_hot = hit; Repaint(); }
}

void CLyricusPrefsDlg::OnMouseLeave() {
    m_tracking = false;
    if (m_hot != kHitNone) { m_hot = kHitNone; Repaint(); }
}

void CLyricusPrefsDlg::OnLButtonDown(UINT /*flags*/, CPoint pt) {
    const int hit = HitTest(pt);
    if (hit == kHitNone) return;

    m_active = hit;
    ::SetCapture(m_hWnd);

    if (hit == kHitSlider) {
        m_dragAlpha = true;
        SetAlphaFromSliderX(pt.x);   // 点哪儿跳到哪儿，而不是只响应拖动
    }
    Repaint();
}

void CLyricusPrefsDlg::OnLButtonUp(UINT /*flags*/, CPoint pt) {
    const int hit  = m_active;
    const bool wasDrag = m_dragAlpha;

    m_active    = kHitNone;
    m_dragAlpha = false;
    if (::GetCapture() == m_hWnd) ::ReleaseCapture();

    if (hit == kHitReset) {
        reset();   // reset() 只改界面，不写存储（见它的说明）
        Repaint();
        return;
    }

    if (hit >= 0 && hit < kPrefsColorCount) {
        COLORREF* p = &(m_edited.*(kColorSlots[hit].member));
        if (PickColor(*p)) {
            NotifyChanged();
            Repaint();
        }
        return;
    }

    if (wasDrag) Repaint();
}

void CLyricusPrefsDlg::SetAlphaFromSliderX(int x) {
    const PrefsLayout L = CurrentLayout();
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int knobR = MulDiv(9, dpi, 96);
    const int left  = L.slider.left + knobR;
    const int right = L.slider.right - knobR;
    if (right <= left) return;

    // 用 long long 作中间量再除 —— 这里要先做 (x - left) 的偏移，
    // 套不进 MulDiv 的形式；而两个 int 相乘在极端 dpi 下有溢出风险。
    int v = kMinAlpha + static_cast<int>(
                static_cast<long long>(x - left) * (kMaxAlpha - kMinAlpha) / (right - left));
    v = ClampAlpha(v);

    if (v == m_edited.alpha) return;
    m_edited.alpha = v;
    NotifyChanged();
    Repaint();
}

BOOL CLyricusPrefsDlg::OnSetCursor(CWindow, UINT, UINT) {
    // 光标在 OnMouseMove 里按命中目标设过了；这里吃掉默认处理，
    // 免得系统在边框/背景上把它改回箭头造成闪烁。
    return TRUE;
}

UINT CLyricusPrefsDlg::OnGetDlgCode(LPMSG) {
    // 方向键留给不透明度滑块用，别被宿主拿去做页面切换。
    return DLGC_WANTARROWS;
}

void CLyricusPrefsDlg::OnKeyDown(TCHAR key, UINT /*repeat*/, UINT /*flags*/) {
    int delta = 0;
    if (key == VK_LEFT)  delta = -1;
    if (key == VK_RIGHT) delta = +1;
    if (delta == 0) return;

    const int v = ClampAlpha(m_edited.alpha + delta);
    if (v == m_edited.alpha) return;
    m_edited.alpha = v;
    NotifyChanged();
    Repaint();
}

// ---------------------------------------------------------------------------
// 与 preferences_page_instance 的契约
// ---------------------------------------------------------------------------

void CLyricusPrefsDlg::NotifyChanged() {
    if (m_callback.is_valid()) m_callback->on_state_changed();
}

t_uint32 CLyricusPrefsDlg::get_state() {
    t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
    if (m_edited != m_applied) state |= preferences_state::changed;
    return state;
}

void CLyricusPrefsDlg::apply() {
    SetPanelAppearance(m_edited);
    m_applied = m_edited;

    DebugLog("首选项页：已应用 曲名=#%02X%02X%02X 当前行=#%02X%02X%02X 普通行=#%02X%02X%02X "
             "暗色=#%02X%02X%02X 警告=#%02X%02X%02X 底色=#%02X%02X%02X alpha=%d",
             GetRValue(m_edited.header),  GetGValue(m_edited.header),  GetBValue(m_edited.header),
             GetRValue(m_edited.current), GetGValue(m_edited.current), GetBValue(m_edited.current),
             GetRValue(m_edited.normal),  GetGValue(m_edited.normal),  GetBValue(m_edited.normal),
             GetRValue(m_edited.dim),     GetGValue(m_edited.dim),     GetBValue(m_edited.dim),
             GetRValue(m_edited.warn),    GetGValue(m_edited.warn),    GetBValue(m_edited.warn),
             GetRValue(m_edited.bg),      GetGValue(m_edited.bg),      GetBValue(m_edited.bg),
             m_edited.alpha);

    // 浮动面板每 250ms 轮询一次外观，这里不主动推它 ——
    // 页面关闭后它自己就会跟上，也就没有"页面销毁后回调到面板"的耦合。
}

void CLyricusPrefsDlg::reset() {
    // reset 只改界面，**不写存储** —— SDK 明确要求这样，
    // 好让用户先看到效果再决定要不要"应用"（preferences_page.h:144）。
    m_edited = PanelAppearance{};
    Repaint();
}

// ---------------------------------------------------------------------------

bool CLyricusPrefsDlg::PickColor(COLORREF& inOut) {
    // 见声明处的说明：先把自己钉住，防模态期间被释放
    service_ptr_t<preferences_page_instance> self = this;

    // 取色走**自绘的色环取色器**（color_picker.cpp）。
    //
    // 【这一路是怎么走到色环的】
    //   1. 最早用的是 WTL 的 CColorDialog。它**会崩**：2026-09-26 用户报
    //      「preference 里的颜色选项，点击之后会崩溃」，崩溃报告写着
    //          Access violation, write, address 0x20
    //          Crash location: ntdll!RtlEnterCriticalSection  (RCX = 0x18)
    //      —— 在空对象上访问偏移 0x18 的临界区。根因是 WTL 的
    //      CStaticDataInitCriticalSectionLock 在构造函数里直接解引用
    //      ATL::_pAtlModule->m_csStaticDataInitAndTypeInfo，一个空指针检查都没有；
    //      而本组件从没创建过 ATL 模块对象，那个指针一直是空的。
    //   2. 换成纯 Win32 的 ChooseColorW，崩是不崩了。但用户看过之后说
    //      「虽然稍微好点，但我还是想要类似色环的」。
    //   3. 于是有了现在的自绘色环：坐标 <-> 颜色的换算在 color_wheel.cpp
    //      （纯函数、离线可测），窗口与绘制在 color_picker.cpp。
    if (!PromptColorWheel(m_hWnd, inOut, L"选择颜色")) return false;

    // 模态期间页面可能已经被销毁（窗口没了）。self 保证对象还在，
    // 但窗口没了就不该再碰控件。
    if (!::IsWindow(m_hWnd)) return false;
    return true;
}

// ---------------------------------------------------------------------------

class LyricusPrefsPage : public preferences_page_impl<CLyricusPrefsDlg> {
public:
    const char* get_name() override { return "Lyricus"; }
    GUID        get_guid() override { return guid_prefs_page; }
    GUID        get_parent_guid() override { return preferences_page::guid_display; }
    double      get_sort_priority() override { return 100; }
};

preferences_page_factory_t<LyricusPrefsPage> g_prefs_page_factory;

} // namespace

// ---------------------------------------------------------------------------

namespace lyricus {

void OpenPrefsPage() {
    // show_preferences(guid) = 激活首选项对话框并跳转到这一页（ui.h:117）。
    // ui_control 拿不到时（无 GUI 的场合）什么都不做 —— 菜单项本来也点不到。
    ui_control::ptr ui = ui_control::get();
    if (ui.is_valid()) ui->show_preferences(guid_prefs_page);
}

} // namespace lyricus
