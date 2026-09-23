#include "stdafx.h"

#include "resource.h"
#include "config.h"
#include "control_window.h"
#include "debug_log.h"

#include <SDK/preferences_page.h>
#include <helpers/atl-misc.h>   // preferences_page_impl
#include <atldlgs.h>            // CColorDialog（在 atldlgs.h 里，不在 atlctrlx.h）

// ---------------------------------------------------------------------------
// Lyricus 首选项页 —— 浮动面板的配色与不透明度
//
// 【为什么这一页只管浮动面板】
// DUI 元素和 CUI 面板跟随宿主主题（用户在 foobar2000 / Columns UI 里改配色，
// 面板就跟着变），那是嵌入面板该有的行为，不该另设一套。
// 只有浮动面板没有宿主，所以给它自己的配置。
//
// 【为什么需要 .rc】
// preferences_page_impl<TDialog> 要求 TDialog 能 Create(parent)，
// 也就是必须基于对话框资源（helpers/atl-misc.h:271-280）。
// 本工程原本没有 .rc，这是第一份。
//
// 【TDialog 的契约】由 preferences_page_instance_impl 反推出来三条：
//   1. 构造函数收 preferences_page_callback::ptr
//   2. 有 IDD 和 Create(parent)（CDialogImpl 提供）
//   3. 继承 preferences_page_instance 并实现 get_state / apply / reset
//      —— 因为 preferences_page_instance_impl<TDialog> **只**继承 TDialog，
//         它得从 TDialog 那里拿到 preferences_page_instance 这个基类。
//
// 【消息处理函数的签名不能凭印象写】
// WTL 的宏会按固定参数表调用处理函数（atlcrack.h 里逐个核过）：
//   MSG_WM_INITDIALOG  -> func((HWND)wParam, lParam)
//   MSG_WM_DRAWITEM    -> func((UINT)wParam, (LPDRAWITEMSTRUCT)lParam)
//   MSG_WM_HSCROLL     -> func((int)LOWORD(wParam), (short)HIWORD(wParam), (HWND)lParam)
//   COMMAND_*_EX       -> func((UINT)HIWORD(wParam), (int)LOWORD(wParam), (HWND)lParam)
// 第三参一律是裸 HWND，不是 CWindow；写成 CWindow 会报"函数不接受 N 个参数"。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

// GUID 段 0x37：首选项页。分配前已 grep 全工程（见 D-022）。
const GUID guid_prefs_page = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x37}};

// 颜色项的绑定表。
//
// 把「控件 ID / 结构体成员 / 显示名」绑在一起，避免 6 个颜色各写一遍
// 几乎相同的代码 —— 那种写法「改一处漏五处」是经典事故。
struct ColorSlot {
    UINT        ctrlId;
    COLORREF    PanelAppearance::*member;
    const char* label;
};

const ColorSlot kColorSlots[] = {
    { IDC_BTN_HEADER,  &PanelAppearance::header,  "曲名"       },
    { IDC_BTN_CURRENT, &PanelAppearance::current, "当前歌词行" },
    { IDC_BTN_NORMAL,  &PanelAppearance::normal,  "其它歌词行" },
    { IDC_BTN_DIM,     &PanelAppearance::dim,     "次要文字"   },
    { IDC_BTN_WARN,    &PanelAppearance::warn,    "警告文字"   },
    { IDC_BTN_BG,      &PanelAppearance::bg,      "面板底色"   },
};

// 感知亮度：决定色块上的文字用黑还是白。
// 用加权而不是简单平均 —— 纯蓝和纯黄的"平均"一样，人眼看上去差得远。
int Luminance(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

class CLyricusPrefsDlg : public CDialogImpl<CLyricusPrefsDlg>,
                         public preferences_page_instance {
public:
    enum { IDD = IDD_LYRICUS_PREFS };

    explicit CLyricusPrefsDlg(preferences_page_callback::ptr callback)
        : m_callback(callback),
          m_edited(GetPanelAppearance()),     // 界面上正在编辑的值
          m_applied(GetPanelAppearance()) {}  // 上次"应用"下去的值

    BEGIN_MSG_MAP(CLyricusPrefsDlg)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_DRAWITEM(OnDrawItem)
        MSG_WM_HSCROLL(OnHScroll)
        COMMAND_ID_HANDLER_EX(IDC_BTN_RESET, OnResetClicked)
        COMMAND_RANGE_HANDLER_EX(IDC_BTN_HEADER, IDC_BTN_BG, OnColorClicked)
    END_MSG_MAP()

    // ---- preferences_page_instance 的契约 ----
    t_uint32 get_state() override;
    void     apply()     override;
    void     reset()     override;

private:
    BOOL OnInitDialog(HWND hwndFocus, LPARAM lParam);
    void OnDrawItem(UINT ctrlId, LPDRAWITEMSTRUCT dis);
    void OnHScroll(int sbCode, short pos, HWND hwndCtl);
    void OnColorClicked(UINT notify, int ctrlId, HWND ctl);
    void OnResetClicked(UINT notify, int ctrlId, HWND ctl);

    COLORREF* ColorForControl(UINT ctrlId);
    void      SyncControls();
    void      NotifyChanged();

    // 弹一个模态取色器。
    //
    // ⚠️ 模态对话框会泵消息，期间**首选项窗口可能被关掉、页面随之被释放**。
    //    SDK 在 preferences_page.h:133 专门警告过这种情况。
    //    所以进来先拿一份自身引用把自己钉住，DoModal 返回后碰成员才安全；
    //    返回后还要再确认一次窗口还在。
    bool PickColor(COLORREF& inOut);

    preferences_page_callback::ptr m_callback;
    PanelAppearance m_edited;
    PanelAppearance m_applied;
};

// ---------------------------------------------------------------------------

COLORREF* CLyricusPrefsDlg::ColorForControl(UINT ctrlId) {
    for (const ColorSlot& s : kColorSlots) {
        if (s.ctrlId == ctrlId) return &(m_edited.*(s.member));
    }
    return nullptr;
}

BOOL CLyricusPrefsDlg::OnInitDialog(HWND, LPARAM) {
    const HWND slider = GetDlgItem(IDC_SLIDER_ALPHA);
    if (slider != nullptr) {
        // 必须用 ::SendMessage —— 不加 :: 会被 ATL 的 CWindow::SendMessageW 抢走，
        // 而它不接受 HWND 作第一个参数。
        ::SendMessage(slider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinAlpha, kMaxAlpha));
        ::SendMessage(slider, TBM_SETTICFREQ, 26, 0);
    }
    SyncControls();
    DebugLog("首选项页：已初始化（alpha=%d）", m_edited.alpha);
    return TRUE;
}

