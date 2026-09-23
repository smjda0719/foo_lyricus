#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 歌词数据模型与 LRC 解析
//
// 设计取舍：
//  * 内部统一用「秒（double）+ 宽字符串」，解析阶段就把各种时间戳精度归一化，
//    避免精度问题渗透到渲染层；
//  * 暂时只做「行级」歌词。逐字（增强型 LRC 的 <mm:ss.xx> 词级标签）留到后面，
//    数据模型上预留了扩展位。
// ---------------------------------------------------------------------------

namespace lyricus {

struct LyricLine {
    double       timeSec = 0.0;
    std::wstring text;
};

class LyricDocument {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    bool   IsEmpty() const { return m_lines.empty(); }
    size_t Count()   const { return m_lines.size(); }

    const LyricLine& At(size_t index) const { return m_lines[index]; }
    const std::vector<LyricLine>& Lines() const { return m_lines; }

    // 返回 timeSec 时刻应显示的行号：
    //   早于第一行 -> 0；晚于最后一行 -> 最后一行；没有歌词 -> npos
    size_t LineIndexAt(double timeSec) const;

    // 从内存解析，自动识别编码（UTF-8 / UTF-16 BOM / GB18030 兜底）
    static LyricDocument Parse(const std::vector<unsigned char>& bytes);

    // 从文件加载。失败时返回空文档（不抛异常）。
    static LyricDocument LoadFromFile(const std::wstring& path);

    // 供日志用的简短描述
    std::wstring Describe() const;

private:
    std::vector<LyricLine> m_lines;
};

// 由音频文件路径推导同目录同名的 .lrc 路径。
// 例：D:\music\a.flac -> D:\music\a.lrc
std::wstring MakeLyricPathForAudio(const std::wstring& audioPath);

// UTF-8 窄字符串 -> 宽字符串（用于 SDK 返回的 UTF-8 路径）
std::wstring Utf8ToWide(const char* utf8);

// 宽字符串 -> UTF-8 窄字符串
std::string WideToUtf8(const std::wstring& wide);

} // namespace lyricus
