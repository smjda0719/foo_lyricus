#include "stdafx.h"

#include "lyrics_view.h"
#include "color_util.h"   // BlendColor（原先在本文件里有一份，已提到公共层）
#include "config.h"
#include "playback_state.h"
#include "debug_log.h"

// CUI 的聚合头。它自己会拉进 pfc 与 foobar2000 SDK（ui_extension.h:19-32），
// 所以必须排在 stdafx.h 之后。工程已把 3rdparty 加进包含目录
//（build/foo_lyricus.vcxproj:71），带前缀的写法既能解析、又能一眼看出
// 这是第三方 SDK 而不是本工程的 src。
#include <columns_ui-sdk/ui_extension.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// CUI（Columns UI）歌词面板 —— 与 dui_element.cpp 平行的一份实现。
//
// 【为什么不做成一个类同时实现 ui_element_instance 和 uie::window】
//   1. 两者都**非虚**继承 service_base：一个类同时继承就形成菱形，
//      service_query / 引用计数的语义立刻含糊（SDK 备了 service_multi_inherit
//      专治这种场景，但为一个歌词面板引入它不值得）；
//   2. 销毁归属相反：DUI 侧窗口由宿主在释放元素时自己 DestroyWindow，
//      CUI 侧 window.h:200-208 明确要求**实现方**在 destroy_window() 里销毁窗口。
//      两套相反的所有权塞进一个类，只能靠标志位互相打架。
// 刻意接受这几十行重复，换掉上面两个坑 —— 共用的只有三层：
// 渲染层 DrawLyricsView、状态层 PlaybackState、日志 DebugLog。
//
// 【纯虚函数清单】逐条抄自 3rdparty/columns_ui-sdk/，行号见各处注释：
//   base.h:18    get_extension_guid()
//   base.h:30    get_name()
//   window.h:98  get_category()
//   window.h:127 get_type()
//   window.h:148 is_available()
//   window.h:197 create_or_transfer_window()
//   window.h:208 destroy_window()
//   window.h:218 get_wnd()
// get_is_single_instance()（window.h:82）**故意不实现**：window.h:80 的注释
// 要求它归工厂管，由 window_implementation<T,false>（window.h:424-434）提供。
//
// 【为什么不直接用 SDK 的 uie::container_window_v3】
// 它会把「窗口类注册 + std::function 消息转发 + 子窗口 SETTINGCHANGE 转发」
// 再包一层；本面板没有子窗口，而 ATL 的 CWindowImpl 已经把窗口类注册和消息
// 映射做完了，且这样和 dui_element.cpp 保持同构，两边可以对着读。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// 刷新定时器。250ms 对行级歌词足够；将来做逐字歌词要么提速，要么换成
// 高精度计时器插值（见 playback_state.h 对 RefreshPosition 的说明）。
// id 用 2，和 dui_element.cpp:23 的取值一致 —— 定时器是挂在 HWND 上的，
// 两个窗口各有一份，不会互相打扰；保持同值只是为了两边好对照。
constexpr UINT_PTR kRefreshTimerId  = 2;
constexpr UINT     kRefreshInterval = 250;

// 动画拍。和逻辑拍分开的理由见 control_window.cpp 的 kAnimTimerId ——
// 逻辑拍要轮询配置，动画拍只重绘。
constexpr UINT_PTR kAnimTimerId  = 3;
constexpr UINT     kAnimInterval = 40;   // 25fps

// 96 dpi 下的最小尺寸，实际用的时候按 dpi 缩放。
// 高度给得比较小：歌词行数是按可用高度自适应的（lyrics_view.cpp:112-114），
// 面板被压扁时少显示几行即可，没必要硬撑。
constexpr int kMinWidth96  = 160;
constexpr int kMinHeight96 = 60;

// 面板窗口样式。逐条对着 window.h:176-187 的约束写：
//   * WS_CHILD —— 必须；
//   * WS_POPUP / WS_CAPTION / WS_VISIBLE —— 一个都不能带；
//   * 对话框 ID 必须为 0（走 ATL Create 的默认 MenuOrID，见 create_or_transfer_window）。
// 样式值抄自 SDK 自己给面板窗口用的默认值（container_window_v3.h:33-34），
// 那是对「CUI 面板窗口该长什么样」最权威的一份参考。
//
// **必须显式传样式**：ATL 给 CWindowImpl 的默认 traits 是 CControlWinTraits
//（atlwin.h:369），展开是 WS_CHILD | **WS_VISIBLE** | WS_CLIPCHILDREN |
// WS_CLIPSIBLINGS，而 CWinTraits::GetWndStyle 只在 dwStyle == 0 时才回落到
// traits 默认值（atlwin.h:3188-3191）。传 0 就等于带上 WS_VISIBLE —— 正好撞在
// CUI 的禁令上（window.h:177-179：不要自己显示窗口，宿主会在准备好的时候做）。
// 下面既显式传样式、又自定义 traits，两条路都不带 WS_VISIBLE。
constexpr DWORD kPanelStyle   = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr DWORD kPanelStyleEx = WS_EX_CONTROLPARENT;

