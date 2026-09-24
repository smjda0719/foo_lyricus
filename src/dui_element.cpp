#include "stdafx.h"
#include "lyrics_view.h"
#include "config.h"
#include "playback_state.h"
#include "debug_log.h"

// ui_element_impl_withpopup 不在 SDK 的聚合头里 —— 它住在 helpers/BumpableElem.h。
// 这个头会连带拉进 libPPUI/CFlashWindow.h（"bump" 时闪一下窗口），而
// CFlashWindow::ShowAbove 用了 libPPUI 的 WIN32_OP 宏，会引出一个 WIN32_OP_FAIL()
// 外部符号。官方 foo_sample 是靠 ProjectReference 引 libPPUI 工程解决的
//（foo_sample.vcxproj:357），本工程的 vcxproj 目前没有引用它 —— 详见交付说明。
#include <helpers/BumpableElem.h>

namespace lyricus {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// 刷新定时器。250ms 对行级歌词足够；将来做逐字歌词要么提速，要么换成高精度
// 计时器插值（playback_state.h 里对 RefreshPosition 的说明）。
// id 用 2：独立面板已经占了 1（control_window.cpp 的 kRefreshTimerId），别撞。
constexpr UINT_PTR kRefreshTimerId  = 2;
constexpr UINT     kRefreshInterval = 250;

// 元素配置格式。目前只有一个字段：面板至少要能装下几行歌词。
// 版本号留着是为了以后加字段时能认出旧数据。
constexpr t_uint32 kConfigVersion       = 1;
constexpr int      kDefaultVisibleLines = 5;
constexpr int      kMinVisibleLines     = 3;
constexpr int      kMaxVisibleLines     = 15;

// 96 dpi 下的度量，实际用的时候按 dpi 缩放
constexpr int kMinWidth96   = 160;
constexpr int kLineHeight96 = 22;
constexpr int kPadding96    = 8;

// 元素 GUID。沿用工程的 1A7C3E90-...-4Dxx 段（见 config.cpp:17 的说明），
// 末字节 0x10 在 config.cpp / menu.cpp 里都没被占用。
const GUID guid_dui_element = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x10}};

