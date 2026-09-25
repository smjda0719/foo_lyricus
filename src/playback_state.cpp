#include "stdafx.h"

#include "playback_state.h"
#include "config.h"
#include "lyric_search.h"
#include "folder_hint.h"
#include "online_lyric.h"
#include "debug_log.h"

#include <algorithm>
#include <atomic>
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

// 核对 /api/search 结果时允许的时长误差（秒）。
//
// 取 5 秒的依据：同一首歌的不同版本（专辑版 / 单曲版）一般差不了几秒，
// 而**一首不相干的歌**几乎不可能刚好也一样长。
// 实测反例：搜 "Best Wishes" 回来的是 Duane Betts 的 198 秒版本，
// 而用户那首 flac 有 51 MB，怎么算都不是 198 秒 —— 5 秒的窗足够把它挡住。
constexpr double kDurationToleranceSec = 5.0;

// 在线查询回调的存活令牌。
//
// online_lyric.h 警告了两件事：回调可能很晚才来；关闭过程中可能**永远不来**。
// 反过来也不能假设它一定在对象还活着的时候来 —— 组件卸载之后
// fb2k::inMainThread 投递的那个 lambda 仍有可能被执行（或者正在执行中）。
//
// 这是个命名空间级静态对象：它在 DLL 加载时构造，卸载时析构，
// 而 PlaybackState 的单例是函数内静态、首次调用时才构造 ——
// 也就是说这个令牌**先构造、后析构**，一定比单例活得久。
// 回调里先查令牌再碰 this，就不会踩到已经析构的单例。
struct OnlineAliveGuard {
    std::atomic_bool alive{ true };
    ~OnlineAliveGuard() { alive.store(false); }
};
OnlineAliveGuard g_onlineAlive;

// 曲目显示名用的 titleformat 脚本。
// 方括号部分在标签缺失时整段消失，所以只有标题的曲子不会显示成 " - 标题"。
titleformat_object::ptr GetDisplayNameScript() {
    static titleformat_object::ptr script;
    static bool tried = false;
    if (!tried) {
        tried = true;
        try {
            titleformat_compiler::get()->compile_safe(script, "[%artist% - ]%title%");
        } catch (...) {
            script.release();   // 编译失败就退回文件名
        }
    }
    return script;
}