using LyricusCuiTraits = ATL::CWinTraits<kPanelStyle, kPanelStyleEx>;

// 窗口类名 / 扩展 GUID。沿用工程的 1A7C3E90-...-4Dxx 段（config.cpp:17），
// 末字节 0x11 紧邻 DUI 元素的 0x10，config.cpp / menu.cpp / dui_element.cpp
// 里都没被占用。
const GUID guid_cui_panel = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x11}};

// 在 from→to 之间线性混合。t 为负表示朝 to 的反方向外推（用来加强对比），
// 结果按 0..255 夹紧。
// BlendColor 已提到公共层 color_util.h。
//
// 这段和 dui_element.cpp 里那份原本是**故意各留一份**的（两个 .cpp 都在匿名
// 命名空间里，internal linkage 不会撞符号），当时的 TODO 写着
//「等第三次需要这个函数时，把它提到公共头里」。
// 后来 prefs_layout（配色推导）和 ui_draw（自绘按钮）也要用，第三次到了 ——
// 抽出来的位置是 color_util.h，而不是当时设想的 lyrics_view.h：
// 它是纯颜色运算，不归渲染层，而且必须**不依赖 SDK** 才能让
// prefs_layout 继续进离线单测台。

// ---------------------------------------------------------------------------
// CUI 面板
// ---------------------------------------------------------------------------

// 继承顺序说明：
//   * uie::window（-> extension_base -> service_base）是唯一的 service_base
//     分支，引用计数与 service_query 都靠它；
//   * cui::colours::common_callback（colours.h:102-106）和
//     cui::fonts::common_callback（fonts.h:60-63）**都不继承 service_base**
//     —— 只是两个纯接口。这正是「顺手把主题回调也接在这个类上」能成立的前提：
//     没有第二个 service_base 分支，就没有菱形。
//   * CWindowImpl 提供窗口类注册与消息映射（和 dui_element.cpp 同款做法）。
class LyricusCuiPanel : public uie::window,
                        public CWindowImpl<LyricusCuiPanel, CWindow, LyricusCuiTraits>,
                        public cui::colours::common_callback,
                        public cui::fonts::common_callback {
public:
    // 窗口类。背景刷传 -1，ATL 展开成 (HBRUSH)(-1 + 1) = NULL，也就是
    // 「不擦背景」—— 底色由我们自己的双缓冲 WM_PAINT 铺，否则会先闪一下
    // 系统窗口色再画。
    DECLARE_WND_CLASS_EX(TEXT("{1A7C3E90-2B41-4C58-9D6E-0F1A2B3C4D11}"), CS_VREDRAW | CS_HREDRAW, (-1));

    LyricusCuiPanel() = default;
    ~LyricusCuiPanel();

    // ---- uie::extension_base 的纯虚契约 ----
    // 返回引用：必须回静态/全局变量（base.h:13-17 的说明）。
    const GUID& get_extension_guid() const override { return guid_cui_panel; }
    void get_name(pfc::string_base& out) const override { out = "Lyricus Lyrics"; }

    // ---- uie::window 的纯虚契约 ----
    // 预定义分类见 window.h:87-89，面板用 "Panels"。
    // 注意 "Splitters" 在 CUI 3.1.0 里改名 "Containers"，但那与本面板无关。
    void get_category(pfc::string_base& out) const override { out = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }
    HWND get_wnd() const override { return m_hWnd; }

    // 多实例窗口：window.h:142 明说「For multi-instance windows, you can always
    // return true」。真正的单实例语义由 window_factory_single 之外的工厂决定。
    bool is_available(const uie::window_host_ptr&) const override { return true; }

    // 可选覆写：给面板列表显示一句说明。返回 true 表示 out 已填好（window.h:110-118）。
    bool get_description(pfc::string_base& out) const override {
        out = "Displays synchronized lyrics of the currently playing track.";
        return true;
    }

    HWND create_or_transfer_window(HWND wnd_parent, const uie::window_host_ptr& p_host,
                                   const ui_helpers::window_position_t& p_position) override;
    void destroy_window() override;

    // ---- cui::colours::common_callback（colours.h:102-106，两个都是 const）----
    void on_colour_changed(uint32_t changed_items_mask) const override;
    void on_bool_changed(uint32_t changed_items_mask) const override;

    // ---- cui::fonts::common_callback（fonts.h:60-63）----
    void on_font_changed(uint32_t changed_items_mask) const override;

    BEGIN_MSG_MAP_EX(LyricusCuiPanel)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
        // 宿主按约定必须转发这三条系统消息（window_host.h:18-22），
        // 拿它们当主题变更的第二条通知路径。
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnSysColorChange)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

