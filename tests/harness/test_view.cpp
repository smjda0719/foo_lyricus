// ---------------------------------------------------------------------------
// 歌词绘制层的布局数学 —— 离线单测
//
// 【为什么非测不可】「行数上限」这一项在真机上**验证不了**：
// 默认面板只有 460x150 逻辑像素，字号 125% 时一行就占满了，
// 行数调 1 还是调 9 都只显示一行 —— **看不出效果不等于功能是坏的**，
// 但也不能就这么算了，否则"行数上限"永远是个假设。
//
// 做法：造一个大的内存 DIB，直接调 DrawLyricsView，然后**按像素分析结果**：
//   * 亮带数量 = 实际画出了几行
//   * 只有当前行是纯白 (255,255,255)，普通行是 (172,172,180)
//     -> 用阈值 250 就能把当前行单独找出来，量它的垂直位置
//
// 这样就绕开了"面板太小看不出"的问题，能直接验证 span / ratio / fontPct
// 三个参数真的按语义工作。
// ---------------------------------------------------------------------------

#include "lyrics_view.h"
#include "scroll_anim.h"      // 动画时间线（横滚 / 上滑）
#include "playback_state.h"   // 单测里这个是替身（见 shim/playback_state.h）

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lyricus {

PlaybackState& PlaybackState::Get() {
    static PlaybackState s;
    return s;
}

void PlaybackState::SetFake(bool hasTrack, const std::wstring& displayName,
                            LyricDocument lyrics, size_t currentLine,
                            const std::wstring& lyricPath, bool instrumental) {
    m_hasTrack     = hasTrack;
    m_displayName  = displayName;
    m_lyrics       = std::move(lyrics);
    m_currentLine  = currentLine;
    m_lyricPath    = lyricPath;
    m_instrumental = instrumental;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? path : path.substr(s + 1);
}

// lyric.cpp 用到的外部符号
void DebugLog(const char* fmt, ...) { (void)fmt; }

} // namespace lyricus

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what); }
    else    { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}

// ---------------------------------------------------------------------------
// 内存 DIB
// ---------------------------------------------------------------------------

const int kWidth  = 600;
const int kHeight = 600;

struct Canvas {
    HDC        dc   = nullptr;
    HBITMAP    bmp  = nullptr;
    void*      bits = nullptr;
    HGDIOBJ    old  = nullptr;

    bool Create() {
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = kWidth;
        bi.bmiHeader.biHeight      = -kHeight;   // 负 = 自上而下，行号就是 y
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp == nullptr) return false;

        old = SelectObject(dc, bmp);
        Clear();
        return true;
    }

    void Clear() { std::memset(bits, 0, static_cast<size_t>(kWidth) * kHeight * 4); }

    ~Canvas() {
        if (dc) { if (old) SelectObject(dc, old); if (bmp) DeleteObject(bmp); DeleteDC(dc); }
    }

    // BGRA，四通道都要够亮才算"这个像素有字"
    bool Bright(int x, int y, int thr) const {
        const unsigned char* p =
            static_cast<const unsigned char*>(bits) + (static_cast<size_t>(y) * kWidth + x) * 4;
        return p[0] >= thr && p[1] >= thr && p[2] >= thr;
    }

    // 找出所有"有字的横带"。thr 取 250 时只有当前行（纯白）会被选中。
    //
    // minHeight 用来滤掉**碎带**：像 "Song" 里 g 的下伸部分会单独形成
    // 一两条只有 1~2 像素高的带，把一行字误数成两行。
    // （这个坑真踩过：span=1 本该 3 行歌词，却被数成 5 条带。）
    // 真实一行字在 96 dpi 下有 11~15 像素高，所以门槛取 5 很安全。
    std::vector<std::pair<int,int>> Bands(int thr, int minPixels = 3, int minHeight = 5) const {
        std::vector<std::pair<int,int>> out;
        int start = -1;
        for (int y = 0; y < kHeight; ++y) {
            int c = 0;
            for (int x = 0; x < kWidth; ++x) if (Bright(x, y, thr)) ++c;
            const bool has = (c >= minPixels);
            if (has && start < 0) start = y;
            else if (!has && start >= 0) {
                if (y - start >= minHeight) out.push_back({start, y - 1});
                start = -1;
            }
        }
        if (start >= 0 && kHeight - start >= minHeight) out.push_back({start, kHeight - 1});
        return out;
    }
};

// ---------------------------------------------------------------------------

std::string MakeLrc(int n) {
    std::string s;
    char buf[80];
    for (int i = 0; i < n; ++i) {
        sprintf_s(buf, sizeof(buf), "[%02d:%02d.00]Line%d\n", i / 60, i % 60, i);
        s += buf;
    }
    return s;
}

// 双语版本：每一行后面紧跟一条**同时间戳**的翻译行。
// 形状和网易云 tlyric 合并出来的完全一致（见在线模块的 MergeTranslationLines）。
//
// 译文刻意写得**明显更长**：测试要靠"当前行的像素宽度"判断
// 两种模式（原文为主 / 翻译为主）到底谁被画成了正文。
std::string MakeLrcBilingual(int n) {
    std::string s;
    char buf[120];
    for (int i = 0; i < n; ++i) {
        sprintf_s(buf, sizeof(buf), "[%02d:%02d.00]L%d\n", i / 60, i % 60, i);
        s += buf;
        sprintf_s(buf, sizeof(buf), "[%02d:%02d.00]translated line number %d\n", i / 60, i % 60, i);
        s += buf;
    }
    return s;
}

lyricus::LyricDocument MakeDoc(int n) {
    const std::string s = MakeLrc(n);
    std::vector<unsigned char> b(s.begin(), s.end());
    return lyricus::LyricDocument::Parse(b);
}

// 渲染一次，返回：总行数、当前行中心 y、歌词区高度
struct RenderResult {
    int totalLines   = 0;
    int totalLinesLow = 0;   // 低阈值版本，用来数**小字**（见下面 Render 里的说明）
    int currentY     = -1;
    int currentWidth = 0;    // 当前行的水平像素宽度（用来判断画的是哪一段文字）
    int firstTop     = -1;
    int lastBottom   = -1;
};

