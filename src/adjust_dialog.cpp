#include "stdafx.h"

#include "resource.h"
#include "adjust_dialog.h"
#include "config.h"
#include "playback_state.h"
#include "debug_log.h"

#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// 「调节面板」—— 五条滑动条，拖动时实时生效
//
// 【为什么要合在一个面板里】用户 2026-09-25 连着两次提这个方向：
//   「改成打开一个面板，通过滑动条调整。这么点太麻烦了。」（说的是歌词偏移）
//   「也把字号，行数，行位置，透明度也这么改。透明度我记得有另外一个面板，
//     但是太深了不好找。」
// 这五个量的共同点是**要一边看面板一边调**，而"打开首选项 → 找到项 → 改数字
// → 关掉 → 看效果 → 再打开"这个循环根本没法用。
// 分开做四个面板只会更难找，所以合成一个。
//
// 【交互上的三个决定】
//   1. **拖动时实时生效**：这几个量都是"看着调"的，不能等点了确定才生效。
//   2. **取消真的还原**：实时生效和"可后悔"必须同时成立 —— 在 OnInitDialog
//      里把五个原值都记下来，取消时逐个还回去（内存和存储都不留痕）。
//   3. **确定才落盘**：拖动一次会发几十上百条 WM_HSCROLL，
//      每条都写配置是白白磨损配置文件。
//
// 【消息处理签名】和 hint_dialog.cpp 同一套（在 atlcrack.h 里核过）：
//   MSG_WM_INITDIALOG -> func((HWND)wParam, lParam)
//   MSG_WM_HSCROLL    -> func((UINT)nBar, (int)nPos, (HWND)hwndCtl)
//   第三参一律是**裸 HWND**，写成 CWindow 会报"函数不接受 N 个参数"。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

// 歌词偏移：±10 秒、步进 100ms。
//
// 上限取 10 秒是因为实测的偏移是 7.0 秒（在线歌词来自另一个剪辑版本，D-048）；
// 再大的差值基本意味着"匹配到了别的歌"，不该用偏移硬凑 ——
// 那种情况该用「指定歌词搜索线索」。
constexpr int kOffsetMaxMs  = 10000;
constexpr int kOffsetStepMs = 100;

int SecToOffsetPos(double sec) {
    int ms = static_cast<int>(sec * 1000.0 + (sec >= 0 ? 0.5 : -0.5));
    if (ms >  kOffsetMaxMs) ms =  kOffsetMaxMs;
    if (ms < -kOffsetMaxMs) ms = -kOffsetMaxMs;
    return ms / kOffsetStepMs;
}

double OffsetPosToSec(int pos) {
    return static_cast<double>(pos * kOffsetStepMs) / 1000.0;
}

class CAdjustDialog : public CDialogImpl<CAdjustDialog> {
public:
    enum { IDD = IDD_LYRICUS_ADJUST };

    BEGIN_MSG_MAP(CAdjustDialog)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_HSCROLL(OnHScroll)
        COMMAND_ID_HANDLER_EX(IDC_BTN_OFFSET_ZERO, OnOffsetZero)
        COMMAND_ID_HANDLER_EX(IDC_BTN_DISPLAY_DEF, OnDisplayDefaults)
        COMMAND_ID_HANDLER_EX(IDOK, OnOk)
        COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
    END_MSG_MAP()

