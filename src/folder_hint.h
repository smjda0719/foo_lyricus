#pragma once

#include "folder_hint_table.h"   // FolderHint / FolderKeyOf / Parse / Format / Lookup / Apply

#include <string>
#include <vector>
#include <utility>

// ---------------------------------------------------------------------------
// 按文件夹指定的歌词线索 —— **SDK 这一层**
//
// 【为什么需要它】无标签的曲目里 %artist% 是占位符「?」、专辑是空的，
// 于是我们只剩曲名一个词去搜 —— 而实测这会被彻底带偏：
//     查「02 遗忘山丘」 -> 青山不改与君携 / 讨好 / 遗憾 …（正确答案连前 6 都进不去）
//     查「哀歌」        -> 和田薫 / 平井堅 …（真答案 id=1333394828 连前 10 都没有）
//     查「阿良良木健 哀歌」 -> 第 1 条就是它           ← 歌手是那把钥匙
//
// 文件夹名可以猜（见 lyric_search.h 的 GuessAlbumHintFromPath），但**不可靠**：
//     `奇爱人生 爸爸`       -> 命中「爸爸……（Interlude）」     ✓
//     `奇爱人生·终焉版 哀歌` -> 寻爱一生 / 众人划桨开大船 …    ✗ 反而是垃圾
// 用户的文件夹命名没有规矩，所以必须给一个人工入口。
//
// 【这一层只剩什么】文本表 <-> 内存表、增删查改**全在 folder_hint_table.h**，
// 那是纯函数、能进离线单测台（hint 组）。这里只有"读 cfg_var -> 调用它 ->
// 写回 + 作废未命中缓存"这一圈 SDK 交互，以及一个对话框。
// ---------------------------------------------------------------------------

namespace lyricus {

// 读写全局表（内部走 cfg_var）。folderKey 传 FolderKeyOf() 的结果。
FolderHint GetFolderHint(const std::wstring& folderKey);

// 写入一条。hint 为空 = 删除这一条。
//
// 写完之后会**作废所有"没有歌词"的缓存结论** —— 那些是用旧查询算出来的，
// 见 folder_hint.cpp 里 InvalidateMissMarkers 的说明。
//
// ★ 但如果这次编辑**没有真的改动表**（重复填同样的歌手），就什么都不做：
//   不写盘，也不作废缓存。否则用户点两次确定，那首歌会白白多查一轮网络。
void SetFolderHint(const std::wstring& folderKey, const FolderHint& hint);

// 表里现在有多少条（首选项/日志用）。
size_t FolderHintCount();

// 弹出「为这个文件夹指定歌词线索」对话框。
//
// audioPath 是曲目的完整路径（会经 FolderKeyOf 折成文件夹键）。
// 返回 true 表示用户点了确定、且内容确实变了 —— 调用方据此重新查一遍歌词。
bool PromptFolderHint(const std::wstring& audioPath, HWND parent);

} // namespace lyricus
