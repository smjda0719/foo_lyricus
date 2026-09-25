#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  在线歌词源的**顺序与启用状态** —— 纯逻辑，不依赖 SDK、可离线单测
//
//  【为什么要有它】用户 2026-09-25：「可以给用户自定义查找歌词顺序的面板」。
//  起因是查证"酷狗到底能不能干活"时发现：网易云排第一位、命中就收工，
//  于是备用源在正常使用中**几乎永远跑不到** —— 想验证它就得先把网易云让开。
//  与其在代码里临时短接，不如把这个顺序交给用户。
//
//  【为什么单独一个文件】这段是**纯函数 + 纯数据**，不碰 cfg_var、不碰窗口。
//  于是它能进离线单测台 —— 而"配置串的解析与容错"恰恰是最容易悄悄写错、
//  又最难在真机上发现的地方（配置坏掉的表现是"我的设置没生效"，
//  而不是崩溃）。
// ---------------------------------------------------------------------------

namespace lyricus {

// ⚠️ 枚举值与**顺序**都是持久化契约：`SourceId()` 的字符串会被写进配置。
//    改名 = 用户设置丢失（会退回默认），加新源要加在**末尾**。
enum class LyricSource {
    NetEase = 0,   // 网易云
    Kugou,         // 酷狗
    Lrclib,        // LRCLIB
};

// 全部源，按**出厂顺序**（也就是"恢复默认"的结果）。
std::vector<LyricSource> AllSources();

// 持久化用的 id：`netease` / `kugou` / `lrclib`。未知返回空串。
std::wstring SourceId(LyricSource s);
bool         SourceFromId(const std::wstring& id, LyricSource& out);

// 界面与日志里显示的名字。
const wchar_t* SourceDisplayName(LyricSource s);

struct SourcePref {
    LyricSource src     = LyricSource::NetEase;
    bool        enabled = true;
};

// 解析配置串 `netease:1,kugou:0,lrclib:1`。
//
// 【容错规则，每一条都有理由】
//   * 空串            -> 全部按出厂顺序启用（新用户、以及"恢复默认"）
//   * 认不出的 id     -> 跳过（配置里可能留着已经删掉的源的名字）
//   * 重复的 id       -> 只认第一次（后写的忽略，避免"同一个源出现两次"）
//   * 配置里没提到的源 -> **追加在末尾并启用**。这一条是为了将来：
//                        加了第四个源之后，老用户的配置里没有它，
//                        不追加的话新源永远不会被尝试 —— 而这种 bug
//                        的表现是"新功能没生效"，最难查。
//                        代价是用户手动停用过的老源在**升级后**不会被重新启用
//                        （因为它被提到了、写的是 :0），这条我们分得清。
//   * 全被停用        -> 原样返回（用户可能就是想要"不联网"，不该偷偷加回来）
std::vector<SourcePref> ParseSourcePrefs(const std::wstring& text);

// 反向：写成配置串。**所有项都写**（含停用的），这样"没提到"就只表示"新源"。
std::wstring FormatSourcePrefs(const std::vector<SourcePref>& prefs);

// 按配置顺序取出**启用的**源。
std::vector<LyricSource> EnabledSources(const std::vector<SourcePref>& prefs);

// 便捷组合：配置串 -> 该试哪些源。
std::vector<LyricSource> SourcesToTry(const std::wstring& text);

// 上移/下移一项；越界时原样返回（界面按钮到边界不该有动作）。
std::vector<SourcePref> MoveSourcePref(const std::vector<SourcePref>& prefs,
                                       size_t index, int delta);

} // namespace lyricus
