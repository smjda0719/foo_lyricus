#include "folder_hint_table.h"
#include "lyric.h"        // WideToUtf8 / Utf8ToWide（纯转换，不碰 SDK）

#include <algorithm>

// ---------------------------------------------------------------------------
// 歌词线索表的纯逻辑。见 folder_hint_table.h 里"为什么单独一层"。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

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
    const size_t begin = (entries.size() > kMaxFolderHints)
                       ? entries.size() - kMaxFolderHints : 0;
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

FolderHint LookupFolderHint(const std::string& text, const std::wstring& folderKey) {
    if (folderKey.empty()) return FolderHint();
    for (const auto& e : ParseFolderHints(text)) {
        if (e.first == folderKey) return e.second;
    }
    return FolderHint();
}

std::string ApplyFolderHintEdit(const std::string& currentText,
                                const std::wstring& folderKey,
                                const FolderHint& hint,
                                bool* changedOut) {
    if (changedOut != nullptr) *changedOut = false;
    if (folderKey.empty()) return currentText;   // 空键：不动，也不算改动

    auto entries = ParseFolderHints(currentText);

    FolderHint h = hint;
    Sanitize(h.artist);
    Sanitize(h.album);

    bool  found   = false;
    bool  changed = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].first != folderKey) continue;

        found = true;
        if (h.Empty()) {
            // 清空 = 删除这一条。
            //
            // ⚠️ 这里刻意用**下标** erase，而不是原先那种
            //     `erase(remove(begin, end, e), end)`：那个写法把 e（一个引用）
            //     当成 remove 的 value 传进去，而 remove 会在搬移元素的过程中
            //     把那块内存覆盖掉 —— 属于"读了正在被移动的对象"。
            //     既然已经知道下标，直接 erase 又简单又没这个隐患。
            entries.erase(entries.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
        } else if (!(entries[i].second == h)) {
            entries[i].second = h;
            changed = true;
        }
        break;
    }

    if (!found && !h.Empty()) {
        entries.emplace_back(folderKey, h);
        changed = true;
    }

    if (changedOut != nullptr) *changedOut = changed;
    return changed ? FormatFolderHints(entries) : currentText;
}

} // namespace lyricus
