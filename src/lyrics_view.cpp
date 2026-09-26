#include "stdafx.h"
#include "lyrics_view.h"
#include "playback_state.h"

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace lyricus {
namespace {

HFONT CreateFontNow(int dpi, int pt, bool bold) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0,
                       bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY,   // 透明底上用 ANTIALIASED，ClearType 依赖不透明背景
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// 字体缓存。
//
// 【为什么要缓存】CreateFontW 比看上去贵得多 —— 它要解析字体名、去字体库里查匹配，
// 不是简单分配一个对象。而 DrawLyricsView 每次重绘都要造 3 个字体。
// 实测「面板定时器一拍」稳定在 5~9ms，其中一条就是从这儿来的；
// 而面板**每 250ms 就要重绘一次**（进度条在动），所以这是持续开销，不是偶发。
//
// 字体句柄进程内一直留着不释放 —— 就那么几个，不值得为它维护生命周期。
// 缓存键是 (dpi, 字号, 粗体)：DPI 会随显示器变化，不改这三个参数就不用重建。
//
// 线程约定：渲染层只在主线程用（和整个工程一致），所以静态 map 不需要加锁。
HFONT MakeFont(int dpi, int pt, bool bold) {
    static std::map<std::tuple<int, int, bool>, HFONT> cache;

    const auto key = std::make_tuple(dpi, pt, bold);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    HFONT f = CreateFontNow(dpi, pt, bold);
    cache.emplace(key, f);
    return f;
}

// 文本行的测量标志：**永远按单行量**。
//
// 高度的语义是"这一行占多高"，和它有多宽无关 —— 单行测量的结果只由字体决定，
// 于是高度缓存可以拿 (字体, 文本) 当键（见下面 HeightCache 的注释）。
//
// ⚠️ DT_NOPREFIX 不能省：歌词里出现 `&` 时，没有它会被当成助记符前缀吃掉。
//    测量和绘制**必须都带上它**，否则量出来的和画出来的对不上。
constexpr UINT kMeasureFlags = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX;

// 绘制标志：放不下用省略号收尾。
//
// 【为什么不是折行】2026-09-25 用户看过一版折行之后反馈「不太美观」。
// 折行的真正代价是**行高不再一致**：一句长词折成三行，上下文的排版被顶得
// 参差不齐，而面板本来就只显示 2~3 行。省略号只占一行，让版面的垂直节奏
// 重新变得可预测 —— 这也是下一步「长歌词横滚」能成立的前提。
// （折行那一版是 4d1c2e8，方向没错但代价太大，这里撤掉。）
//
// 【为什么不是光秃秃的 DT_SINGLELINE】那正是 2026-09-25 之前的行为：
// 用户截图里「池光化新茶 掺着新雪 煨了炉火曾入」右边顶到边就没了，
// 连"后面还有字"都看不出来。DT_END_ELLIPSIS 至少给一个 `…`。
//
// **下一步**会把当前行换成横滚（单行、不带省略号、靠裁剪区限宽），
// 那时这一档只留给上下文行、参照行和曲名。
constexpr UINT kDrawEllipsisFlags =
    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;

// 绘制标志：当前行专用 —— 单行、**不省略**。
//
// 它的宽度由 DC 裁剪区（遮罩）负责，而不是由 DrawTextW 的矩形负责：
// 横滚要的是「文字位置在动、裁剪框不动」，只有裁剪区能做到。
// 所以这里既不能折行、也不能省略号 —— 一省略就等于把要滚的内容先扔了。
constexpr UINT kDrawScrollFlags = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX;

// 文本行的高度缓存。
//
// 【为什么要缓存】DrawTextW(DT_CALCRECT) 不是免费操作 —— 要走一遍文本整形。
// 而一帧里同一行会被量 2~3 次（先 MeasureLine 探路，再 DrawLine 里又量一次），
// 可见的 20 行就是四五十次；面板每 250ms 重绘一次，同一批字每秒被量近 200 次，
// 而它们中间绝大多数根本没变。
//
// 键只带 (字体, 文本)：测量走 kMeasureFlags，是**单行**的，高度只由字体决定。
// ⚠️ 哪天恢复折行，必须把 maxWidth 加回键里 —— 折行之后高度和可用宽度有关，
//    漏了它会"面板一改宽度就命中旧高度"，折行数对不上、排版直接错位。
//
// 容量超了直接清空重来 —— 一首歌的可见行数就那么几十条，
// 真清空也是极低频事件，不值得为它维护 LRU。
std::map<std::pair<HFONT, std::wstring>, int>& HeightCache() {
    static std::map<std::pair<HFONT, std::wstring>, int> cache;
    return cache;
}

// ⚠️ 字体由**调用方**选进 DC（MeasureLine / DrawLine 都先 SelectObject）。
//    所以这里不接 font 参数 —— 从前接了一个却从不使用（C4100），
//    而缓存键里又拿它当依据，等于给"忘了选字体"留了个后门。
int MeasureLineRaw(HDC dc, const wchar_t* text, int maxWidth) {
    RECT r{ 0, 0, maxWidth, 0 };
    DrawTextW(dc, text, -1, &r, kMeasureFlags | DT_CALCRECT);
    return r.bottom - r.top;
}

int CachedLineHeight(HDC dc, const wchar_t* text, HFONT font, int maxWidth) {
    auto& cache = HeightCache();

    const auto key = std::make_pair(font, std::wstring(text));
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    const int h = MeasureLineRaw(dc, text, maxWidth);

    if (cache.size() >= 1024) cache.clear();
    cache.emplace(key, h);
    return h;
}

// 只量高度，不画。用于「先算总高再决定铺几行」。
int MeasureLine(HDC dc, const wchar_t* text, HFONT font, int maxWidth) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    const int h = CachedLineHeight(dc, text, font, maxWidth);
    SelectObject(dc, oldFont);
    return h;
}

// 一整行的**像素宽度**（不折、不省略）。
//
// 用 GetTextExtentPoint32W 而不是 DrawTextW(DT_CALCRECT)：
//   * 它不认 DT_NOPREFIX，但也**不特殊对待 `&`** —— 和 kDrawScrollFlags
//     的画法正好一致（那条路也带 DT_NOPREFIX）。两边对齐，不会差一个字符。
//   * 便宜：不用走一遍 DrawText 的排版流程。
//
// 这个宽度是"要不要滚、滚多远"的唯一依据，所以必须和真正画出来的宽度一致。
int MeasureLineWidth(HDC dc, const wchar_t* text, HFONT font) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE sz{ 0, 0 };
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
    SelectObject(dc, oldFont);
    return sz.cx;
}

