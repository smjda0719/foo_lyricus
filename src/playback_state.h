#pragma once

#include "lyric.h"

// ---------------------------------------------------------------------------
// 当前播放状态 + 歌词缓存
//
// 线程约定：所有方法都只在**主线程**调用。
//   * play_callback 的回调本身就是主线程（SDK 头文件明确说明）；
//   * 窗口定时器也在主线程。
// 所以这里不需要加锁。
//
// 注意：play_callback 里**不能**调用 playback_control（SDK 明令禁止，
// 会引发竞态）。位置查询统一放在 RefreshPosition()，由窗口定时器驱动。
// ---------------------------------------------------------------------------

namespace lyricus {

// RefreshPosition() 这一拍到底变了什么。
//
// 【为什么不直接返回 bool】两者的代价差得远：
//   * 换行     —— 要重画整块歌词
//   * 位置变化 —— 只影响进度条和时间文字
// 而位置**每 250ms 就变一次**（播放时一直在走），换行几分钟才一次。
// 混成一个 bool 的话调用方只能每次都全量重画，实测这导致面板每秒重绘 4 次、
// 每次 5~7ms，白白占住主线程 20~28ms/s —— 用户拖专辑进库时，
// foobar2000 自己也在抢主线程，这就是那份卡顿的来源（见 D-031）。
enum class TickChange {
    None,       // 什么都没变
    Position,   // 只有播放位置在走，当前歌词行没变
    Line,       // 当前歌词行变了
};

// 「只有播放位置在走」时，各宿主统一的重绘节流间隔（毫秒）。
//
// 定时器仍是 250ms 一拍（要保证换行、换曲能被及时发现），
// 但位置变化没必要每拍都重画：它只影响进度条和时间文字，
// 而进度条在 4 分钟的曲子上每秒才走约 1 个像素 —— 4Hz 里
// 有 3 次画出来的画面和上一次完全一样，时间文字本来也只每秒变一次。
//
// 取 1000ms 就是「信息一点都不丢」的上限。换行 / 换曲 / 改设置 / 用户拖动
// 都**不受这个节流限制**，各自有独立触发条件，仍然立刻重绘。
//
// 三个宿主（独立面板 / DUI 元素 / CUI 面板）共用同一个值：
// 它们烧的是同一条主线程，没有理由各不相同。
constexpr ULONGLONG kPositionRepaintMs = 1000;

class PlaybackState {
public:
    static PlaybackState& Get();

    // --- 由 play_callback 驱动 ---
    void OnNewTrack(metadb_handle_ptr track);
    void OnStop();

    // --- 由窗口定时器驱动 ---
    // 刷新播放状态与位置，并报告这一拍变了什么。
    // 调用方按 TickChange 决定是「立刻重绘」还是「可以攒一攒」。
    TickChange RefreshPosition();

    // 状态代次：换曲、加载/清除歌词时都会自增。
    // 调用方只要发现自己记的值不一致就重绘 —— 比逐字段比对可靠。
    //
    // 之所以需要它：「当前行」在上面的场景里可能是 npos == npos 而被判定为
    // 「没变化」，于是画面停留在上一首的歌词上（实测踩过）。
    unsigned Revision() const { return m_revision; }

    bool                 HasTrack()  const { return m_hasTrack; }
    const std::wstring&  TrackPath() const { return m_trackPath; }
    // 给人看的曲目名：优先用标签渲染出的 "%artist% - %title%"，
    // 标签为空时退回文件名（去掉扩展名）。
    const std::wstring&  DisplayName() const { return m_displayName; }
    const std::wstring&  LyricPath() const { return m_lyricPath; }
    const LyricDocument& Lyrics()    const { return m_lyrics; }
    double               PositionSec() const { return m_positionSec; }
    double               LengthSec()   const { return m_lengthSec; }
    size_t               CurrentLine() const { return m_currentLine; }

    // 当前行的**显示序号**（参照行不占号）。
    //
    // 【为什么动画必须用它而不是 CurrentLine】双语歌词是"同时间戳、原文在前、
    // 翻译在后"两行，而 LineIndexAt 会退到组首 —— 于是原始行号每推进一个
    // 时间戳就跳 **2**（实测 34→35→37→38→41…）。换行上滑的判据是"顺序 +1"，
    // 拿原始行号去比几乎永远不成立，上滑等于没做（D-046）。
    //
    // 换算成显示序号之后，"下一句"恒为 +1，seek 仍然是 +N 或负数 —— 判据不变，
    // 只是量纲对了。
    size_t               DisplayLine() const;

    // ---- 逐曲目的歌词时间偏移 ----
    //
    // 正值 = **歌词提前**（同一播放位置去歌词里更靠后的地方找），用来修
    // 「面板总比声音慢」那种整首歌的偏移。见 config.h 里 cfg_lyric_offset_map。
    double LyricOffsetSec() const;

