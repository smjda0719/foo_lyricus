#pragma once

#include <string>
#include <vector>
#include <utility>

// ---------------------------------------------------------------------------
// 歌词线索表的**纯逻辑**：文件夹键归一化、文本表 <-> 内存表、增删查改。
//
// 【为什么单独一层】用户 2026-09-26 选了这个待办（plan 第 10 项）：
// 「存储层还没进离线单测台（依赖 SDK 的 cfg_var）」。
// 原先整个 folder_hint.cpp 因为 include 了 config.h / online_lyric.h，
// 一行都测不了 —— 而它里面真正容易出错的恰恰是这些纯逻辑：
// 文本格式的容错、空字段、512 条上限、重复键、UTF-8 往返。
//
// 切法：本文件**不碰 SDK**（只要 lyric.h 的 UTF-8 转换），
// 于是能进 tests/harness 的 hint 组；folder_hint.cpp 只剩一层薄封装
// （读 cfg_var -> 调用这里 -> 写回 + 作废缓存）。
//
// 【为什么按文件夹而不是按曲目】失败模式是**整张专辑没标签** ——
// 一次指定能救十几首；按曲目指定太累。
// ---------------------------------------------------------------------------

namespace lyricus {

struct FolderHint {
    std::wstring artist;   // 歌手：进主查询词，**并且**参与演唱者闸门
    std::wstring album;    // 专辑：只作为搜索的兜底词（它经常猜错，不能挤掉主查询）

    bool Empty() const { return artist.empty() && album.empty(); }
    bool operator==(const FolderHint& o) const {
        return artist == o.artist && album == o.album;
    }
};

// 音频路径 -> 查表用的"文件夹键"。查不出返回空串。
//
// 归一化规则：统一小写（Windows 路径大小写不敏感）、去掉结尾的斜杠。
// 不做更激进的归一化（比如解析 ../）—— 那属于"猜用户意图"，
// 而这个表本来就是用户自己写进去的，原样比对上就好。
std::wstring FolderKeyOf(const std::wstring& audioPath);

// 表 <=> 文本。
//
// 格式：每条一行，`文件夹键 \t 歌手 \t 专辑`。
// 歌手或专辑可以为空（用户只想填一个），所以**不能**用"非空"来判断字段有没有 ——
// 靠 TAB 的个数。
std::string FormatFolderHints(const std::vector<std::pair<std::wstring, FolderHint>>& entries);
std::vector<std::pair<std::wstring, FolderHint>> ParseFolderHints(const std::string& text);

// 在文本表里查一条。查不到返回空 FolderHint。
FolderHint LookupFolderHint(const std::string& text, const std::wstring& folderKey);

// 对文本表做一次编辑（设置或删除一条），返回**新的文本表**。
//
// 这就是 SetFolderHint 的全部逻辑，只是不碰 cfg_var。
// hint 为空 = 删除这一条。
//
// ★ changedOut：这一次编辑**有没有真的改动表**。
//   调用方靠它决定"要不要写盘、要不要作废未命中缓存"——
//   写了同样的内容却去作废缓存，会让用户白白多查一轮网络。
//   传 nullptr 表示不关心。
//
// 会顺手把字段里的 TAB / 换行换成空格（否则整张表会被撑坏）。
std::string ApplyFolderHintEdit(const std::string& currentText,
                                const std::wstring& folderKey,
                                const FolderHint& hint,
                                bool* changedOut = nullptr);

// 表里最多留多少条。超了从**最旧的**开始丢。
constexpr size_t kMaxFolderHints = 512;

} // namespace lyricus