private:
    // ---- 窗口消息 ----
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL& bHandled);
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL& bHandled);
    LRESULT OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& bHandled);
    LRESULT OnGetMinMaxInfo(UINT, WPARAM, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSysColorChange(UINT, WPARAM, LPARAM, BOOL& bHandled);
    LRESULT OnSettingChange(UINT, WPARAM, LPARAM, BOOL& bHandled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& bHandled);

    // ---- 主题 ----
    // 注册/注销 CUI 的主题变更回调。服务指针本身就是「已注册」的标志：
    // 拿到了 manager 才会去注册，注销时用完即释放。
    void RegisterThemeCallbacks();
    void UnregisterThemeCallbacks();

    // 重查配色与字体。**只在 OnPaint 里调用** —— 回调里不做这事，理由见
    // MarkThemeDirty()。
    void RefreshTheme();

    // 标脏 + 请求重绘。const：CUI 的两个回调都是 const 成员函数。
    void MarkThemeDirty(bool erase) const;

    HBRUSH EnsureBackgroundBrush();
    void   ReleaseGdiObjects();

    // 宿主。迁移分支要用它调 relinquish_ownership，destroy_window() 里释放 ——
    // CUI 的容器持有子面板的引用，我们反过来持有宿主的引用，不放就是一个环。
    uie::window_host_ptr m_host;

    // 缓存的配色。dpi 由重绘路径维护（见 OnPaint），颜色来自 RefreshTheme()。
    LyricsViewTheme m_theme;

    // 面板底色。LyricsViewTheme 里**没有**背景字段（底色归宿主，见
    // lyrics_view.h:12-15），所以自己存一份。默认值和结构体那套深色默认值对齐。
    COLORREF m_background = RGB(28, 28, 30);

    // 背景刷：每帧都要用，现建现毁太浪费；跟着主题一起重建。
    HBRUSH m_bgBrush = nullptr;

    // 宿主字体的快照 —— 刻意**只存 LOGFONT，不存 HFONT**，后者随时会失效。
    LOGFONTW m_fontDesc{};
    bool     m_hasFontDesc = false;

    // CUI 主题服务。有效即代表「回调已注册」。
    cui::colours::manager::ptr m_coloursApi;
    cui::fonts::manager::ptr   m_fontsApi;

    // 「主题脏了，下一帧重查」。mutable：标记它的正是两个 const 回调。
    // 初值 true —— 第一帧一定会查一次，正好把空白面板填上。
    mutable bool m_themeDirty = true;

    // 上一次重绘时看到的 PlaybackState 代次。初值 0 与真实代次（从 1 起）
    // 必然不等，所以第一帧一定会重绘一次。和 dui_element.cpp:153-155 同一套判断。
    unsigned m_lastRevision = 0;

    // 上一次**因为播放位置变化**而重绘的时刻（GetTickCount64）。
    // 把「位置在走」那种重绘节流到每秒一次 —— 完整理由见 control_window.cpp
    // 里 kPositionRepaintMs 的说明（进度条每秒才走约 1 像素，4Hz 是白烧的）。
    // 初值 0 让第一次位置变化就能通过。
    ULONGLONG m_lastPositionRepaint = 0;

    // 用户设置（字号 / 行数 / 当前行位置）。
    // 高级首选项的改动**没有任何通知机制**，只能在定时器里轮询比对 ——
    // 三个 int 的比较，代价可以忽略。详见 settings.cpp 的说明。
    LyricDisplayConfig m_displayCfg;   // 上一次看到的原始设置，用来判断"变了没"
    LyricsViewLayout   m_layout;       // m_displayCfg 的渲染视图，跟着它一起更新

    // ---- 动画（长行横滚 / 换行上滑）----
    // 时间线在 scroll_anim.cpp（纯逻辑、可离线单测），这里只管驱动和重绘。
    LyricAnimator     m_animator;
    LyricAnimFrame    m_animFrame;     // 正在显示的那一帧
    LyricsViewResult  m_lastResult;    // 上一次绘制量出来的宽出量 / 步距
    bool              m_animTimerOn = false;

    // 推进时间线；返回 true = 帧变了，调用方要重绘。**必须在重绘之前调**。
    bool AdvanceAnimation(ULONGLONG now);
    void TickAnimation(ULONGLONG now);
};

LyricusCuiPanel::~LyricusCuiPanel() {
    // 兜底：宿主没按约定调 destroy_window()（window.h:205-207 说它「在正常情况下
    // 总会被调用」）时，窗口可能还活着而我们马上就要没了 —— 必须先把自己拆干净，
    // 否则之后的任何一条消息都会走进已析构的对象。正常路径下 m_hWnd 早已是 NULL，
    // 这里全是空操作。
    // TODO(未验证): 这里假定对象在**主线程**上释放（窗口只能由创建它的线程销毁）。
    // CUI 是在主线程拆卸面板的，所以正常路径没问题；万一有别的线程持有最后一份
    // 引用，::DestroyWindow 会直接失败（返回 FALSE，窗口留着）。真出现这种场景，
    // 正解是 service_impl_helper::release_object_delayed()，不是在这儿加锁。
    UnregisterThemeCallbacks();
    if (m_hWnd != nullptr) {
        DebugLog("CUI: 析构时窗口仍在，补一次 DestroyWindow");
        DestroyWindow();   // ATL 版本：内部 ::DestroyWindow + 把 m_hWnd 置空
    }
    ReleaseGdiObjects();
}