// 画一行，返回「实际高度 + 行距」供调用方推进光标。
// 用 DT_CALCRECT 量真实高度而不是写死像素 —— 这是 DPI 无关的关键。
//
// flags 默认省略号；下一步「长歌词横滚」会给当前行传不带省略号的那一档
//（那时宽度由 DC 裁剪区负责，见 DrawLyricsView 里的遮罩）。
int DrawLine(HDC dc, const wchar_t* text, int x, int y, int maxWidth,
             HFONT font, COLORREF color, int gapAfter,
             UINT flags = kDrawEllipsisFlags) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);

    // 高度直接取缓存，不再为了「量一下」多画一次 DrawTextW。
    const int height = CachedLineHeight(dc, text, font, maxWidth);

    RECT draw{ x, y, x + maxWidth, y + height };
    DrawTextW(dc, text, -1, &draw, flags);

    SelectObject(dc, oldFont);
    return height + gapAfter;
}

// 百分比字号 -> 实际 pt。
//
// **两级相乘**：先乘用户设的 fontPct，再乘面板尺寸带来的 panelScalePct
//（后者由宿主决定要不要给，见 lyrics_view.h 里那个字段的说明）。
//
// 刻意**不让渲染层自己按面板高度缩放字号**：那会让同一个设置值在独立面板
// （460x150 逻辑像素）和 DUI 元素（可能很高）里得到不同字号，用户根本没法
// 预期自己调的是什么。面板那一级之所以安全，是因为它**显式且有界**
//（PanelFontScalePct 夹在 80~160），而不是拿 rc 去无限联动。
int ScalePt(int basePt, int pct, int panelScalePct) {
    const int user = MulDiv(basePt, pct, 100);
    const int pt   = MulDiv(user, panelScalePct, 100);
    return (pt < 1) ? 1 : pt;
}