// 在 from→to 之间线性混合。t 为负表示朝 to 的反方向外推（用来加强对比），
// 结果按 0..255 夹紧。
COLORREF BlendColor(COLORREF from, COLORREF to, float t) {
    const auto mix = [t](BYTE a, BYTE b) -> BYTE {
        const float v = static_cast<float>(a) +
                        (static_cast<float>(b) - static_cast<float>(a)) * t;
        if (v <= 0.0f)   return 0;
        if (v >= 255.0f) return 255;
        return static_cast<BYTE>(v + 0.5f);
    };
    return RGB(mix(GetRValue(from), GetRValue(to)),
               mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

int ClampVisibleLines(int lines) {
    if (lines < kMinVisibleLines) return kMinVisibleLines;
    if (lines > kMaxVisibleLines) return kMaxVisibleLines;
    return lines;
}

// ---------------------------------------------------------------------------
// DUI 元素实例
// ---------------------------------------------------------------------------

// 消息全部由这个类自己处理：本 SDK 的 ui_element_instance **没有**
// handle_message / on_message 之类的虚函数入口（ui_element.h:266-326 里
// 只有 get_wnd / set_configuration / get_configuration / get_guid /
// get_subclass / get_min_max_info / notify 这些），所以必须自己带一个
// WTL 窗口类 —— 和官方 foo_sample/ui_element.cpp 的做法一致。
class LyricusDui : public ui_element_instance, public CWindowImpl<LyricusDui> {
public:
    // 窗口类。背景刷传 -1，ATL 展开成 (HBRUSH)(-1 + 1) = NULL（atlwin.h:3139），
    // 也就是「不擦背景」—— 底色由我们自己的 WM_ERASEBKGND 铺，
    // 否则会先闪一下系统窗口色再画，拖动时尤其明显。
    DECLARE_WND_CLASS_EX(TEXT("{1A7C3E90-2B41-4C58-9D6E-0F1A2B3C4D10}"), CS_VREDRAW | CS_HREDRAW, (-1));

    // 构造签名由 fb2k::newUIElement 定死（atl-misc.h:319-323 把
    // instantiate_helper 收到的 cfg / callback 原样转发过来）。
    LyricusDui(ui_element_config::ptr config, ui_element_instance_callback::ptr callback);

    // ---- ui_element_instance 的纯虚契约，必须实现 ----
    // get_wnd 返回 *this：CWindowImpl 有 operator HWND()。
    HWND get_wnd() override { return *this; }
    void set_configuration(ui_element_config::ptr config) override;
    ui_element_config::ptr get_configuration() override { return m_config; }
    ui_element_min_max_info get_min_max_info() override;
    void notify(const GUID& what, t_size param1, const void* param2, t_size param2size) override;

    // ---- 元素身份：ui_element_impl<> 会读这几个静态函数 ----
    static GUID g_get_guid() { return guid_dui_element; }
    static GUID g_get_subclass() { return ui_element_subclass_playback_information; }
    static void g_get_name(pfc::string_base& out) { out = "Lyricus Lyrics"; }
    static ui_element_config::ptr g_get_default_configuration();
    static const char* g_get_description();

    // 宿主把对象建好之后才调它，这时候才真正创建子窗口
    //（atl-misc.h:321：newUIElement 里 new 完立刻调 initialize_window）。
    void initialize_window(HWND parent);

    BEGIN_MSG_MAP_EX(LyricusDui)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

protected:
    // **必须**是 protected：ui_element_impl_withpopup<> 生成的
    // ImplementBumpableElem::_bump() 会直接访问 this->m_callback
    //（BumpableElem.h:45），改成 private 直接编译不过。
    const ui_element_instance_callback_ptr m_callback;

private:
    // ---- 窗口消息 ----
    LRESULT OnEraseBkgnd(UINT, WPARAM wParam, LPARAM, BOOL& bHandled);
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL& bHandled);
    LRESULT OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& bHandled);
    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL& bHandled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& bHandled);

    // ---- 主题 ----
    // 颜色/字体都归宿主管，但**不能每次重绘都去问**：query_font_ex 返回的句柄
    // 生命周期只到下一次字体变更回调（ui_element.h:151 的原文注释），
    // 所以只在变更通知里问一次，自己缓存住。
    void     RefreshTheme();
    COLORREF QueryBackground() const;
    HBRUSH   EnsureBackgroundBrush();

    // ---- 配置 ----
    void ApplyConfig(ui_element_config::ptr config);

    ui_element_config::ptr m_config;

    // 缓存的主题。dpi 由重绘路径维护（见 OnPaint），颜色来自 RefreshTheme()。
    LyricsViewTheme m_theme;

    // 背景刷：WM_ERASEBKGND 每帧都要用，现建现毁太浪费；跟着主题一起重建。
    HBRUSH m_bgBrush = nullptr;

    // 宿主字体的快照 —— 刻意**只存 LOGFONT，不存 HFONT**，后者随时会失效。
    LOGFONTW m_fontDesc{};
    bool     m_hasFontDesc = false;

    // 面板承诺能完整显示几行歌词。只影响最小高度，见 get_min_max_info()。
    // 注意它**不是**实际显示行数 —— 真正画几行由用户设置（m_layout.span）决定，
    // span = 0 时按可用高度自适应。这个字段只用来撑住最小高度，
    // 保证面板不会被拉到连一行都放不下。
    int m_visibleLines = kDefaultVisibleLines;

    // 上一次重绘时看到的 PlaybackState 代次。初值 0 与真实代次（从 1 起）
    // 必然不等，所以第一帧一定会重绘一次 —— 正好把空白面板填上。
    unsigned m_lastRevision = 0;

    // 上一次**因为播放位置变化**而重绘的时刻（GetTickCount64）。
    // 把「位置在走」那种重绘节流到每秒一次 —— 完整理由见 playback_state.h
    // 里 kPositionRepaintMs 的说明。初值 0 让第一次位置变化就能通过。
    ULONGLONG m_lastPositionRepaint = 0;

    // 用户设置（字号 / 行数 / 当前行位置）。
    // 高级首选项的改动**没有任何通知机制**，只能在定时器里轮询比对 ——
    // 三个 int 的比较，代价可以忽略。详见 settings.cpp 的说明。
    LyricDisplayConfig m_displayCfg;   // 上一次看到的原始设置，用来判断"变了没"
    LyricsViewLayout   m_layout;       // m_displayCfg 的渲染视图，跟着它一起更新
};