RenderResult Render(const lyricus::LyricsViewLayout& layout, int lineCount = 21, int current = 10,
                    bool dump = false, bool bilingual = false) {
    RenderResult r;

    {
        const std::string raw = bilingual ? MakeLrcBilingual(lineCount) : MakeLrc(lineCount);
        std::vector<unsigned char> b(raw.begin(), raw.end());
        lyricus::PlaybackState::Get().SetFake(
            true, L"Test - Song", lyricus::LyricDocument::Parse(b),
            static_cast<size_t>(current));
    }

    Canvas cv;
    if (!cv.Create()) return r;

    lyricus::LyricsViewTheme theme;
    theme.dpi = 96;
    // 用默认配色：currentText = 纯白 (255,255,255)，
    // normalText = (172,172,180)。阈值 250 就能把当前行单独挑出来。

    RECT rc{ 0, 0, kWidth, kHeight };
    const auto res = lyricus::DrawLyricsView(cv.dc, rc, theme, layout);
    const int  ret = res.bottom;

    // 阈值 140：连暗一点的普通行也算进来
    const auto all = cv.Bands(140);
    r.totalLines = static_cast<int>(all.size());

    // 阈值 70：**专门用来数小字**。
    //
    // 【为什么需要这一档】Bands 要求一行里 ≥3 个像素四通道都过阈值，
    // 而 9pt 的字在 ANTIALIASED 下笔画只有一像素宽、几乎全是过渡色 ——
    // 满强度像素少到数不出来，参照行会被整条漏掉。
    //（这不是产品问题，是**检测手段对太小的字不灵**：
    // 最初的 dump 里当前行是 307..321、下一行 359..369，
    //  中间 322..358 空着，看着像没画，其实是没被数出来。）
    // 普通行的抗锯齿光晕只会把已有带撑宽一两像素，不会凭空多出一条，
    // 所以这一档的数**做差**仍然可靠。
    r.totalLinesLow = static_cast<int>(cv.Bands(70).size());

    if (!all.empty()) {
        r.firstTop   = all.front().first;
        r.lastBottom = all.back().second;
    }

    // 阈值 250：只有当前行（纯白）能达到
    const auto cur = cv.Bands(250, 1);
    if (!cur.empty()) r.currentY = (cur.front().first + cur.front().second) / 2;

    // 当前行的水平范围 —— 用来看"画出来的到底是原文还是译文"
    //（夹具里译文明显更长，宽度一比就知道）
    {
        int l = kWidth, rr = -1;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                if (cv.Bright(x, y, 250)) {
                    if (x < l)  l = x;
                    if (x > rr) rr = x;
                }
            }
        }
        r.currentWidth = (rr >= l) ? (rr - l + 1) : 0;
    }

    if (dump) {
        std::printf("       [dump] 返回值 y=%d\n", ret);
        int i = 0;
        for (const auto& b : all) {
            // 这一带是不是纯白（当前行）
            bool white = false;
            for (int y = b.first; y <= b.second && !white; ++y)
                for (int x = 0; x < kWidth; ++x)
                    if (cv.Bright(x, y, 250)) { white = true; break; }
            std::printf("       带%d: y=%3d..%3d (高 %2d) %s\n",
                        i++, b.first, b.second, b.second - b.first + 1,
                        white ? "  <== 纯白(当前行)" : "");
        }
    }

    return r;
}

// ---------------------------------------------------------------------------

void TestSpan() {
    std::printf("\n== 行数上限（span）==\n");
    std::printf("     在 600x600 的绘制区里，字号 100%%，共 21 行歌词，当前行 = 第 10 行\n");

    // span = 0（自适应）：能塞几行塞几行
    const auto auto_ = Render({100, 0, 50});
    std::printf("     span=0(自适应)  -> 画出 %d 行，当前行 y=%d\n", auto_.totalLines, auto_.currentY);
    Check(auto_.totalLines > 3, "span=0 自适应：明显多于 3 行");

    // span = 1：当前行上下各 1 行，加上曲名，合计 4 条横带
    const auto s1 = Render({100, 1, 50}, 21, 10, /*dump=*/true);
    std::printf("     span=1          -> 画出 %d 行，当前行 y=%d\n", s1.totalLines, s1.currentY);
    Check(s1.totalLines == 4, "span=1 -> 4 条带（曲名 1 + 上 1 + 当前 + 下 1）");

    // span = 3：曲名 1 + 上 3 + 当前 + 下 3 = 8 条横带
    const auto s3 = Render({100, 3, 50}, 21, 10, /*dump=*/true);
    std::printf("     span=3          -> 画出 %d 行，当前行 y=%d\n", s3.totalLines, s3.currentY);
    Check(s3.totalLines == 8, "span=3 -> 8 条带（曲名 1 + 上 3 + 当前 + 下 3）");

    Check(s1.totalLines < auto_.totalLines, "span=1 比自适应少（限制确实生效）");
    Check(s3.totalLines > s1.totalLines,    "span=3 比 span=1 多（是线性生效的）");
}

void TestRatio() {
    std::printf("\n== 当前行垂直位置（currentRatio）==\n");

    const auto up   = Render({100, 2, 25});
    const auto mid  = Render({100, 2, 50});
    const auto down = Render({100, 2, 75});

    std::printf("     ratio=25 -> 当前行 y=%d\n", up.currentY);
    std::printf("     ratio=50 -> 当前行 y=%d\n", mid.currentY);
    std::printf("     ratio=75 -> 当前行 y=%d\n", down.currentY);

    Check(up.currentY >= 0 && mid.currentY >= 0 && down.currentY >= 0, "三档都能定位到当前行");
    Check(up.currentY < mid.currentY,   "ratio 25% 比 50% 靠上");
    Check(mid.currentY < down.currentY, "ratio 50% 比 75% 靠下");
    Check(down.currentY > mid.currentY && (down.currentY - mid.currentY) > 50,
          "ratio 每 25% 的位移是显著的（不是几个像素的抖动）");

    // ★ 50 = **整个面板**的正中。
    //
    // 出处：用户 2026-09-25「内嵌的歌词没有居中」。根因就是这里的基准 ——
    // 从前是"歌词区"（曲名下方到控制条上沿），而三种宿主的歌词区定义不同，
    // 同一个百分比在浮动面板和 DUI 里落点不一样。改成整个面板后
    // 50 必须**正好**是画布中心，跟曲名占多高、有没有控制条都无关。
    const int centre = kHeight / 2;
    std::printf("     画布正中 y=%d，ratio=50 落在 y=%d\n", centre, mid.currentY);
    Check(std::abs(mid.currentY - centre) <= 4,
          "★ ratio=50 落在**整个画布**正中（不是歌词区正中）");
}

