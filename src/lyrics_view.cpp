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
// 刻意**不做**「按面板高度自动缩放字号」：字号该由用户拍板，
// 面板高度影响的是**行数**（见下面 span 那段）。两者混在一起的话，
// 同一个设置值在独立面板（460x150 逻辑像素）和 DUI 元素（可能很高）里
// 会得到不同字号，用户根本没法预期自己调的是什么。
int ScalePt(int basePt, int pct) {
    const int pt = MulDiv(basePt, pct, 100);
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

// 这一行是不是**参照行**（也就是合并进来的翻译）。
//
// 判据是"和上一行同一时间戳"—— 双语歌词就是这么写的
//（见 online_lyric.cpp 的 MergeTranslationLines：原文在前、翻译紧随其后、同戳）。
// ⚠️ 没给 LyricLine 加字段是有意的：加了就得改解析器，
//    而那是全工程共用的（D-038 那次一改就打挂 26 条断言）。
//    这里靠"同戳"这个**格式本身**来判断，解析器一行不用动。
bool IsSubLine(const LyricDocument& doc, size_t i) {
    if (i == 0 || i >= doc.Count()) return false;   // 第 0 行不可能是参照行
    const double a = doc.At(i).timeSec;
    const double b = doc.At(i - 1).timeSec;
    const double d = (a > b) ? (a - b) : (b - a);
    return d < 0.01;
}

// 第 i 行的**正文**文本（i 必须不是参照行）。
//
// tlPrimary = false：正文是原文。
// tlPrimary = true ：正文是翻译 —— 听日语/同人曲的人很多只看得懂译文，
//                    原文对他们反而是参照（用户 2026-09-24 提的）。
//                    那一行没有翻译时**退回原文**，不能空着。
const std::wstring& MainTextOf(const LyricDocument& doc, size_t i, bool tlPrimary) {
    if (tlPrimary && IsSubLine(doc, i + 1)) return doc.At(i + 1).text;
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
        return IsSubLine(doc, i + 1) ? &doc.At(i).text : nullptr;
    }
    return IsSubLine(doc, i + 1) ? &doc.At(i + 1).text : nullptr;
}

} // namespace

int DrawLyricsView(HDC dc, const RECT& rc, const LyricsViewTheme& theme,
                   const LyricsViewLayout& layout) {
    if (dc == nullptr) return rc.top;

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

    HFONT fHeader  = MakeFont(dpi, ScalePt(11, pct), false);   // 曲名：刻意比歌词小，别抢戏
    HFONT fCurrent = MakeFont(dpi, ScalePt(15, pct), true);    // 当前歌词行
    HFONT fBody    = MakeFont(dpi, ScalePt(11, pct), false);   // 其它歌词行
    HFONT fSub     = MakeFont(dpi, ScalePt(9,  pct), false);   // 当前行的翻译（参照行）

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
            const wchar_t* msg = L"（无歌词）";
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

            // ---- 往上铺 ----
            // above[0] 是紧邻当前行的上一行，依次向远处排。
            //
            // ⚠️ 跳过参照行：往前翻的时候，**不要**把前几句的翻译也当成独立一行
            //    显示出来 —— 那会让"行数上限"被小字占满，而且视觉上很吵。
            std::vector<size_t> above;
            int up = curTop;
            for (size_t i = cur; i-- > 0; ) {
                if (IsSubLine(doc, i)) continue;
                if (span > 0 && above.size() >= static_cast<size_t>(span)) break;
                // 用 MainTextOf —— 翻译为主时上下行也要显示翻译，
                // 否则会出现"当前行是译文、上下文是原文"的割裂
                const int h = MeasureLine(dc, MainTextOf(doc, i, tlPrimary).c_str(),
                                          fBody, maxW) + gapNormal;
                if (up - h < y) break;      // 越过歌词区上边界就停
                up -= h;
                above.push_back(i);
            }

            // ---- 往下铺 ----
            // 起点要跳过当前行的参照行（如果有）—— 否则翻译会被当成
            // "下一句歌词"再显示一遍。
            std::vector<size_t> below;
            int down = curTop + curBlockH + gapCurrent;
            for (size_t i = cur + 1 + (curSub != nullptr ? 1 : 0); i < total; ++i) {
                if (IsSubLine(doc, i)) continue;
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
                up += DrawLine(dc, MainTextOf(doc, above[k], tlPrimary).c_str(), left, up, maxW,
                               fBody, theme.normalText, gapNormal);
            }

            // 当前行
            DrawLine(dc, curText, left, curTop, maxW,
                     fCurrent, theme.currentText, gapCurrent);

            // 当前行的参照行：小字、暗色，紧贴在正文下面。
            //
            // 用 dimText 而不是 normalText —— 它是**参照**，不该和正文抢注意力。
            if (curSub != nullptr && subTextH > 0) {
                DrawLine(dc, curSub->c_str(), left,
                         curTop + curTextH + gapSub, maxW,
                         fSub, theme.dimText, 0);
            }

            // 下方：正序，光标从当前行**整块**下面接着走。
            int dy = curTop + curBlockH + gapCurrent;
            for (size_t idx : below) {
                dy += DrawLine(dc, doc.At(idx).text.c_str(), left, dy, maxW,
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
    return y;
}

HFONT GetCachedUiFont(int dpi, int pt, bool bold) {
    return MakeFont(dpi, pt, bold);
}

} // namespace lyricus
