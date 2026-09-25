#include "source_order.h"

#include <algorithm>

namespace lyricus {
namespace {

// 配置里用的 id。**改这些字符串 = 用户设置丢失**，见头文件里的警告。
const wchar_t* const kIds[] = { L"netease", L"kugou", L"lrclib" };

const wchar_t* const kNames[] = { L"网易云", L"酷狗", L"LRCLIB" };

constexpr size_t kCount = sizeof(kIds) / sizeof(kIds[0]);

// 静态断言：枚举值和这两个表的长度必须一致。
// 加源时忘了往表里补一行是很容易发生的，而后果是越界读 —— 编译期挡住最省事。
static_assert(kCount == 3, "加了新源就要同步补 kIds / kNames 两张表");

size_t IndexOf(LyricSource s) { return static_cast<size_t>(s); }

std::wstring TrimWs(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto sp = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (b < e && sp(s[b])) ++b;
    while (e > b && sp(s[e - 1])) --e;
    return s.substr(b, e - b);
}

} // namespace

std::vector<LyricSource> AllSources() {
    std::vector<LyricSource> v;
    v.reserve(kCount);
    for (size_t i = 0; i < kCount; ++i) v.push_back(static_cast<LyricSource>(i));
    return v;
}

std::wstring SourceId(LyricSource s) {
    const size_t i = IndexOf(s);
    return (i < kCount) ? std::wstring(kIds[i]) : std::wstring();
}

bool SourceFromId(const std::wstring& id, LyricSource& out) {
    const std::wstring t = TrimWs(id);
    for (size_t i = 0; i < kCount; ++i) {
        if (t == kIds[i]) { out = static_cast<LyricSource>(i); return true; }
    }
    return false;
}

const wchar_t* SourceDisplayName(LyricSource s) {
    const size_t i = IndexOf(s);
    return (i < kCount) ? kNames[i] : L"?";
}

std::vector<SourcePref> ParseSourcePrefs(const std::wstring& text) {
    std::vector<SourcePref> out;
    const std::wstring t = TrimWs(text);

    if (t.empty()) {
        for (LyricSource s : AllSources()) out.push_back(SourcePref{ s, true });
        return out;
    }

    // 按逗号切。空段（`a,,b` 或结尾多一个逗号）直接跳过。
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(L',', pos);
        if (comma == std::wstring::npos) comma = t.size();
        const std::wstring item = TrimWs(t.substr(pos, comma - pos));
        pos = comma + 1;

        if (!item.empty()) {
            const size_t colon = item.find(L':');
            const std::wstring id = (colon == std::wstring::npos)
                                        ? item : item.substr(0, colon);
            bool enabled = true;
            if (colon != std::wstring::npos) {
                // `:0` 停用；其它任何值（`:1`、写错成 `:yes`）都当启用 ——
                // 宁可多试一个源，也不要因为拼写问题让某个源再也不工作。
                enabled = (TrimWs(item.substr(colon + 1)) != L"0");
            }

            LyricSource s;
            if (SourceFromId(id, s)) {
                const bool dup = std::any_of(out.begin(), out.end(),
                                             [s](const SourcePref& p) { return p.src == s; });
                if (!dup) out.push_back(SourcePref{ s, enabled });
            }
            // 认不出的 id 静默跳过：配置里可能留着已经删掉的源的名字
        }
        if (comma >= t.size()) break;
    }

    // 配置里没提到的源 -> 追加到末尾并启用（将来加了新源，老用户也能用上）
    for (LyricSource s : AllSources()) {
        const bool known = std::any_of(out.begin(), out.end(),
                                       [s](const SourcePref& p) { return p.src == s; });
        if (!known) out.push_back(SourcePref{ s, true });
    }
    return out;
}

std::wstring FormatSourcePrefs(const std::vector<SourcePref>& prefs) {
    std::wstring s;
    for (const SourcePref& p : prefs) {
        const std::wstring id = SourceId(p.src);
        if (id.empty()) continue;   // 表里没有的（理论上不该出现）不写进去
        if (!s.empty()) s += L',';
        s += id;
        s += p.enabled ? L":1" : L":0";
    }
    return s;
}

std::vector<LyricSource> EnabledSources(const std::vector<SourcePref>& prefs) {
    std::vector<LyricSource> v;
    for (const SourcePref& p : prefs) {
        if (p.enabled) v.push_back(p.src);
    }
    return v;
}

std::vector<LyricSource> SourcesToTry(const std::wstring& text) {
    return EnabledSources(ParseSourcePrefs(text));
}

std::vector<SourcePref> MoveSourcePref(const std::vector<SourcePref>& prefs,
                                       size_t index, int delta) {
    if (delta == 0) return prefs;
    if (index >= prefs.size()) return prefs;   // 没选中任何一项

    const int target = static_cast<int>(index) + delta;
    if (target < 0 || target >= static_cast<int>(prefs.size())) return prefs;   // 到边界

    std::vector<SourcePref> v = prefs;
    std::swap(v[index], v[static_cast<size_t>(target)]);
    return v;
}

} // namespace lyricus
