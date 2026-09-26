#include "stdafx.h"
#include "folder_hint.h"
#include "config.h"
#include "debug_log.h"
#include "lyric.h"          // WideToUtf8 / Utf8ToWide
#include "online_lyric.h"   // InvalidateMissMarkers

// ---------------------------------------------------------------------------
// 歌词线索 —— SDK 这一层。
//
// 表的解析、格式化、查、改**全在 folder_hint_table.cpp**（纯函数、离线可测，
// 见 hint 组）。这里只剩"读配置 -> 改 -> 写回 + 作废缓存"。
// 切这一刀的原因见 folder_hint_table.h。
// ---------------------------------------------------------------------------

namespace lyricus {

namespace {

// cfg_var 里存的是 pfc::string8，这里统一转成 std::string。
std::string ReadTableText() {
    const pfc::string8 raw = cfg_folder_hints.get();
    return std::string(raw.get_ptr(), raw.length());
}

} // namespace

FolderHint GetFolderHint(const std::wstring& folderKey) {
    return LookupFolderHint(ReadTableText(), folderKey);
}

void SetFolderHint(const std::wstring& folderKey, const FolderHint& hint) {
    if (folderKey.empty()) return;

    bool changed = false;
    const std::string next = ApplyFolderHintEdit(ReadTableText(), folderKey, hint, &changed);

    // ★ 没改动就到此为止：不写盘，也**不作废未命中缓存**。
    //
    // 原先无论有没有改动都往下走，于是"重复填同一个歌手"（或者点了确定但
    // 没改内容）会白白作废一次缓存 —— 那首歌下次换回来要重新联网查一轮。
    if (!changed) {
        DebugLog("歌词线索：内容未变，跳过（文件夹=%s）", WideToUtf8(folderKey).c_str());
        return;
    }

    cfg_folder_hints = next.c_str();

    // 用作弊的方式拿到"这条现在长什么样"只为日志 —— 表本身已经写回去了。
    FolderHint applied = hint;
    DebugLog("歌词线索：%s -> 歌手=「%s」 专辑=「%s」（表内共 %zu 条）",
             applied.Empty() ? "已清除" : "已保存",
             WideToUtf8(applied.artist).c_str(), WideToUtf8(applied.album).c_str(),
             FolderHintCount());

    // ★ 改了线索，之前用**旧查询**算出来的「没有歌词」就不算数了。
    //
    // 不清的话会出一个很难理解的现象：用户在菜单里填了歌手、界面毫无反应 ——
    // 因为那个曲目早就被写下 7 天有效的未命中标记，连接网都不联。
    // 清掉之后下次换曲/重播就会带着新线索重新查。
    InvalidateMissMarkers();
}

size_t FolderHintCount() {
    return ParseFolderHints(ReadTableText()).size();
}

} // namespace lyricus