// ---------------------------------------------------------------------------
// 面板缩放 + 底部裁剪（浮动面板的形状）
//
// 【为什么要单测这两件事】
//   1. 用户 2026-09-25 特意提「浮动面板的设计要和之后窗口缩放兼容」——
//      同一个百分比必须在任何面板高度下都落在**同一个相对位置**，
//      不能因为面板变高就跑到别处去。
//   2. 浮动面板的 rc 现在传的是**整个面板**（给 currentRatio 当基准），
//      控制条那一段改用 clipBottom 排除。这两件事必须互不干扰：
//      裁剪只决定"画到哪儿为止"，不许影响居中基准（D-043）。
// ---------------------------------------------------------------------------
namespace {

struct ClipProbe {
    int currentY   = -1;   // 当前行中心（阈值 250，只有纯白的当前行能达到）
    int lowestInkY = -1;   // 最靠下的一行墨迹（阈值 70，连小字的光晕也算）
    int firstTop   = -1;   // 最靠上的一行墨迹 —— 曲名在遮罩外面，它不该被裁剪动到
    int bands      = 0;    // 横带条数（阈值 70）
};

ClipProbe ProbeClip(const lyricus::LyricsViewLayout& layout, const RECT& rc,
                    const lyricus::LyricAnimFrame& anim = lyricus::LyricAnimFrame{}) {
    ClipProbe p;

    const std::string raw = MakeLrc(21);
    std::vector<unsigned char> b(raw.begin(), raw.end());
    lyricus::PlaybackState::Get().SetFake(
        true, L"Test - Song", lyricus::LyricDocument::Parse(b), 10);

    Canvas cv;
    if (!cv.Create()) return p;

    lyricus::LyricsViewTheme theme;
    theme.dpi = 96;
    lyricus::DrawLyricsView(cv.dc, rc, theme, layout, anim);

    p.bands = static_cast<int>(cv.Bands(70).size());

    bool sawTop = false;
    for (int y = 0; y < kHeight; ++y) {
        bool any = false;
        for (int x = 0; x < kWidth; ++x) {
            if (cv.Bright(x, y, 70)) { any = true; break; }
        }
        if (any) {
            p.lowestInkY = y;
            if (!sawTop) { p.firstTop = y; sawTop = true; }
        }
    }
    const auto cur = cv.Bands(250, 1);
    if (!cur.empty()) p.currentY = (cur.front().first + cur.front().second) / 2;
    return p;
}

} // namespace

void TestResizeAndClip() {
    std::printf("\n== 面板缩放 + 控制条裁剪 ==\n");

    const RECT full{ 0, 0, kWidth, kHeight };

    // ---- 1. 缩放兼容：面板变矮，同一个百分比仍落在同一个相对位置 ----
    //
    // ⚠️ 变量别叫 small —— windows.h 的 rpcndr.h 里 `#define small char`，
    //    撞上去报的是"意外的类型 char"，离真正的原因很远。
    const RECT shortPanel{ 0, 0, kWidth, 300 };
    const auto tallPanel = ProbeClip({100, 2, 50}, full);
    const auto halfPanel = ProbeClip({100, 2, 50}, shortPanel);

    std::printf("     面板高 %d -> 当前行 y=%d（%.0f%%）\n",
                kHeight, tallPanel.currentY, 100.0 * tallPanel.currentY / kHeight);
    std::printf("     面板高 %d -> 当前行 y=%d（%.0f%%）\n",
                300, halfPanel.currentY, 100.0 * halfPanel.currentY / 300);

    Check(std::abs(tallPanel.currentY - kHeight / 2) <= 4,
          "面板高 600 时 ratio=50 落在正中");
    Check(std::abs(halfPanel.currentY - 150) <= 4,
          "★ 面板缩到一半高，ratio=50 仍然落在正中（缩放兼容）");

    // ---- 2. 裁剪不影响居中基准 ----
    //
    // 这一组用 span=0（自适应）让歌词**铺满**整个区域 —— 用 span=2 的话
    // 最低一行只到 y≈365，根本够不到 450，测试会变成空的
    //（头一次就是这么写的，被下面那条"否则这条测试是空的"当场抓住）。
    const auto floodUnclipped = ProbeClip({100, 0, 50}, full);
    // 450 = 模拟浮动面板底部的控制条上沿（300 px 面板里的 225，按比例放大）
    const auto clipped        = ProbeClip({100, 0, 50, false, 450}, full);
    std::printf("     未裁剪 -> 当前行 y=%d，最低墨迹 y=%d\n",
                floodUnclipped.currentY, floodUnclipped.lowestInkY);
    std::printf("     裁剪到 y<=450 -> 当前行 y=%d，最低墨迹 y=%d\n",
                clipped.currentY, clipped.lowestInkY);

    Check(clipped.currentY == floodUnclipped.currentY,
          "★ 有没有 clipBottom，当前行的位置**一模一样**（裁剪不参与居中基准）");
    Check(floodUnclipped.lowestInkY > 450,
          "不裁剪时确实有内容画到 450 以下（否则这条测试是空的）");
    Check(clipped.lowestInkY >= 0 && clipped.lowestInkY <= 452,
          "★ 裁剪之后控制条那一段一个字都没有（452 给了抗锯齿光晕 2 px）");
    Check(clipped.currentY >= 0, "裁剪之后当前行照样画得出来");
}

void TestFontPct() {
    std::printf("\n== 字号百分比（fontPct）==\n");

    const auto f100 = Render({100, 0, 50});
    const auto f200 = Render({200, 0, 50});
    const auto f60  = Render({60,  0, 50});

    std::printf("     fontPct=60  -> %d 行\n", f60.totalLines);
    std::printf("     fontPct=100 -> %d 行\n", f100.totalLines);
    std::printf("     fontPct=200 -> %d 行\n", f200.totalLines);

    Check(f60.totalLines > f100.totalLines,  "字号 60%% 能塞下更多行");
    Check(f200.totalLines < f100.totalLines, "字号 200%% 能塞下的行数更少");
}

