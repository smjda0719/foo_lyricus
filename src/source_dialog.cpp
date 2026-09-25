#include "stdafx.h"

#include "resource.h"
#include "config.h"
#include "source_order.h"
#include "lyric.h"      // Utf8ToWide / WideToUtf8
#include "debug_log.h"

#include <string>

// ---------------------------------------------------------------------------
// 「歌词源顺序」面板
//
// 【为什么要它】用户 2026-09-25：先说「暂时把网易云源短接掉」，紧接着自己给了
// 更好的办法 —— 「可以给用户自定义查找歌词顺序的面板」。
// 起因是查证"酷狗到底能不能干活"时发现：网易云排第一位、命中就收工，
// 于是备用源在正常使用中**几乎永远跑不到**（实测连放十几首，酷狗一次都没轮到）。
//
// 【为什么界面这么朴素】只有三行、要表达的只有"顺序"和"启用"两件事。
// 普通 LISTBOX 加几个按钮就够了；owner-draw 要多写 WM_DRAWITEM 和一套配色，
// 换来的只是几个像素的对齐。
//
// 【不需要实时生效】这一项只影响**以后**的查询，不改变当前正在显示的歌词，
// 所以"确定才落盘"就够了，不必像调节面板那样拖动即生效。
//
// 【消息处理签名】和 adjust_dialog.cpp 同一套（在 atlcrack.h 里核过）：
//   MSG_WM_INITDIALOG     -> func((HWND)wParam, lParam)
//   COMMAND_HANDLER_EX(id, code, func) -> func((UINT)wNotifyCode, (int)wID, (HWND)hwndCtl)
//                          （**不要用 MSG_WM_COMMAND**，见消息映射里那段）
//   第三参一律是**裸 HWND**，写成 CWindow 会报"函数不接受 N 个参数"。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

class CSourceOrderDialog : public CDialogImpl<CSourceOrderDialog> {
public:
    enum { IDD = IDD_LYRICUS_SOURCES };

    BEGIN_MSG_MAP(CSourceOrderDialog)
        MSG_WM_INITDIALOG(OnInitDialog)

        // ⚠️⚠️ **不要在这里用 MSG_WM_COMMAND**（我第一版就是那么写的，翻过车）。
        //
        // MSG_WM_COMMAND 是 WM_COMMAND 的**裸处理器**，WTL 把它展开成
        //     if (uMsg == WM_COMMAND) { func(...); bHandled = TRUE; }
        // —— 无条件 `bHandled = TRUE`。而消息映射是**按顺序**匹配的，
        // 它排在下面那些 COMMAND_ID_HANDLER_EX 前面，于是把**所有**命令
        // （每个按钮、确定、取消）统统吃掉。
        //
        // 症状极具误导性：对话框能正常显示、列表也填好了，但**点什么都没反应、
        // 连取消都关不掉** —— 看起来像"窗口卡死"，其实是消息根本没分发下去。
        // 用户 2026-09-26 报的就是这个：「无法改变顺序，也无法退出」。
        //
        // 只想接"双击列表项"的话，用下面这种带 id + 通知码的精确处理器。
        COMMAND_HANDLER_EX(IDC_LIST_SOURCES, LBN_DBLCLK, OnListDblClk)

        COMMAND_ID_HANDLER_EX(IDC_BTN_SRC_UP, OnUp)
        COMMAND_ID_HANDLER_EX(IDC_BTN_SRC_DOWN, OnDown)
        COMMAND_ID_HANDLER_EX(IDC_BTN_SRC_TOGGLE, OnToggle)
        COMMAND_ID_HANDLER_EX(IDC_BTN_SRC_RESET, OnReset)
        COMMAND_ID_HANDLER_EX(IDOK, OnOk)
        COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
    END_MSG_MAP()

    bool m_committed = false;

private:
    // 打开时读配置；空串 = 出厂顺序且全部启用（见 source_order.h）
    std::vector<SourcePref> m_prefs;

    BOOL OnInitDialog(CWindow /*wndFocus*/, LPARAM /*lInitParam*/) {
        m_prefs = ParseSourcePrefs(Utf8ToWide(cfg_lyric_source_order.get().get_ptr()));
        Refill(0);
        GetDlgItem(IDC_LIST_SOURCES).SetFocus();
        return FALSE;   // 自己设了焦点
    }

    CListBox List() { return GetDlgItem(IDC_LIST_SOURCES); }

