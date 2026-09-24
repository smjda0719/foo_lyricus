#pragma once

#include <string>
#include <vector>
#include <utility>

// ---------------------------------------------------------------------------
// 按文件夹指定的歌词线索
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

// 表 <=> 文本。**纯函数，离线可测。**
//
// 格式：每条一行，`文件夹键 \t 歌手 \t 专辑`。
// 歌手或专辑可以为空（用户只想填一个），所以**不能**用"非空"来判断字段有没有 ——
// 靠 TAB 的个数。
std::string FormatFolderHints(const std::vector<std::pair<std::wstring, FolderHint>>& entries);
std::vector<std::pair<std::wstring, FolderHint>> ParseFolderHints(const std::string& text);

// 读写全局表（内部走 cfg_var）。folderKey 传 FolderKeyOf() 的结果。
FolderHint GetFolderHint(const std::wstring& folderKey);

// 写入一条。hint 为空 = 删除这一条。
// 写完之后会**作废所有"没有歌词"的缓存结论** —— 那些是用旧查询算出来的，
// 见下面 InvalidateMissMarkers 的说明。
void SetFolderHint(const std::wstring& folderKey, const FolderHint& hint);

// 表里现在有多少条（首选项/日志用）。
size_t FolderHintCount();

// 弹出「为这个文件夹指定歌词线索」对话框。
//
// audioPath 是曲目的完整路径（会经 FolderKeyOf 折成文件夹键）。
// 返回 true 表示用户点了确定、且内容确实变了 —— 调用方据此重新查一遍歌词。
bool PromptFolderHint(const std::wstring& audioPath, HWND parent);

} // namespace lyricus