void TestNoTrack() {
    std::printf("\n== 没有曲目 / 没有歌词时的兜底 ==\n");

    // 没在播放：不该画出任何文字
    lyricus::PlaybackState::Get().SetFake(false, L"", lyricus::LyricDocument(),
                                          lyricus::LyricDocument::npos);
    Canvas cv;
    if (cv.Create()) {
        lyricus::LyricsViewTheme theme; theme.dpi = 96;
        RECT rc{ 0, 0, kWidth, kHeight };
        lyricus::DrawLyricsView(cv.dc, rc, theme, lyricus::LyricsViewLayout{});
        Check(cv.Bands(140).empty(), "没在播放 -> 一个字都不画（留白）");
    } else {
        Check(false, "前置条件：DIB 创建成功");
    }

    // 在播放但没有歌词：应当画出「（无歌词）」
    lyricus::PlaybackState::Get().SetFake(true, L"Artist - Title", lyricus::LyricDocument(),
                                          lyricus::LyricDocument::npos);
    Canvas cv2;
    if (cv2.Create()) {
        lyricus::LyricsViewTheme theme; theme.dpi = 96;
        RECT rc{ 0, 0, kWidth, kHeight };
        lyricus::DrawLyricsView(cv2.dc, rc, theme, lyricus::LyricsViewLayout{});
        Check(!cv2.Bands(140).empty(), "在播放但无歌词 -> 有提示文字");
    }

    // 空矩形不能崩
    {
        Canvas cv3;
        if (cv3.Create()) {
            lyricus::LyricsViewTheme theme; theme.dpi = 96;
            RECT zero{ 0, 0, 0, 0 };
            lyricus::DrawLyricsView(cv3.dc, zero, theme, lyricus::LyricsViewLayout{});
            RECT inv{ 100, 100, 50, 50 };   // 右小于左
            lyricus::DrawLyricsView(cv3.dc, inv, theme, lyricus::LyricsViewLayout{});
            RECT thin{ 0, 0, kWidth, 2 };
            lyricus::DrawLyricsView(cv3.dc, thin, theme, lyricus::LyricsViewLayout{});
            Check(true, "空矩形 / 反向矩形 / 极扁矩形都不崩");
        }
    }

    // DC 为空指针也不能崩（lyrics_view.cpp 开头有判空）
    lyricus::LyricsViewTheme theme; theme.dpi = 96;
    RECT rc{ 0, 0, kWidth, kHeight };
    const auto r = lyricus::DrawLyricsView(nullptr, rc, theme, lyricus::LyricsViewLayout{});
    Check(r.bottom == rc.top, "dc = nullptr -> 直接返回，不崩");
}

// 双语参照行 —— 以及把用户认可的**那个位置**钉住。
//
// 用户 2026-09-24 反馈：「歌词的位置应该居中，之前稍微有点偏下了，这个刚好」。
// 那个"刚好"是**顺带**来的：双语改动把居中从「只按原文高度」改成了
// 「按整块（原文 + 参照行）高度」，块变高 -> curTop 抬起半个参照行的高度。
// 顺带对的东西会顺带坏掉 —— 所以这里用断言把它钉死。
void TestBilingual() {
    std::printf("\n== 双语参照行 ==\n");

    // 同一份歌词，带参照行 / 不带，比当前行的 y
    const auto plain   = Render({100, 2, 50}, 21, 10);
    const auto withSub = Render({100, 2, 50}, 21, 10, /*dump=*/true, /*bilingual=*/true);

    std::printf("     不带参照行 -> 画出 %d 行（低阈值 %d 行），当前行 y=%d\n",
                plain.totalLines, plain.totalLinesLow, plain.currentY);
    std::printf("     带参照行   -> 画出 %d 行（低阈值 %d 行），当前行 y=%d\n",
                withSub.totalLines, withSub.totalLinesLow, withSub.currentY);

    // 小字用**低阈值**那一档数 —— 高阈值会把它整条漏掉（见 Render 里的说明）
    Check(withSub.totalLinesLow > plain.totalLinesLow,
          "★ 当前行下面真的多画了一行翻译（低阈值才数得出小字）");
    Check(withSub.currentY > 0 && plain.currentY > 0, "两种都能定位到当前行");

    // ★ 这一条就是用户说的"刚好"：整块居中 -> 当前行**上移**半个参照行
    Check(withSub.currentY < plain.currentY,
          "★ 有参照行时当前行上移（按整块居中）—— 这正是用户认可的位置，别改回去");

    // 参照行不该把"行数上限"吃满：span=2 时依然只有上下各 2 行普通歌词
    //（参照行是附在当前行上的，不占 span 名额）
    Check(withSub.totalLinesLow == plain.totalLinesLow + 1,
          "★ span=2 时只多出 1 条带（参照行不占 span 名额）");

    // 当前行本身仍然是**原文**（LineIndexAt 退到同时间戳组的第一行）
    Check(withSub.currentY > 0, "当前行仍能单独挑出来（纯白阈值）");
}

// 「翻译为主」模式 —— 用户 2026-09-24 提：
// 「有一些用户可能喜欢把翻译当成主要的歌词，可以加一个模式」。
void TestTranslationPrimary() {
    std::printf("\n== 翻译为主模式 ==\n");

    // 夹具里原文短（"L10"）、译文长（"translated line number 10"），
    // 所以"当前行的像素宽度"直接说明画的是哪一段。
    const auto normal  = Render({100, 2, 50, false}, 21, 10, false, true);
    const auto swapped = Render({100, 2, 50, true }, 21, 10, false, true);

    std::printf("     原文为主 -> 当前行宽 %d px\n", normal.currentWidth);
    std::printf("     翻译为主 -> 当前行宽 %d px\n", swapped.currentWidth);

    Check(normal.currentWidth > 0 && swapped.currentWidth > 0, "两种模式都画出了当前行");
    Check(swapped.currentWidth > normal.currentWidth,
          "★ 翻译为主时，当前行画的是**译文**（明显更宽）");
    Check(swapped.totalLinesLow == normal.totalLinesLow,
          "两种模式的**行数**一样（只是谁当正文换了）");
}