HWND LyricusCuiPanel::create_or_transfer_window(HWND wnd_parent, const uie::window_host_ptr& p_host,
                                                const ui_helpers::window_position_t& p_position) {
    if (m_hWnd != nullptr) {
        // 「迁移到新宿主」分支。window.h:171-174 说这条路径只对单实例窗口有意义，
        // 多实例窗口可以 uBugCheck。但按官方样例（window.h:155-167）写出来只有
        // 几行，比留一个一碰就崩的断言划算。
        //
        // 关于 window.h:179「不要自己把窗口显示出来」这条规矩，这里出现的
        // ShowWindow 是**唯一**一处，而且方向只可能朝隐藏（SW_HIDE）——
        // 它就是 window.h:157 官方样例会里的那一步（重挂父窗口前先藏起来，
        // 显示与否仍旧由新宿主决定）。全文件没有第二处 ShowWindow。
        ::ShowWindow(m_hWnd, SW_HIDE);
        ::SetParent(m_hWnd, wnd_parent);
        // relinquish_ownership 只允许在 create_or_transfer_window 里调
        //（window_host.h:162-166），别挪到 destroy_window 去。
        if (m_host.is_valid()) m_host->relinquish_ownership(m_hWnd);
        m_host = p_host;
        ::SetWindowPos(m_hWnd, nullptr, p_position.x, p_position.y,
                       static_cast<int>(p_position.cx), static_cast<int>(p_position.cy),
                       SWP_NOZORDER);
        return m_hWnd;
    }

    m_host = p_host;

    // 位置/尺寸由宿主给。cx/cy 是 unsigned，非负，转 int 是安全的，
    // 但显式 static_cast 免得 /W4 下报 C4267 之类的窄化告警。
    RECT rc{};
    rc.left   = p_position.x;
    rc.top    = p_position.y;
    rc.right  = p_position.x + static_cast<int>(p_position.cx);
    rc.bottom = p_position.y + static_cast<int>(p_position.cy);

    // Create 的第 6 个参数是「菜单句柄 / 对话框控件 ID」，默认 0U —— 正好满足
    // window.h:187「窗口的对话框 ID 必须为 0」。第 7 个参数 lpCreateParam 保持默认
    // NULL：ATL 不靠它找回 this，而是靠 _AtlWinModule 的创建数据栈
    //（atlwin.h:3535 的 ExtractCreateWndData + atlwin.h:3631 的 AddCreateWndData）。
    // rect 必须以**指针**形式传：ATL 的 _U_RECT 只有 LPRECT 和 RECT&（非 const）
    // 两个构造（atlwin.h:109-119），传临时量编不过。
    if (Create(wnd_parent, &rc, nullptr, kPanelStyle, kPanelStyleEx) == nullptr) {
        const DWORD err = ::GetLastError();
        DebugLog("CUI: 创建面板窗口失败，GetLastError=%lu", err);
        m_host.release();
        return nullptr;   // 契约就是「失败回 nullptr」（window.h:195），这里不能抛
    }

    // 主题回调放在窗口建好之后注册：回调里会对 m_hWnd 调 InvalidateRect，
    // 窗口还没建就注册，等于给自己留一条空指针路径。
    RegisterThemeCallbacks();

    // 窗口**没有** WS_VISIBLE，这里也**不**调 ShowWindow —— window.h:177-179
    // 明确要求由宿主决定显示时机。
    ::SetTimer(m_hWnd, kRefreshTimerId, kRefreshInterval, nullptr);

    DebugLog("CUI: 面板窗口已创建 hwnd=%p", static_cast<void*>(m_hWnd));
    return m_hWnd;
}

void LyricusCuiPanel::destroy_window() {
    // 先摘回调：销毁过程中 CUI 仍可能广播主题变更，摘掉以后就没人再回调到一个
    // 正在拆的窗口上。
    UnregisterThemeCallbacks();

    if (m_hWnd != nullptr) {
        // ATL 的 DestroyWindow：内部 ::DestroyWindow + 把 m_hWnd 置空。
        // WM_DESTROY 会同步到达（见 OnDestroy），定时器和 GDI 对象在那里释放。
        DestroyWindow();
    }

    // 松开宿主引用，破掉 CUI 容器与面板之间的引用环。SDK 自己的
    // container_ui_extension_t 也是在 destroy_window 里 p_host.release()
    //（window_helper.h:146-150）。
    m_host.release();
}

