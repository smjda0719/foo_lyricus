#include "stdafx.h"

#include "resource.h"
#include "config.h"
#include "control_window.h"
#include "debug_log.h"
#include "prefs_layout.h"
#include "prefs_page.h"         // OpenPrefsPage（View 菜单的入口）
#include "dpi_util.h"           // GetDpiForWindowSafe（与控制面板共用同一份）
#include "ui_draw.h"            // 圆角矩形/文字/字体/宿主主题（与色环取色器共用）
#include "color_util.h"         // ColorLuminance（判"该配黑字还是白字"）
#include "color_picker.h"       // PromptColorWheel（取色走自绘色环）
#include "lyric.h"              // Utf8ToWide / WideToUtf8（字体族是 UTF-8 存的）

#include <SDK/preferences_page.h>
#include <SDK/ui.h>             // ui_control::show_preferences（菜单"外观设置"要用）
#include <SDK/ui_element.h>     // ui_config_manager：宿主主题色 + 暗色模式
#include <helpers/atl-misc.h>   // preferences_page_impl
// ⚠️ 取色和取字体都走纯 Win32 的通用对话框（ChooseColorW / ChooseFontW），
//    刻意不碰 WTL 的 CColorDialog / CFontDialog —— 原因写在 PickColor() 里。
#include <commdlg.h>

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
constexpr int kHitNone    = -1;
constexpr int kHitSlider  = -2;
constexpr int kHitReset   = -3;
constexpr int kHitFontBtn = -4;
// ---- 外观预设（D-088）----
constexpr int kHitPresetCombo  = -5;
constexpr int kHitPresetSave   = -6;
constexpr int kHitPresetDelete = -7;
constexpr int kHitPresetImport = -8;
constexpr int kHitPresetExport = -9;

// 布局判定"这块地方画不下"时返回的是空矩形，逐项判空后跳过即可。
//
// ⚠️ 它原本是 DrawPage 里的一个**局部 lambda**。加 DrawPresetArea 时才发现
//    那种写法只有那一个函数能用，于是第二个绘制函数只好自己再判一遍 ——
//    而"漏判"的表现是某个控件在窄窗口下画到客户区外面去，糊在别的控件上。
//    提到文件作用域，所有绘制函数共用同一份判断。
bool empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