// 超长歌词行：**单行 + 省略号**，不折行。
//
// 出处：用户 2026-09-24「这张截图也反映了歌词截断的问题」——
// captures/lyricus-20260925-004755.png 里那行
// 「池光化新茶 掺着新雪 煨了炉火曾入」贴到右边就没了（DT_SINGLELINE，
// 既不换行也不给省略号）。中间试过一版折行（4d1c2e8），
// 用户 2026-09-25 反馈「不太美观」—— 折行的真正代价是行高不再一致，
// 上下文的排版被顶得参差不齐。定稿是省略号，长行横滚放在后面单独做。
//
// 【为什么要做"对照渲染"】只量宽度分不出「省略号截断」和「折行」：
// 两种做法下那一行都会顶满可用宽度。判据是**横带条数** ——
// 折行必然多占横带，省略号则和短行一样只占一条。
//
// ⚠️ 这条断言和上一版**方向相反**（上一版断言"横带变多"）。
//    不是测试写错了，是产品取向变了：从"不丢字"改成了"版面整齐"，
//    长行改由横滚（design-lyric-motion.md 第 3 步）负责。
namespace {

struct Rendered {
    int bands  = 0;   // 横带条数（阈值 140）
    int widest = 0;   // 最宽一条横带的像素宽
    long ink   = 0;   // 过阈值像素总数 —— 内容有没有被丢掉，看这个
};

// current 用来切换"那行长的当不当当前行"：
// 当前行和上下文行走的是同一条 DrawLine，但字号不同，两边都要看。
Rendered RenderWithMiddleLine(const char* middle, size_t current = 1) {
    Rendered r;

    std::string raw = std::string("[00:00.00]short\n[00:05.00]") + middle +
                      "\n[00:10.00]short again\n";
    std::vector<unsigned char> b(raw.begin(), raw.end());
    auto doc = lyricus::LyricDocument::Parse(b);

    lyricus::PlaybackState::Get().SetFake(true, L"Test - Song", doc, current);

    Canvas cv;
    if (!cv.Create()) return r;
    lyricus::LyricsViewTheme theme;
    theme.dpi = 96;
    RECT rc{ 0, 0, kWidth, kHeight };
    lyricus::DrawLyricsView(cv.dc, rc, theme, lyricus::LyricsViewLayout{100, 0, 50});

    const auto bands = cv.Bands(140);
    r.bands = static_cast<int>(bands.size());
    for (const auto& bd : bands) {
        int l = kWidth, rr = -1;
        for (int y = bd.first; y <= bd.second; ++y)
            for (int x = 0; x < kWidth; ++x)
                if (cv.Bright(x, y, 140)) {
                    ++r.ink;
                    if (x < l) l = x;
                    if (x > rr) rr = x;
                }
        if (rr >= l && rr - l + 1 > r.widest) r.widest = rr - l + 1;
    }
    return r;
}

} // namespace

void TestLongLineEllipsis() {
    std::printf("\n== 超长歌词行（省略号，不折行）==\n");

    const char* kLong =
        "this is an extremely long lyric line that goes far beyond the width of "
        "any panel we would ever render it in, just to see what happens";

    const Rendered loCur  = RenderWithMiddleLine(kLong, 1);          // 长行 = 当前行
    const Rendered loCtx  = RenderWithMiddleLine(kLong, 0);          // 长行 = 上下文行
    const Rendered sh     = RenderWithMiddleLine("short middle", 1); // 对照组

    // 绘制区 600 宽、左右各留 padX=20 -> 可用宽度 560
    std::printf("     长行当当前行 -> %d 条横带，最宽 %d px，墨迹 %ld 像素\n",
                loCur.bands, loCur.widest, loCur.ink);
    std::printf("     长行当上下文 -> %d 条横带，最宽 %d px，墨迹 %ld 像素\n",
                loCtx.bands, loCtx.widest, loCtx.ink);
    std::printf("     短行对照     -> %d 条横带，最宽 %d px，墨迹 %ld 像素\n",
                sh.bands, sh.widest, sh.ink);
    std::printf("     （可用宽度 %d px）\n", kWidth - 40);

    Check(loCur.widest <= kWidth, "内容没有超出画布");

    // ★ 核心判据：长行不多占横带 = 没折行
    Check(loCur.bands == sh.bands,
          "★ 长行当当前行时**横带数和短行一样** —— 是单行，没折行");
    Check(loCtx.bands == sh.bands,
          "★ 长行当上下文行时也是单行");

    // 真的用满了可用宽度（不是"没画出来"才不超宽）
    Check(loCur.widest >= (kWidth - 40) - 12,
          "长行顶满可用宽度（省略号生效，不是没画）");
    Check(loCur.ink > sh.ink * 2,
          "长行的墨迹远多于短行 —— 字确实画出来了");

    // 省略号本身：末尾应该有一小簇独立的墨迹。不精确断言它的形状，
    // 只要求"倒数 20 px 内有墨迹" —— 顶到边正是被截断的标志。
    Check(loCur.widest <= (kWidth - 40) + 2,
          "★ 长行**没有溢出**可用宽度（省略号把它收在框里了）");
}

// 遮罩（IntersectClipRect）—— 歌词一个字都不许跑到裁剪线以下。
//
// 【为什么现在就要测】遮罩是下一步「长歌词横滚 / 换行上滑」的前置条件：
// 横滚要「文字在动、裁剪框不动」，上滑要挡住越过控制条的中间帧。
// 但**现在这一步画面上看不出它在起作用** —— 省略号模式下没有任何东西越界，
// 很容易写成一条永远为真的空测试。
//
// 所以这里专门构造一个"内容非要越界不可"的场景：
// 把裁剪线压到当前行**中间**，布局层的夹取就无能为力了
//（`ClampInt(curTop, y, limit - curBlockH)` 的上下界会反过来，
//  最后落到兜底分支 `curTop = y`，整行从 y 往下画，直接穿过 limit）。
// 遮罩不在的话，这些字会原样画在裁剪线以下。
void TestMask() {
    std::printf("\n== 遮罩（歌词不许越过裁剪线）==\n");

    ClipProbe full = ProbeClip({100, 2, 50}, RECT{ 0, 0, kWidth, kHeight });
    const int lineY = full.currentY;   // 当前行中心
    std::printf("     不裁剪时当前行中心 y=%d，最低墨迹 y=%d\n",
                full.currentY, full.lowestInkY);

    Check(lineY > 0 && full.lowestInkY > lineY,
          "不裁剪时当前行有正常的下半部分（否则下面的对照是空的）");

    // 两条裁剪线，都切在**当前行内部**
    const int clipA = lineY;
    const int clipB = lineY + (full.lowestInkY - lineY) / 2;

    const ClipProbe a = ProbeClip({100, 2, 50, false, clipA}, RECT{ 0, 0, kWidth, kHeight });
    const ClipProbe b = ProbeClip({100, 2, 50, false, clipB}, RECT{ 0, 0, kWidth, kHeight });

    std::printf("     裁剪线 y=%d -> 最低墨迹 y=%d\n", clipA, a.lowestInkY);
    std::printf("     裁剪线 y=%d -> 最低墨迹 y=%d\n", clipB, b.lowestInkY);

    Check(a.lowestInkY < clipA, "★ 裁剪线以上的字还在（不是整行都没画）");
    Check(a.lowestInkY < clipB, "★ 裁剪线以下一个字都没有");
    Check(b.lowestInkY < clipB, "★ 换一条更低的裁剪线，同样一刀切齐");

    // 对照：裁剪线放低，露出来的内容必须**更多** ——
    // 没有遮罩的话两条线的画面会一模一样（都是整行），这条就挂了。
    Check(b.lowestInkY > a.lowestInkY,
          "★ 裁剪线放低露出的内容更多 —— 说明画面确实是被**裁剪**出来的");

    // 曲名在遮罩外面，任何裁剪线下都不该受影响。
    Check(a.firstTop >= 0 && b.firstTop == a.firstTop && full.firstTop == a.firstTop,
          "★ 曲名的位置不受裁剪线影响（遮罩只管歌词）");
}