// 取单个标签字段。
//
// 为什么不复用上面的 GetDisplayNameScript：那个脚本把 artist 和 title
// 拼成一根字符串，只为显示；搜索需要的是**分开**的字段 ——
// 要拼 "artist - title" / "title" / "title - artist" 好几种组合，
// 拼好的东西拆不回来。
//
// 编译结果由调用方持有的 cache 缓存：只在换曲时调一次，不值得每次重编。
std::wstring RenderTagField(const metadb_handle_ptr& track, const char* expr,
                            titleformat_object::ptr& cache) {
    if (track.is_empty()) return std::wstring();

    if (cache.is_empty()) {
        try {
            titleformat_compiler::get()->compile_safe(cache, expr);
        } catch (...) {
            cache.release();
        }
    }
    if (cache.is_empty()) return std::wstring();   // 编译不过就当没有这个标签

    try {
        pfc::string8 rendered;
        track->format_title(nullptr, rendered, cache, nullptr);
        return Utf8ToWide(rendered.get_ptr());
    } catch (...) {
        return std::wstring();
    }
}

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
    // 这条路径整个跑在**主线程**的 play_callback 里 ——
    // 它慢一点，换曲时用户就能感觉到卡顿。超过 8ms 记一笔。
    ScopedTimer timer("OnNewTrack（换曲主线程）", 8.0);

    ++m_revision;   // 换曲必然要重绘
    m_hasTrack    = true;
    m_isPlaying   = true;
    m_positionSec = 0.0;
    m_currentLine = LyricDocument::npos;
    m_displayLine = LyricDocument::npos;
    m_lyrics      = LyricDocument();
    m_trackUrl.clear();
    m_trackPath.clear();
    m_lyricPath.clear();

    // 换曲就作废所有在途的在线查询结果：代次一变，回来的回调会被丢掉。
    // 同时清掉"正在查"的标记，让新曲目能立刻发起自己的查询。
    ++m_onlineGeneration;
    m_onlinePendingUrl.clear();
    m_onlineWanted = false;

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

    // 显示名：优先用标签。文件名里常带音轨号和版本后缀（"06 xxx [Remastered]"），
    // 标签里才是给人看的曲名。
    m_trackHandle = track;
    m_displayName.clear();
    {
        const titleformat_object::ptr script = GetDisplayNameScript();
        if (!script.is_empty()) {
            try {
                pfc::string8 rendered;
                track->format_title(nullptr, rendered, script, nullptr);
                m_displayName = Utf8ToWide(rendered.get_ptr());
            } catch (...) {
                m_displayName.clear();
            }
        }
        if (m_displayName.empty()) m_displayName = FileStemOf(m_trackPath);
        DebugLog("显示名: %s", WideToUtf8(m_displayName).c_str());
    }

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
    // 主线程。搜索要枚举目录、跑模糊匹配，目录大时会明显变慢。
    ScopedTimer timer("ReloadLyrics（搜索+加载）", 8.0);

    ++m_revision;
    m_lyrics      = LyricDocument();
    m_currentLine = LyricDocument::npos;
    m_displayLine = LyricDocument::npos;
    m_lyricPath.clear();

    // ★ 重新加载 = "把之前知道的忘掉，重新问一遍"。
    //
    // 【不清这两样会出什么】`StartOnlineLookup()` 开头有一句
    //     if (m_onlinePendingUrl == m_trackUrl) return;   // 这首已经在查了
    // 那是用来防**重复并发查询**的。但"重新加载"是**故意**要再查一次的 ——
    // 不清标记的话，在**同一首曲目**上重新加载根本发不出查询，要切歌才生效。
    //
    // 实测踩到的现象（用户 2026-09-24 报）：给无标签曲目填线索时，
    // 先填一个歌手 -> 切了首歌 -> 好了；再加一个歌手 -> 没切歌 -> **毫无反应**。
    // 用户以为是"填多了搜不到"，其实是"这一首压根没重查"。
    //
    // 代次也要 ++：同名同曲目再查时 url 是不变的，只比 url 分不出新旧，
    // 旧查询的在途结果会被当成新的收下 —— 而那是用**旧线索**搜出来的东西。
    ++m_onlineGeneration;
    m_onlinePendingUrl.clear();

    if (m_trackPath.empty()) return;

    // 标签只在拿得到 metadb 时才有；拿不到就留空，
    // 搜索模块会自动退回到「只靠文件名」的那几条策略。
    // 存进成员而不是用局部变量：在线查询也要用**同一份** ——
    // 两边各渲染一次的话，值不一致会很难查。
    static titleformat_object::ptr s_artistScript, s_titleScript, s_albumScript;
    {
        // 单独计时：这三行是「换曲/拖入新专辑时为什么慢」的头号嫌疑。
        // 刚拖进播放器的文件还没读过标签，第一次 format_title 会
        // **同步触发磁盘读取** —— 大 WAV 上一次就是十几毫秒。
        ScopedTimer tagTimer("RenderTagField × 3（读标签）", 3.0);
        m_tagArtist = RenderTagField(m_trackHandle, "%artist%", s_artistScript);
        m_tagTitle  = RenderTagField(m_trackHandle, "%title%",  s_titleScript);
        m_tagAlbum  = RenderTagField(m_trackHandle, "%album%",  s_albumScript);
    }

    // 多策略搜索：精确 / 去前缀 / 标签构造 / 模糊，取置信度最高的一条。
    // 详见 lyric_search.h 的策略表与计分说明。
    //
    // 旧版这里只有 MakeLyricPathForAudio（同目录 + 同名 + .lrc），
    // 遇到「塞壬唱片-MSR - Battleplan Obliteration.wav」配
    // 「Battleplan Obliteration.lrc」这种带厂牌前缀的摆放必然落空。
    const LyricSearchConfig searchCfg = GetLyricSearchConfig();
    const LyricSearchHit hit = [&] {
        ScopedTimer findTimer("FindLyricFile（目录搜索）", 3.0);
        return FindLyricFile(m_trackPath, m_tagArtist, m_tagTitle, m_tagAlbum, searchCfg);
    }();

    if (hit.path.empty()) {
        DebugLog("未找到本地歌词（精确/去前缀/标签%s 都没命中）: %s",
                 searchCfg.fuzzy ? "/模糊" : "",
                 WideToUtf8(FileStemOf(m_trackPath)).c_str());

        // 交给在线兜底。**真正的发起不在这里**，在 RefreshPosition() ——
        // 这条路径的起点是 OnNewTrack，而那个回调里不能调 playback_control
        // （SDK 明令禁止），此刻还不知道曲子多长，而时长正是核对
        // /api/search 返回值的关键判据（见 D-024）。
        //
        // 开关在这里判而不是在 online_lyric 里：那是"用户允许联网吗"的策略问题，
        // 归调用方管，藏到网络层里以后不好排查。
        m_onlineWanted = OnlineLyricEnabled();
        if (m_onlineWanted) DebugLog("在线歌词：已排队，等 RefreshPosition 拿到时长后发起");
        return;
    }

    // 只在**真的命中文件**时才记路径。
    // 旧版无条件写成推导出的 .lrc 路径，哪怕那个文件根本不存在 ——
    // 界面就会在「（无歌词）」下面挂一个不存在的文件名，误导排查方向。
    m_lyricPath = hit.path;
    m_lyrics = LyricDocument::LoadFromFile(m_lyricPath);

    if (m_lyrics.IsEmpty()) {
        // 文件在但解析不出内容，和"没找到文件"要分开报，否则会去查错方向
        DebugLog("歌词文件存在但解析为空（可能是纯文本无时间轴）: %s",
                 WideToUtf8(m_lyricPath).c_str());
        return;
    }

    DebugLog("歌词命中 [%s, %d 分]: %s", WideToUtf8(hit.how).c_str(), hit.score,
             WideToUtf8(m_lyricPath).c_str());
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
    m_displayLine = LyricDocument::npos;
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