    // 当前曲目的偏移增减 deltaSec（正 = 提前），夹在 ±30 秒内，并**记住**到该曲目。
    // 名字叫 Nudge 而不是 Set 是有意的：用户入口是"再提前半秒"这种相对动作，
    // 而绝对值他没法凭空知道。
    void NudgeLyricOffset(double deltaSec);
    void ResetLyricOffset();

    bool                 IsPlaying()  const { return m_isPlaying; }
    bool                 IsPaused()   const { return m_isPaused; }
    float                VolumeDb()   const { return m_volumeDb; }
    bool                 IsMuted()    const { return m_isMuted; }

    // 按当前曲目路径重新推导并加载 .lrc（自动匹配）
    void ReloadLyrics();

    // 手动指定歌词文件（菜单里选文件用）。
    // remember=true 时记住该选择并绑定到当前曲目；详见 .cpp 里的绑定逻辑。
    bool LoadLyricFile(const std::wstring& path, bool remember = true);

    // 解除手动绑定，回到自动匹配
    void ClearManualLyric();

private:
    PlaybackState() = default;
    PlaybackState(const PlaybackState&) = delete;
    PlaybackState& operator=(const PlaybackState&) = delete;

    // 发起一次在线查询（LRCLIB）。调用方是 RefreshPosition()，理由见下面
    // m_onlineWanted 的说明。重复调用同一曲目会被忽略。
    void StartOnlineLookup();

    // 后台线程把结果投递回主线程后的处理。
    // gen / url 用来判断这份结果是不是已经过期（期间换曲了）。
    void ApplyOnlineResult(unsigned gen, const std::string& url,
                           const struct OnlineLyricResult& res);

    bool                m_hasTrack   = false;
    bool                m_isPlaying  = false;
    unsigned            m_revision   = 1;
    std::string         m_trackUrl;      // metadb 给的原始 URL（file://...）
    std::wstring        m_trackPath;     // 转换后的本地文件系统路径
    std::wstring        m_displayName;   // 由标签渲染，供界面显示
    metadb_handle_ptr   m_trackHandle;   // 拿标签用
    std::wstring        m_lyricPath;
    LyricDocument       m_lyrics;
    double              m_positionSec = 0.0;
    double              m_lengthSec   = 0.0;
    bool                m_isPaused    = false;
    float               m_volumeDb    = 0.0f;   // playback_control 的音量单位是 dB，0 为满音量
    bool                m_isMuted     = false;
    size_t              m_currentLine = LyricDocument::npos;
    size_t              m_displayLine = LyricDocument::npos;   // 见 DisplayLine()
    double              m_lyricOffsetSec = 0.0;                // 见 LyricOffsetSec()

    // ---- 标签缓存 ----
    // ReloadLyrics() 取一次，歌词搜索和在线查询共用。
    // 在这里存一份而不是各自渲染：titleformat 每次都重新跑一遍没意义，
    // 而且两边用**不同**的值去查同一首歌会很难排查。
    std::wstring        m_tagArtist, m_tagTitle, m_tagAlbum;

    // ---- 在线歌词（LRCLIB，见 online_lyric.h）----

    // 本地没找到歌词时置位。**真正的发起在 RefreshPosition() 里**，不在这里：
    // OnNewTrack 不能调 playback_control（SDK 明令禁止），那一刻拿不到曲子时长，
    // 而时长是核对 /api/search 返回值的关键判据（见 D-024）。
    // 为了一个查询把上面的禁令破掉，不值得。
    bool                m_onlineWanted = false;

    // 正在查询的曲目 URL。非空 = 这首已经在查。
    // online_lyric.h 明确说了模块本身**不做去重**，重复触发要调用方自己拦。
    std::string         m_onlinePendingUrl;

    // 换曲时自增。回调回来时对不上就说明用户已经换歌了，结果直接丢弃 ——
    // 后台查询可能几十秒才回来，期间换歌是常态。
    unsigned            m_onlineGeneration = 0;

    // **实际发出去查询的歌手**（标签里的，或者用户给文件夹填的线索）。
    //
    // 核对在线结果时要用它，不能重新去读 m_tagArtist ——
    // 无标签曲目那边是占位符「?」，而真正的歌手来自线索，两个值不一样。
    // 用 m_tagArtist 的话，时长又对不上时就会"手里没有歌手可用"而丢掉正确结果
    //（实测：「爸爸……（Interlude）」差 15 秒、「春风来（Love Elegia Ver.）」差 7.1 秒）。
    std::wstring        m_onlineArtistUsed;
};

// 取路径的文件名部分（含扩展名）
std::wstring FileNameOf(const std::wstring& path);

// 取路径的文件名，去掉扩展名
std::wstring FileStemOf(const std::wstring& path);

} // namespace lyricus