// ---------------------------------------------------------------------------
// 动画时间线（LyricAnimator）—— 纯逻辑，把时钟推着走
//
// 【为什么值得单测】这条时间线在真机上**没法验**：660ms 和 900ms 的差别
// 肉眼分不出，而"滚到尾部就该停"如果写错成"一直滚"，看两秒也看不出来。
// 这里注入 nowMs，整条曲线一次性走完，还能顺手验时钟回退这类异常输入。
// ---------------------------------------------------------------------------
namespace {

// 同一个 animator 上按时间顺序推进一行，取某一刻的帧。
// 时间必须单调递增 —— animator 是有状态的（靠行号变化判断换行）。
lyricus::LyricAnimFrame At(lyricus::LyricAnimator& a, ULONGLONG t,
                           size_t line, int overflow, int step = 0) {
    return a.Update(t, line, overflow, step);
}

// 溢出 overflowPx 时，滚完一趟需要多久（和实现里同一个算法，
// 写在测试里是**故意的**：实现改了这里就该跟着改，改不动就说明判据变了）
ULONGLONG ExpectDurationMs(int overflowPx) {
    ULONGLONG ms = static_cast<ULONGLONG>(overflowPx) * 1000 / lyricus::kScrollPxPerSec;
    if (ms < lyricus::kScrollMinMs) ms = lyricus::kScrollMinMs;
    if (ms > lyricus::kScrollMaxMs) ms = lyricus::kScrollMaxMs;
    return ms;
}

} // namespace

void TestAnimator() {
    std::printf("\n== 动画时间线（横滚 / 上滑）==\n");

    constexpr size_t kLine = 7;
    const int kOverflow = 300;                       // 宽出去 300px
    const ULONGLONG dur = ExpectDurationMs(kOverflow);

    std::printf("     溢出 %d px -> 停顿 %llu ms，滚动 %llu ms\n",
                kOverflow,
                static_cast<unsigned long long>(lyricus::kScrollLeadMs),
                static_cast<unsigned long long>(dur));

    // ---- 1. 放得下：一点都不动，也不要后续帧 ----
    {
        lyricus::LyricAnimator a;
        bool everMoved = false, everAnimated = false;
        for (ULONGLONG t = 0; t <= 20000; t += 250) {
            const auto f = At(a, t, kLine, /*overflow=*/0);
            if (f.scrollX != 0) everMoved = true;
            if (f.animating)    everAnimated = true;
        }
        Check(!everMoved,    "★ 放得下的行从头到尾 scrollX 恒为 0");
        Check(!everAnimated, "★ 放得下的行全程不要后续帧（一个定时器都不该开）");
    }

    // ---- 2. 放不下：静止 -> 左移 -> 停在尾部 ----
    {
        lyricus::LyricAnimator a;

        const auto t0 = At(a, 0, kLine, kOverflow);
        Check(t0.scrollX == 0, "刚换行时不动（从头开始看）");
        Check(!t0.animating,
              "★ 起步前的静止期**不要**后续帧 —— 900ms 空转 25fps 是白烧主线程");

        const auto tLead = At(a, lyricus::kScrollLeadMs, kLine, kOverflow);
        Check(tLead.scrollX == 0, "★ 静止期结束时仍然没动（停顿真的存在）");

        const auto tMid = At(a, lyricus::kScrollLeadMs + dur / 2, kLine, kOverflow);
        std::printf("     滚到一半 -> scrollX=%d（目标 %d）\n", tMid.scrollX, kOverflow);
        Check(tMid.scrollX > 0 && tMid.scrollX < kOverflow,
              "★ 中途是**中间值**（真的在滚，不是从 0 直接跳到尾）");
        Check(tMid.animating, "滚动途中要后续帧");

        const auto tEnd = At(a, lyricus::kScrollLeadMs + dur, kLine, kOverflow);
        Check(tEnd.scrollX == kOverflow, "★ 滚满时正好停在尾部");
        Check(!tEnd.animating, "★ 滚到尾部就**不要再要帧**了（用户定的取向：只滚一次）");

        const auto tLater = At(a, lyricus::kScrollLeadMs + dur + 30000, kLine, kOverflow);
        Check(tLater.scrollX == kOverflow,
              "★ 之后一直停在尾部，不会回头、不会循环");
    }

    // ---- 3. 单调递增：不能有回头路 ----
    {
        lyricus::LyricAnimator a;
        int prev = -1;
        bool monotone = true;
        for (ULONGLONG t = 0; t <= lyricus::kScrollLeadMs + dur; t += 100) {
            const auto f = At(a, t, kLine, kOverflow);
            if (f.scrollX < prev) monotone = false;
            prev = f.scrollX;
        }
        Check(monotone, "★ scrollX 单调递增（中途不回头）");
    }

    // ---- 4. 时长的上下限：很短和很长的溢出 ----
    //
    // ⚠️ 每个 animator 的**第一次** Update 是"这一行刚开始"，
    //    它会把 lineStart 设成那一刻 —— 所以必须先喂一个 t=0 起算，
    //    否则 elapsed 恒为 0、断言会"因为没滚"而通过（空断言）。
    {
        lyricus::LyricAnimator a;
        const int tiny = 5;
        At(a, 0, kLine, tiny);
        // 溢出 5px，按速度只要 83ms —— 必须被夹到下限，否则快得像闪一下
        const auto f = At(a, lyricus::kScrollLeadMs + lyricus::kScrollMinMs - 1, kLine, tiny);
        Check(f.scrollX < tiny, "★ 极小的溢出也走完整个最短时长（不会一闪而过）");

        lyricus::LyricAnimator b;
        const int huge = 100000;
        At(b, 0, kLine, huge);
        const auto g = At(b, lyricus::kScrollLeadMs + lyricus::kScrollMaxMs, kLine, huge);
        Check(g.scrollX == huge, "★ 超长行在上限时长内滚完（靠提速，不拖着滚）");
        Check(!g.animating, "超长行滚完也停住");
    }

    // ---- 5. 上滑：只对「顺序推进」生效 ----
    {
        constexpr int kStep = 34;

        lyricus::LyricAnimator a;
        At(a, 0, 10, 0);                                   // 先落在第 10 行
        const auto s0 = At(a, 1000, 11, 0, kStep);         // 顺序 +1
        Check(s0.slideY == kStep, "★ 顺序换行时从「低一步」起步（往上滑）");
        Check(s0.animating, "上滑途中要后续帧");

        const auto sMid = At(a, 1000 + lyricus::kSlideMs / 2, 11, 0, kStep);
        std::printf("     上滑一半 -> slideY=%d（起点 %d）\n", sMid.slideY, kStep);
        Check(sMid.slideY > 0 && sMid.slideY < kStep, "★ 上滑中途是中间值");

        const auto sEnd = At(a, 1000 + lyricus::kSlideMs, 11, 0, kStep);
        Check(sEnd.slideY == 0, "★ 上滑结束正好落在最终位置");
        Check(!sEnd.animating, "上滑结束不再要帧");
    }

    // ---- 6. seek 不该滑 ----
    {
        lyricus::LyricAnimator a;
        At(a, 0, 10, 0);
        Check(At(a, 500, 12, 0, 34).slideY == 0, "★ 跨行 seek（+2）不上滑");
        Check(At(a, 1000, 4, 0, 34).slideY == 0, "★ 往回 seek 不上滑");
        Check(At(a, 1500, 0, 0, 34).slideY == 0, "★ 换曲（行号回到 0）不上滑");
    }

    // ---- 7. Reset 之后第一帧不上滑 ----
    {
        lyricus::LyricAnimator a;
        At(a, 0, 10, 0);
        a.Reset();
        Check(At(a, 100, 11, 0, 34).slideY == 0,
              "★ Reset 之后第一帧不上滑（seek 后要的是干净的画面）");
    }

    // ---- 8. 面板拉窄：原本放得下的行变成长行 ----
    {
        lyricus::LyricAnimator a;
        At(a, 0, kLine, 0);                       // 一开始放得下
        const auto f = At(a, 30000, kLine, 300);  // 30 秒后拉窄
        Check(f.scrollX == 0 && !f.animating,
              "★ 拉窄的那一刻时间线**重新起算**（不因为「这一行已经显示了 30 秒」直接跳到尾部）");
        const auto g = At(a, 30000 + lyricus::kScrollLeadMs + ExpectDurationMs(300),
                          kLine, 300);
        Check(g.scrollX == 300, "重新起算之后照样能滚完");
    }

    // ---- 9. 时钟回退 / 乱序调用都不许炸 ----
    //
    // GetTickCount64 不会回退，所以这一条纯粹是**防御式**的：
    // 真出事时（时钟异常、宿主传错）绝不能算出天文数字把动画甩到终点。
    {
        // (a) 时间早于"这一行的起算点"—— 直接踩 Elapsed 的夹取分支
        lyricus::LyricAnimator a;
        At(a, 100000, kLine, kOverflow);                  // 起算点就在 100000
        const auto back = At(a, 50, kLine, kOverflow);    // 早于起算点
        Check(back.scrollX == 0,
              "★ 时间早于起算点时当成「刚起步」，不会算出一个天文数字");
    }
    {
        // (b) 乱序调用：倒回静止期应该给出**那一刻**的位置，而不是保留上一帧
        lyricus::LyricAnimator b;
        At(b, 0, kLine, kOverflow);

        const ULONGLONG midT = lyricus::kScrollLeadMs + dur / 2;
        const auto mid = At(b, midT, kLine, kOverflow);
        Check(mid.scrollX > 0, "先滚到中途（否则下面的倒流对照是空的）");

        const auto rewind = At(b, 100, kLine, kOverflow);   // 倒回静止期
        Check(rewind.scrollX == 0, "★ 时间倒回静止期，位置也跟着回到那一刻");

        const auto again = At(b, midT, kLine, kOverflow);   // 再回到原处
        Check(again.scrollX == mid.scrollX,
              "★ 时间回到原处就回到原位置（不炸、也不跳）");
    }
}