LyricusDui::LyricusDui(ui_element_config::ptr config, ui_element_instance_callback::ptr callback)
    : m_callback(callback), m_config(config) {
    // 宿主理论上会给一份配置；给空的话自己补默认值，免得 get_configuration()
    // 回一个空指针出去（宿主保存布局时会用到它）。
    if (m_config.is_empty()) m_config = g_get_default_configuration();
    ApplyConfig(m_config);
}

ui_element_config::ptr LyricusDui::g_get_default_configuration() {
    ui_element_config_builder builder;
    builder << kConfigVersion << static_cast<t_uint32>(kDefaultVisibleLines);
    return builder.finish(guid_dui_element);
}

const char* LyricusDui::g_get_description() {
    return "Displays synchronized lyrics of the currently playing track.";
}

void LyricusDui::initialize_window(HWND parent) {
    // 和官方 foo_sample/ui_element.cpp:20 同款：Create 失败必须抛出去，
    // 让宿主知道实例化没成功 —— 留一个 get_wnd() 返回 NULL 的元素在布局里，
    // 宿主后面迟早会踩空，而且那时候已经查不出是谁的锅了。
    if (Create(parent) == nullptr) {
        const DWORD err = ::GetLastError();
        DebugLog("DUI: 创建子窗口失败，GetLastError=%lu", err);
        throw exception_win32(err);
    }

    // 窗口建好之后再问一次主题：构造阶段还没有 HWND，
    // 而 query_color / query_font_ex 都可能要看窗口的深浅模式上下文。
    RefreshTheme();

    ::SetTimer(m_hWnd, kRefreshTimerId, kRefreshInterval, nullptr);
}

void LyricusDui::set_configuration(ui_element_config::ptr config) {
    m_config = config.is_valid() ? config : g_get_default_configuration();
    ApplyConfig(m_config);
}

void LyricusDui::ApplyConfig(ui_element_config::ptr config) {
    t_uint32 version = 0;
    t_uint32 lines   = static_cast<t_uint32>(kDefaultVisibleLines);

    if (config.is_valid()) {
        try {
            ui_element_config_parser parser(config);
            parser >> version >> lines;
        } catch (exception_io_data) {
            // 空配置、数据被截断、格式对不上都落在这里
            //（exception_io_data_truncation 也派生自它，见 SDK/exception_io.h:16-18）。
            // **一律回退默认值** —— 把没解析成功的残留当配置用，比用默认值危险得多。
            version = 0;
            lines   = static_cast<t_uint32>(kDefaultVisibleLines);
        }
    }

    // 版本对不上说明这段数据不是我们这一代写进去的，同样回到默认值。
    if (version != kConfigVersion) lines = static_cast<t_uint32>(kDefaultVisibleLines);

    const int wanted = ClampVisibleLines(static_cast<int>(lines));
    if (wanted == m_visibleLines) return;

    m_visibleLines = wanted;

    // 行数变了 → 最小高度跟着变，必须让宿主重问一次尺寸约束。
    // 窗口还没建时不通知：构造阶段宿主正处在 instantiate() 里面，
    // 这时候回调进去属于自己咬自己；它随后本来就会查一遍尺寸。
    if (m_hWnd != nullptr && m_callback.is_valid()) {
        m_callback->on_min_max_info_change();
    }
}