void LyricusCuiPanel::RegisterThemeCallbacks() {
    // 走的是「全局配色 + 公共字体」这条路：
    //   * cui::colours::helper 不传 client GUID == 全局配色（colours.h:177-186）；
    //   * cui::fonts::manager 取公共项目字体（fonts.h:66-96）。
    // SDK 其实更推荐为每个面板实现一个 client:: 对象，这样用户能在 CUI 的
    // 「颜色和字体」里单独调本面板；那需要一张「活着的面板实例」表来分发通知，
    // 不是这一版要做的事。
    // TODO(未验证): 升级到 client 路线的收益 = 用户可给歌词面板单独配色配字体。
    if (!m_coloursApi.is_valid()) {
        // std_api_try_get 不抛异常，失败就是 false（service.h:653-665）。
        // 顺带说明：manager 上的 register_common_callback 在头文件里有空实现
        //（colours.h:118），所以旧版 CUI 上这里会「静默注册失败」—— 表现为
        // 改了配色面板不跟着变，需要手动重开面板。
        // TODO(未验证): 无法区分「注册成功」与「旧版 CUI 空实现」。
        if (fb2k::std_api_try_get(m_coloursApi)) {
            m_coloursApi->register_common_callback(this);
        } else {
            DebugLog("CUI: cui::colours::manager 不可用，配色将退回 LyricsViewTheme 默认值");
        }
    }

    if (!m_fontsApi.is_valid()) {
        if (fb2k::std_api_try_get(m_fontsApi)) {
            m_fontsApi->register_common_callback(this);
        } else {
            DebugLog("CUI: cui::fonts::manager 不可用，字体将退回渲染层默认值");
        }
    }

    m_themeDirty = true;
}

void LyricusCuiPanel::UnregisterThemeCallbacks() {
    if (m_coloursApi.is_valid()) {
        m_coloursApi->deregister_common_callback(this);
        m_coloursApi.release();
    }
    if (m_fontsApi.is_valid()) {
        m_fontsApi->deregister_common_callback(this);
        m_fontsApi.release();
    }
}

void LyricusCuiPanel::MarkThemeDirty(bool erase) const {
    m_themeDirty = true;

    // 这里**不**顺手去查主题，理由有两条：
    //   1. 回调发生在 CUI 广播通知的过程中，此时回调进 manager/helper 属于重入
    //      CUI 自己，能避就避；
    //   2. 此刻窗口未必可见、手上也没有 DC。而 OnPaint 那边正好可以顺手把 dpi
    //      一起更新掉（GetDeviceCaps 需要 DC）。
    // 所以统一推迟到下一帧 OnPaint 里查。
    if (m_hWnd != nullptr) {
        ::InvalidateRect(m_hWnd, nullptr, erase ? TRUE : FALSE);
    }
}

void LyricusCuiPanel::on_colour_changed(uint32_t /*changed_items_mask*/) const {
    // 用的是全局配色，正文/高亮/背景任一项变了都可能影响我们，所以不按 mask 挑
    //（漏判一项的代价是配色错到下次主题变更才纠正）。底色可能变了 → 连背景重画。
    MarkThemeDirty(true);
}

void LyricusCuiPanel::on_bool_changed(uint32_t changed_items_mask) const {
    // 目前只有 bool_flag_dark_mode_enabled 会走到这里（colours.h:40-48、223-246），
    // 它直接决定底色深浅，必须连背景一起重画。
    if ((changed_items_mask & cui::colours::bool_flag_dark_mode_enabled) != 0) {
        MarkThemeDirty(true);
    }
}

void LyricusCuiPanel::on_font_changed(uint32_t /*changed_items_mask*/) const {
    // 字体变了不影响底色，erase=FALSE 少闪一下。
    MarkThemeDirty(false);
}