// ---------------------------------------------------------------------------
// 在线歌词（LRCLIB）
// ---------------------------------------------------------------------------

namespace {

// /api/search 是**模糊**接口，实测会返回完全不相干的歌 ——
// 搜 "Best Wishes" 回来的第一条是 Duane Betts 的同名曲。
// 所以来自模糊接口的结果必须核对得上才敢用：
// **显示错的歌词比不显示糟糕得多**，用户会以为整个插件是坏的（见 D-024）。
//
// 精确接口 /api/get 不需要核对：请求里就带了 artist + track + duration，
// LRCLIB 三项都对上才会返回 200。
bool OnlineResultTrustworthy(const OnlineLyricResult& res,
                             const std::wstring& localTitle,
                             const std::wstring& localArtist,
                             double localDurationSec) {
    if (!res.ok) return false;
    if (!res.fromSearch) return true;

    // 1) 曲名必须对得上。归一化后比较，忽略大小写 / 标点 / 全半角差异。
    //
    //    ⚠️ **不能只比原样**。文件没有标签时，foobar2000 的 %title% 会退化成
    //    **文件名**（形如 "Artist - Title" 或 "02 遗忘山丘"），而在线的曲名
    //    往往只有干净的 "Title"。实测踩到两次：
    //      * 本地 [周杰伦 - 晴天] vs 在线 [晴天]（D-027）
    //      * 本地 [02 遗忘山丘]   vs 在线 [遗忘山丘]（无标签 WAV）
    //    所以要拿 MakeTitleCandidates() 切出来的**全部候选**去比 ——
    //    它同时处理「艺术家 - 曲名」前缀和音轨号前缀。
    //
    //    ⚠️ 这里的候选集合必须和源头（online_lyric.cpp 的 PickNetEaseCandidate）
    //    **完全一致**。两边不一致就会出现「挑的时候按 A 判、核对的时候按 B 判」，
    //    表现是「明明候选里有人对得上，结果还是被丢掉了」——极难排查。
    if (localTitle.empty() || res.matchedTrack.empty()) return false;
    {
        const std::wstring wantNorm =
            NormalizeLyricStem(StripEditionMarkerForMatch(res.matchedTrack));

        // ⚠️ 本地这边也要剥版本标记 —— 和源头用**同一套**。
        //
        // 实测（2026-09-24）：「最后的歌（LA LA LA）[Remastered]」和
        // 「心加心 [Remastered]」两条，源头都靠剥版本标记认对了，
        // 却在这里被判"对不上"丢掉 —— 因为这里只做 MakeTitleCandidates、
        // 没有剥 [Remastered]，归一化后是「…remastered」，
        // 和在线那边的干净曲名永远不相等。
        bool titleOk = false;
        for (const std::wstring& cand :
                 MakeTitleCandidates(StripEditionMarkerForMatch(localTitle))) {
            if (NormalizeLyricStem(cand) == wantNorm) { titleOk = true; break; }
        }
        if (!titleOk) return false;
    }

    // 2) 时长和演唱者 —— **二者其一对上即可**。
    //
    //    【为什么时长不能再当硬闸】用户 2026-09-24 拍板「宁可偏，也要有词」：
    //    他的 remaster 专辑《恋爱理论》里「心加心」本地 308.0s、网易云原版
    //    273.9s，差 34 秒 —— 但曲名全等、演唱者也对得上。同一个歌手的同一首
    //    曲子，差的是编曲长度，配上原版的词时间轴会整体偏，但比一片空白强。
    //
    //    【但两道不能一起失效】曲名对、时长差很多、演唱者又对不上，
    //    那多半是**同名的另一首歌**（D-024 实测：搜 "Best Wishes" 回来的是
    //    完全不相干的歌，328s vs 240s）。这种仍然要拒。
    //
    //    「?」「未知艺术家」是 foobar2000 对**没有这个字段**的占位符，
    //    不是歌手名 —— 先当空处理，否则无标签文件永远过不了演唱者这一关。
    const bool localArtistKnown  = !IsPlaceholderTag(localArtist);
    const bool onlineArtistKnown = !IsPlaceholderTag(res.matchedArtist);

    if (localDurationSec > 0.0 && res.matchedDuration > 0.0) {
        const double diff = res.matchedDuration - localDurationSec;
        if (diff <= kDurationToleranceSec && diff >= -kDurationToleranceSec) {
            return true;   // 曲名 + 时长都对上 -> 可信
        }
        // 时长差很多：只有当演唱者也对得上时才放行（见上面"两道不能一起失效"）
        if (localArtistKnown && onlineArtistKnown &&
            ArtistNamesOverlap(localArtist, res.matchedArtist)) {
            DebugLog("在线歌词：时长差 %.1f 秒（超过 %.0f 秒容差），"
                     "但演唱者对得上，仍采用",
                     diff, kDurationToleranceSec);
            return true;
        }
        return false;
    }

    // 3) 时长有一边缺（在线源偶尔不给 duration，本地也可能还没读到），
    //    只能核对演唱者。两边都缺就**不放行** —— 宁可不显示。
    if (!localArtistKnown || !onlineArtistKnown) return false;
    return ArtistNamesOverlap(localArtist, res.matchedArtist);
}

} // namespace

