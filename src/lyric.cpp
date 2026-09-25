#include "stdafx.h"
#include "lyric.h"
#include "debug_log.h"

#include <algorithm>
#include <cstring>    // memcpy / strlen / _wcsicmp（MSVC 扩展，显式写出不靠传递包含）
#include <cwctype>

namespace lyricus {
namespace {

bool ToDouble(const std::wstring& s, double& out) {
    if (s.empty()) return false;
    double v = 0.0;
    for (wchar_t ch : s) {
        if (ch < L'0' || ch > L'9') return false;
        v = v * 10.0 + (ch - L'0');
    }
    out = v;
    return true;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
    };
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 判断是不是 [ti:] [ar:] [offset:] 这类元数据标签（而不是时间戳）
bool LooksLikeMetadataTag(const std::wstring& tag) {
    const size_t colon = tag.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        if (!iswalpha(tag[i])) return false;
    }
    return true;
}

// 解析 [mm:ss] / [mm:ss.fff] / [mm:ss:fff] / [hh:mm:ss.fff]
//
// 【关键】小数位数决定量级：1 位 = 十分之一秒，2 位 = 厘秒，3 位 = 毫秒。
// 写死成两位会把 [00:01.094] 这种毫秒精度歌词整体读偏 —— 单看某一行不明显，
// 但整首歌会稳定偏移几十到几百毫秒。
bool ParseTimestampTag(const std::wstring& tag, double& outSec) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t ch : tag) {
        if (ch == L':') { parts.push_back(cur); cur.clear(); }
        else            { cur.push_back(ch); }
    }
    parts.push_back(cur);
    if (parts.size() < 2) return false;

    std::wstring last = parts.back();
    parts.pop_back();

    std::wstring secStr = last, fracStr;
    const size_t dot = last.find(L'.');
    if (dot != std::wstring::npos) {
        secStr  = last.substr(0, dot);
        fracStr = last.substr(dot + 1);
    }

    double hours = 0.0, minutes = 0.0, seconds = 0.0;
    if (parts.size() == 1) {
        if (!ToDouble(parts[0], minutes)) return false;
    } else if (parts.size() == 2) {
        if (!ToDouble(parts[0], hours) || !ToDouble(parts[1], minutes)) return false;
    } else {
        return false;
    }
    if (!ToDouble(secStr, seconds)) return false;

    double frac = 0.0;
    if (!fracStr.empty()) {
        double raw = 0.0;
        if (!ToDouble(fracStr, raw)) return false;
        double scale = 1.0;
        for (size_t i = 0; i < fracStr.size(); ++i) scale *= 10.0;
        frac = raw / scale;
    }

    outSec = hours * 3600.0 + minutes * 60.0 + seconds + frac;
    return true;
}

std::wstring Utf8ToWideStrict(const char* p, int len, bool& ok) {
    ok = false;
    if (len <= 0) { ok = true; return std::wstring(); }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, &out[0], n);
    ok = true;
    return out;
}

// 编码识别顺序：BOM -> 严格 UTF-8 -> GB18030 兜底
std::wstring DecodeToWide(const std::vector<unsigned char>& b) {
    if (b.empty()) return std::wstring();

    auto fromCodePage = [&b](UINT cp, size_t skip) -> std::wstring {
        const char* p = reinterpret_cast<const char*>(b.data()) + skip;
        const int len = static_cast<int>(b.size() - skip);
        if (len <= 0) return std::wstring();
        const int n = MultiByteToWideChar(cp, 0, p, len, nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(cp, 0, p, len, &out[0], n);
        return out;
    };

    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        DebugLog("歌词编码: UTF-8 (BOM)");
        return fromCodePage(CP_UTF8, 3);
    }
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        DebugLog("歌词编码: UTF-16 LE");
        const size_t chars = (b.size() - 2) / 2;
        std::wstring out(chars, L'\0');
        memcpy(&out[0], b.data() + 2, chars * 2);
        return out;
    }
    if (b.size() >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        DebugLog("歌词编码: UTF-16 BE");
        const size_t chars = (b.size() - 2) / 2;
        std::wstring out(chars, L'\0');
        for (size_t i = 0; i < chars; ++i) {
            out[i] = static_cast<wchar_t>((b[2 + i * 2] << 8) | b[3 + i * 2]);
        }
        return out;
    }

    bool ok = false;
    std::wstring utf8 = Utf8ToWideStrict(reinterpret_cast<const char*>(b.data()),
                                        static_cast<int>(b.size()), ok);
    if (ok) {
        DebugLog("歌词编码: UTF-8 (无 BOM)");
        return utf8;
    }

    DebugLog("歌词编码: 非 UTF-8，按 GB18030 兜底");
    return fromCodePage(54936, 0);   // 54936 = GB18030
}

} // namespace