void LyricusCuiPanel::RefreshTheme() {
    // 「拿不到就用默认值」的落法：LyricsViewTheme 的成员默认值本身就是一套
    // 深色主题（lyrics_view.h:23-30），所以先默认构造打底、再逐项覆盖 ——
    // 任何一步拿不到就自然保留默认值，不需要一堆分支。
    LyricsViewTheme theme;
    theme.dpi = m_theme.dpi;   // dpi 归重绘路径管（见 OnPaint），这里不动

    COLORREF background = RGB(28, 28, 30);   // 与上面那套深色默认值配套

    if (m_coloursApi.is_valid()) {
        // 不传 client GUID == 全局配色（colours.h:177-186）。
        // helper 内部已经处理过「服务不在」的情况：会退回 g_get_system_color()
        //（colours.h:127-133），所以这里取到的永远是可用颜色。
        cui::colours::helper colours;

        background = colours.get_colour(cui::colours::colour_background);

        // CUI 只定义「正文色 / 选中背景色」这类通用项，没有「标题色 / 次要色 /
        // 警告色」。映射方式和 dui_element.cpp:271-292 保持一致，便于两边对照：
        //   正文色   -> headerText（曲名）与 normalText（普通歌词行）
        //   选中背景 -> currentText（当前行；对应 DUI 的 ui_color_highlight）
        //   次要色   = 正文色朝背景方向混 35%
        theme.headerText = colours.get_colour(cui::colours::colour_text);
        theme.normalText = theme.headerText;
        theme.currentText = colours.get_colour(cui::colours::colour_selection_background);
        theme.dimText = BlendColor(theme.normalText, background, 0.35f);
        // warnText 保持默认：SDK 里没有「警告色」这一项，凭空猜一个不如不猜。
        // TODO(未验证): 浅色主题下 warnText（默认 205,165,165）配浅底对比度偏低，
        // 「（无歌词）」那行会发灰；要不要按深浅模式换一档，得目视校准。
        //
        // TODO(未验证): 上面这套映射和系数都是推出来的，没有官方依据 ——
        // 需要拿真实主题目视校准（尤其是浅色主题 + 用户自定义高亮色的组合）。
    } else {
        // CUI 的配色服务不在。理论上不该发生（面板本身就跑在 CUI 里），
        // 所以不猜深浅模式：保留深色默认值 + 配套的深色底，至少自洽。
        DebugLog("CUI: 配色服务不可用，全套退回 LyricsViewTheme 默认值");
    }

    m_background = background;
    m_theme = theme;

    // 背景刷跟着背景色重建（旧刷子立刻作废）。
    if (m_bgBrush != nullptr) {
        ::DeleteObject(m_bgBrush);
        m_bgBrush = nullptr;
    }

    // 字体：同样「拿不到就用默认」—— 没有字体快照时渲染层自己按 dpi 造字体。
    // **只留 LOGFONT 快照，绝不缓存 HFONT**：HFONT 是「回调周期内有效」的句柄
    //（dui_element.cpp:125-127、304-306 记的是同一件事），存下来就是一个随时
    // 悬垂的句柄；LOGFONT 是纯数据，存多久都安全。
    m_hasFontDesc = false;
    if (m_fontsApi.is_valid()) {
        LOGFONTW desc{};
        // 字体类型取 items（列表项目字体）：歌词是「一行一条」的列表式文本。
        // TODO(未验证): font_type_items / font_type_labels / core_default_font_id
        // 三选一没有定论（fonts.h:5-8、fonts.h:37-45），要拿真实配置目视校准；
        // DUI 侧用的是 ui_font_default（dui_element.cpp:309、321-322）。
        m_fontsApi->get_font(cui::fonts::font_type_items, desc);
        // 服务在、但返回一份空 LOGFONT 时也当拿不到：CreateFontIndirect 一份全零
        // 描述会得到一个说不清是什么的字体。
        if (desc.lfHeight != 0 || desc.lfFaceName[0] != L'\0') {
            m_fontDesc = desc;
            m_hasFontDesc = true;
        }
    }
    // TODO(未验证): LyricsViewTheme 没有字体字段，这份 LOGFONT 暂时没人消费
    //（DrawLyricsView 自己按 dpi 造字体）。等渲染层接受宿主字体时，在这里
    // CreateFontIndirect(&m_fontDesc) 现造一个、用完就删 —— 别退回存 HFONT。
    // TODO(未验证): manager::get_font(font_type_t, LOGFONT&) 没有 dpi 参数；
    // 真要按 dpi 取字体，应改用 manager_v2::get_common_font(type, dpi)
    //（fonts.h:108-135），代价是要求 CUI 1.7.0 beta 1 以上。

    DebugLog("CUI: 主题刷新 background=%06lX text=%06lX current=%06lX dim=%06lX dpi=%d 宿主字体=%s",
             static_cast<unsigned long>(m_background),
             static_cast<unsigned long>(theme.headerText),
             static_cast<unsigned long>(theme.currentText),
             static_cast<unsigned long>(theme.dimText),
             theme.dpi,
             m_hasFontDesc ? "有" : "无");
}

HBRUSH LyricusCuiPanel::EnsureBackgroundBrush() {
    if (m_bgBrush == nullptr) m_bgBrush = ::CreateSolidBrush(m_background);
    return m_bgBrush;
}

void LyricusCuiPanel::ReleaseGdiObjects() {
    if (m_bgBrush != nullptr) {
        ::DeleteObject(m_bgBrush);
        m_bgBrush = nullptr;
    }
}

LRESULT LyricusCuiPanel::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) {
    // 底色统一交给 OnPaint 的双缓冲处理，这里什么都不做。
    // 窗口类的背景刷是 NULL（见 DECLARE_WND_CLASS_EX 的 -1），系统也不会替我们擦。
    // 返回 TRUE = 「擦除已处理」，避免 DefWindowProc 再插一脚。
    //
    // 注意：这里**不能**改成铺底 —— 那会和 OnPaint 的铺底重复一次，白闪一下。
    return TRUE;
}

