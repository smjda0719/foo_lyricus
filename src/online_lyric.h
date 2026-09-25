#pragma once

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 在线歌词查询（数据源：LRCLIB，https://lrclib.net）
//
// 设计取舍：
//
//  * 本模块**只管"把 LRC 文本拿回来"**，不认识 LyricDocument，也不碰 UI。
//    返回的是已经解码成宽字符的 LRC 原文，调用方拿去喂 LyricDocument::Parse
//    即可（那个函数直接吃字节数组，所以这里会再转一次 UTF-8 —— 见 .cpp 里的说明）。
//
//  * **是否联网由调用方决定**。config.h 里虽然有 OnlineLyricEnabled()，
//    但本模块**故意不调用它**：一个是"用户允许联网吗"的策略问题，
//    一个组件里可能有好几个入口（面板、菜单、批量补词），
//    策略放在各入口上比藏在网络层里更好排查。所以这里没有任何开关判断，
//    调了就是查。
//
//  * 同步版和异步版都在。同步版**只能在后台线程调用**（见下面的注释）。
//    异步版是给 UI 用的，它自己保证不阻塞主线程。
// ---------------------------------------------------------------------------

namespace lyricus {

// 一次查询的输入。
//
// 三个字段都允许为空 —— 空字段不会让查询失败，只会让请求发得更"窄"：
//   artist 空 -> 跳过 /api/get（LRCLIB 的 artist_name 是必填，空着发出去就是 400），
//                直接走 /api/search；
//   album  空 -> 请求里不带 album_name 参数（实测不带照样能命中）；
//   title  空 -> 没法查，直接返回失败（track_name 也是必填）。
struct OnlineLyricRequest {
    std::wstring artist, title, album;
    double durationSec = 0.0;   // <= 0 表示未知

    // 搜索线索（可选）。**只影响搜索词，不参与曲目身份的判定。**
    //
    // 【为什么需要它】无标签的曲目里 artist 是占位符「?」、album 是空的，
    // 于是我们只剩曲名一个词去搜 —— 实测会被带偏：
    //     查「02 遗忘山丘」 -> 青山不改与君携 / 讨好 / 遗憾 …（正确答案连前 6 都进不去）
    //     查「奇爱人生 遗忘山丘」 -> 命中
    // 线索通常是**曲目所在文件夹的名字**（用户那些专辑一个标签都没打，
    // 但文件夹名往往就是专辑名），也可能是用户在菜单里手动指定的。
    //
    // ⚠️ 刻意**不放进缓存键**（MakeCacheKey 只看 artist/title/album/duration）：
    //    它是"怎么找"的辅助，不是"是哪首歌"的一部分。
    //    代价是用户改了线索之后，之前写下的未命中标记还在 ——
    //    所以改动线索时要顺手清掉 .miss（见 SetFolderHint 那条路径）。
    std::wstring searchHint;
};

// 一次查询的输出。
//
// fromCache 与 ok 是**两个维度**，不要互相替代：
//   ok=true  + fromCache=false -> 这次真的联网拿到了
//   ok=true  + fromCache=true  -> 磁盘缓存里有，没联网
//   ok=false + fromCache=true  -> 缓存里存着"这首歌没有歌词"这个**负结果**
//                                 （见 .cpp 里的未命中标记），所以连网都没联
//   ok=false + fromCache=false -> 这次联网查了，确实没有 / 或者查询失败
//
// httpStatus 是诊断信息，**不要拿它判断成败**：
//   * 负结果的 httpStatus 可能是 200（搜索接口成功返回了一个空数组）、
//     404（精确查询未命中）、或者 0（压根没发出请求）；
//   * 缓存命中时它是从缓存标记里还原出来的历史值，不是这次的状态。
//   唯一的成功判据是 ok。
struct OnlineLyricResult {
    bool         ok = false;
    std::wstring lrcText;       // 已解码为宽字符的 LRC 原文；ok=false 时为空
    std::wstring error;         // ok=false 时的中文原因，写日志用
    bool         fromCache = false;
    long         httpStatus = 0;