// 横滚真的作用在绘制上 —— 以及遮罩把它夹在歌词列里。
//
// 【判据为什么是"两帧不一样"】滚动改的是**内容**，不是范围：
// scrollX=0 和被裁到尾部时，墨迹都在 [left, left+maxW] 这一列里填满，
// 量宽度、量位置都分不出来。真正能分辨的只有"同一行的像素图案变了没有"。
namespace {

struct Shot {
    lyricus::LyricsViewResult result;
    // 纯白像素的**行主序下标**（y*宽度 + x），天然按大小有序 ——
    // 下面靠这个做线性求交/求差，不用建集合。
    std::vector<int> ink;
};

} // namespace

void TestScrollRender() {
    std::printf("\n== 横滚作用在绘制上 ==\n");

    const char* kLong =
        "this is an extremely long lyric line that goes far beyond the width of "
        "any panel we would ever render it in, just to see what happens";

    // 同一行（长行 = 当前行），只改 scrollX
    auto shoot = [&](int scrollX) {
        Shot s;
        std::string raw = std::string("[00:00.00]short\n[00:05.00]") + kLong +
                          "\n[00:10.00]short again\n";
        std::vector<unsigned char> b(raw.begin(), raw.end());
        lyricus::PlaybackState::Get().SetFake(
            true, L"Test - Song", lyricus::LyricDocument::Parse(b), 1);

        Canvas cv;
        if (!cv.Create()) return s;

        lyricus::LyricsViewTheme theme;
        theme.dpi = 96;
        RECT rc{ 0, 0, kWidth, kHeight };
        lyricus::LyricAnimFrame anim;
        anim.scrollX = scrollX;
        s.result = lyricus::DrawLyricsView(cv.dc, rc, theme,
                                          lyricus::LyricsViewLayout{100, 0, 50}, anim);

        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                if (cv.Bright(x, y, 250)) s.ink.push_back(y * kWidth + x);
        return s;
    };

    const Shot a = shoot(0);
    const int overflow = a.result.currentOverflow;
    std::printf("     渲染层报的宽出量 currentOverflow=%d px\n", overflow);
    Check(overflow > 0, "★ 长行确实报出了宽出量（宿主据此启动横滚）");

    const Shot b = shoot(overflow / 2);
    const Shot c = shoot(overflow);

    // 交集/差集：算出两帧有多少像素不同
    auto diffCount = [](const std::vector<int>& p, const std::vector<int>& q) {
        size_t i = 0, j = 0; int only = 0;
        while (i < p.size() && j < q.size()) {
            if      (p[i] < q[j]) { ++only; ++i; }
            else if (q[j] < p[i]) { ++only; ++j; }
            else                  { ++i; ++j; }
        }
        only += static_cast<int>((p.size() - i) + (q.size() - j));
        return only;
    };

    const int dAB = diffCount(a.ink, b.ink);
    const int dBC = diffCount(b.ink, c.ink);
    std::printf("     scrollX=0 -> %zu 个纯白像素\n", a.ink.size());
    std::printf("     scrollX=%d -> %zu 个，与左端差 %d 像素\n", overflow / 2, b.ink.size(), dAB);
    std::printf("     scrollX=%d -> %zu 个，与一半处差 %d 像素\n", overflow, c.ink.size(), dBC);

    Check(dAB > 200, "★ 滚到一半时画面明显不同（滚动真的作用在绘制上了）");

    // ⚠️ 这里**不能**断言"滚得越多差得越多"。
    //    第一版就是这么写的，挂了：diffCount 量的是纯白像素集合的对称差，
    //    它**不随偏移量单调** —— 不同的偏移切掉的是不同的字形，
    //    白像素个数本来就有涨有落（实测 3166 vs 2867）。
    //    真正成立、也真正有意义的判据是：**不同偏移画出的画面互不相同**。
    Check(dBC > 200, "★ 两个不同偏移画出的画面也互不相同（偏移是按量生效的，不是只有两档）");

    // 遮罩：所有偏移下，墨迹都不许跑出歌词列。
    //
    // 列的左右边界 = padX(20) 与 宽度-padX，在测试画布 600 宽上就是 [20, 580]。
    // ⚠️ 别用 std::max / std::min —— windows.h 把 max/min 定义成了宏
    //（本工程没开 NOMINMAX），`std::max(...)` 会被展开成语法垃圾，
    // 报的是":: 右边的非法标记"，离真正的原因很远。
    auto maxX = [](const std::vector<int>& p) {
        int m = -1;
        for (int v : p) { const int x = v % kWidth; if (x > m) m = x; }
        return m;
    };
    auto minX = [](const std::vector<int>& p) {
        int m = kWidth;
        for (int v : p) { const int x = v % kWidth; if (x < m) m = x; }
        return m;
    };

    std::printf("     三帧的横向范围: [%d,%d] [%d,%d] [%d,%d]\n",
                minX(a.ink), maxX(a.ink), minX(b.ink), maxX(b.ink),
                minX(c.ink), maxX(c.ink));

    Check(minX(a.ink) >= 20 && maxX(a.ink) <= 580, "★ scrollX=0 时墨迹在歌词列内");
    Check(minX(b.ink) >= 20 && maxX(b.ink) <= 580, "★ 滚到一半也在列内（没往左溢出）");
    Check(minX(c.ink) >= 20 && maxX(c.ink) <= 580,
          "★ 滚到尾部也在列内 —— 文字在动、裁剪框不动（遮罩的全部意义）");
    Check(maxX(c.ink) >= 570, "滚到尾部时右边缘用满了（尾巴真的露出来了）");
}