LRESULT LyricusCuiPanel::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
    CPaintDC dc(m_hWnd);

    // 主题延迟到这一帧才查（见 MarkThemeDirty 里的两条理由）：
    // 到这里窗口一定有效、手上也有 DC。
    if (m_themeDirty) {
        RefreshTheme();
        m_themeDirty = false;
    }

    // dpi 只能在重绘时拿：GetDpiForWindow 在本工程的 WINVER 下没有声明
    //（control_window.cpp 已经踩过同一个坑），而 GetDeviceCaps 一直可用。
    // 每帧读一次，窗口被拖到不同缩放的显示器上时自动跟上。
    m_theme.dpi = ::GetDeviceCaps(dc.m_hDC, LOGPIXELSY);

    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return 0;

    // 【必须双缓冲】每帧都要「先铺底、再画字」，两件事缺一不可：
    //   * 不铺底 → 旧帧残留，文字叠在一起（dui_element.cpp:370-372 记的实测坑）；
    //   * 直接往窗口 DC 上先铺底再画字 → 中间那一瞬间会闪。
    // 画进内存 DC 再一次性 BitBlt，两个问题一起解决。
    // 这样定时器那边就**必须**用 erase=FALSE（见 OnTimer），否则 WM_ERASEBKGND
    // 会先铺一遍底色，闪一下再被这里覆盖。
    //
    // CreateCompatibleDC/Bitmap 失败（OOM）时下面几步会安静地失败 —— GDI 对 NULL
    // 句柄是安全的，DrawLyricsView 也先判了空（lyrics_view.cpp:52）。表现为空一帧，
    // 不会崩；和 dui_element.cpp:376-390 的处理一致，不额外加分支。
    const HDC mem = ::CreateCompatibleDC(dc.m_hDC);
    const HBITMAP bmp = ::CreateCompatibleBitmap(dc.m_hDC, w, h);
    const HGDIOBJ oldBmp = ::SelectObject(mem, bmp);

    ::FillRect(mem, &rc, EnsureBackgroundBrush());

    // 绘制交给公共渲染层：独立面板 / DUI / CUI 共用同一份实现（lyrics_view.h:6-16）。
    // 返回值存下来：横滚要的"宽出多少"和上滑要的"步距"只有渲染层知道。
    m_lastResult = DrawLyricsView(mem, rc, m_theme, m_layout, m_animFrame);

    ::BitBlt(dc.m_hDC, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    return 0;
}

LRESULT LyricusCuiPanel::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& bHandled) {
    // 动画拍：推进时间线，变了就重绘。**不查任何东西**（见 control_window.cpp
    // 里动画定时器的说明 —— 逻辑拍那套轮询搬到 25fps 就是每秒几十次白查表）。
    if (wParam == kAnimTimerId) {
        TickAnimation(::GetTickCount64());
        return 0;
    }
    if (wParam != kRefreshTimerId) {
        bHandled = FALSE;   // 不是我们的定时器，让 DefWindowProc 去管
        return 0;
    }

    // 每 250ms 一次的热路径。CUI 面板和浮动面板**同时在跑**，
    // 所以两边各有一份轮询开销。
    ScopedTimer tick("CUI 面板定时器一拍", 5.0);

    auto& state = PlaybackState::Get();

    // 位置刷新会顺带重算「当前歌词行」——play_callback 里不能查播放位置，
    // 位置查询统一放在这里（见 playback_state.h 开头的线程约定）。
    const TickChange change = state.RefreshPosition();
    const bool lineChanged = (change == TickChange::Line);

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
        DebugLog("CUI 显示设置变更 -> %s", DescribeDisplayConfig(cfg).c_str());
    }

    // 只有「位置在走、歌词行没变」时才节流；换行 / 换曲 / 改设置仍然立刻重绘。
    const ULONGLONG now = GetTickCount64();
    const bool positionDue =
        (change == TickChange::Position) &&
        (now - m_lastPositionRepaint >= kPositionRepaintMs);

    // 换曲 / 改显示设置 -> 动画状态清零（否则上一首滚到一半的偏移会带到新歌上）
    if (revisionChanged || cfgChanged) {
        m_animator.Reset();
        m_animFrame = LyricAnimFrame{};
    }

    // 动画状态**先推进**，再决定要不要重绘 —— 顺序不能反。
    // 反过来的话换行那一拍会先用上一行的滚动偏移把新行画一遍，
    // 白多一次整帧渲染，而且那一帧是错的（看着闪一下）。
    const bool animChanged = AdvanceAnimation(now);

    if (lineChanged || revisionChanged || cfgChanged || positionDue || animChanged) {
        m_lastPositionRepaint = now;
        m_lastRevision = state.Revision();
        // erase=FALSE 是**故意**的，而且必须配合 OnPaint 里的双缓冲：
        // OnPaint 每帧自己铺底 + 画内容，再一次性 BitBlt 上去。
        // 传 TRUE 只会多一次 WM_ERASEBKGND 铺底 —— 闪一下才被覆盖。
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
    return 0;
}

