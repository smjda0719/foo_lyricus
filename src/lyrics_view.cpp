#include "stdafx.h"
#include "lyrics_view.h"
#include "playback_state.h"

#include <map>
#include <string>
#include <tuple>
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

// 文本行的绘制/测量标志。**这两个地方必须一模一样**。
//
// DT_WORDBREAK 而不是 DT_SINGLELINE —— 用户 2026-09-24 报：
// 「这张截图也反映了歌词截断的问题」。
//
// 实测（captures/lyricus-20260925-004755.png）：面板上那行
//     「池光化新茶 掺着新雪 煨了炉火曾入」
// 右边**顶到边就没了** —— DT_SINGLELINE 既不换行也不给省略号，
// 超出的部分直接截断，一个字都看不到。对歌词来说这是丢内容，
// 而面板本来就有富余的垂直空间（可见 2~3 行、字号 125% 时还空着）。
//
// 换成 DT_WORDBREAK 之后：短行还是一行（高度不变，排版完全不动），
// 超长行折成两行以上 —— 下面的走位逻辑本来就是按**量出来的高度**排的，
// 所以不用改布局代码，它自然就适应了。
//
// ⚠️ DT_NOPREFIX 不能省：歌词里出现 `&` 时，没有它会被当成助记符前缀吃掉。
//    这两处（MeasureLineRaw 和 DrawLine）用同一个常量，避免哪天只改一处 ——
//    那会导致"量出来的高度"和"画出来的行数"不一致，排版直接错位。
constexpr UINT kLineDrawFlags = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;

// 文本行的高度缓存。
//
// 【为什么要缓存】DrawTextW(DT_CALCRECT) 不是免费操作 —— 要走一遍文本整形。
// 而一帧里同一行会被量 2~3 次（先 MeasureLine 探路，再 DrawLine 里又量一次），
// 可见的 20 行就是四五十次；面板每 250ms 重绘一次，同一批字每秒被量近 200 次，
// 而它们中间绝大多数根本没变。
//
// ⚠️ 自从改成 DT_WORDBREAK，**高度就与 maxWidth 有关了** —— 所以键必须带上它。
//    （原来是 DT_SINGLELINE，高度只由字体决定，那时候不带宽度是对的。）
//    键里漏了宽度的话，面板一改宽度就会命中旧高度，折行数对不上、排版错位。
//
// 容量超了直接清空重来 —— 一首歌的可见行数就那么几十条，
// 真清空也是极低频事件，不值得为它维护 LRU。
std::map<std::tuple<HFONT, std::wstring, int>, int>& HeightCache() {
    static std::map<std::tuple<HFONT, std::wstring, int>, int> cache;
    return cache;
}

int MeasureLineRaw(HDC dc, const wchar_t* text, HFONT font, int maxWidth) {
    RECT r{ 0, 0, maxWidth, 0 };
    DrawTextW(dc, text, -1, &r, kLineDrawFlags | DT_CALCRECT);
    return r.bottom - r.top;
}

int CachedLineHeight(HDC dc, const wchar_t* text, HFONT font, int maxWidth) {
    auto& cache = HeightCache();

    const auto key = std::make_tuple(font, std::wstring(text), maxWidth);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    const int h = MeasureLineRaw(dc, text, font, maxWidth);

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
int DrawLine(HDC dc, const wchar_t* text, int x, int y, int maxWidth,
             HFONT font, COLORREF color, int gapAfter) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);

    // 高度直接取缓存，不再为了「量一下」多画一次 DrawTextW。
    // 量出来的值和原来 DT_CALCRECT 的结果完全一致（单行高度只由字体决定），
    // 所以排版不变，只是省掉一次文本整形。
    const int height = CachedLineHeight(dc, text, font, maxWidth);

    RECT draw{ x, y, x + maxWidth, y + height };
    DrawTextW(dc, text, -1, &draw, kLineDrawFlags);

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
    const int limit = rc.bottom;

    const int pct = (layout.fontPct > 0) ? layout.fontPct : 100;

    HFONT fHeader  = MakeFont(dpi, ScalePt(11, pct), false);   // 曲名：刻意比歌词小，别抢戏
    HFONT fCurrent = MakeFont(dpi, ScalePt(15, pct), true);    // 当前歌词行
    HFONT fBody    = MakeFont(dpi, ScalePt(11, pct), false);   // 其它歌词行
    HFONT fSub     = MakeFont(dpi, ScalePt(9,  pct), false);   // 当前行的翻译（参照行）

    const auto& st = PlaybackState::Get();
    int y = top;

    // ---- 曲名 ----
    if (st.HasTrack()) {
        y += DrawLine(dc, st.DisplayName().c_str(), left, y, maxW,
                      fHeader, theme.headerText, S(8));
    } else {
        y += S(8);
    }

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
            // ratio = 50 时锚点落在歌词区正中，与旧版行为一致 ——
            // 旧版把整块居中，而上下预算对称，块中心就是当前行中心。
            // 所以把这个值默认成 50 是**忠实保留**，不是新调的手感。
            const int ratio  = ClampInt(layout.currentRatio, 0, 100);
            const int anchor = y + MulDiv(avail, ratio, 100);

            int curTop = anchor - curBlockH / 2;
            // 锚点算出的位置可能把当前行顶出歌词区（ratio 取极端值时），夹回来。
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

    // 字体由 MakeFont 缓存持有，进程内复用 —— **这里绝不能删**，
    // 删了缓存里就是野句柄，下一帧 SelectObject 会拿到已释放的 GDI 对象。
    return y;
}

HFONT GetCachedUiFont(int dpi, int pt, bool bold) {
    return MakeFont(dpi, pt, bold);
}

} // namespace lyricus