void PlaybackState::StartOnlineLookup() {
    // 主线程。这里面除了拼请求，还会调 fb2k::splitTask 起后台任务 ——
    // 如果 splitTask 本身有可观的开销，就会卡在这里。
    ScopedTimer timer("StartOnlineLookup（含 splitTask）", 5.0);

    if (m_trackUrl.empty()) return;

    if (m_tagTitle.empty()) {
        // 没有曲名查不了：LRCLIB 的 track_name 是必填，空着发出去只会拿 400。
        DebugLog("在线歌词：没有曲名标签，跳过");
        return;
    }
    if (m_onlinePendingUrl == m_trackUrl) return;   // 这首已经在查了

    m_onlinePendingUrl = m_trackUrl;
    const unsigned gen = m_onlineGeneration;        // 回来时用它判断有没有换曲

    OnlineLyricRequest req;
    req.title       = m_tagTitle;
    req.album       = m_tagAlbum;
    req.durationSec = m_lengthSec;

    // 占位符不能当歌手发出去。
    //
    // 用户那批无标签 WAV 的 %artist% 渲染出来是字面的「?」，而实测把它拼进
    // 搜索词之后，网易云返回的 5 条里后 4 条直接被带偏成「青山不改与君携」
    // 「一如既往」这类完全不相干的结果（干净的「遗忘山丘」则 5 条全都靠谱）。
    // 所以这里当空处理，让搜索词只剩曲名。
    req.artist = IsPlaceholderTag(m_tagArtist) ? std::wstring() : m_tagArtist;

    // ---- 用户按文件夹指定的线索 ----
    //
    // 【为什么需要】无标签的曲目只剩曲名一个搜索词，而实测这会被彻底带偏：
    //     查「哀歌」 -> 和田薫 / 平井堅 …（真答案 id=1333394828 连前 10 都没有）
    //     查「阿良良木健 哀歌」 -> 第 1 条就是它            ← 歌手是那把钥匙
    //
    // 【歌手进主查询，专辑只进兜底】两者可信度不一样：
    //   歌手是用户明确告诉我们"这首歌是谁的"，直接拼进主查询词，
    //   顺带也让**演唱者闸门**第一次能对无标签曲目起作用（它原来永远是死的）。
    //   专辑经常是猜的（文件夹名），猜错会把正确答案挤出前 10 —— 所以
    //   只作为主查询失败后的第三趟兜底（见 TryNetEase 的三趟结构）。
    const std::wstring folderKey = FolderKeyOf(m_trackPath);
    const FolderHint hint = GetFolderHint(folderKey);

    if (!hint.artist.empty() && req.artist.empty()) {
        // 只在标签**没有**歌手时才用线索覆盖 —— 标签是作者自己写的，优先级更高
        req.artist = hint.artist;
    }
    if (!hint.album.empty()) {
        req.searchHint = hint.album;
    } else if (m_tagAlbum.empty() || IsPlaceholderTag(m_tagAlbum)) {
        // 线索里没写专辑 -> 退回按文件夹名猜（猜得不一定准，但只影响兜底那一趟）
        req.searchHint = GuessAlbumHintFromPath(m_trackPath);
    }

    DebugLog("在线歌词：发起查询 [%s - %s] %.1fs%s",
             WideToUtf8(req.artist).c_str(), WideToUtf8(req.title).c_str(),
             req.durationSec,
             req.searchHint.empty() ? ""
                                    : ("  线索=" + WideToUtf8(req.searchHint)).c_str());

    const std::string url = m_trackUrl;

    // 记下**实际发出去查询的歌手**，回来核对时要用同一个。
    //
    // 直接拿 m_tagArtist 去核对是不行的：无标签曲目那里是占位符「?」，
    // 而真正的歌手来自用户填的文件夹线索 —— 两个值不一样，
    // 核对那一步就会"手里没有歌手可用"而把正确结果丢掉（见 ApplyOnlineResult）。
    m_onlineArtistUsed = req.artist;
    FetchLyricOnlineAsync(req, std::wstring(),
        [this, gen, url](OnlineLyricRequest, OnlineLyricResult res) {
            // 先查存活令牌再碰 this —— 理由见 g_onlineAlive 的声明处。
            if (!g_onlineAlive.alive.load()) return;
            ApplyOnlineResult(gen, url, res);
        });
}

