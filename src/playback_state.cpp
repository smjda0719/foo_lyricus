#include "stdafx.h"

#include "playback_state.h"
#include "config.h"
#include "debug_log.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace lyricus {

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::wstring FileStemOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

// ---------------------------------------------------------------------------
// 「曲目 URL -> 歌词路径」表
//
// 存在 cfg_manual_lyric_map 里，纯文本格式：每条一行，URL 与路径用 TAB 分隔。
// 用最朴素的格式是为了出问题时能直接在配置文件里看和改。
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kManualMapMaxEntries = 128;
using MapEntry = std::pair<std::string, std::string>;

std::vector<MapEntry> ParseManualMap() {
    std::vector<MapEntry> out;
    const pfc::string8 raw = cfg_manual_lyric_map.get();
    const std::string s(raw.get_ptr(), raw.length());
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos) eol = s.size();
        const std::string line = s.substr(pos, eol - pos);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
        }
        pos = eol + 1;
    }
    return out;
}

void SaveManualMap(const std::vector<MapEntry>& entries) {
    std::string s;
    const size_t begin = (entries.size() > kManualMapMaxEntries)
                       ? entries.size() - kManualMapMaxEntries : 0;
    for (size_t i = begin; i < entries.size(); ++i) {
        s += entries[i].first; s += '\t'; s += entries[i].second; s += '\n';
    }
    cfg_manual_lyric_map = s.c_str();
}

bool ManualMapLookup(const std::string& url, std::string& outPath) {
    if (url.empty()) return false;
    for (const auto& e : ParseManualMap()) {
        if (e.first == url) { outPath = e.second; return true; }
    }
    return false;
}

void ManualMapSet(const std::string& url, const std::string& path) {
    auto entries = ParseManualMap();
    bool replaced = false;
    for (auto& e : entries) {
        if (e.first == url) { e.second = path; replaced = true; break; }
    }
    if (!replaced) entries.emplace_back(url, path);
    SaveManualMap(entries);
}

void ManualMapRemove(const std::string& url) {
    auto entries = ParseManualMap();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&url](const MapEntry& e) { return e.first == url; }),
                  entries.end());
    SaveManualMap(entries);
}

// 「选文件时没在播放」留下的待定记录（URL 为空），改挂到指定曲目上
bool ManualMapAdoptPending(const std::string& url) {
    for (const auto& e : ParseManualMap()) {
        if (e.first.empty()) {
            const std::string pending = e.second;
            ManualMapRemove("");
            ManualMapSet(url, pending);
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------

PlaybackState& PlaybackState::Get() {
    static PlaybackState instance;
    return instance;
}

void PlaybackState::OnNewTrack(metadb_handle_ptr track) {
    ++m_revision;   // 换曲必然要重绘
    m_hasTrack    = true;
    m_isPlaying   = true;
    m_positionSec = 0.0;
    m_currentLine = LyricDocument::npos;
    m_lyrics      = LyricDocument();
    m_trackUrl.clear();
    m_trackPath.clear();
    m_lyricPath.clear();

    if (track.is_empty()) {
        m_hasTrack = false;
        DebugLog("换曲: <空>");
        return;
    }

    // 【关键】metadb 给的是 URL（形如 file://D:\...），不是文件系统路径。
    // 直接拿它去拼 .lrc 再 CreateFileW 是打不开的 —— 必须转换。
    m_trackUrl = track->get_path();
    {
        pfc::string8 native;
        if (filesystem::g_get_native_path(m_trackUrl.c_str(), native)) {
            m_trackPath = Utf8ToWide(native.get_ptr());
        } else {
            m_trackPath = Utf8ToWide(m_trackUrl.c_str());   // 兜底：万一已经是本地路径
        }
    }
    DebugLog("换曲: %s", WideToUtf8(m_trackPath).c_str());

    // 手动指定的歌词优先 —— 按曲目查表。
    // 先处理「选文件时没在播放」留下的待定记录，让它归属到当前曲目。
    if (ManualMapAdoptPending(m_trackUrl)) {
        DebugLog("待定的手动歌词被当前曲目收养");
    }

    std::string manualPath;
    if (ManualMapLookup(m_trackUrl, manualPath)) {
        DebugLog("命中手动指定的歌词（按曲目记住）");
        LoadLyricFile(Utf8ToWide(manualPath.c_str()), false);
        return;
    }

    ReloadLyrics();
}

void PlaybackState::OnStop() {
    m_isPlaying = false;
    ++m_revision;
    DebugLog("播放停止");
}

void PlaybackState::ReloadLyrics() {
    ++m_revision;
    m_lyrics      = LyricDocument();
    m_currentLine = LyricDocument::npos;
    m_lyricPath.clear();

    if (m_trackPath.empty()) return;

    m_lyricPath = MakeLyricPathForAudio(m_trackPath);
    m_lyrics = LyricDocument::LoadFromFile(m_lyricPath);

    if (m_lyrics.IsEmpty()) {
        DebugLog("未找到可用歌词: %s", WideToUtf8(m_lyricPath).c_str());
    }
}

bool PlaybackState::LoadLyricFile(const std::wstring& path, bool remember) {
    LyricDocument doc = LyricDocument::LoadFromFile(path);
    if (doc.IsEmpty()) {
        DebugLog("手动加载歌词失败或为空: %s", WideToUtf8(path).c_str());
        return false;
    }
    m_lyrics      = std::move(doc);
    m_lyricPath   = path;
    m_currentLine = LyricDocument::npos;
    ++m_revision;

    if (remember) {
        // 没在播放时 m_trackUrl 为空 —— 那就先存成"待定"，等下一首播放时认领
        ManualMapSet(m_trackUrl, WideToUtf8(path));
    }
    DebugLog("手动加载歌词成功: %s", WideToUtf8(path).c_str());
    return true;
}

void PlaybackState::ClearManualLyric() {
    ManualMapRemove(m_trackUrl);   // 只解除当前曲目的
    ManualMapRemove("");           // 顺手清掉待定记录
    DebugLog("已解除当前曲目的手动歌词绑定，回到自动匹配");
    ReloadLyrics();
}

bool PlaybackState::RefreshPosition() {
    auto pc = playback_control::get();
    bool positionChanged = false;

    if (pc.is_valid()) {
        m_isPlaying = pc->is_playing();
        m_isPaused  = pc->is_paused();
        m_volumeDb  = pc->get_volume();

        if (m_isPlaying) {
            const double pos = pc->playback_get_position();
            if (pos != m_positionSec) { m_positionSec = pos; positionChanged = true; }
            m_lengthSec = pc->playback_get_length();
        }
    }

    const size_t idx = m_lyrics.LineIndexAt(m_positionSec);
    if (idx != m_currentLine) {
        m_currentLine = idx;
        return true;
    }
    return positionChanged;
}

// ---------------------------------------------------------------------------
// 播放事件回调
//
// play_callback 是纯接口，10 个方法都得实现 —— 我们只关心 3 个，其余留空。
// 再次强调：这里**不能**调用 playback_control。
// ---------------------------------------------------------------------------
namespace {

class LyricusPlayCallback : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_new_track
             | flag_on_playback_stop
             | flag_on_playback_starting;
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}

    void on_playback_new_track(metadb_handle_ptr track) override {
        PlaybackState::Get().OnNewTrack(track);
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        PlaybackState::Get().OnStop();
    }

    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};

play_callback_static_factory_t<LyricusPlayCallback> g_play_callback;

} // namespace

} // namespace lyricus