    // =======================================================================
    //  命中的到底是哪首歌 —— 调用方**必须**用这几个字段核对
    //
    //  为什么要有它们：/api/search 是**模糊**接口，它会返回完全不相干的歌。
    //  实测搜 "Best Wishes" 回来的第一条是 Duane Betts 的同名曲，
    //  根本不是要找的那首。如果拿回来就直接显示，用户会在 A 歌上看到
    //  B 歌的歌词 —— 这是这个功能最糟糕的失败模式，比"查不到"差得多
    //  （查不到用户只会觉得可惜，显示错了用户会觉得这插件是坏的）。
    //
    //  所以：**fromSearch == true 时，调用方必须先核对
    //  matchedArtist / matchedTrack / matchedDuration 和当前播放的曲目是否
    //  对得上，对不上就丢掉这份结果。** 本模块不做这个判断 ——
    //  只有调用方手里才有 foobar2000 那一边的真实标签。
    //
    //  这几个字段在"命中"时一定有值，包括**缓存命中**：缓存条目由
    //  .lrc + .meta 两个文件共同组成，.meta 里存着这份元数据；
    //  两个文件缺任何一个都当缓存无效、重新联网取，绝不会出现
    //  "有歌词但没有元数据、调用方无从核对"的中间状态。
    //  （ok=false 时它们都为空 / 0，没有意义。）
    // =======================================================================
    std::wstring matchedArtist;         // 响应里的 artistName
    std::wstring matchedTrack;          // 响应里的 trackName
    std::wstring matchedAlbum;          // 响应里的 albumName
    double       matchedDuration = 0.0; // 响应里的 duration（秒）；0 表示响应里没有
    bool         fromSearch = false;    // true = 结果来自 /api/search（模糊接口）
};

// 同步查询。内部先查缓存，缓存没有才联网。
//
// ⚠️ **只允许在后台线程调用**。它会阻塞到网络往返结束
//    （最坏情况会一路吃满超时：DNS 10s + 连接 10s + 接收 20s，
//      遇到 503/429/超时还会退避重试，总时长由内部的
//      kTotalRetryBudgetMs 兜住，见 .cpp）。
//    在主线程调用 = UI 冻住。
//    之所以不做成"主线程调用就自动转异步"：那样就没法把结果作为返回值给出，
//    接口会变成另一个样子，反而容易误用。这里改成在函数开头放一个
//    PFC_ASSERT（调试版会当场炸出来），并且在 .cpp 里写明理由。
//
// 关于"查不到"和"查失败"，缓存层是**分开**的（这条很重要）：
//   * 服务端明确回答"没有"（/api/get 404，或 /api/search 成功返回一个
//     没有任何歌词的数组）-> 写一个有效期 7 天的未命中标记，
//     下次直接返回负结果、不再联网；
//   * 查询失败（传输层错误、超时、429、5xx）-> **什么都不写**，
//     下次播放会重新尝试。
//   之所以要分开：LRCLIB 实测会限流并返回 503 ServerOverloaded，
//   如果把它也记成"这首歌没有歌词"，一次服务端抖动就能让用户
//   7 天之内再也查不到这首歌，而且完全看不出原因。
OnlineLyricResult FetchLyricOnline(const OnlineLyricRequest& req, const std::wstring& cacheDir);

// 异步查询的回调。参数按值传，回调可以随便存起来慢慢用。
using OnlineLyricCallback = std::function<void(OnlineLyricRequest, OnlineLyricResult)>;

// 异步查询：**立刻返回**，真正的活干在 foobar2000 的后台线程里，
// 干完之后把回调**投递到主线程**执行（用的是 SDK 的 fb2k::inMainThread，
// 不是自己 PostMessage 或者定时器轮询那一套）。
//
// ⚠️ 调用方需要注意两件事：
//
//   1. **回调是在"未来某个时刻"的主线程上执行的，可能很晚**
//      （网络卡住时几十秒都有可能），而且**在 foobar2000 关闭过程中
//      回调可能永远不会被调用**（那一刻服务系统已经不可用了，
//      结果根本投递不出去）。所以调用方**不能**在回调里假设
//      "发起查询时活着的那个对象现在还活着" —— 窗口可能已经被销毁了。
//      典型做法是在回调里先确认宿主还在（例如 IsWindow(hwnd)，
//      或者用 pfc 的服务指针/引用计数），确认不了就安静地丢掉结果。
//      本模块不做任何生命周期管理，也不持有宿主的指针。
//
//   2. 同一个曲目连发多次查询会有多份并发请求，本模块**不做去重**
//      （缓存只在请求**完成**之后才生效）。调用方如果可能重复触发，
//      自己记一下"这个曲目正在查"。
//
//   3. 回调里拿到的 OnlineLyricResult **不要**只看 ok 就直接用：
//      ok=true 且 fromSearch=true 时必须核对 matchedArtist / matchedTrack /
//      matchedDuration（理由见 OnlineLyricResult 里的说明）。
//
// cacheDir 传空串表示用 DefaultOnlineCacheDir()。
void FetchLyricOnlineAsync(const OnlineLyricRequest& req,
                           const std::wstring& cacheDir,
                           OnlineLyricCallback cb);

// 默认缓存目录：DLL 同级的 cache\ 子目录。
//
// 为什么不用 core_api::pathInProfile 这种"正规"位置：
// 和 debug_log.cpp 同一个理由 —— 那份路径实测写不进去（原因未查明），
// 而 DLL 自己所在的目录是确定可写、用户也确定找得到的
// （组件装在 user-components-xxx\foo_lyricus\ 下）。
// 副作用是换组件目录时缓存不会跟着走，可以接受：缓存本来就是可再生的。
std::wstring DefaultOnlineCacheDir();

// 把所有「没有歌词」的负结果缓存全部作废（删掉 .miss 标记）。
//
// 【什么时候要调】用户改了**搜索线索**（按文件夹指定的歌手/专辑）之后。
//
// 【为什么必须调】不清的话会出一个很难理解的现象：用户在菜单里填了歌手、
// 界面毫无反应 —— 因为那个曲目早就被写下 7 天有效的未命中标记，
// 连接网都不联，新线索根本没机会用上。
//
// 【为什么不连 .lrc 一起删】已经查到的歌词是有效资产，与"用什么查询词找到的"
// 无关。只有"没找到"这个结论依赖于当时的查询词。
//
// 返回删掉了几个标记。
size_t InvalidateMissMarkers();

// 列出「在所有在线源上都没找到歌词」的曲目身份（形如
// `artist=[阿良良木健] title=[哀歌] album=[] duration=319s`），已排序。
//
// 【为什么要能列出来】用户的说法一直是「还是有部分没匹配到」，可"部分"是哪些，
// 光看面板永远不知道 —— 面板只在**播到那一首**时才告诉你。
// 知道了才谈得上行动（给那个文件夹指定搜索线索，那会作废标记并重查）。
//
// 只包含**当前逻辑版本**写下的标记；老格式（没有身份那一行）也列不出来，
// 因为那时根本没记曲目是谁。
std::vector<std::wstring> ListUnmatchedTracks();

// 剥掉曲名末尾「同内容」的版本标记：`白夜梦 [Remastered]` -> `白夜梦`。
//
// 【为什么必须导出给调用方】在线查询有**两道**曲名闸：
//   1. 源头挑候选（online_lyric.cpp 的 PickNetEaseCandidate）
//   2. 结果回到播放器后的核对（playback_state.cpp 的 OnlineResultTrustworthy）
// 两道都必须用**同一套**归一化。只用在一道上的话会出现
// 「源头靠剥版本标记认对了、调用方却因为没剥而丢掉」——
//
// 实测（2026-09-24）：
//     「最后的歌（LA LA LA）[Remastered]」 与在线「最后的歌（LA LA LA）」
//     「心加心 [Remastered]」              与在线「心加心」
// 两条都是源头选中了、调用方判"对不上"丢弃。
//
// ⚠️ 只剥不影响内容的标记（Remastered / Hi-Res / Interlude）——
//     Live / Instrumental / Cover / Ver. **不剥**，那些版本内容真的不一样。
std::wstring StripEditionMarkerForMatch(const std::wstring& title);

} // namespace lyricus