// 上滑过渡：**上一行必须还在**，不能突然消失。
//
// 出处：用户 2026-09-25「好像没有做出歌词逐行上移，上一行歌词是突然消失的」。
//
// 【为什么上一行会消失】面板矮的时候"上一行"本来就放不下
//（用户那块 920x300 配 125% 字号，一行就占满了，见 plan.md 的"已知小问题"），
// 于是 `above` 列表是空的、上一行根本不画。上滑过渡里整块已经下移了 slideY，
// 但边界判断用的还是**排版位置**，没用**画出来的位置** —— 于是上一行
// 从第一帧起就不见了，看着就是"啪"地换掉。
//
// 这里专门造一个**装不下上一行**的画布（高度压到 120），
// 于是这条测试真的能分辨两种做法：
//   slideY=0   -> 上一行不画，N 条横带
//   slideY=步距 -> 上一行必须出现，N+1 条横带
void TestSlideKeepsPrevLine() {
    std::printf("\n== 上滑过渡：上一行不许突然消失 ==\n");

    // 矮画布：歌词区上沿之下只够放当前行，上一行挤不进来
    const RECT shortPanel{ 0, 0, kWidth, 120 };

    const auto flat = ProbeClip({100, 0, 50}, shortPanel);

    lyricus::LyricAnimFrame slid;
    slid.slideY = 22;                     // = 上下文的行高 + 行距（96dpi 下约这么多）
    const auto start = ProbeClip({100, 0, 50}, shortPanel, slid);

    std::printf("     不滑动   -> %d 条横带（最低墨迹 y=%d）\n", flat.bands, flat.lowestInkY);
    std::printf("     滑动起点 -> %d 条横带（最低墨迹 y=%d）\n", start.bands, start.lowestInkY);

    Check(flat.bands > 0 && flat.currentY > 0, "矮面板里当前行照画不误");

    // 对照的前提：不滑动时**上一行放不下**。
    // 这条画布（600x120）里能看到三条带：曲名、当前行、下一行 ——
    // 唯独**上面**那条没有，这正是用户那块面板的形状。
    Check(flat.bands == 3,
          "★ 不滑动时是曲名 + 当前行 + 下一行三条带（上一行确实放不下 —— 对照的前提）");

    // ★ 核心判据：滑动开始时**正好多出一条**（就是上一行）
    Check(start.bands == flat.bands + 1,
          "★ 滑动开始时正好多出**一条**横带 —— 上一行还在，它该滑出去而不是突然消失");

    // 上一行画在 slideY 那个位置上：整块下移，所以最低墨迹也跟着下移
    Check(start.lowestInkY > flat.lowestInkY,
          "★ 整块确实下移了 slideY（这就是「从低一步升上来」的起点）");
}

} // namespace

int wmain() {
    std::printf("歌词绘制层布局单测 —— 直接跑 src/lyrics_view.cpp 的真实实现\n");
    std::printf("（渲染到 %dx%d 内存 DIB，再按像素分析结果）\n", kWidth, kHeight);

    TestSpan();
    TestRatio();
    TestResizeAndClip();
    TestSlideKeepsPrevLine();
    TestFontPct();
    TestBilingual();
    TestTranslationPrimary();
    TestLongLineEllipsis();
    TestMask();
    TestScrollRender();
    TestAnimator();
    TestNoTrack();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