class CLyricusPrefsDlg : public CDialogImpl<CLyricusPrefsDlg>,
                         public preferences_page_instance {
public:
    enum { IDD = IDD_LYRICUS_PREFS };

    explicit CLyricusPrefsDlg(preferences_page_callback::ptr callback)
        : m_callback(callback),
          m_edited(GetPanelAppearance()),     // 界面上正在编辑的值
          m_applied(GetPanelAppearance()),    // 上次"应用"下去的值
          m_editedFontFace(GetLyricDisplayConfig().fontFace),
          m_appliedFontFace(GetLyricDisplayConfig().fontFace) {}

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
    void DrawButton(HDC dc, const RECT& r, const wchar_t* text, int hitId,
                    const PrefsTheme& T, bool leftAlign);
    void DrawResetButton(HDC dc, const RECT& r);
    void DrawFontButton(HDC dc, const RECT& r);
    void DrawPresetArea(HDC dc, const PrefsLayout& L);

    // ---- 外观预设的动作 ----
    // 全是**立刻生效**的，不走"应用 / 取消"：切一套配色就是要马上看到效果，
    // 再让他点一次应用没有意义（和色块那种"先在界面上试"不是一回事）。
    void OnPresetCombo();          // 弹菜单：选一套 / 另存为新预设
    void OnPresetSaveCurrent();    // 把当前外观覆盖进选中的预设
    void OnPresetDelete();         // 删掉选中的（内置的 = 恢复内置默认）
    void OnPresetImport();         // 从文件读一套进来
    void OnPresetExport();         // 把选中的写出去（分享 / 备份）

    // 把"当前的外观"采集出来。界面上正在编辑的颜色 + 正在编辑的字体，
    // 再加上**不在这一页上**的两项（字号、通透度）—— 后两者从配置读，
    // 因为"保存当前外观"的语义就是"保存现在的实际状态"。
    AppearancePreset SnapshotAppearance(const std::wstring& name) const;

    // 选中的预设名。空 = 还没挑过，界面上显示成「（未选择）」。
    // ⚠️ 它**不是配置项**：没有"当前预设"这个东西要持久化 ——
    //    应用过的值已经在各项设置里了，这个字符串只决定按钮上写什么。
    std::wstring m_presetName;

    void SetAlphaFromSliderX(int x);

    // 弹一个模态取色器。
    //
    // ⚠️ 模态对话框会泵消息，期间**首选项窗口可能被关掉、页面随之被释放**。
    //    SDK 在 preferences_page.h:133 专门警告过这种情况。
    //    所以进来先拿一份自身引用把自己钉住，返回后碰成员才安全；
    //    返回后还要再确认一次窗口还在。
    bool PickColor(COLORREF& inOut);
    void PickFont();   // 弹系统字体对话框，把选中的**字体族**记进"正在编辑"的值

    preferences_page_callback::ptr m_callback;
    PanelAppearance m_edited;
    PanelAppearance m_applied;

    // 字体族（UTF-8，空 = 跟随宿主）。**和颜色一样走"应用 / 取消"**。
    //
    // 【为什么不立即写存储】原来是立即写的（D-082），理由是 ChooseFontW 是模态的、
    // 点确定就是明确意图。但那带来一个**当时没想到的副作用**：
    // `reset()` 会把它清掉，而颜色要"点应用"才落盘 —— 于是
    // **点「恢复默认」再点「取消」= 颜色回来了、字体回不来**。
    // 实测用户就是这么丢掉「方正姚体」的（配置里 lyricus.fontFace 还在，值是空的）。
    //
    // 【为什么是 UTF-8 的 std::string】和 LyricDisplayConfig::fontFace 同一口径，
    // 中间不必来回转换；Config::fontFace 本来就是 UTF-8 存的。
    std::string m_editedFontFace;
    std::string m_appliedFontFace;

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
    if (inside(L.fontBtn)) return kHitFontBtn;
    // ---- 外观预设（D-088）----
    if (inside(L.presetCombo))  return kHitPresetCombo;
    if (inside(L.presetSave))   return kHitPresetSave;
    if (inside(L.presetDelete)) return kHitPresetDelete;
    if (inside(L.presetImport)) return kHitPresetImport;
    if (inside(L.presetExport)) return kHitPresetExport;
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

    // 逐项判空后跳过 —— empty() 现在是文件作用域的共用函数（见开头的说明）。

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
    if (!empty(L.fontBtn)) DrawFontButton(dc, L.fontBtn);
    DrawPresetArea(dc, L);
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
    DrawTextIn(dc, r, text, ColorLuminance(c) > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255),
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

// 自绘按钮。「恢复默认」和「字体...」共用 —— 两个按钮的视觉本来就该一致，
// 各写一份的话圆角、按下位移、文字色判断迟早各改各的，慢慢就看出不一样了。
//
// leftAlign：字体名长短差很多（「跟随」vs「Microsoft YaHei UI」），
// 左对齐 + 省略号才看得清开头；固定文案居中更好看。
void CLyricusPrefsDlg::DrawButton(HDC dc, const RECT& r, const wchar_t* text, int hitId,
                                  const PrefsTheme& T, bool leftAlign) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));

    const bool hot    = (m_hot == hitId);
    const bool active = (m_active == hitId);

    RECT box = r;
    if (active) OffsetRect(&box, 0, MulDiv(1, dpi, 96));

    const int radius = MulDiv(6, dpi, 96);
    const COLORREF fill = active ? T.accent : (hot ? T.cardHot : T.cardBg);
    FillRoundRect(dc, box, radius, fill);
    StrokeRoundRect(dc, box, radius, 1, T.border);

    // ★ 文字色按**填充色自己的亮度**选，而不是按状态猜。
    //
    // 这里原本写的是 `active ? 白 : T.text`，而它**当时是对的** ——
    // 因为填充色是从 T.pageBg 推导的、T.text 就是配它的那个前景色，两者同源。
    // 改成按 fill 判断是为了和 color_picker.cpp 保持同一个模式：
    // 那边出过一次「白色按钮配白色文字」（主题标志与实际取到的颜色不同源），
    // 按填充色判断是那种情况下唯一永远正确的做法。
    const COLORREF fg = (ColorLuminance(fill) > 140) ? RGB(20, 20, 24)
                                                     : RGB(255, 255, 255);

    RECT textRc = box;
    UINT flags = DT_VCENTER | DT_SINGLELINE;
    if (leftAlign) {
        textRc.left += MulDiv(10, dpi, 96);
        flags |= DT_LEFT | DT_END_ELLIPSIS;
    } else {
        flags |= DT_CENTER;
    }
    DrawTextIn(dc, textRc, text, fg, m_fontBody, flags);
}

