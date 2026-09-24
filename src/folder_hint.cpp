#include "stdafx.h"
#include "folder_hint.h"
#include "config.h"
#include "debug_log.h"
#include "lyric.h"          // WideToUtf8 / Utf8ToWide
#include "online_lyric.h"   // InvalidateMissMarkers

#include <algorithm>

namespace lyricus {
namespace {

constexpr size_t kMaxEntries = 512;

// 文本表里的一行拆成三段。TAB 的**个数**决定字段含义 ——
// 歌手/专辑允许为空，不能靠"值非空"来判断。
bool SplitLine(const std::string& line, std::string& key, std::string& artist, std::string& album) {
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) return false;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) return false;
    key    = line.substr(0, t1);
    artist = line.substr(t1 + 1, t2 - t1 - 1);
    album  = line.substr(t2 + 1);
    return !key.empty();
}

// 字段里出现 TAB 或换行会把整张表撑坏 —— 存进去之前先清掉。
// 用空格替换而不是直接删：文件夹名里的空格是有意义的。
void Sanitize(std::wstring& s) {
    for (wchar_t& c : s) {
        if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    }
}

} // namespace

std::wstring FolderKeyOf(const std::wstring& audioPath) {
    if (audioPath.empty()) return std::wstring();

    // 取到最后一个分隔符为止（含）—— 也就是"文件所在的目录"
    const size_t sep = audioPath.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return std::wstring();

    std::wstring dir = audioPath.substr(0, sep);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) return std::wstring();

    // 统一小写：Windows 路径大小写不敏感，用户在不同地方敲出来的大小写可能不同
    std::transform(dir.begin(), dir.end(), dir.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return dir;
}

std::string FormatFolderHints(const std::vector<std::pair<std::wstring, FolderHint>>& entries) {
    std::string s;
    // 满了就丢最旧的（从头丢）—— 和手动歌词表同一套策略
    const size_t begin = (entries.size() > kMaxEntries) ? entries.size() - kMaxEntries : 0;
    for (size_t i = begin; i < entries.size(); ++i) {
        const std::wstring& key = entries[i].first;
        if (key.empty()) continue;
        s += WideToUtf8(key);
        s += '\t';
        s += WideToUtf8(entries[i].second.artist);
        s += '\t';
        s += WideToUtf8(entries[i].second.album);
        s += '\n';
    }
    return s;
}

std::vector<std::pair<std::wstring, FolderHint>> ParseFolderHints(const std::string& text) {
    std::vector<std::pair<std::wstring, FolderHint>> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);

        std::string key, artist, album;
        if (SplitLine(line, key, artist, album)) {
            FolderHint h;
            h.artist = Utf8ToWide(artist.c_str());
            h.album  = Utf8ToWide(album.c_str());
            out.emplace_back(Utf8ToWide(key.c_str()), std::move(h));
        }
        pos = eol + 1;
    }
    return out;
}

FolderHint GetFolderHint(const std::wstring& folderKey) {
    if (folderKey.empty()) return FolderHint();

    const pfc::string8 raw = cfg_folder_hints.get();
    const std::string s(raw.get_ptr(), raw.length());
    for (const auto& e : ParseFolderHints(s)) {
        if (e.first == folderKey) return e.second;
    }
    return FolderHint();
}

void SetFolderHint(const std::wstring& folderKey, const FolderHint& hint) {
    if (folderKey.empty()) return;

    const pfc::string8 raw = cfg_folder_hints.get();
    const std::string s(raw.get_ptr(), raw.length());
    auto entries = ParseFolderHints(s);

    FolderHint h = hint;
    Sanitize(h.artist);
    Sanitize(h.album);

    bool replaced = false;
    for (auto& e : entries) {
        if (e.first == folderKey) {
            if (h.Empty()) {
                // 清空 = 删除这一条
                entries.erase(std::remove(entries.begin(), entries.end(), e), entries.end());
            } else {
                e.second = h;
            }
            replaced = true;
            break;
        }
    }
    // 没找到且不为空 -> 追加
    if (!replaced && !h.Empty()) entries.emplace_back(folderKey, h);

    const std::string text = FormatFolderHints(entries);
    cfg_folder_hints = text.c_str();

    DebugLog("歌词线索：%s -> 歌手=「%s」 专辑=「%s」（表内共 %zu 条）",
             h.Empty() ? "已清除" : "已保存",
             WideToUtf8(h.artist).c_str(), WideToUtf8(h.album).c_str(),
             entries.size());

    // ★ 改了线索，之前用**旧查询**算出来的「没有歌词」就不算数了。
    //
    // 不清的话会出一个很难理解的现象：用户在菜单里填了歌手、界面毫无反应 ——
    // 因为那个曲目早就被写下 7 天有效的未命中标记，连接网都不联。
    // 清掉之后下次换曲/重播就会带着新线索重新查。
    InvalidateMissMarkers();
}

size_t FolderHintCount() {
    const pfc::string8 raw = cfg_folder_hints.get();
    const std::string s(raw.get_ptr(), raw.length());
    return ParseFolderHints(s).size();
}

} // namespace lyricus
