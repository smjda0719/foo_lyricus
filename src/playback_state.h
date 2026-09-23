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

class PlaybackState {
public:
    static PlaybackState& Get();

    // --- 由 play_callback 驱动 ---
    void OnNewTrack(metadb_handle_ptr track);
    void OnStop();

    // --- 由窗口定时器驱动 ---
    // 刷新播放位置。返回 true 表示「当前歌词行」变了，调用方应当重绘。
    bool RefreshPosition();

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
};

// 取路径的文件名部分（含扩展名）
std::wstring FileNameOf(const std::wstring& path);

// 取路径的文件名，去掉扩展名
std::wstring FileStemOf(const std::wstring& path);

} // namespace lyricus
