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

    // 这一行是不是**参照行**（也就是合并进来的翻译）。
    //
    // 判据是"和上一行同一时间戳"—— 双语歌词就是这么写的
    //（见 online_lyric.cpp 的 MergeTranslationLines：原文在前、翻译紧随其后、同戳）。
    // ⚠️ 没给 LyricLine 加字段是有意的：加了就得改解析器，
    //    而那是全工程共用的（D-038 那次一改就打挂 26 条断言）。
    //    这里靠"同戳"这个**格式本身**来判断，解析器一行不用动。
    //
    // 【为什么是文档的方法而不是渲染层的私有助手】它不是"怎么画"的问题，
    // 而是"这份文档长什么样"的问题 —— 除了渲染层，PlaybackState 算
    // **显示序号**（DisplayLine）时也要用它。放在渲染层的匿名命名空间里
    // 只有一个文件能用，第二个需要它的地方就只好抄一份（D-044 记过
    // "同一个判据散在多处"这个反复咬人的坑）。
    bool IsSubLine(size_t index) const;

    // 把行号换算成**显示序号**：这一行是第几句"真正要显示的歌词"。
    //
    // 参照行不占序号 —— 于是"下一句"永远是 +1，而原始行号每推进一个时间戳
    // 会跳 2（原文 + 翻译两行）。换行上滑的判据必须用显示序号：
    // 用原始行号的话 `== 上一行 + 1` 几乎永远不成立
    //（实测 34→35→37→38→41…，见 D-046）。
    //
    // index 为 npos 或越界时返回 npos。
    size_t DisplayIndex(size_t index) const;

    // 从内存解析，自动识别编码（UTF-8 / UTF-16 BOM / GB18030 兜底）
    static LyricDocument Parse(const std::vector<unsigned char>& bytes);

    // 从文件加载。失败时返回空文档（不抛异常）。
    static LyricDocument LoadFromFile(const std::wstring& path);

    // 供日志用的简短描述
    std::wstring Describe() const;

private:
    std::vector<LyricLine> m_lines;
};

// 歌词**文件的查找**已经搬到 lyric_search.h 的 FindLyricFile() ——
// 那边做的是多策略匹配（精确 / 去前缀 / 标签构造 / 模糊），
// 不再是「同目录同名」这一条。
//
// 原来的 MakeLyricPathForAudio() 已删除：它只覆盖最理想的一种摆放
// （D:\music\a.flac -> D:\music\a.lrc），实测遇到「厂牌 - 曲名.wav」
// 配「曲名.lrc」必然落空。留着会让人误以为那就是当前的匹配逻辑。

// UTF-8 窄字符串 -> 宽字符串（用于 SDK 返回的 UTF-8 路径）
std::wstring Utf8ToWide(const char* utf8);

// 宽字符串 -> UTF-8 窄字符串
std::string WideToUtf8(const std::wstring& wide);

} // namespace lyricus
