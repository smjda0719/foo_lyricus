#pragma once

// ---------------------------------------------------------------------------
// 单测用的**假** PlaybackState。
//
// 真实那份要读 metadb / playback_control（全是 SDK），而绘制层
// （lyrics_view.cpp）只用到下面这几个**只读访问器**：
//     Get() / HasTrack() / DisplayName() / Lyrics() / CurrentLine() / LyricPath()
// 把它们换成可设置的假货，布局数学就能在内存 DC 上离线验证 ——
// 不必起播放器、不必真的有一首歌。
//
// 这个文件在单测工作目录里会**覆盖** src/playback_state.h
//（harness 把源码拷到临时目录再编，所以同名的替身能生效）。
//
// 【为什么值得测】「行数上限」这一项在真机上**没法验证**：
// 默认面板 460x150 逻辑像素，字号 125% 时一行就占满了，
// 调大调小都只显示一行 —— 看不出效果不等于功能是坏的。
// 只有把布局数学单独拎出来量，才能确认它真的按预期工作。
// ---------------------------------------------------------------------------

#include "lyric.h"

#include <string>

namespace lyricus {

class PlaybackState {
public:
    static PlaybackState& Get();

    bool                 HasTrack()    const { return m_hasTrack; }
    const std::wstring&  DisplayName() const { return m_displayName; }
    const LyricDocument& Lyrics()      const { return m_lyrics; }
    size_t               CurrentLine() const { return m_currentLine; }
    const std::wstring&  LyricPath()   const { return m_lyricPath; }

    // ---- 只有单测用的设置接口 ----
    void SetFake(bool hasTrack, const std::wstring& displayName,
                 LyricDocument lyrics, size_t currentLine,
                 const std::wstring& lyricPath = std::wstring());

private:
    PlaybackState() = default;

    bool                m_hasTrack   = false;
    std::wstring        m_displayName;
    LyricDocument       m_lyrics;
    size_t              m_currentLine = LyricDocument::npos;
    std::wstring        m_lyricPath;
};

// 取路径的文件名部分（含扩展名）—— 绘制层用它显示"无歌词"时的文件名
std::wstring FileNameOf(const std::wstring& path);

} // namespace lyricus