void PlaybackState::ApplyOnlineResult(unsigned gen, const std::string& url,
                                      const OnlineLyricResult& res) {
    // 后台查询可能几十秒才回来，期间换歌是常态。
    if (gen != m_onlineGeneration || url != m_trackUrl) {
        DebugLog("在线歌词：结果已过期（期间换过曲），丢弃");
        return;
    }
    m_onlinePendingUrl.clear();

    if (!res.ok) {
        // 「确实没有」和「查询失败」在缓存层已经分开了（见 online_lyric.h），
        // 这里原样转述，不要自己再加判断。
        DebugLog("在线歌词：未命中 —— %s", WideToUtf8(res.error).c_str());
        return;
    }

    // 核对时用的歌手必须是**实际发出去查询的那个**，不能是标签里的。
    //
    // 【为什么】无标签曲目的 %artist% 是占位符「?」，用户给文件夹填了线索之后
    // 真正的歌手在 m_onlineArtistUsed 里。这里要是还用 m_tagArtist，
    // 就会出现「源头靠线索认出对了、调用方却因为手里没有歌手而丢掉」——
    //
    // 实测（2026-09-24）：
    //     「爸爸……（Interlude）」 本地 49.0s / 在线 64.0s    差 15 秒
    //     「春风来（Love Elegia Ver.）」 本地 240.0s / 在线 247.1s  差 7.1 秒
    // 两首曲名都对、演唱者也对（阿良良木健），就因为时长超出 ±5 秒、
    // 而"退一步看演唱者"这一步拿到的是空的，双双被丢。
    const std::wstring artistForCheck =
        m_onlineArtistUsed.empty() ? m_tagArtist : m_onlineArtistUsed;

    if (!OnlineResultTrustworthy(res, m_tagTitle, artistForCheck, m_lengthSec)) {
        DebugLog("在线歌词：结果与当前曲目对不上，丢弃。"
                 "在线=[%s - %s, %.1fs%s]  本地=[%s - %s, %.1fs]",
                 WideToUtf8(res.matchedArtist).c_str(),
                 WideToUtf8(res.matchedTrack).c_str(),
                 res.matchedDuration,
                 res.fromSearch ? ", 模糊搜索" : ", 精确查询",
                 WideToUtf8(artistForCheck).c_str(),
                 WideToUtf8(m_tagTitle).c_str(),
                 m_lengthSec);
        return;
    }

    // 先回落成 UTF-8 字节再交给解析器：它认的是字节流 + 自己嗅探编码，
    // 而我们手里已经是宽字符了，UTF-8 是唯一能无损走这一趟的窄编码。
    const std::string utf8 = WideToUtf8(res.lrcText);
    const std::vector<unsigned char> bytes(utf8.begin(), utf8.end());
    LyricDocument doc = LyricDocument::Parse(bytes);

    if (doc.IsEmpty()) {
        // 常见原因：拿到的是 plainLyrics（没有时间轴），解析不出带时间戳的行。
        // 这种歌词对逐行滚动没有意义，当作没拿到。
        DebugLog("在线歌词：解析不出带时间轴的行（可能是纯文本歌词），丢弃");
        return;
    }

    m_lyrics      = std::move(doc);
    m_lyricPath.clear();   // 歌词不在本地文件里，这个字段没有对应物
    m_currentLine = LyricDocument::npos;
    m_displayLine = LyricDocument::npos;
    ++m_revision;

    DebugLog("在线歌词：已载入 %zu 行（%s）",
             m_lyrics.Count(), res.fromCache ? "磁盘缓存" : "本次联网");
}