void CLyricusPrefsDlg::SyncControls() {
    for (const ColorSlot& s : kColorSlots) {
        // 色块内容由 WM_DRAWITEM 画，这里只要让它重画。
        // 必须加 :: —— 不然会被 ATL 的 CWindow::InvalidateRect 抢走，
        // 而那个成员只接受 (LPRECT, BOOL) 两个参数。
        ::InvalidateRect(GetDlgItem(s.ctrlId), nullptr, TRUE);
    }

    const HWND slider = GetDlgItem(IDC_SLIDER_ALPHA);
    if (slider != nullptr) ::SendMessage(slider, TBM_SETPOS, TRUE, m_edited.alpha);

    wchar_t buf[32];
    swprintf_s(buf, L"%d / 255", m_edited.alpha);
    SetDlgItemTextW(IDC_LBL_ALPHA, buf);
}

void CLyricusPrefsDlg::OnDrawItem(UINT /*ctrlId*/, LPDRAWITEMSTRUCT dis) {
    if (dis == nullptr) return;

    const COLORREF* p = ColorForControl(dis->CtlID);
    if (p == nullptr) return;

    const COLORREF c = *p;

    // 色块本体
    HBRUSH br = CreateSolidBrush(c);
    if (br != nullptr) { FillRect(dis->hDC, &dis->rcItem, br); DeleteObject(br); }

    // 边框：色块可能和对话框底色撞色，加一圈灰边保证边界可见
    FrameRect(dis->hDC, &dis->rcItem, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    // 十六进制值。前景色按感知亮度选，保证浅色和深色底上都读得清。
    wchar_t text[16];
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    SetTextColor(dis->hDC, Luminance(c) > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    SetBkMode(dis->hDC, TRANSPARENT);

    RECT rc = dis->rcItem;
    DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

bool CLyricusPrefsDlg::PickColor(COLORREF& inOut) {
    // 见声明处的说明：先把自己钉住，防 DoModal 期间被释放
    service_ptr_t<preferences_page_instance> self = this;

    CColorDialog dlg(inOut, CC_FULLOPEN | CC_ANYCOLOR, m_hWnd);
    if (dlg.DoModal(m_hWnd) != IDOK) return false;

    // 模态期间页面可能已经被销毁（窗口没了）。self 保证对象还在，
    // 但窗口没了就不该再碰控件。
    // ::IsWindow —— 同理，不加 :: 会被 CWindow::IsWindow（无参）抢走。
    if (!::IsWindow(m_hWnd)) return false;

    inOut = dlg.GetColor();
    return true;
}

void CLyricusPrefsDlg::OnColorClicked(UINT, int ctrlId, HWND) {
    COLORREF* p = ColorForControl(ctrlId);
    if (p == nullptr) return;

    if (PickColor(*p)) {
        SyncControls();
        NotifyChanged();
    }
}

void CLyricusPrefsDlg::OnHScroll(int sbCode, short, HWND) {
    // 只关心拖动/点击产生的位移
    if (sbCode != SB_THUMBPOSITION && sbCode != SB_THUMBTRACK &&
        sbCode != SB_LINELEFT && sbCode != SB_LINERIGHT &&
        sbCode != SB_PAGELEFT && sbCode != SB_PAGERIGHT) {
        return;
    }

    const HWND slider = GetDlgItem(IDC_SLIDER_ALPHA);
    if (slider == nullptr) return;

    const int v = static_cast<int>(::SendMessage(slider, TBM_GETPOS, 0, 0));
    if (v == m_edited.alpha) return;

    m_edited.alpha = ClampAlpha(v);
    SyncControls();
    NotifyChanged();
}

void CLyricusPrefsDlg::OnResetClicked(UINT, int, HWND) {
    // reset 只改界面，**不写存储** —— SDK 明确要求这样，
    // 好让用户先看到效果再决定要不要"应用"（preferences_page.h:144）。
    m_edited = PanelAppearance{};
    SyncControls();
    NotifyChanged();
    DebugLog("首选项页：已恢复界面上的默认值（尚未应用）");
}

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
    m_edited = PanelAppearance{};
    SyncControls();
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