// 夹取到 [lo, hi]。
// 不用 std::clamp：本文件里 windows.h 的 min/max 宏和 <algorithm> 的老问题
// 已经踩过一次（std::max 解析不出来），索性全部手写比较。
int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 「这一行是不是参照行（合并进来的翻译）」已经搬到 LyricDocument::IsSubLine ——
// 它不是"怎么画"的问题，而是"这份文档长什么样"的问题，PlaybackState 算显示序号时
// 也要用它。留在这里的话就只有本文件能用，第二个需要它的地方只好再抄一份。
// （D-044 记过"同一个判据散在多处"这个反复咬人的坑，别再犯。）

// 第 i 行的**正文**文本（i 必须不是参照行）。
//
// tlPrimary = false：正文是原文。
// tlPrimary = true ：正文是翻译 —— 听日语/同人曲的人很多只看得懂译文，
//                    原文对他们反而是参照（用户 2026-09-24 提的）。
//                    那一行没有翻译时**退回原文**，不能空着。
const std::wstring& MainTextOf(const LyricDocument& doc, size_t i, bool tlPrimary) {
    if (tlPrimary && doc.IsSubLine(i + 1)) return doc.At(i + 1).text;
    return doc.At(i).text;
}

// 第 i 行的**参照行**文本（小字，画在正文下面）。没有就返回 nullptr。
//
// 参照行正好是"配对的另一半"：
//   原文为主 -> 参照行是翻译（紧跟其后、同时间戳那一行）
//   翻译为主 -> 参照行是原文（就是它自己那一行）
const std::wstring* SubTextOf(const LyricDocument& doc, size_t i, bool tlPrimary) {
    if (tlPrimary) {
        // 没有翻译时不该凭空多出一行"原文参照"—— 那等于把同一句话显示两遍
        return doc.IsSubLine(i + 1) ? &doc.At(i).text : nullptr;
    }
    return doc.IsSubLine(i + 1) ? &doc.At(i + 1).text : nullptr;
}

} // namespace

int PanelFontScalePct(int panelWidthLogical, int baseWidthLogical) {
    if (panelWidthLogical <= 0 || baseWidthLogical <= 0) return 100;

    int pct = MulDiv(panelWidthLogical, 100, baseWidthLogical);

    // 夹在 [80, 160]：最扁 0.8 倍、最鼓 1.6 倍。
    // 面板能被拖得很宽（实测 992 逻辑像素），不夹的话字号会一路涨上去。
    if (pct < 80)  pct = 80;
    if (pct > 160) pct = 160;
    return pct;
}