std::wstring Utf8ToWide(const char* utf8) {
    if (utf8 == nullptr || *utf8 == '\0') return std::wstring();
    bool ok = false;
    return Utf8ToWideStrict(utf8, static_cast<int>(strlen(utf8)), ok);
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

size_t LyricDocument::LineIndexAt(double timeSec) const {
    if (m_lines.empty()) return npos;
    if (timeSec < m_lines.front().timeSec) return 0;

    // 找最后一个 timeSec <= 目标时间的行
    size_t lo = 0, hi = m_lines.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (m_lines[mid].timeSec <= timeSec) lo = mid + 1;
        else                                 hi = mid;
    }
    size_t idx = (lo == 0) ? 0 : lo - 1;

    // ★ 往回退到**同一时间戳组的第一行**。
    //
    // 【为什么】双语歌词是"同一条时间戳、原文在前、翻译在后"两行
    //（见 online_lyric.cpp 的 MergeTranslationLines）。
    // 上面那个二分找的是"最后一条 ≤ t 的行"，也就是**翻译**那一行 ——
    // 于是面板会把小字翻译当成主行来高亮，原文反倒成了"上一句"。
    // 退到组首就回到了原文，翻译由渲染层作为参照行画在它下面。
    //
    // 【对普通 LRC 是完全的无操作】没有重复时间戳时组里只有一行，
    // 循环一次都不进 —— 所以这条改动不影响任何既有行为。
    while (idx > 0 && m_lines[idx - 1].timeSec == m_lines[idx].timeSec) --idx;
    return idx;
}

bool LyricDocument::IsSubLine(size_t index) const {
    if (index == 0 || index >= m_lines.size()) return false;   // 第 0 行不可能是参照行
    const double a = m_lines[index].timeSec;
    const double b = m_lines[index - 1].timeSec;
    const double d = (a > b) ? (a - b) : (b - a);
    return d < 0.01;
}

size_t LyricDocument::DisplayIndex(size_t index) const {
    if (index == npos || index >= m_lines.size()) return npos;

    // 参照行归到它所属的那一句：往回退到组首。
    //
    // 调用方正常传进来的都是组首（LineIndexAt 退过了），所以这个循环
    // 在真实路径上一次都不进。但把"参照行不占序号"写成一句空话，
    // 不如让它**真的**成立 —— 否则这是一个将来一定会咬人的隐含前提
    //（单测当场就把这条抓出来了）。
    while (index > 0 && IsSubLine(index)) --index;

    // 数一数它前面有多少行"真正要显示的"。O(n)，但 n 是一首歌的行数
    //（几十），而且调用点每 250ms 才一次 —— 不值得为它做缓存。
    size_t ordinal = 0;
    for (size_t i = 0; i < index; ++i) {
        if (!IsSubLine(i)) ++ordinal;
    }
    return ordinal;
}

LyricDocument LyricDocument::Parse(const std::vector<unsigned char>& bytes) {    LyricDocument doc;
    const std::wstring text = DecodeToWide(bytes);
    if (text.empty()) return doc;

    double offsetSec = 0.0;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();

        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        // 一行可以有多个时间戳（[t1][t2]歌词），逐个收集
        std::vector<double> times;
        size_t i = 0;
        while (i < line.size() && line[i] == L'[') {
            const size_t close = line.find(L']', i);
            if (close == std::wstring::npos) break;
            const std::wstring tag = line.substr(i + 1, close - i - 1);

            double t = 0.0;
            if (ParseTimestampTag(tag, t)) {
                times.push_back(t);
            } else if (LooksLikeMetadataTag(tag)) {
                // [offset:+500] 之类：只处理 offset，其余忽略
                const size_t colon = tag.find(L':');
                const std::wstring key = tag.substr(0, colon);
                if (_wcsicmp(key.c_str(), L"offset") == 0) {
                    const std::wstring val = Trim(tag.substr(colon + 1));
                    double ms = 0.0;
                    size_t d = 0;
                    if (!val.empty() && (val[0] == L'+' || val[0] == L'-')) d = 1;
                    if (ToDouble(val.substr(d), ms)) {
                        offsetSec = (val[0] == L'-') ? -ms / 1000.0 : ms / 1000.0;
                    }
                }
            } else {
                break;
            }
            i = close + 1;
        }

        if (!times.empty()) {
            const std::wstring content = Trim(line.substr(i));
            // 空文本行不计入 —— 夹具里歌词之间夹着空行，不能渲染成空行
            if (!content.empty()) {
                for (double t : times) {
                    LyricLine l;
                    l.timeSec = t;
                    l.text = content;
                    doc.m_lines.push_back(std::move(l));
                }
            }
        }

        if (end == text.size()) break;
        pos = end + 1;
    }

    std::stable_sort(doc.m_lines.begin(), doc.m_lines.end(),
                     [](const LyricLine& a, const LyricLine& b) {
                         return a.timeSec < b.timeSec;
                     });

    if (offsetSec != 0.0) {
        for (auto& l : doc.m_lines) l.timeSec += offsetSec;
    }
    return doc;
}

LyricDocument LyricDocument::LoadFromFile(const std::wstring& path) {
    LyricDocument doc;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return doc;   // 文件不存在是常态，不刷日志
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (16 << 20)) {
        CloseHandle(h);
        return doc;
    }

    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(h);

    if (!ok || read == 0) return doc;
    bytes.resize(read);

    doc = Parse(bytes);
    DebugLog("歌词已加载: %s  行数=%zu", WideToUtf8(path).c_str(), doc.Count());
    return doc;
}

std::wstring LyricDocument::Describe() const {
    wchar_t buf[128];
    swprintf_s(buf, L"%zu 行歌词", m_lines.size());
    return buf;
}

} // namespace lyricus