    bool m_committed = false;

private:
    BOOL OnInitDialog(CWindow /*wndFocus*/, LPARAM /*lInitParam*/) {
        auto& st = PlaybackState::Get();

        // ---- 记下原值：取消时要逐个还回去 ----
        m_startOffset = st.LyricOffsetSec();
        m_startCfg    = GetLyricDisplayConfig();
        m_startAlpha  = GetPanelAppearance().alpha;

        // ---- 曲目名（让用户确认没调错歌；没在播放就把偏移那条禁用）----
        const bool hasTrack = st.HasTrack();
        SetDlgItemText(IDC_LBL_TRACK,
                       hasTrack ? st.DisplayName().c_str() : L"（没有在播放）");
        if (!hasTrack) GetDlgItem(IDC_SLIDER_OFFSET).EnableWindow(FALSE);

        // ---- 滑动条范围 ----
        SetupSlider(IDC_SLIDER_OFFSET, -kOffsetMaxMs / kOffsetStepMs,
                                       kOffsetMaxMs / kOffsetStepMs,
                                       /*page=*/5, /*tickFreq=*/10);
        SetupSlider(IDC_SLIDER_FONT,  50, 300, /*page=*/10, /*tickFreq=*/50);
        SetupSlider(IDC_SLIDER_SPAN,   0,  30, /*page=*/1,  /*tickFreq=*/5);
        SetupSlider(IDC_SLIDER_RATIO,  0, 100, /*page=*/5,  /*tickFreq=*/10);
        SetupSlider(IDC_SLIDER_ADJ_ALPHA, kMinAlpha, kMaxAlpha, /*page=*/10, /*tickFreq=*/30);

        SetSliderPos(IDC_SLIDER_OFFSET, SecToOffsetPos(m_startOffset));
        SetSliderPos(IDC_SLIDER_FONT,   m_startCfg.fontPct);
        SetSliderPos(IDC_SLIDER_SPAN,   m_startCfg.span);
        SetSliderPos(IDC_SLIDER_RATIO,  m_startCfg.currentRatio);
        SetSliderPos(IDC_SLIDER_ADJ_ALPHA,  m_startAlpha);

        RefreshLabels();
        GetDlgItem(IDC_SLIDER_OFFSET).SetFocus();
        return FALSE;   // 自己设了焦点
    }