void LyricusCuiPanel::TickAnimation(ULONGLONG now) {
    if (AdvanceAnimation(now)) ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

// 推进动画时间线。返回 true = 帧变了（调用方要重绘）。必须在重绘之前调。
bool LyricusCuiPanel::AdvanceAnimation(ULONGLONG now) {
    if (m_hWnd == nullptr) return false;

    const auto& state = PlaybackState::Get();

    const LyricAnimFrame f = m_animator.Update(
        now, state.DisplayLine(), m_lastResult.currentOverflow, m_lastResult.currentStepH);

    const bool frameChanged = (f.scrollX != m_animFrame.scrollX) ||
                              (f.slideY  != m_animFrame.slideY);
    m_animFrame = f;

    if (f.animating && !m_animTimerOn) {
        ::SetTimer(m_hWnd, kAnimTimerId, kAnimInterval, nullptr);
        m_animTimerOn = true;
    } else if (!f.animating && m_animTimerOn) {
        ::KillTimer(m_hWnd, kAnimTimerId);
        m_animTimerOn = false;
    }

    // ⚠️ 返回的是**帧变了没有**，不是 animating（见 scroll_anim.h 的说明）
    return frameChanged;
}

LRESULT LyricusCuiPanel::OnGetMinMaxInfo(UINT, WPARAM, LPARAM lParam, BOOL&) {
    // window.h:222-226：尺寸约束的官方途径就是处理 WM_GETMINMAXINFO，
    // 而 uie::window::get_size_limits() 的默认实现（window.h:227-239）也是给
    // 窗口发这条消息来取值的 —— 在这儿写一次，两条路都生效。
    auto* mmi = reinterpret_cast<LPMINMAXINFO>(lParam);
    if (mmi == nullptr) return 0;

    // TODO(未验证): 首帧之前 m_theme.dpi 还是 96，高 DPI 下这时报出去的最小尺寸
    // 会偏小；等第一帧画完就准了（和 dui_element.cpp:232-237 的存疑点相同）。
    const int dpi = m_theme.dpi;
    mmi->ptMinTrackSize.x = MulDiv(kMinWidth96, dpi, 96);
    mmi->ptMinTrackSize.y = MulDiv(kMinHeight96, dpi, 96);
    // ptMaxTrackSize 不动：歌词面板没有理由限制用户能拉多大。
    return 0;
}

LRESULT LyricusCuiPanel::OnSysColorChange(UINT, WPARAM, LPARAM, BOOL& bHandled) {
    // 宿主按约定必须转发 WM_SYSCOLORCHANGE（window_host.h:18-22）。正常路径上
    // CUI 也会走 on_colour_changed，这里是第二条通知路径：CUI 的配色若设成
    // 「跟随系统」，通知有可能只到这条消息为止。
    MarkThemeDirty(true);
    bHandled = FALSE;   // 不吞掉：别的窗口/控件也可能关心
    return 0;
}

LRESULT LyricusCuiPanel::OnSettingChange(UINT, WPARAM, LPARAM, BOOL& bHandled) {
    // 系统度量/字体设置变了（宿主同样必须转发）。它会连带影响 CUI 的字体，
    // 所以也按主题变更处理。
    MarkThemeDirty(true);
    bHandled = FALSE;
    return 0;
}

LRESULT LyricusCuiPanel::OnDestroy(UINT, WPARAM, LPARAM, BOOL& bHandled) {
    ::KillTimer(m_hWnd, kRefreshTimerId);
    if (m_animTimerOn) { ::KillTimer(m_hWnd, kAnimTimerId); m_animTimerOn = false; }

    // 窗口没了，刷子也不会再有人用。在这里释放，不给类留清理逻辑：
    // SDK/ATL 保证窗口先于对象销毁（atl-misc.h 那句
    // PFC_ASSERT(this->m_hWnd == NULL) 就是这条约定）。析构里只做兜底。
    ReleaseGdiObjects();

    // **不能吞掉 WM_DESTROY**：ATL 自己也要靠它做收尾。
    bHandled = FALSE;
    return 0;
}

// ---------------------------------------------------------------------------
// 注册
// ---------------------------------------------------------------------------

// 多实例面板：window_factory<T>（window.h:438-456）—— 歌词面板完全可能被用户
// 摆两块（一块在侧栏、一块在顶栏），没有理由做成单实例。
// 匿名命名空间 + static：别的 cpp 没有任何理由看见这个符号，同名也不会互相干扰
//（和 dui_element.cpp:452-459 同款写法）。
static uie::window_factory<LyricusCuiPanel> g_lyricus_cui_panel;

} // namespace
} // namespace lyricus