ui_element_min_max_info LyricusDui::get_min_max_info() {
    // TODO(未验证): 两处存疑 ——
    //  1. 这里选择重写 get_min_max_info()，而不是让基类回落去发 WM_GETMINMAXINFO
    //   （ui_element.cpp:210-221 的默认实现）。两种做法都能用，但哪个更符合
    //     宿主预期没验证过。
    //  2. 首次绘制之前 m_theme.dpi 还是 96，这时报出去的最小高度在高 DPI 屏上
    //     会偏小；等第一帧画完就准了。
    ui_element_min_max_info info;
    const int dpi = m_theme.dpi;

    // 高度按「上下留白 + 行高 × 行数」给：配置里的行数是面板承诺能显示的行数，
    // 不给够高度就会出现「配了 5 行、只看得见 2 行」。
    info.m_min_width  = static_cast<t_uint32>(MulDiv(kMinWidth96, dpi, 96));
    info.m_min_height = static_cast<t_uint32>(MulDiv(kPadding96, dpi, 96) * 2 +
                                               MulDiv(kLineHeight96, dpi, 96) * m_visibleLines);
    // m_max_* 保持 ui_element_min_max_info 的默认（UINT32_MAX）——
    // 歌词面板没有理由限制用户能拉多大。
    return info;
}

void LyricusDui::notify(const GUID& what, t_size param1, const void* param2, t_size param2size) {
    if (what == ui_element_notify_colors_changed || what == ui_element_notify_font_changed) {
        // 宿主的配色/字体变了：重新问一遍，然后重绘。
        // 这里只标脏不直接画 —— 此刻窗口未必可见，手上也没有 DC。
        RefreshTheme();
        if (m_hWnd != nullptr) {
            // erase=TRUE：底色也跟着换了，必须连背景一起重画。
            ::InvalidateRect(m_hWnd, nullptr, TRUE);
        }
    }

    // 继续往基类传。ui_element_instance::notify 本身是空实现，
    // 但保持链路完整，免得以后这里加了别的通知语义忘了转发。
    ui_element_instance::notify(what, param1, param2, param2size);
}

void LyricusDui::RefreshTheme() {
    LyricsViewTheme theme;   // 先全取默认值：下面任何一步拿不到就保持原样
    theme.dpi = m_theme.dpi; // dpi 归重绘路径管（见 OnPaint），这里不动

    t_ui_color value = 0;
    const bool hasText = m_callback.is_valid() && m_callback->query_color(ui_color_text, value);
    if (hasText) theme.normalText = static_cast<COLORREF>(value);

    value = 0;
    const bool hasHighlight = m_callback.is_valid() && m_callback->query_color(ui_color_highlight, value);
    if (hasHighlight) theme.currentText = static_cast<COLORREF>(value);

    if (hasText) {
        // 宿主只定义「正文色 / 高亮色」这类通用项，没有「标题色 / 次要色」。
        // 结构体里的默认值是给深色主题用的浅色，浅色主题下照用等于白底白字，
        // 所以从正文色往背景色方向推。
        const COLORREF background = QueryBackground();
        theme.headerText = theme.normalText;
        theme.dimText    = BlendColor(theme.normalText, background, 0.35f);
        if (!hasHighlight) {
            // 连高亮色都问不到时，把正文色朝背景的**反方向**推半步当当前行色，
            // 至少还能看出唱到哪一行了。
            theme.currentText = BlendColor(theme.normalText, background, -0.5f);
        }
    }
    // warnText 保持默认：SDK 里没有「警告色」这一项，凭空猜一个不如不猜。

    // TODO(未验证): 整套映射关系是推出来的，没有官方依据：
    //   ui_color_text      -> normalText
    //   ui_color_highlight -> currentText
    //   headerText = normalText；dimText = 朝背景混 35%；
    //   currentText 兜底 = 朝背景反方向外推 50%
    // 这些系数和「哪一项该映射到哪个字段」都需要拿真实主题目视校准一遍
    //（尤其是浅色主题 + 用户自定义高亮色的组合）。

    m_theme = theme;

    // 字体同理，但**只留 LOGFONT**：query_font_ex 返回的 HFONT 只在下一次字体
    // 变更回调之前有效（ui_element.h:151），存下来就是一个随时会悬垂的句柄；
    // LOGFONT 是纯数据，随便存。
    m_hasFontDesc = false;
    if (m_callback.is_valid()) {
        const HFONT font = m_callback->query_font_ex(ui_font_default);
        if (font != nullptr) {
            LOGFONTW desc{};
            if (::GetObjectW(font, sizeof(desc), &desc) == static_cast<int>(sizeof(desc))) {
                m_fontDesc    = desc;
                m_hasFontDesc = true;
            }
        }
    }
    // TODO(未验证): LyricsViewTheme 目前没有字体字段，所以这份 LOGFONT 快照
    // 暂时没人消费（DrawLyricsView 自己按 dpi 造字体）。等渲染层要接受宿主字体时，
    // 在这里 CreateFontIndirect(&m_fontDesc) 现造一个用 —— 别退回存 HFONT。
    // TODO(未验证): 到底该取哪个 ui_font_* 没有定论，先取 ui_font_default
    //（官方 sample 也用它）；若歌词按列表类文本排版，ui_font_lists 可能更合适。

    // 背景刷跟着背景色重建（旧刷子立刻作废）
    if (m_bgBrush != nullptr) {
        ::DeleteObject(m_bgBrush);
        m_bgBrush = nullptr;
    }
}