    // 重建列表并把选中项放回 index（越界则夹到范围内）。
    //
    // 【为什么每次操作都重建】只有三项，重建的代价可以忽略；
    // 而"改一项再手工同步列表和模型"正是最容易让两者对不上的写法。
    void Refill(int selectIndex) {
        // ⚠️ 不能用 `const CListBox` —— ResetContent/AddString/SetCurSel 都是
        //    非 const 成员，编译器会报"不能将 this 从 const 转换"。踩过一次。
        CListBox lb = List();
        lb.ResetContent();

        for (const SourcePref& p : m_prefs) {
            std::wstring line;
            line += p.enabled ? L"✔ " : L"✘ ";
            line += SourceDisplayName(p.src);
            if (!p.enabled) line += L"（已停用）";
            lb.AddString(line.c_str());
        }

        const int n = static_cast<int>(m_prefs.size());
        if (n > 0) {
            if (selectIndex < 0) selectIndex = 0;
            if (selectIndex >= n) selectIndex = n - 1;
            lb.SetCurSel(selectIndex);
        }
        UpdateHint();
    }

    int Selected() const {
        return static_cast<int>(
            ::SendMessage(GetDlgItem(IDC_LIST_SOURCES), LB_GETCURSEL, 0, 0));
    }

    void UpdateHint() {
        std::wstring s;
        const std::vector<LyricSource> tryList = EnabledSources(m_prefs);
        if (tryList.empty()) {
            s = L"当前一个源都没启用 —— 联网查询会被完全跳过（本地歌词不受影响）。";
        } else {
            s = L"实际尝试顺序：";
            for (size_t i = 0; i < tryList.size(); ++i) {
                if (i) s += L" → ";
                s += SourceDisplayName(tryList[i]);
            }
        }
        SetDlgItemText(IDC_LBL_SRC_HINT, s.c_str());
    }

    // 列表里双击 = 切换启用状态（顺手，不用去够按钮）。
    // 用 COMMAND_HANDLER_EX 精确匹配 id + 通知码 —— 不要退回 MSG_WM_COMMAND，
    // 原因见消息映射里那段警告。
    void OnListDblClk(UINT /*notify*/, int /*id*/, HWND /*hwndCtl*/) {
        OnToggle(0, 0, nullptr);
    }

    void OnUp(UINT, int, HWND) {
        const int sel = Selected();
        if (sel < 0) return;
        m_prefs = MoveSourcePref(m_prefs, static_cast<size_t>(sel), -1);
        // ⚠️ 选中项要跟着走：`MoveSourcePref` 换了位置，但"哪个源被选中"没变。
        //    不跟的话用户会觉得"点了一下，选中的变成别人了"。
        Refill(sel - 1);
    }

    void OnDown(UINT, int, HWND) {
        const int sel = Selected();
        if (sel < 0) return;
        m_prefs = MoveSourcePref(m_prefs, static_cast<size_t>(sel), +1);
        Refill(sel + 1);
    }

    void OnToggle(UINT, int, HWND) {
        const int sel = Selected();
        if (sel < 0 || sel >= static_cast<int>(m_prefs.size())) return;
        m_prefs[static_cast<size_t>(sel)].enabled = !m_prefs[static_cast<size_t>(sel)].enabled;
        Refill(sel);
    }

    void OnReset(UINT, int, HWND) {
        m_prefs = ParseSourcePrefs(std::wstring());   // 空串 = 出厂顺序、全部启用
        Refill(0);
    }

    void OnOk(UINT, int, HWND) {
        const std::wstring text = FormatSourcePrefs(m_prefs);
        cfg_lyric_source_order = WideToUtf8(text).c_str();

        const std::vector<LyricSource> tryList = EnabledSources(m_prefs);
        std::wstring names;
        for (size_t i = 0; i < tryList.size(); ++i) {
            if (i) names += L" -> ";
            names += SourceDisplayName(tryList[i]);
        }
        DebugLog("歌词源顺序：已保存（配置=%s，实际顺序=%s）",
                 WideToUtf8(text).c_str(),
                 names.empty() ? "(一个都没启用)" : WideToUtf8(names).c_str());

        m_committed = true;
        EndDialog(IDOK);
    }

    void OnCancel(UINT, int, HWND) {
        EndDialog(IDCANCEL);   // 什么都没改（落盘只在 OnOk 里做）
    }
};

} // namespace

namespace lyricus {

bool PromptSourceOrder(HWND parent) {
    CSourceOrderDialog dlg;
    const INT_PTR r = dlg.DoModal(parent);
    return r == IDOK && dlg.m_committed;
}

} // namespace lyricus