TickChange PlaybackState::RefreshPosition() {
    auto pc = playback_control::get();
    bool positionChanged = false;

    if (pc.is_valid()) {
        m_isPlaying = pc->is_playing();
        m_isPaused  = pc->is_paused();
        m_volumeDb  = pc->get_volume();

        if (m_isPlaying) {
            const double pos = pc->playback_get_position();
            if (pos != m_positionSec) { m_positionSec = pos; positionChanged = true; }
        }

        // 时长**故意放在 m_isPlaying 分支之外**：暂停时也要拿。
        // 在线歌词靠它核对 LRCLIB 的返回值，而暂停状态下永远进不了上面那个分支 ——
        // 放进去的话就永远等不到时长、永远不发起查询。
        m_lengthSec = pc->playback_get_length();
    }

    // 本地没找到歌词、且已经知道曲子多长 —— 现在才是发起在线查询的时候。
    // 不放在 OnNewTrack 里的理由见 m_onlineWanted 的声明处。
    if (m_onlineWanted && m_lengthSec > 0.0) {
        m_onlineWanted = false;
        StartOnlineLookup();
    }

    const size_t idx = m_lyrics.LineIndexAt(m_positionSec);
    if (idx != m_currentLine) {
        m_currentLine  = idx;
        m_displayLine  = m_lyrics.DisplayIndex(idx);
        return TickChange::Line;
    }
    return positionChanged ? TickChange::Position : TickChange::None;
}

size_t PlaybackState::DisplayLine() const {
    return m_displayLine;
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