COLORREF LyricusDui::QueryBackground() const {
    t_ui_color value = 0;
    if (m_callback.is_valid() && m_callback->query_color(ui_color_background, value)) {
        return static_cast<COLORREF>(value);
    }
    // 宿主没定义背景色：按深浅模式挑一个中性底色。
    // 直接用纯黑/纯白会和默认文字色撞上 —— 深色主题下正文色就是近白。
    const bool dark = m_callback.is_valid() && m_callback->is_dark_mode();
    return dark ? RGB(28, 28, 30) : RGB(250, 250, 250);
}

HBRUSH LyricusDui::EnsureBackgroundBrush() {
    if (m_bgBrush == nullptr) m_bgBrush = ::CreateSolidBrush(QueryBackground());
    return m_bgBrush;
}

LRESULT LyricusDui::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) {
    // 底色统一交给 OnPaint 的双缓冲处理，这里什么都不做。
    // 窗口类的背景刷是 NULL（见 DECLARE_WND_CLASS_EX 的 -1），系统也不会替我们擦。
    // 返回 TRUE = 「擦除已处理」，避免 DefWindowProc 再插一脚。
    //
    // 注意：这里**不能**改成铺底 —— 那会和 OnPaint 的铺底重复一次，白闪一下。
    return TRUE;
}

LRESULT LyricusDui::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
    CPaintDC dc(m_hWnd);

    // dpi 只能在重绘时拿：GetDpiForWindow 在本工程的 WINVER 下没有声明
    //（control_window.cpp:108 已经踩过同一个坑），而 GetDeviceCaps 一直可用。
    // 每帧读一次，窗口被拖到不同缩放的显示器上时自动跟上。
    m_theme.dpi = ::GetDeviceCaps(dc.m_hDC, LOGPIXELSY);

    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return 0;

    // 【必须双缓冲】每帧都要「先铺底、再画字」，两件事缺一不可：
    //   * 不铺底 → 旧帧残留，文字叠在一起（实测踩过）；
    //   * 直接往窗口 DC 上先铺底再画字 → 中间那一瞬间会闪。
    // 画进内存 DC 再一次性 BitBlt，两个问题一起解决。
    // 这样定时器那边就**必须**用 erase=FALSE（见 OnTimer），
    // 否则 WM_ERASEBKGND 会先铺一遍底色，闪一下再被这里覆盖。
    const HDC mem = ::CreateCompatibleDC(dc.m_hDC);
    const HBITMAP bmp = ::CreateCompatibleBitmap(dc.m_hDC, w, h);
    const HGDIOBJ oldBmp = ::SelectObject(mem, bmp);

    ::FillRect(mem, &rc, EnsureBackgroundBrush());

    // 绘制交给公共渲染层：独立面板 / DUI / 将来的 CUI 共用同一份实现，
    // 免得三套代码慢慢跑偏（见 lyrics_view.h 开头的说明）。
    DrawLyricsView(mem, rc, m_theme, m_layout);

    ::BitBlt(dc.m_hDC, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    return 0;
}