    void SetupSlider(UINT id, int lo, int hi, int page, int tickFreq) {
        const CWindow s = GetDlgItem(id);
        ::SendMessage(s, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        ::SendMessage(s, TBM_SETPAGESIZE, 0, page);
        ::SendMessage(s, TBM_SETTICFREQ, tickFreq, 0);
    }

    void SetSliderPos(UINT id, int pos) {
        ::SendMessage(GetDlgItem(id), TBM_SETPOS, TRUE, pos);
    }

    int SliderPos(UINT id) const {
        return static_cast<int>(::SendMessage(GetDlgItem(id), TBM_GETPOS, 0, 0));
    }

    // ---- 实时生效：这里**不落盘** ----
    //
    // 拖一次会发几十上百条 WM_HSCROLL，每条都写配置是白白磨损配置文件。
    // 落盘统一交给 OnOk。
    void OnHScroll(UINT /*nBar*/, int /*nPos*/, HWND hwndCtl) {
        const HWND h = hwndCtl != nullptr ? hwndCtl : nullptr;
        if (h == GetDlgItem(IDC_SLIDER_OFFSET)) {
            PlaybackState::Get().SetLyricOffsetLive(OffsetPosToSec(SliderPos(IDC_SLIDER_OFFSET)));
        } else if (h == GetDlgItem(IDC_SLIDER_FONT)) {
            SetLyricFontPct(SliderPos(IDC_SLIDER_FONT));
        } else if (h == GetDlgItem(IDC_SLIDER_SPAN)) {
            SetLyricSpan(SliderPos(IDC_SLIDER_SPAN));
        } else if (h == GetDlgItem(IDC_SLIDER_RATIO)) {
            SetLyricCurrentRatio(SliderPos(IDC_SLIDER_RATIO));
        } else if (h == GetDlgItem(IDC_SLIDER_ADJ_ALPHA)) {
            ApplyAlpha(SliderPos(IDC_SLIDER_ADJ_ALPHA));
        }
        RefreshLabels();
    }

    // 面板不透明度。
    //
    // ⚠️ 必须顺手把背景模式切成"半透明"：alpha 只在那一档有意义，
    //    Mica / Acrylic 下拖它一点反应都没有（菜单里那条切换命令也是这么做的）。
    void ApplyAlpha(int alpha) {
        PanelAppearance ap = GetPanelAppearance();
        ap.alpha = ClampAlpha(alpha);
        SetPanelAppearance(ap);
        cfg_backdrop_mode = static_cast<int>(BackdropMode::Translucent);
    }

    void RefreshLabels() {
        wchar_t buf[80];

        const double off = OffsetPosToSec(SliderPos(IDC_SLIDER_OFFSET));
        swprintf_s(buf, L"%+.1f 秒", off);
        SetDlgItemText(IDC_LBL_V_OFFSET, buf);

        swprintf_s(buf, L"%d%%", SliderPos(IDC_SLIDER_FONT));
        SetDlgItemText(IDC_LBL_V_FONT, buf);

        const int span = SliderPos(IDC_SLIDER_SPAN);
        if (span > 0) swprintf_s(buf, L"%d 行", span);
        else          swprintf_s(buf, L"自适应");
        SetDlgItemText(IDC_LBL_V_SPAN, buf);

        const int ratio = SliderPos(IDC_SLIDER_RATIO);
        if (ratio == 50) swprintf_s(buf, L"%d%% 正中", ratio);
        else             swprintf_s(buf, L"%d%%", ratio);
        SetDlgItemText(IDC_LBL_V_RATIO, buf);

        const int alpha = SliderPos(IDC_SLIDER_ADJ_ALPHA);
        swprintf_s(buf, L"%d / 255", alpha);
        SetDlgItemText(IDC_LBL_V_ALPHA, buf);
    }

    void OnOffsetZero(UINT /*uNotifyCode*/, int /*nID*/, HWND /*wndCtl*/) {
        SetSliderPos(IDC_SLIDER_OFFSET, 0);
        PlaybackState::Get().SetLyricOffsetLive(0.0);
        RefreshLabels();
    }

    void OnDisplayDefaults(UINT /*uNotifyCode*/, int /*nID*/, HWND /*wndCtl*/) {
        // 和高级首选项里那几项的初值保持一致（字号 100 / 自适应 / 50%）。
        // 不透明度用 PanelAppearance 的默认值，不是当前值 ——
        // "恢复默认"要真的回到默认，而不是回到"我改之前"。
        SetSliderPos(IDC_SLIDER_FONT,  100);
        SetSliderPos(IDC_SLIDER_SPAN,    0);
        SetSliderPos(IDC_SLIDER_RATIO,  50);
        SetSliderPos(IDC_SLIDER_ADJ_ALPHA,  PanelAppearance{}.alpha);

        SetLyricFontPct(100);
        SetLyricSpan(0);
        SetLyricCurrentRatio(50);
        ApplyAlpha(PanelAppearance{}.alpha);
        RefreshLabels();
    }

    void OnOk(UINT /*uNotifyCode*/, int /*nID*/, HWND /*wndCtl*/) {
        // 到这一下才落盘。
        //
        // 显示设置那三项不用管：SetLyricFontPct/SetLyricSpan/SetLyricCurrentRatio
        // 写的就是 advconfig 本身，拖动时已经写进去了（它们没有"未提交"的概念）。
        // 真正需要这一步的是**歌词偏移**（拖动时只改了内存）和不透明度
        //（SetPanelAppearance 也是直接写存储）。
        PlaybackState::Get().CommitLyricOffset();

        const LyricDisplayConfig c = GetLyricDisplayConfig();
        DebugLog("调节面板：确定 字号=%d%% 行数=%d 位置=%d%% alpha=%d 偏移=%+.1fs",
                 c.fontPct, c.span, c.currentRatio,
                 GetPanelAppearance().alpha, PlaybackState::Get().LyricOffsetSec());

        m_committed = true;
        EndDialog(IDOK);
    }

    void OnCancel(UINT /*uNotifyCode*/, int /*nID*/, HWND /*wndCtl*/) {
        // 实时生效过，所以取消必须**真的还原**（内存里也还回去），
        // 否则用户点了取消却留下一堆改过的值。
        //
        // 显示设置那三项走 Set 函数写回原值；偏移和 alpha 同理。
        SetLyricFontPct(m_startCfg.fontPct);
        SetLyricSpan(m_startCfg.span);
        SetLyricCurrentRatio(m_startCfg.currentRatio);
        ApplyAlpha(m_startAlpha);
        PlaybackState::Get().SetLyricOffsetLive(m_startOffset);

        DebugLog("调节面板：取消，已还原（字号=%d%% 行数=%d 位置=%d%% alpha=%d 偏移=%+.1fs）",
                 m_startCfg.fontPct, m_startCfg.span, m_startCfg.currentRatio,
                 m_startAlpha, m_startOffset);
        EndDialog(IDCANCEL);
    }

    double             m_startOffset = 0.0;
    LyricDisplayConfig m_startCfg;
    int                m_startAlpha = 0;
};

} // namespace

namespace lyricus {

bool PromptAdjustPanel(HWND parent) {
    CAdjustDialog dlg;
    const INT_PTR r = dlg.DoModal(parent);
    return r == IDOK && dlg.m_committed;
}

} // namespace lyricus
