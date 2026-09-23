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
#include "playback_state.h"   // 单测里这个是替身（见 shim/playback_state.h）

#include <windows.h>

#include <cstdio>
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
                            const std::wstring& lyricPath) {
    m_hasTrack    = hasTrack;
    m_displayName = displayName;
    m_lyrics      = std::move(lyrics);
    m_currentLine = currentLine;
    m_lyricPath   = lyricPath;
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

lyricus::LyricDocument MakeDoc(int n) {
    const std::string s = MakeLrc(n);
    std::vector<unsigned char> b(s.begin(), s.end());
    return lyricus::LyricDocument::Parse(b);
}

// 渲染一次，返回：总行数、当前行中心 y、歌词区高度
struct RenderResult {
    int totalLines   = 0;
    int currentY     = -1;
    int firstTop     = -1;
    int lastBottom   = -1;
};

RenderResult Render(const lyricus::LyricsViewLayout& layout, int lineCount = 21, int current = 10,
                    bool dump = false) {
    RenderResult r;

    lyricus::PlaybackState::Get().SetFake(true, L"Test - Song", MakeDoc(lineCount),
                                          static_cast<size_t>(current));

    Canvas cv;
    if (!cv.Create()) return r;

    lyricus::LyricsViewTheme theme;
    theme.dpi = 96;
    // 用默认配色：currentText = 纯白 (255,255,255)，
    // normalText = (172,172,180)。阈值 250 就能把当前行单独挑出来。

    RECT rc{ 0, 0, kWidth, kHeight };
    const int ret = lyricus::DrawLyricsView(cv.dc, rc, theme, layout);

    // 阈值 140：连暗一点的普通行也算进来
    const auto all = cv.Bands(140);
    r.totalLines = static_cast<int>(all.size());
    if (!all.empty()) {
        r.firstTop   = all.front().first;
        r.lastBottom = all.back().second;
    }

    // 阈值 250：只有当前行（纯白）能达到
    const auto cur = cv.Bands(250, 1);
    if (!cur.empty()) r.currentY = (cur.front().first + cur.front().second) / 2;

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
    const int r = lyricus::DrawLyricsView(nullptr, rc, theme, lyricus::LyricsViewLayout{});
    Check(r == rc.top, "dc = nullptr -> 直接返回，不崩");
}

} // namespace

int wmain() {
    std::printf("歌词绘制层布局单测 —— 直接跑 src/lyrics_view.cpp 的真实实现\n");
    std::printf("（渲染到 %dx%d 内存 DIB，再按像素分析结果）\n", kWidth, kHeight);

    TestSpan();
    TestRatio();
    TestFontPct();
    TestNoTrack();

    std::printf("\n----------------------------------------\n");
    std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