LRESULT LyricusDui::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& bHandled) {
    if (wParam != kRefreshTimerId) {
        bHandled = FALSE;   // 不是我们的定时器，让 DefWindowProc 去管
        return 0;
    }

    ScopedTimer tick("DUI 元素定时器一拍", 5.0);

    auto& state = PlaybackState::Get();

    // 位置刷新会顺带重算「当前歌词行」——play_callback 里不能查播放位置，
    // 位置查询统一放在这里（见 playback_state.h 开头的线程约定）。
    const TickChange change = state.RefreshPosition();
    const bool lineChanged = (change != TickChange::None);

    // 换曲 / 加载或清除歌词只体现在代次上：那种场景下「当前行」可能
    // npos == npos 而被判成「没变化」，只看 lineChanged 会一直停在上一首的歌词上。
    const bool revisionChanged = (state.Revision() != m_lastRevision);

    // 用户改了「高级首选项」里的字号 / 行数 / 当前位置 —— 那套配置
    // **没有变更通知**，只能每帧轮询比对（三个 int，代价可忽略）。
    const LyricDisplayConfig cfg = GetLyricDisplayConfig();
    const bool cfgChanged = (cfg != m_displayCfg);
    if (cfgChanged) {
        m_displayCfg = cfg;
        m_layout = { cfg.fontPct, cfg.span, cfg.currentRatio, cfg.tlPrimary };
        DebugLog("DUI 显示设置变更 -> %s", DescribeDisplayConfig(cfg).c_str());
    }

    // 只有「位置在走、歌词行没变」时才节流；换行 / 换曲 / 改设置仍然立刻重绘。
    const ULONGLONG now = GetTickCount64();
    const bool positionDue =
        (change == TickChange::Position) &&
        (now - m_lastPositionRepaint >= kPositionRepaintMs);

    if (lineChanged || revisionChanged || cfgChanged || positionDue) {
        m_lastPositionRepaint = now;
        m_lastRevision = state.Revision();
        // erase=FALSE 是**故意**的，而且必须配合 OnPaint 里的双缓冲：
        // OnPaint 每帧自己铺底 + 画内容，再一次性 BitBlt 上去。
        // 如果这里传 TRUE，WM_ERASEBKGND 会先铺一遍底色 —— 闪一下才被覆盖。
        // （早先这里的注释把因果关系写反了，配的是单缓冲 OnPaint，
        //   结果背景从没被清过，旧帧一直叠着。）
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
    return 0;
}

LRESULT LyricusDui::OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&) {
    // 只有编辑模式下的点击才算「要换掉这个元素」。正常播放时点一下歌词
    // 就把面板弄没了，绝对不是用户想要的（官方 sample 是无条件替换的）。
    if (m_callback.is_valid() && m_callback->is_edit_mode_enabled()) {
        m_callback->request_replace(this);
    }
    return 0;
}

LRESULT LyricusDui::OnDestroy(UINT, WPARAM, LPARAM, BOOL& bHandled) {
    ::KillTimer(m_hWnd, kRefreshTimerId);

    // 窗口没了，刷子也不会再有人用。在这里释放、不给类加析构函数 ——
    // SDK 保证窗口先于对象销毁（atl-misc.h:229 那句
    // PFC_ASSERT(this->m_hWnd == NULL) 就是这条约定）。
    if (m_bgBrush != nullptr) {
        ::DeleteObject(m_bgBrush);
        m_bgBrush = nullptr;
    }

    // **不能吞掉 WM_DESTROY**：BumpableElem.h:17 和 atl-misc.h:200 都挂了
    // MSG_WM_DESTROY，ATL 自己也要靠它做收尾。
    bHandled = FALSE;
    return 0;
}

// ---------------------------------------------------------------------------
// 注册
// ---------------------------------------------------------------------------

// ui_element_impl_withpopup 在 ui_element_impl 的基础上多做两件事：
//   1. 生成一份可独立弹出的窗口（菜单里的 "Lyricus Lyrics"）；
//   2. 用 ImplementBumpableElem 记住所有实例，已经开着就闪一下窗口而不是再开一个。
class LyricusDuiElement : public ui_element_impl_withpopup<LyricusDui> {};

// 把类登记成 ui_element 入口点。匿名命名空间 + static：别的 cpp 没有任何理由
// 看见这些符号，同名也不会互相干扰（官方 sample 的写法）。
static service_factory_single_t<LyricusDuiElement> g_lyricus_dui_factory;

} // namespace
} // namespace lyricus