void CLyricusPrefsDlg::DrawResetButton(HDC dc, const RECT& r) {
    DrawButton(dc, r, L"恢复默认", kHitReset, CurrentTheme(), false);
}

void CLyricusPrefsDlg::DrawFontButton(HDC dc, const RECT& r) {    // 显示**正在编辑**的字体，不是已落盘的那个 —— 和色块画 m_edited 一致。
    // 用配置里的值的话，用户选完字体按钮上还是旧名字，看着像没反应。
    std::wstring label = L"字体：";
    if (m_editedFontFace.empty()) {
        label += L"跟随";
    } else {
        label += Utf8ToWide(m_editedFontFace.c_str());
    }
    DrawButton(dc, r, label.c_str(), kHitFontBtn, CurrentTheme(), true);
}

// 外观预设区：标题 + 下拉框 + 四个按钮。
//
// 【为什么下拉用系统菜单而不是自绘列表】自绘列表要就地展开，而这一页只有
// 424 逻辑像素高 —— 列表必然盖住下面几行控件，于是要么被客户区裁掉，
// 要么得再开一个弹窗去处理失焦 / 滚动 / 键盘 / 点到外面关闭。
// TrackPopupMenu 这几件事全是现成的，而且位置它自己会算。
void CLyricusPrefsDlg::DrawPresetArea(HDC dc, const PrefsLayout& L) {
    if (empty(L.titlePreset) && empty(L.presetCombo)) return;   // 整体降级了

    const PrefsTheme& T = CurrentTheme();
    const int r4 = MulDiv(4, L.dpi, 96);

    if (!empty(L.titlePreset)) {
        DrawTextIn(dc, L.titlePreset, L"外观预设", T.text, m_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (!empty(L.presetCombo)) {
        const bool hot = (m_hot == kHitPresetCombo);
        FillRoundRect(dc, L.presetCombo, r4, hot ? T.cardHot : T.cardBg);
        StrokeRoundRect(dc, L.presetCombo, r4, 1, T.border);

        RECT textRc = L.presetCombo;
        textRc.left  += MulDiv(8,  L.dpi, 96);
        textRc.right -= MulDiv(22, L.dpi, 96);      // 给 ▼ 留位置

        // 没选过时显示成**暗色**的提示语 —— 用 textDim 而不是 text，
        // 这样"这是一句说明"和"这是一个预设名"一眼能分开。
        const bool hasSel = !m_presetName.empty();
        DrawTextIn(dc, textRc, hasSel ? m_presetName.c_str() : L"（未选择）",
                   hasSel ? T.text : T.textDim, m_fontBody,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // ▼ 实心三角，贴在右边
        const int cx = L.presetCombo.right - MulDiv(13, L.dpi, 96);
        const int cy = (L.presetCombo.top + L.presetCombo.bottom) / 2;
        const int hw = MulDiv(4, L.dpi, 96);
        const int hh = MulDiv(3, L.dpi, 96);
        POINT tri[3] = { { cx - hw, cy - hh }, { cx + hw, cy - hh }, { cx, cy + hh } };

        HBRUSH br = CreateSolidBrush(T.textDim);
        HGDIOBJ oldBr  = SelectObject(dc, br);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBr);
        DeleteObject(br);
    }

    if (!empty(L.presetSave))   DrawButton(dc, L.presetSave,   L"保存", kHitPresetSave,   T, false);
    if (!empty(L.presetDelete)) DrawButton(dc, L.presetDelete, L"删除", kHitPresetDelete, T, false);
    if (!empty(L.presetImport)) DrawButton(dc, L.presetImport, L"导入", kHitPresetImport, T, false);
    if (!empty(L.presetExport)) DrawButton(dc, L.presetExport, L"导出", kHitPresetExport, T, false);
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

    if (hit == kHitFontBtn) {
        PickFont();   // 自己负责写设置 + NotifyChanged
        Repaint();
        return;
    }

    // ---- 外观预设（D-088）----
    // 这几条**立刻生效**，不走"应用 / 取消"：切一套配色就是要马上看到效果。
    // 和色块那种"先在界面上试、点应用才落地"不是一回事。
    if (hit == kHitPresetCombo)  { OnPresetCombo();         Repaint(); return; }
    if (hit == kHitPresetSave)   { OnPresetSaveCurrent();   Repaint(); return; }
    if (hit == kHitPresetDelete) { OnPresetDelete();        Repaint(); return; }
    if (hit == kHitPresetImport) { OnPresetImport();        Repaint(); return; }
    if (hit == kHitPresetExport) { OnPresetExport();        Repaint(); return; }

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

// ---------------------------------------------------------------------------
// 外观预设的动作（D-088）
// ---------------------------------------------------------------------------

AppearancePreset CLyricusPrefsDlg::SnapshotAppearance(const std::wstring& name) const {
    AppearancePreset p;
    p.name    = name;
    p.header  = m_edited.header;
    p.current = m_edited.current;
    p.normal  = m_edited.normal;
    p.dim     = m_edited.dim;
    p.warn    = m_edited.warn;
    p.bg      = m_edited.bg;
    p.alpha   = m_edited.alpha;
    p.fontFace = m_editedFontFace;
    // 这两项不在本页上，从配置读当前值
    p.fontPct      = GetLyricDisplayConfig().fontPct;
    p.backdropMode = static_cast<int>(cfg_backdrop_mode.get());
    return p;
}

void CLyricusPrefsDlg::OnPresetCombo() {
    const auto presets = GetAppearancePresets();

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    // id 从 1 起：`TrackPopupMenu` 返回 0 表示"用户点到外面关掉了"。
    const UINT kIdBase = 1;
    const UINT kIdNew  = kIdBase + static_cast<UINT>(presets.size());

    for (size_t i = 0; i < presets.size(); ++i) {
        UINT flags = MF_STRING;
        if (presets[i].name == m_presetName) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, kIdBase + static_cast<UINT>(i), presets[i].name.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdNew, L"另存为新预设");

    const PrefsLayout L = CurrentLayout();
    POINT pt{ L.presetCombo.left, L.presetCombo.bottom };

    // ★ 必须转到**屏幕坐标**再交给 TrackPopupMenu。
    //
    // 布局是按**客户区**算的（和绘制同一套坐标），而 TrackPopupMenu 收的是
    // 屏幕坐标 —— 直接把客户区坐标递过去，菜单会弹到屏幕的另一个位置去
    //（对话框离屏幕原点越远，偏得越离谱）。绘制那边不需要这一步，
    // 所以这个错很容易漏：**同一对坐标，两个 API 要的口径不一样**。
    // ⚠️ 这里必须写 `::` —— 和 MessageBoxW / SetWindowTextW 是同一个坑：
    //    CDialogImpl -> CWindow 有一大批和 Win32 API **同名**的成员函数，
    //    不加 `::` 时编译器优先选成员版。这次参数个数对不上所以报错了，
    //    但**参数个数恰好相同的那些会静默选错**，那才危险。
    ::ClientToScreen(m_hWnd, &pt);

    const UINT cmd = TrackPopupMenu(menu,
                                    TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                    pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 0) return;      // 点到外面

    if (cmd == kIdNew) {
        // 自动起一个不重名的名字。
        //
        // 【为什么不弹输入框】Win32 没有现成的 InputBox，为它动态造一个对话框
        // 的代价远大于收益。名字本来就能靠"导出 → 改 → 导入"来定，
        // 而那条路还顺带支持了分享 —— 所以这里选自动命名。
        for (int n = 2; n < 1000; ++n) {
            wchar_t buf[64];
            swprintf_s(buf, L"预设 %d", n);
            if (FindPreset(presets, buf) == nullptr) { m_presetName = buf; break; }
        }
        SaveAppearancePreset(m_presetName, SnapshotAppearance(m_presetName));
        return;
    }

    const size_t idx = static_cast<size_t>(cmd - kIdBase);
    if (idx >= presets.size()) return;

    m_presetName = presets[idx].name;
    ApplyAppearancePreset(presets[idx]);

    // ★ 应用之后要让**界面上"正在编辑"的那份**跟上，否则色块还画着旧颜色、
    //   而面板已经变了 —— 看起来像"点了没生效"。
    m_edited          = GetPanelAppearance();
    m_applied         = m_edited;
    m_editedFontFace  = GetLyricDisplayConfig().fontFace;
    m_appliedFontFace = m_editedFontFace;
    NotifyChanged();
}

void CLyricusPrefsDlg::OnPresetSaveCurrent() {
    if (m_presetName.empty()) {
        ::MessageBoxW(m_hWnd,
            L"先在左边的下拉里选一套预设。\n\n"
            L"「保存」是**覆盖**选中的那一套；要新建请用下拉菜单里的"
            L"「另存为新预设」。",
            L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }
    SaveAppearancePreset(m_presetName, SnapshotAppearance(m_presetName));
}

void CLyricusPrefsDlg::OnPresetDelete() {
    if (m_presetName.empty()) {
        ::MessageBoxW(m_hWnd, L"先在左边的下拉里选一套预设。",
                    L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool builtin = IsBuiltinPresetName(m_presetName);

    // ★ 删不掉就**如实说明**，不要假装成功。
    //   内置那几套没有落盘，所以对它们必然删不动 —— 但提示里要讲清
    //   "什么情况下才有东西可删"，否则用户会以为这个按钮坏了。
    if (!DeleteAppearancePreset(m_presetName)) {
        wchar_t buf[512];
        swprintf_s(buf,
            L"「%s」是内置预设，而且你没有覆盖过它，所以没有东西可删。\n\n"
            L"如果你改过它并点过「保存」，那时才有一份属于你的副本 —— "
            L"删掉那份就会回到内置的样子。",
            m_presetName.c_str());
        ::MessageBoxW(m_hWnd, buf, L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t buf[256];
    swprintf_s(buf, builtin
        ? L"已删掉你对「%s」的改动，恢复成内置的那一份。"
        : L"已删除预设「%s」。", m_presetName.c_str());
    DebugLog("外观预设：%s", WideToUtf8(buf).c_str());
}

void CLyricusPrefsDlg::OnPresetImport() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hWnd;
    ofn.lpstrFilter = L"Lyricus 预设 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"导入外观预设";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;      // 用户取消

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::MessageBoxW(m_hWnd, L"打不开这个文件。", L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string text;
    char buf[4096];
    DWORD got = 0;
    // 预设就是一行，但别人可能存成了带说明的文本 —— 读到一个上限就够，
    // 避免有人误选了一个几百 MB 的文件把内存吃光。
    constexpr DWORD kMaxRead = 64 * 1024;
    while (text.size() < kMaxRead && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        text.append(buf, got);
    }
    CloseHandle(h);

    AppearancePreset p;
    if (!ImportPreset(text, p)) {
        ::MessageBoxW(m_hWnd,
            L"这个文件里没有可识别的预设。\n\n"
            L"预设文件应该长这样（名字、TAB、然后一串 key=value）：\n"
            L"我的配色\tbg=1C1C1E;current=FFFFFF;alpha=215;fontPct=100;backdrop=4",
            L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }

    // ★ 同名的话先问一声。
    //
    // 【为什么这道确认非有不可】导入是**分享**那条路的主场景 —— 别人发来
    // 一个叫「暗色」的预设，而用户自己多半也有一套叫「暗色」。直接存下去会
    // 静默覆盖他那份，而且**没有任何办法察觉**（下拉里还是同一个名字，
    // 颜色变了也容易以为是自己记错了）。
    // ⚠️ 默认按钮是「否」：这个对话框是**打断**用户的，误按回车不该毁掉东西。
    if (FindPreset(GetAppearancePresets(), p.name) != nullptr) {
        wchar_t ask[512];
        swprintf_s(ask,
            L"已经有一套叫「%s」的预设了。\n\n"
            L"覆盖它？\n\n"
            L"（选「否」会取消这次导入 —— 你可以先把文件里的名字改掉再来。）",
            p.name.c_str());
        if (::MessageBoxW(m_hWnd, ask, L"Lyricus",
                          MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    // 存下来并选中，但**不自动应用** —— 用户可能只是想收着，
    // 不想现在的画面被换掉。
    m_presetName = p.name;
    SaveAppearancePreset(p.name, p);

    wchar_t msg[256];
    swprintf_s(msg, L"已导入「%s」。\n\n它现在是选中状态，但**没有**自动应用 ——"
                    L"想用就再点一次下拉里的它。", p.name.c_str());
    ::MessageBoxW(m_hWnd, msg, L"Lyricus", MB_OK | MB_ICONINFORMATION);
}

void CLyricusPrefsDlg::OnPresetExport() {
    if (m_presetName.empty()) {
        ::MessageBoxW(m_hWnd, L"先在左边的下拉里选一套预设。",
                    L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto presets = GetAppearancePresets();
    const AppearancePreset* p = FindPreset(presets, m_presetName);
    if (p == nullptr) return;

    wchar_t path[MAX_PATH] = L"";
    // 用预设名做默认文件名，导出多个时不用自己想名字。
    // ⚠️ 名字里的 \ / : 等字符不能进文件名，先换掉。
    std::wstring safe = m_presetName;
    for (wchar_t& c : safe) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?'  || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            c = L'_';
        }
    }
    wcsncpy_s(path, safe.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hWnd;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"导出外观预设";
    ofn.lpstrDefExt = L"txt";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    const std::string out = ExportPreset(*p);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::MessageBoxW(m_hWnd, L"写不了这个文件（权限或路径问题）。",
                    L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(h);

    if (!ok || written != out.size()) {
        ::MessageBoxW(m_hWnd, L"写入没有完成。", L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }
    DebugLog("外观预设：已导出「%s」到 %s",
             WideToUtf8(m_presetName).c_str(), WideToUtf8(path).c_str());
}

void CLyricusPrefsDlg::SetAlphaFromSliderX(int x) {    const PrefsLayout L = CurrentLayout();
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
    // ★ 字体也要算"改了" —— 它和颜色一样等"应用"才落盘，
    //   不带它的话用户选完字体，"应用"按钮还是灰的。
    if (m_edited != m_applied || m_editedFontFace != m_appliedFontFace)
        state |= preferences_state::changed;
    return state;
}

void CLyricusPrefsDlg::apply() {
    SetPanelAppearance(m_edited);
    m_applied = m_edited;

    // ★ 字体也在这里才落盘（和颜色一致）。
    //   空串是**合法值**（= 跟随宿主），不是"清空错误"。
    SetLyricFontFace(m_editedFontFace);
    m_appliedFontFace = m_editedFontFace;

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

    // ★ 字体回到"跟随宿主"，但同样**只改界面**。
    //
    // 【这里改过一次】从前这一句是直接 `SetLyricFontFace("")`（立刻写存储），
    // 理由是"字体本来就是立即生效的"。但那造出一个当时没想到的坑：
    // **点「恢复默认」再点「取消」= 颜色回来了、字体回不来** ——
    // 因为颜色在 m_edited 里等着应用，而字体已经落盘了。
    // 实测用户就是这么丢掉「方正姚体」的（配置里 lyricus.fontFace 还在，值是空的）。
    // 现在两边同一条路：改内存 -> 点应用才落盘 -> 取消就整页还原。
    m_editedFontFace.clear();

    NotifyChanged();
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

void CLyricusPrefsDlg::PickFont() {
    // 和 PickColor 一样：模态期间页面可能被关掉、对象被释放
    //（SDK 在 preferences_page.h:133 警告过这件事）。
    service_ptr_t<preferences_page_instance> self = this;

    const LyricDisplayConfig cfg = GetLyricDisplayConfig();

    // 对话框的初始值。用 m_editedFontFace 而不是 cfg —— 后者是**已落盘**的值，
    // 若用户上次选了字体没点应用又来打开，起点该是他看到的那个。
    LOGFONTW lf{};
    if (!m_editedFontFace.empty()) {
        // 用户设过 —— 以它为起点，打开就是当前值
        const std::wstring w = Utf8ToWide(m_editedFontFace.c_str());
        wcsncpy_s(lf.lfFaceName, w.c_str(), _TRUNCATE);
        lf.lfHeight = -MulDiv(12, 96, 72);
    } else {
        // 没设过 —— 拿**系统界面字体**当起点。用户多半只是想微调一下
        // 现在看到的那个字体，从这个起点改最省事。
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            lf = ncm.lfMessageFont;
        } else {
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            lf.lfHeight = -MulDiv(12, 96, 72);
        }
    }

    // ⚠️ 刻意**不用** WTL 的 CFontDialog —— 和 CColorDialog 是同一个坑：
    //    两者都走 CStaticDataInitCriticalSectionLock，而那个锁的构造函数直接
    //    解引用 ATL::_pAtlModule。本组件从没创建过 ATL 模块对象，那指针是空的，
    //    于是 RtlEnterCriticalSection 收到 0x18 —— 正是 2026-09-26 那次崩溃
    //    （failure_00000001.txt，见 D-070）。ChooseFontW 是纯 Win32，
    //    不经过任何 ATL 全局状态，整条路直接没了。
    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = m_hWnd;
    cf.lpLogFont   = &lf;
    cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;

    if (!::ChooseFontW(&cf)) return;   // 用户取消
    if (!::IsWindow(m_hWnd)) return;   // 模态期间页面被关了

    // ★ 只取**字体族**，忽略对话框里挑的字号和粗体。
    //
    // 字号归「字号百分比」那条滑块管，是另一个维度。混在一起的话，
    // 用户挑一次字体就会把辛苦调好的字号一并覆盖掉 —— 而且他多半
    // 根本没注意到自己在字体对话框里也动了字号。
    // ★ 只改**正在编辑**的值，不写存储 —— 和颜色一样，等用户点"应用"。
    //   这样"选了字体又点取消"能真正撤销（从前的立即写盘会把它留下）。
    m_editedFontFace = WideToUtf8(lf.lfFaceName);
    NotifyChanged();

    DebugLog("首选项页：字体 -> 「%s」（待应用）", m_editedFontFace.c_str());
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
