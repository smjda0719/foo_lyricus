#include "stdafx.h"

#include "resource.h"
#include "folder_hint.h"
#include "lyric_search.h"   // GuessAlbumHintFromPath
#include "debug_log.h"
#include "lyric.h"          // WideToUtf8

#include <string>

// ---------------------------------------------------------------------------
// 「为这个文件夹指定歌词线索」对话框
//
// 【为什么需要它】无标签的曲目里 %artist% 是占位符「?」、专辑是空的，
// 只剩曲名一个搜索词 —— 实测这会被彻底带偏：
//     查「哀歌」 -> 和田薫 / 平井堅 …（真答案 id=1333394828 连前 10 都没有）
//     查「阿良良木健 哀歌」 -> 第 1 条就是它            ← 歌手就是那把钥匙
// 而文件夹名不可靠（`奇爱人生·终焉版 哀歌` 反而返回《寻爱一生》这种垃圾），
// 所以必须有人工入口。
//
// 【为什么按文件夹而不是按曲目】失败模式是**整张专辑没标签** ——
// 一次指定能救十几首。
//
// 【消息处理签名不凭印象写】和 prefs_page.cpp 同一套（atlcrack.h 里核过）：
//   MSG_WM_INITDIALOG -> func((HWND)wParam, lParam)
//   COMMAND_*_EX      -> func((UINT)HIWORD(wParam), (int)LOWORD(wParam), (HWND)lParam)
// 第三参一律是**裸 HWND**；写成 CWindow 会报"函数不接受 N 个参数"。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

class CHintDialog : public CDialogImpl<CHintDialog> {
public:
    enum { IDD = IDD_LYRICUS_HINT };

    // 入参：文件夹键（FolderKeyOf 的结果）与显示用的文件夹名
    CHintDialog(const std::wstring& folderKey, const std::wstring& folderDisplay)
        : m_key(folderKey), m_display(folderDisplay) {}

    BEGIN_MSG_MAP(CHintDialog)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_ID_HANDLER_EX(IDOK, OnOk)
        COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow /*wndFocus*/, LPARAM /*lInitParam*/) {
        // 文件夹名显示出来 —— 用户得确认自己改的是哪一个（一个专辑常常有
        // 本体/数字版 这样的多个子目录，键是不一样的）
        SetDlgItemText(IDC_LBL_FOLDER, m_display.c_str());

        // 预填现有的线索；没有就用"猜的专辑名"打底，用户改起来省事
        const FolderHint existing = GetFolderHint(m_key);
        const std::wstring artist = existing.artist;
        const std::wstring album  = existing.album.empty()
                                        ? GuessAlbumHintFromPath(m_display + L"\\x")
                                        : existing.album;

        SetDlgItemText(IDC_EDIT_ARTIST, artist.c_str());
        SetDlgItemText(IDC_EDIT_ALBUM, album.c_str());

        // 把焦点放到歌手那一格 —— 它是决定性的那个词
        GetDlgItem(IDC_EDIT_ARTIST).SetFocus();
        return FALSE;   // 我们自己设了焦点，返回 FALSE
    }

    std::wstring GetText(UINT id) const {
        // WTL 的 CWindow 没有 GetDlgItemTextLength —— 用子窗口自己的长度查询。
        const CWindow wnd = GetDlgItem(id);
        const int len = wnd.GetWindowTextLength();
        if (len <= 0) return std::wstring();
        std::wstring s(static_cast<size_t>(len), L'\0');
        wnd.GetWindowText(&s[0], len + 1);
        return s;
    }

    // 本文件里没有 lyric_search.cpp 那个匿名 Trim，写一个局部的。
    static std::wstring TrimWs(const std::wstring& s) {
        size_t b = 0, e = s.size();
        auto sp = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (b < e && sp(s[b])) ++b;
        while (e > b && sp(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    void OnOk(UINT /*uNotifyCode*/, int /*nID*/, HWND /*wndCtl*/) {
        FolderHint h;
        h.artist = TrimWs(GetText(IDC_EDIT_ARTIST));
        h.album  = TrimWs(GetText(IDC_EDIT_ALBUM));

        // 存空 = 删掉这一条线索（SetFolderHint 里判的）
        SetFolderHint(m_key, h);
        m_changed = true;

        EndDialog(IDOK);
    }

    void OnCancel(UINT /*uNotifyCode*/, int /*nID*/, HWND /*wndCtl*/) {
        EndDialog(IDCANCEL);
    }

    std::wstring m_key;
    std::wstring m_display;

public:
    bool m_changed = false;
};

} // namespace

namespace lyricus {

// 弹出对话框让用户给这个文件夹指定线索。返回 true 表示用户点了确定
//（也就是"线索可能变了，该重新查一遍"）。
bool PromptFolderHint(const std::wstring& audioPath, HWND parent) {
    const std::wstring key = FolderKeyOf(audioPath);
    if (key.empty()) {
        DebugLog("歌词线索：这个曲目解析不出所在文件夹，无法指定");
        return false;
    }

    // 显示用的文件夹名：取最后一段，比整条路径好读
    std::wstring display = key;
    const size_t sep = display.find_last_of(L"\\/");
    if (sep != std::wstring::npos) display = display.substr(sep + 1);

    CHintDialog dlg(key, display);
    const INT_PTR r = dlg.DoModal(parent);
    return r == IDOK && dlg.m_changed;
}

} // namespace lyricus