LyricsViewResult DrawLyricsView(HDC dc, const RECT& rc, const LyricsViewTheme& theme,
                                const LyricsViewLayout& layout,
                                const LyricAnimFrame& anim) {
    LyricsViewResult result;
    if (dc == nullptr) { result.bottom = rc.top; return result; }

    SetBkMode(dc, TRANSPARENT);

    const int dpi = (theme.dpi > 0) ? theme.dpi : 96;
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };

    const int padX  = S(20);
    int maxW = (rc.right - rc.left) - padX * 2;
    if (maxW < 1) maxW = 1;      // 面板窄到不合理时兜底，避免负宽度
    const int left  = rc.left + padX;
    const int top   = rc.top + S(14);

    // 歌词只画到 limit 为止。浮动面板把控制条那一段排除掉（clipBottom），
    // DUI/CUI 整个元素都是歌词区（clipBottom = 0 -> 用 rc.bottom）。
    //
    // ⚠️ limit 只决定**画在哪儿**，不参与 currentRatio —— 那个基准是 rc
    //    整个面板。两者分开正是为了让"控制条高度"这种装饰不影响居中（D-043）。
    int limit = rc.bottom;
    if (layout.clipBottom > rc.top && layout.clipBottom < limit) limit = layout.clipBottom;

    const int pct = (layout.fontPct > 0) ? layout.fontPct : 100;

    HFONT fHeader  = MakeFont(dpi, ScalePt(11, pct, layout.panelScalePct), false);   // 曲名：刻意比歌词小，别抢戏
    HFONT fCurrent = MakeFont(dpi, ScalePt(15, pct, layout.panelScalePct), true);    // 当前歌词行
    HFONT fBody    = MakeFont(dpi, ScalePt(11, pct, layout.panelScalePct), false);   // 其它歌词行
    HFONT fSub     = MakeFont(dpi, ScalePt(9,  pct, layout.panelScalePct), false);   // 当前行的翻译（参照行）

    const auto& st = PlaybackState::Get();
    int y = top;

    // ---- 曲名 ----
    // 曲名也在遮罩**外面**画：它是这一屏唯一允许出现在歌词区上方的文字，
    // 所以先把遮罩的范围定在它下方，再开遮罩。
    if (st.HasTrack()) {
        y += DrawLine(dc, st.DisplayName().c_str(), left, y, maxW,
                      fHeader, theme.headerText, S(8));
    } else {
        y += S(8);
    }

    // -------------------------------------------------------------------
    // 遮罩：歌词只许出现在 [left, y] .. [left + maxW, limit] 里
    //
    // 【为什么必须有】`DrawTextW` 只能裁到**它拿到的那个矩形**。而横滚要的是
    // 「文字位置在动、裁剪框不动」—— 传进去的矩形一动，裁剪框跟着动，等于没裁。
    // 只有给 DC 设一个独立的裁剪区才能做到。上滑过渡同理：中间帧的内容会越过
    // limit 压到控制条上（见 docs/design-lyric-motion.md）。
    //
    // 现在这一步（第 1 步）画面上还看不出它的作用 —— 省略号模式下没有任何东西
    // 会越界。但它是**下一步的前置条件**，而且现在就有测试能钉住它：
    // 歌词区被压到比一行还矮时，下面的兜底分支会把整块画在 y 上、直接越过
    // limit，那段越界正是遮罩挡掉的（tests/harness/test_view.cpp 的 TestMask）。
    //
    // 用 SaveDC/RestoreDC 而不是 SelectClipRgn(dc, nullptr) 复位：
    // 宿主可能自己带着裁剪区进来（局部失效重绘），后者会把它一并抹掉。
    // -------------------------------------------------------------------
    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, left, y, left + maxW, limit);

    // ---- 歌词 ----
    const LyricDocument& doc = st.Lyrics();
    if (doc.IsEmpty()) {
        if (!st.HasTrack()) {
            // 没在播放：什么都不说，留白
        } else {
            // 有源明确说过"这是纯音乐"（它返回的是「纯音乐，请欣赏」占位文本）
            // 就显示得更具体一点。
            //
            // 【为什么值得区分】用户 2026-09-26 指出：占位文本本身不是问题，
            // 它是有用的信息 —— 比笼统的「（无歌词）」精确。
            // 所以那句话不作为歌词收下（不写歌词缓存，见 IsPlaceholderLyric），
            // 但结论留了下来，由**本地**在这里显示。
            const wchar_t* msg = st.IsInstrumental() ? L"（纯音乐，请欣赏）"
                                                     : L"（无歌词）";
            std::wstring sub = st.LyricPath().empty() ? std::wstring()
                                                      : FileNameOf(st.LyricPath());
            const int gap = S(4);
            const int h1 = MeasureLine(dc, msg, fBody, maxW);
            const int h2 = sub.empty() ? 0 : MeasureLine(dc, sub.c_str(), fBody, maxW);
            const int block = h1 + (h2 > 0 ? h2 + gap : 0);

            int dy = y + ((limit - y) - block) / 2;
            if (dy < y) dy = y;

            dy += DrawLine(dc, msg, left, dy, maxW, fBody, theme.warnText, gap);
            if (h2 > 0) {
                DrawLine(dc, sub.c_str(), left, dy, maxW, fBody, theme.dimText, 0);
            }
            y = limit;
        }
    } else {
        const size_t cur   = st.CurrentLine();
        const size_t total = doc.Count();

        const int gapCurrent = S(10);
        const int gapNormal  = S(6);
        const int avail      = limit - y;

        if (cur != LyricDocument::npos && total > 0 && avail > 0) {
            // 哪个是正文由设置决定（layout.tlPrimary），下面所有取值都走这两个助手，
            // 免得出现"当前行按翻译显示、上下行按原文显示"这种自相矛盾。
            const bool        tlPrimary = layout.tlPrimary;
            const std::wstring& curMain = MainTextOf(doc, cur, tlPrimary);
            const std::wstring* curSub  = SubTextOf(doc, cur, tlPrimary);

            const wchar_t* curText = curMain.c_str();
            const int curTextH = MeasureLine(dc, curText, fCurrent, maxW);

            // 当前行**完整**的像素宽度 —— "要不要滚、滚多远"的唯一依据。
            // 用当前行字体量（大而粗），量出来的就是它真正画出来占的宽度。
            const int curTextW = MeasureLineWidth(dc, curText, fCurrent);

            // ---- 溢出多少才值得滚 ----
            //
            // 用户 2026-09-25 定的：**溢出不到一个字宽就不滚，改用省略号**。
            //
            // 【为什么】超 5px 也要滚的话，`ScrollDurationMs` 的下限 600ms 会把
            // 它变成"整行用 600ms 慢慢挪 5 个像素、然后永远停在那儿" ——
            // 用户报的「整句漂移」就是这个形状（D-048）。
            // 一个字以内本来也读不出少了什么，画个 `…` 说明"后面还有字"更划算。
            const int oneCharW = MeasureLineWidth(dc, L"字", fCurrent);
            result.currentOverflow = (curTextW > maxW + oneCharW) ? (curTextW - maxW) : 0;

            // 溢出不值得滚时退回省略号那一档；放得下时它也是无操作。
            const UINT curFlags = (result.currentOverflow > 0) ? kDrawScrollFlags
                                                               : kDrawEllipsisFlags;

            // 上滑的步距 = **上一行**作为上下文行时的行高 + 常规行距。
            //
            // 用上一行而不是当前行：换行时升上来的那个"低一步"位置，
            // 就是上一行原本待的地方。参照行（翻译）不算独立一行 ——
            // 跳过它，否则步距会短一个小字的距离。
            if (cur > 0) {
                size_t prev = cur - 1;
                if (doc.IsSubLine(prev) && prev > 0) --prev;
                result.currentStepH = MeasureLine(dc, MainTextOf(doc, prev, tlPrimary).c_str(),
                                                  fBody, maxW) + gapNormal;
            }

            // ---- 当前行的双语参照行 ----
            //
            // 双语歌词是"同一条时间戳、原文在前、翻译在后"两行。
            // 只在**当前行**下面画那一行小字，其余行保持单行 ——
            // 面板只有 460×150、可见 2~3 行，完整双语会把可见行数砍半
            //（用户 2026-09-24 定的取向，见 plan.md #12）。
            const int gapSub = S(2);
            const int subTextH = (curSub != nullptr)
                                     ? MeasureLine(dc, curSub->c_str(), fSub, maxW)
                                     : 0;
            // 当前行**整块**的高度（正文 + 参照行）—— 居中、夹取、往下铺都要用它。
            //
            // ⚠️ 用"整块"而不是"正文"是有原因的：用户 2026-09-24 反馈
            //    「歌词的位置应该居中，之前稍微有点偏下了，这个刚好」——
            //    那个"刚好"就是从这里来的（块变高 -> 整块居中 -> 正文上抬半行）。
            //    别改回 curTextH / 2，会退回"偏下"。
            const int curBlockH = curTextH + (subTextH > 0 ? gapSub + subTextH : 0);

            // 当前行的垂直锚点。
            //
            // 【基准是整个面板】见 lyrics_view.h 里 currentRatio 的说明和 D-043。
            // 从前这里用的是 `y + avail * ratio%`（y = 曲名下方的歌词区上沿），
            // 于是同一个百分比在浮动面板和 DUI 里落点不同 ——
            // 用户 2026-09-25 报「内嵌的歌词没有居中」就是它。
            //
            // 改成整个面板之后 50 恒等于面板正中；面板被拉高拉矮时
            // 百分比自动跟着走，不需要另加缩放规则（用户特意提过要兼容缩放）。
            //
            // fromEdge 只是"锚点相对面板上沿的距离"，夹取仍按歌词区走 ——
            // 上方给曲名留位置、下方不压控制条，这两件事和居中基准是两回事。
            const int ratio    = ClampInt(layout.currentRatio, 0, 100);
            const int panelH   = rc.bottom - rc.top;
            const int fromEdge = MulDiv(panelH, ratio, 100);
            const int anchor   = (panelH > 0) ? rc.top + fromEdge : y + MulDiv(avail, ratio, 100);

            int curTop = anchor - curBlockH / 2;
            // 锚点算出的位置可能把当前行顶出歌词区（ratio 取极端值、
            // 或者面板矮到装不下一行时），夹回来。
            curTop = ClampInt(curTop, y, limit - curBlockH);
            if (curTop < y) curTop = y;   // 歌词区比一行还矮时的最后兜底

            // span = 0 表示「按可用高度自适应」。这是默认行为，也是三种宿主
            // 共用一个设置还能各自合理的原因：独立面板只有 460x150 逻辑像素
            // （副屏远距离看，3 行正合适，见 D-013），而 DUI 元素可能很高，
            // 写死行数会在 DUI 里浪费大片空间。
            const int span = (layout.span > 0) ? layout.span : 0;

            // ---- 动画量 ----
            //
            // 必须在铺行**之前**算出来：slideY 要参与"上一行画不画得下"的判断
            //（判据是**画出来的位置**，见下面 above 循环里的说明）。
            //
            // slideY = 整块歌词**下移**多少像素（0 = 最终位置）。换行的过渡里
            // 它从 currentStepH 降到 0，看上去就是整块往上滑了一行的距离。
            //
            // ⚠️ 它只作用在**绘制**上，不参与排版：行数、`above`/`below` 的成员、
            //    夹取全都照原来算。否则过渡途中"能塞下的行数"会变，
            //    滑到位的一瞬间会**跳一下**（行数突变）。
            //    （唯一的例外就是上面那个"减去 slideY"的边界判断，
            //      它的作用是让上一行**多**留一会儿，方向是单调的、不会突变。）
            const int slideY = (anim.slideY > 0) ? anim.slideY : 0;

            // scrollX = 当前行往左移出多少像素。它在绘制时改变 x，**不改宽度**：
            // 宽度是整行的真实宽度，永远不变，所以裁剪框（DC 裁剪区）也永远不动。
            // 这就是"文字在动、框不动"，横滚的全部秘密。
            const int scrollX = (anim.scrollX > 0) ? anim.scrollX : 0;

            // ---- 往上铺 ----
            // above[0] 是紧邻当前行的上一行，依次向远处排。
            //
            // ⚠️ 跳过参照行：往前翻的时候，**不要**把前几句的翻译也当成独立一行
            //    显示出来 —— 那会让"行数上限"被小字占满，而且视觉上很吵。
            std::vector<size_t> above;
            int up = curTop;
            for (size_t i = cur; i-- > 0; ) {
                if (doc.IsSubLine(i)) continue;
                if (span > 0 && above.size() >= static_cast<size_t>(span)) break;
                // 用 MainTextOf —— 翻译为主时上下行也要显示翻译，
                // 否则会出现"当前行是译文、上下文是原文"的割裂
                const int h = MeasureLine(dc, MainTextOf(doc, i, tlPrimary).c_str(),
                                          fBody, maxW) + gapNormal;

                // ⚠️ 判据是**画出来的位置**，不是排版位置 —— 两者差一个 slideY。
                //
                // 上滑过渡里整块下移了 slideY，所以上一行**画**在 up + slideY，
                // 比 up 低。用 up 判的话，面板刚够不下一行时（用户那块 920x300
                // 就是这样）上一行会被判成"放不下"而**根本不画** ——
                // 表现就是用户说的「上一行歌词突然消失」，完全没有上移的过程。
                //
                // 放低 slideY 之后：过渡开始时上一行正好还留在原位，
                // 随着 slideY 减小被往上推出裁剪区；等它被判定为"放不下"时，
                // 它的绘制位置已经贴着 y（裁剪线）了，所以**看不见突变**。
                if (up - h < y - slideY) break;   // 越过歌词区上边界就停
                up -= h;
                above.push_back(i);
            }

            // ---- 往下铺 ----
            // 起点要跳过当前行的参照行（如果有）—— 否则翻译会被当成
            // "下一句歌词"再显示一遍。
            std::vector<size_t> below;
            int down = curTop + curBlockH + gapCurrent;
            for (size_t i = cur + 1 + (curSub != nullptr ? 1 : 0); i < total; ++i) {
                if (doc.IsSubLine(i)) continue;
                if (span > 0 && below.size() >= static_cast<size_t>(span)) break;
                const int h = MeasureLine(dc, MainTextOf(doc, i, tlPrimary).c_str(),
                                          fBody, maxW) + gapNormal;
                if (down + h > limit) break;
                below.push_back(i);
                down += h;
            }

            // 上方：above 是「由近及远」，画的时候要从最远的一行开始（倒序）。
            // up 此刻正好停在最上面那一行的 y 上。
            for (size_t k = above.size(); k-- > 0; ) {
                up += DrawLine(dc, MainTextOf(doc, above[k], tlPrimary).c_str(),
                               left, up + slideY, maxW,
                               fBody, theme.normalText, gapNormal);
            }

            // 当前行。
            //
            // ⚠️ 三个参数和上下文行都不一样，缺一不可：
            //   * 不省略号（kDrawScrollFlags）—— 一省略就等于把要滚的内容先扔了
            //   * x 减去 scrollX —— 文字往左挪，裁剪框不动
            //   * 宽度给 curTextW 而不是 maxW —— 矩形要**刚好装下整行**，
            //     这样"裁掉多少"完全由 DC 裁剪区决定，DrawTextW 不参与截断。
            //     给 maxW 的话整行会被 DrawTextW 自己裁在 maxW 处，
            //     滚动就变成了"窗口在动、文字不动"。
            DrawLine(dc, curText, left - scrollX, curTop + slideY,
                     (curTextW > 0 ? curTextW : maxW),
                     fCurrent, theme.currentText, gapCurrent, curFlags);

            // 当前行的参照行：小字、暗色，紧贴在正文下面。
            //
            // 用 dimText 而不是 normalText —— 它是**参照**，不该和正文抢注意力。
            // 它**不滚**，放不下就省略号：两行各自滚会因为宽度不同而漂移，
            // 视觉上很乱（见 docs/design-lyric-motion.md 第 2 节）。
            if (curSub != nullptr && subTextH > 0) {
                DrawLine(dc, curSub->c_str(), left,
                         curTop + slideY + curTextH + gapSub, maxW,
                         fSub, theme.dimText, 0);
            }

            // 下方：正序，光标从当前行**整块**下面接着走。
            int dy = curTop + curBlockH + gapCurrent;
            for (size_t idx : below) {
                dy += DrawLine(dc, doc.At(idx).text.c_str(), left, dy + slideY, maxW,
                               fBody, theme.normalText, gapNormal);
            }
            y = dy;

            // 行数上限生效时，上下会各自留出空白 —— 这是预期行为：
            // 用户要的是「只显示 N 行」，不是「把 N 行撑满整个区域」。
        }
    }

    RestoreDC(dc, savedDc);

    // 字体由 MakeFont 缓存持有，进程内复用 —— **这里绝不能删**，
    // 删了缓存里就是野句柄，下一帧 SelectObject 会拿到已释放的 GDI 对象。
    result.bottom = y;
    return result;
}

HFONT GetCachedUiFont(int dpi, int pt, bool bold) {
    return MakeFont(dpi, pt, bold);
}

} // namespace lyricus
