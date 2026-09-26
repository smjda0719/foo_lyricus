#include "stdafx.h"

#include "resource.h"
#include "config.h"
#include "control_window.h"
#include "debug_log.h"
#include "prefs_layout.h"
#include "prefs_page.h"         // OpenPrefsPage（View 菜单的入口）
#include "dpi_util.h"           // GetDpiForWindowSafe（与控制面板共用同一份）
#include "ui_draw.h"            // 圆角矩形/文字/字体/宿主主题（与色环取色器共用）
#include "color_util.h"         // ColorLuminance（判"该配黑字还是白字"）
#include "color_picker.h"       // PromptColorWheel（取色走自绘色环）
#include "lyric.h"              // Utf8ToWide / WideToUtf8（字体族是 UTF-8 存的）

#include <SDK/preferences_page.h>
#include <SDK/ui.h>             // ui_control::show_preferences（菜单"外观设置"要用）
#include <SDK/ui_element.h>     // ui_config_manager：宿主主题色 + 暗色模式
#include <helpers/atl-misc.h>   // preferences_page_impl
// ⚠️ 取色和取字体都走纯 Win32 的通用对话框（ChooseColorW / ChooseFontW），
//    刻意不碰 WTL 的 CColorDialog / CFontDialog —— 原因写在 PickColor() 里。
#include <commdlg.h>

#include <cstdio>

// ---------------------------------------------------------------------------
// Lyricus 首选项页 —— **全自绘**
//
// 【为什么要重做】用户 2026-09-26 说「这个界面有点老旧，在 lyricus 那个
// 二级界面实现一个更现代化的面板」。旧版是 GROUPBOX 分组框 + 六个 62x14 的
// 小色块按钮 + 系统 trackbar，控件长什么样完全由系统主题决定，改不动。
// 所以这一版把控件**全部删掉**（见 lyricus.rc），内容全在这里画。
//
// 【布局与配色是纯函数】都在 prefs_layout.h/.cpp 里，能进离线单测台
// （run.ps1 的 prefs 组）。这个文件只负责"把算好的矩形画出来"和"处理鼠标"。
// 那条界线很重要：自绘界面最容易出的两类问题（算错位置、配色读不清）
// 都是纯计算，放进单测比在截图里找强得多。
//
// 【页面底色必须问宿主要】自绘页面最怕颜色写死 —— 用户切暗色模式后一片惨白。
// 这里用 ui_config_manager::getSysColor()：它先查 foobar2000 的主题配置，
// 查不到才回退系统色（见 SDK/ui_element.cpp:259）。于是页面底色和首选项
// 窗口是同一个来源，接缝处不会有色差。
// ---------------------------------------------------------------------------

namespace {

using namespace lyricus;

// GUID 段 0x37：首选项页。分配前已 grep 全工程（见 D-022）。
const GUID guid_prefs_page = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x37}};

// 颜色项的绑定表：结构体成员 + 显示名。
//
// ⚠️ **顺序必须和 prefs_layout 的卡片编号一致**（索引 0..5）。
// 布局是按索引算矩形的（两行三列，从左到右、从上到下），这里换了顺序，
// 界面上的位置就跟着换 —— 两边必须同步改。
struct ColorSlot {
    COLORREF       PanelAppearance::*member;
    const wchar_t*                    label;
};

const ColorSlot kColorSlots[kPrefsColorCount] = {
    { &PanelAppearance::header,  L"曲名"       },
    { &PanelAppearance::current, L"当前歌词行" },
    { &PanelAppearance::normal,  L"其它歌词行" },
    { &PanelAppearance::dim,     L"次要文字"   },
    { &PanelAppearance::warn,    L"警告文字"   },
    { &PanelAppearance::bg,      L"面板底色"   },
};

// 4 个**控件基色**的绑定（D-093）。
//
// ⚠️ 顺序必须和 prefs_layout 里那 2x2 的排布一致（从左到右、从上到下）——
//    和 kColorSlots 同一个约定，改了这边不改那边，位置就对不上。
//
// 【为什么只有 4 个】控制条上一共有 11 类颜色（按钮的悬停/按下、图标的
// 普通/主操作/悬停/按下、滑块的轨道/填充、时间文字、音量图标、浮层底板）。
// 全暴露给用户太多了 —— 挑色本身就是负担，何况还得保证它们互相搭配。
// 每组一个基色、组内其余由程序推导，是这个模式能用的前提。
struct CtrlColorSlot {
    COLORREF       PanelAppearance::*member;
    const wchar_t*                    label;
};

const CtrlColorSlot kCtrlSlots[kPrefsCtrlColorCount] = {
    { &PanelAppearance::ctrlButton, L"按钮" },
    { &PanelAppearance::ctrlIcon,   L"图标" },
    { &PanelAppearance::ctrlSlider, L"滑块" },
    { &PanelAppearance::ctrlText,   L"文字" },
};

// 命中目标。色块直接用数组下标（0..5），其余用负值区分 ——
// 这样返回值能直接当数组下标用，少一层映射。
constexpr int kHitNone    = -1;
constexpr int kHitSlider  = -2;
constexpr int kHitReset   = -3;
constexpr int kHitFontBtn = -4;
// ---- 外观预设（D-088）----
constexpr int kHitPresetCombo  = -5;
constexpr int kHitPresetSave   = -6;
constexpr int kHitPresetDelete = -7;
constexpr int kHitPresetImport = -8;
constexpr int kHitPresetExport = -9;
// 控件配色（D-093）
constexpr int kHitCtrlMode     = -10;
// ---- 背景图（D-098）----
constexpr int kHitBgPick    = -11;   // 「选择图片…」
constexpr int kHitBgClear   = -12;   // 「清除」
constexpr int kHitBgFit     = -13;   // 适配方式（点击循环，不是下拉）
constexpr int kHitBgOpacity = -14;   // 下面三个是滑块
constexpr int kHitBgBlur    = -15;
constexpr int kHitBgDim     = -16;
// 预览区（D-103）/ 四角手柄（D-107）
constexpr int kHitBgPreview = -17;

// ⚠️ 角手柄用**连续的一段**，且这一段必须和上面那个值**完全不重叠**（D-113）。
//
//    原来写成 -20..-17，而 kHitBgPreview 正好是 -17 —— 于是**左下角手柄
//    和预览区是同一个编号**。HitTest 里手柄先判、返回 -17，OnLButtonDown
//    再把它当成预览区处理，结果**左下角手柄永远无效**（拖它是平移，不是缩放）。
//
//    这类 bug 特别隐蔽：两条分支各自看都对，只有"这两个常量挨着"这件事错。
//    所以宁可留出空档，也不要让两套编号首尾相接。
constexpr int kHitBgHandleBase = -30;

// 控件基色块用**独立的索引区**，不和上面那 6 个配色色块（0..5）混。
// 混在一起的话 HitTest 的 `hit < kPrefsColorCount` 判断会把它们误当成配色色块，
// 于是点控件色块改的是歌词颜色。
constexpr int kHitCtrlColorBase = 100;

// 布局判定"这块地方画不下"时返回的是空矩形，逐项判空后跳过即可。
//
// ⚠️ 它原本是 DrawPage 里的一个**局部 lambda**。加 DrawPresetArea 时才发现
//    那种写法只有那一个函数能用，于是第二个绘制函数只好自己再判一遍 ——
//    而"漏判"的表现是某个控件在窄窗口下画到客户区外面去，糊在别的控件上。
//    提到文件作用域，所有绘制函数共用同一份判断。
bool empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

class CLyricusPrefsDlg : public CDialogImpl<CLyricusPrefsDlg>,
                         public preferences_page_instance {
public:
    enum { IDD = IDD_LYRICUS_PREFS };

    explicit CLyricusPrefsDlg(preferences_page_callback::ptr callback)
        : m_callback(callback),
          m_edited(GetPanelAppearance()),     // 界面上正在编辑的值
          m_applied(GetPanelAppearance()),    // 上次"应用"下去的值
          m_editedFontFace(GetLyricDisplayConfig().fontFace),
          m_appliedFontFace(GetLyricDisplayConfig().fontFace) {}

    ~CLyricusPrefsDlg() { FreeFonts(); }

    // 字体在**两处**都释放：WM_DESTROY 和析构。两边都先把指针置空，
    // 所以谁先来都不会重复 DeleteObject。
    //
    // ⚠️ 别写成 `for (HFONT& f : { m_fontBody, ... })` ——
    //    initializer_list 的元素是 const，非 const 引用绑不上（编译不过）。
    void FreeFonts() {
        if (m_fontBody  != nullptr) { DeleteObject(m_fontBody);  m_fontBody  = nullptr; }
        if (m_fontBold  != nullptr) { DeleteObject(m_fontBold);  m_fontBold  = nullptr; }
        if (m_fontSmall != nullptr) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
    }

    BEGIN_MSG_MAP(CLyricusPrefsDlg)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_ERASEBKGND(OnEraseBkgnd)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSELEAVE(OnMouseLeave)
        MSG_WM_SETCURSOR(OnSetCursor)
        MSG_WM_GETDLGCODE(OnGetDlgCode)
        MSG_WM_KEYDOWN(OnKeyDown)
        MSG_WM_VSCROLL(OnVScroll)
        MSG_WM_MOUSEWHEEL(OnMouseWheel)
        MSG_WM_SIZE(OnSize)
        // 让 PrintWindow 能抓到自绘内容（见 OnPrintClient 的说明）
        MESSAGE_HANDLER(WM_PRINTCLIENT, OnPrintClient)
    END_MSG_MAP()

    // ---- preferences_page_instance 的契约 ----
    t_uint32 get_state() override;
    void     apply()     override;
    void     reset()     override;

private:
    // 当前客户区对应的布局与配色。两者都是纯函数，按需重算即可（很便宜）。
    PrefsLayout CurrentLayout() const;
    PrefsTheme  CurrentTheme() const;

    int  HitTest(POINT pt) const;              // 返回 kHit* 或色块下标
    void Repaint();
    void NotifyChanged();

    BOOL OnInitDialog(HWND hwndFocus, LPARAM lParam);
    void OnDestroy();
    BOOL OnEraseBkgnd(CDCHandle dc);
    void OnPaint(CDCHandle dc);
    // 把整页画到给定的 DC 上 —— WM_PAINT 和 WM_PRINTCLIENT 共用这一份
    void PaintTo(HDC dc, const RECT& rc);
    LRESULT OnPrintClient(UINT nMsg, WPARAM wp, LPARAM lp, BOOL& bHandled);

    // ---- 滚动（D-094）----
    void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar pScrollBar);
    BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    int  ContentHeightPx() const;   // 内容总高度（物理像素）
    void UpdateScrollBar();         // 按客户区与内容高度设置滚动条
    bool EnsureVScrollStyle();      // 确保样式里有 WS_VSCROLL（宿主动态抹过它）
    void ScrollTo(int y);           // 夹取后设置并重绘

    // ★ 客户区坐标 -> 内容坐标（D-124）。
    //
    // 【为什么值得单独一个函数】这个转换要在 HitTest、OnBgHandleDrag、
    //    BeginBgHandleDrag 三处做，而**漏掉任何一处**都会让"鼠标位置"和
    //    "布局算出来的位置"变成两个坐标系 —— D-121 / D-122 连着两次栽在
    //    这上面（一次是该加没加，一次是只加了一半、记起点那处漏了）。
    //
    //    表现还特别有迷惑性：方向看着是对的、只是数值乱，
    //    于是看起来像"比例没调好"，而不像"坐标系错了"。
    //
    //    收进一个函数之后，"忘记转"在语法上就不会发生了 ——
    //    凡是拿布局矩形和鼠标位置比较的地方，都从这一个入口拿坐标。
    CPoint ContentPoint(POINT clientPt) const {
        return CPoint(clientPt.x, clientPt.y + m_scrollY);
    }
    void OnSize(UINT nType, CSize size);
    void OnMouseMove(UINT flags, CPoint pt);
    void OnLButtonDown(UINT flags, CPoint pt);
    void OnLButtonUp(UINT flags, CPoint pt);
    void OnMouseLeave();
    BOOL OnSetCursor(CWindow wnd, UINT hitTest, UINT message);
    UINT OnGetDlgCode(LPMSG msg);
    void OnKeyDown(TCHAR key, UINT repeat, UINT flags);

    void DrawPage(HDC dc, const RECT& rc, const PrefsLayout& L, const PrefsTheme& T);
    void DrawColorCard(HDC dc, const RECT& card, int index);
    // 滑块绘制。maxValue 让同一个函数服务不同量程的滑块 ——
    // 不透明度是 0..255、图片不透明度 0..100、磨砂 0..40、压暗 0..90。
    // 各写一份的话，手柄位置/圆角/热区那几行会在四个地方慢慢跑偏。
    // 滑块绘制。hit 是它的命中目标 —— 有了它，"这个滑块现在是不是热的"
    // 就不用猜（`m_hot == hit || m_dragSlider == hit`）。
    // min/max 让同一个函数服务不同量程：面板不透明度 60..255、
    // 图片不透明度 0..100、磨砂 0..40、压暗 0..90。
    // 各写一份的话，手柄位置那几行会在四个地方慢慢跑偏。
    void DrawSlider(HDC dc, const RECT& r, int hit, int value, int minValue, int maxValue);
    void DrawButton(HDC dc, const RECT& r, const wchar_t* text, int hitId,
                    const PrefsTheme& T, bool leftAlign);
    void DrawResetButton(HDC dc, const RECT& r);
    void DrawFontButton(HDC dc, const RECT& r);
    void DrawPresetArea(HDC dc, const PrefsLayout& L);
    void DrawCtrlColorArea(HDC dc, const PrefsLayout& L);

    // ---- 控件配色（D-093）----
    // 点模式开关：自动 <-> 自定义。**从自动切到自定义时要把 4 个基色
    // 预填成当前推导的结果**，否则一按颜色就跳（见实现里的说明）。
    void OnCtrlModeToggle();
    // 取 4 个基色里的某一个：index 0..3
    void PickCtrlColor(int index);

    // ---- 外观预设的动作 ----
    // 全是**立刻生效**的，不走"应用 / 取消"：切一套配色就是要马上看到效果，
    // 再让他点一次应用没有意义（和色块那种"先在界面上试"不是一回事）。
    void OnPresetCombo();          // 弹菜单：选一套 / 另存为新预设
    void OnPresetSaveCurrent();    // 把当前外观覆盖进选中的预设
    void OnPresetDelete();         // 删掉选中的（内置的 = 恢复内置默认）
    void OnPresetImport();         // 从文件读一套进来
    void OnPresetExport();         // 把选中的写出去（分享 / 备份）

    // 把"当前的外观"采集出来。界面上正在编辑的颜色 + 正在编辑的字体，
    // 再加上**不在这一页上**的两项（字号、通透度）—— 后两者从配置读，
    // 因为"保存当前外观"的语义就是"保存现在的实际状态"。
    AppearancePreset SnapshotAppearance(const std::wstring& name) const;

    // 选中的预设名。空 = 还没挑过，界面上显示成「（未选择）」。
    // ⚠️ 它**不是配置项**：没有"当前预设"这个东西要持久化 ——
    //    应用过的值已经在各项设置里了，这个字符串只决定按钮上写什么。
    std::wstring m_presetName;

    // 把鼠标 x 换算成某个滑块的值并写进 m_edited。
    //
    // 泛化自原来的 SetAlphaFromSliderX —— 现在有四个滑块（面板不透明度、
    // 图片不透明度、磨砂、压暗），量程各不相同。每个各写一份的话，
    // 手柄半径的偏移、long long 防溢出这些细节会在四处慢慢跑偏，
    // 而"哪个滑块偏了 2 像素"是很难看出来的。
    void SetSliderFromX(int hit, int x);
    // 当前鼠标 x 落在哪个滑块的值上（点哪儿跳到哪儿用）
    void DrawBgArea(HDC dc, const PrefsLayout& L);
    void OnBgPick();   // 「选择图片…」

    // ---- 背景图手动构图（D-103）----
    void DrawBgPreview(HDC dc, const PrefsLayout& L);
    // 当前的手动构图参数（已夹取）
    BgManual CurrentManual() const;
    // 拖动预览：dx/dy 是本次鼠标位移（像素）
    void OnBgPreviewDrag(int dx, int dy);
    // 拖角缩放（D-107）。pt 是当前鼠标的客户区坐标。
    void OnBgHandleDrag(CPoint pt);
    // 开始拖角：记下起点状态
    void BeginBgHandleDrag(int hit, CPoint pt, const PrefsLayout& L);
    // 切到"手动"适配模式（拖动/缩放时自动调）
    void EnsureManualFit();
    // 图片在预览区里**实际占的矩形**（客户区坐标）。
    // 四角手柄贴的是它，不是预览框 —— 用户拖的是"这张图的角"（D-108）。
    // 返回 false 表示算不出来（没图、读不到尺寸、尺寸非法）。
    //
    // m 可以传一组**假设的**手动参数（拖角时要用它预演"缩放之后图在哪"）——
    // 传当前值就是"现在图在哪"。
    bool PreviewImageRect(const PrefsLayout& L, const BgManual& m, RECT& out) const;
    bool PreviewImageRect(const PrefsLayout& L, RECT& out) const {
        return PreviewImageRect(L, CurrentManual(), out);
    }

    // 弹一个模态取色器。
    //
    // ⚠️ 模态对话框会泵消息，期间**首选项窗口可能被关掉、页面随之被释放**。
    //    SDK 在 preferences_page.h:133 专门警告过这种情况。
    //    所以进来先拿一份自身引用把自己钉住，返回后碰成员才安全；
    //    返回后还要再确认一次窗口还在。
    bool PickColor(COLORREF& inOut);
    void PickFont();   // 弹系统字体对话框，把选中的**字体族**记进"正在编辑"的值

    preferences_page_callback::ptr m_callback;
    PanelAppearance m_edited;
    PanelAppearance m_applied;

    // 字体族（UTF-8，空 = 跟随宿主）。**和颜色一样走"应用 / 取消"**。
    //
    // 【为什么不立即写存储】原来是立即写的（D-082），理由是 ChooseFontW 是模态的、
    // 点确定就是明确意图。但那带来一个**当时没想到的副作用**：
    // `reset()` 会把它清掉，而颜色要"点应用"才落盘 —— 于是
    // **点「恢复默认」再点「取消」= 颜色回来了、字体回不来**。
    // 实测用户就是这么丢掉「方正姚体」的（配置里 lyricus.fontFace 还在，值是空的）。
    //
    // 【为什么是 UTF-8 的 std::string】和 LyricDisplayConfig::fontFace 同一口径，
    // 中间不必来回转换；Config::fontFace 本来就是 UTF-8 存的。
    std::string m_editedFontFace;
    std::string m_appliedFontFace;

    // 交互状态
    int  m_hot       = kHitNone;   // 鼠标悬停在谁身上
    int  m_active    = kHitNone;   // 按下了谁
    // 正在拖哪个滑块（kHitNone = 没有）。用**哪个**而不是布尔，
    // 是因为现在有四个滑块；布尔的话每加一个就要多一个标志，
    // 而漏掉"松开时清哪一个"就是滑块粘住鼠标。
    int    m_dragSlider = kHitNone;
    // 拖角缩放（D-107 / D-115）：**以对角那个手柄为锚点**。
    //
    // 【为什么锚点是对角】用户拖右下角时，期望的是"左上角钉住不动、
    //    右下角跟着鼠标走" —— 那正是图片编辑器的标准交互。
    //    围绕框心缩放做不到这一点：那样左下和右上也会跟着动，
    //    看起来像"图在框里漂"。
    //
    // 记的是**按下瞬间**的一整套状态（锚点位置、图的大小、offset、
    // 起点到锚点的距离）。拖动过程中全部以它为基准重算，而不是
    // 每帧用上一帧的结果递推 —— 递推会把取整误差累积起来，
    // 拖久了图会明显跑偏。
    CPoint m_dragAnchor{};        // 对角手柄的位置（客户区坐标）
    CPoint m_dragStartPt{};       // 按下时鼠标的位置（算曼哈顿距离比的基准）
    int    m_dragStartZoom   = 100;
    int    m_dragStartOffX   = 0; // 按下时的 offset
    int    m_dragStartOffY   = 0;
    RECT   m_dragStartDst{};      // 按下时图的实际矩形（客户区坐标）
    int    m_dragHandle      = kHitNone;
    bool   m_dragPreview = false;
    CPoint m_dragPreviewLast{};
    bool m_tracking  = false;      // 已登记 TME_LEAVE

    // 字体按 dpi 建一次就够（对话框存续期间不会变 dpi）
    HFONT m_fontBody  = nullptr;
    HFONT m_fontBold  = nullptr;
    HFONT m_fontSmall = nullptr;

    // ---- 滚动（D-094）----
    //
    // 页面内容一共 kPrefsHeight96 逻辑像素高，而宿主给的容器常常装不下 ——
    // 用户 2026-09-26 报的「加了这个之后页面太长了，需要一个滚动条」。
    //
    // ⚠️ 关键在于**布局按内容高度算，不是按客户区高度算**：
    //    `CurrentLayout()` 从前传的是客户区高度，于是窗口一矮，
    //    ComputePrefsLayout 就整体降级（色块从三列变两列、控件一个个消失）——
    //    而真正该发生的是"内容不动、加个滚动条"。
    //    这两件事混在一起时，表现是"窗口一小，设置项就不见了"，
    //    而不是"能滚下去看"。
    int m_scrollY = 0;      // 当前滚动偏移（物理像素，>= 0）
};

// ---------------------------------------------------------------------------

PrefsLayout CLyricusPrefsDlg::CurrentLayout() const {
    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    // ⚠️ 高度传的是**内容高度**，不是客户区高度。
    //
    // 布局回答的是"这些控件怎么排"，和"窗口现在显示到哪一段"是两件事。
    // 从前传客户区高度，于是窗口一矮 ComputePrefsLayout 就整体降级 ——
    // 色块从三列变两列、控件按顺序一个个消失。而用户期望的是
    // **内容不动、加个滚动条**（他 2026-09-26 报的正是这个）。
    // 这两件事混在一起时，表现是"窗口一小设置项就不见了"，很难联想到是布局降级。
    return ComputePrefsLayout(rc.right - rc.left, ContentHeightPx(),
                              static_cast<int>(GetDpiForWindowSafe(m_hWnd)));
}

int CLyricusPrefsDlg::ContentHeightPx() const {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    return MulDiv(kPrefsHeight96, (dpi > 0) ? dpi : 96, 96);
}

// 确保窗口样式里有 WS_VSCROLL。返回"这次是不是补了"。
//
// 【为什么需要一个能反复调用的函数】这个页的 style 会被**宿主动态重设**：
// 资源模板里写了、OnInitDialog 里也读得到（所以那次检查会认为"不用补"），
// 但 foobar2000 之后布局容器时又把它抹掉了。实测时间线就是这样 ——
// 初始化日志正常，而事后 GetWindowLongW(GWL_STYLE) 是 0x00010501。
//
// 所以"做一次就完"是不够的，得在每次绘制前顺手确认一下。
// 代价可以忽略：GetWindowLongPtr 极便宜，而 SWP_FRAMECHANGED 只在
// 真缺那一位时才发（正常情况一次都不会发）。
//
// ⚠️ 不补的后果**完全静默**：SetScrollInfo / SetScrollPos 会对一个不存在的
//    滚动条说话、统统失败，表现成"滚轮和拖动都没反应、页面下半截永远
//    看不到"，而代码逐行看都对。这个坑是靠 GetScrollInfo 返回 false 定位的。
bool CLyricusPrefsDlg::EnsureVScrollStyle() {
    const LONG_PTR style = ::GetWindowLongPtrW(m_hWnd, GWL_STYLE);
    if ((style & WS_VSCROLL) != 0) return false;

    ::SetWindowLongPtrW(m_hWnd, GWL_STYLE,
                        static_cast<LONG_PTR>(style | WS_VSCROLL));
    // ⚠️ 改 GWL_STYLE 之后必须 SWP_FRAMECHANGED 重算非客户区，
    //    否则滚动条不会真的出现（SetWindowLongPtr 只改记录的值）。
    ::SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_NOACTIVATE | SWP_FRAMECHANGED);
    DebugLog("首选项页：补上 WS_VSCROLL（原 style=0x%08lX）",
             static_cast<unsigned long>(style));
    return true;
}

void CLyricusPrefsDlg::UpdateScrollBar() {
    EnsureVScrollStyle();

    RECT rc{};
    if (!::GetClientRect(m_hWnd, &rc)) return;

    const int contentH = ContentHeightPx();
    const int clientH  = rc.bottom - rc.top;
    const int maxY     = (contentH > clientH) ? (contentH - clientH) : 0;

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = contentH - 1;
    si.nPage  = static_cast<UINT>((clientH > 0) ? clientH : 1);
    si.nPos   = m_scrollY;
    // SIF_DISABLENOSCROLL：装得下时**仍然显示**滚动条但置灰。
    // 不这样做的话它在"刚好装得下"和"差一点"之间来回显示/隐藏，
    // 而滚动条一出现客户区宽度就变，布局跟着重排 —— 会闪。
    ::SetScrollInfo(m_hWnd, SB_VERT, &si, TRUE);

    // 内容变矮了（或窗口变高了）就把偏移夹回来，否则会停在空白处
    if (m_scrollY > maxY) {
        m_scrollY = maxY;
        ::SetScrollPos(m_hWnd, SB_VERT, m_scrollY, TRUE);
    }
}

void CLyricusPrefsDlg::ScrollTo(int y) {
    RECT rc{};
    if (!::GetClientRect(m_hWnd, &rc)) return;

    const int maxY = ContentHeightPx() - (rc.bottom - rc.top);
    if (y < 0) y = 0;
    if (y > maxY) y = (maxY > 0) ? maxY : 0;
    if (y == m_scrollY) return;

    m_scrollY = y;
    ::SetScrollPos(m_hWnd, SB_VERT, m_scrollY, TRUE);
    Repaint();
}

void CLyricusPrefsDlg::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    ::GetScrollInfo(m_hWnd, SB_VERT, &si);

    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int lineStep = MulDiv(24, (dpi > 0) ? dpi : 96, 96);

    int target = m_scrollY;
    switch (nSBCode) {
        case SB_LINEUP:        target -= lineStep; break;
        case SB_LINEDOWN:      target += lineStep; break;
        case SB_PAGEUP:        target -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN:      target += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: target = static_cast<int>(si.nTrackPos); break;
        case SB_TOP:           target = 0; break;
        case SB_BOTTOM:        target = si.nMax; break;
        default: return;
    }
    ScrollTo(target);
}

BOOL CLyricusPrefsDlg::OnMouseWheel(UINT, short zDelta, CPoint) {
    // ⚠️ 滚轮**只滚页面**，不再管预览区的缩放（D-107）。
    //
    // 之前预览区上的滚轮会缩放构图，但那和"滚轮在这个页面上该做的事"冲突 ——
    // 用户想上下看看、结果图被缩放了。而且预览区占了大半个页面宽，
    // 想滚过它几乎必然经过它。
    // 缩放改用**拖四角**（图片编辑器的标准交互），滚轮交还给页面。
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int step = MulDiv(24 * 3, (dpi > 0) ? dpi : 96, 96);
    ScrollTo(m_scrollY - (static_cast<int>(zDelta) * step) / WHEEL_DELTA);
    return TRUE;
}

void CLyricusPrefsDlg::OnSize(UINT, CSize) {
    // 宿主改变容器大小时要重算滚动范围 —— 不重算的话，拖过首选项窗口之后
    // 滚动条的范围还是旧的：要么能滚出一段空白，要么滚不到底。
    UpdateScrollBar();
}

PrefsTheme CLyricusPrefsDlg::CurrentTheme() const {
    // 底色与文字问宿主要 —— 走 ui_draw 里的公共实现。
    // 它和色环取色器用的是同一份：两边各取各的，会出现"一个跟着 foobar2000
    // 的暗色主题走、另一个还是系统亮色"，两个窗口并排打开时非常刺眼。
    const HostTheme h = QueryHostTheme();
    return MakePrefsTheme(h.dark, h.bg, h.fg);
}

int CLyricusPrefsDlg::HitTest(POINT pt) const {
    // ⚠️ 先把**客户区坐标转成内容坐标** —— 布局是按内容坐标算的。
    //    不转的话，滚下去之后点哪儿都不对，而且偏移多少就错多少
    //   （表现是"滚过一段之后按钮全点不中"，很难联想到是坐标系没换）。
    pt = ContentPoint(pt);

    const PrefsLayout L = CurrentLayout();

    auto inside = [&pt](const RECT& r) {
        return r.right > r.left && r.bottom > r.top &&
               pt.x >= r.left && pt.x < r.right &&
               pt.y >= r.top  && pt.y < r.bottom;
    };

    for (int i = 0; i < kPrefsColorCount; ++i) {
        // 名称也算命中区域 —— 它紧贴色块下方，只点色块的话手感很别扭。
        if (inside(L.cards[i]) || inside(L.cardLabels[i])) return i;
    }
    if (inside(L.slider)) return kHitSlider;
    if (inside(L.reset))  return kHitReset;
    if (inside(L.fontBtn)) return kHitFontBtn;
    // ---- 外观预设（D-088）----
    if (inside(L.presetCombo))  return kHitPresetCombo;
    if (inside(L.presetSave))   return kHitPresetSave;
    if (inside(L.presetDelete)) return kHitPresetDelete;
    if (inside(L.presetImport)) return kHitPresetImport;
    if (inside(L.presetExport)) return kHitPresetExport;
    // ---- 背景图（D-098）----
    if (inside(L.bgClear))         return kHitBgClear;
    if (inside(L.bgPick))          return kHitBgPick;
    if (inside(L.bgFit))           return kHitBgFit;
    if (inside(L.bgOpacitySlider)) return kHitBgOpacity;
    if (inside(L.bgBlurSlider))    return kHitBgBlur;
    if (inside(L.bgDimSlider))     return kHitBgDim;

    // ⚠️ 角手柄要**先于**预览区判 —— 手柄贴在图的四角上，
    //    顺序反了的话它们永远会被预览区先吃掉，拖角就变成了平移。
    //    这类"优先级"错误的表现是"功能没反应"，很难联想到是判断顺序。
    RECT imgRect{};
    if (PreviewImageRect(L, imgRect)) {
        const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
        const int visSize = MulDiv(12, dpi, 96);
        const int hitSize = BgPreviewHandleHitSize(visSize);

        // ★★ 命中区是**以角为中心的一个小方块**，不是"从手柄往外扩"（D-109）。
        //
        // 【为什么】手柄本身画在框内（12 像素见方），再往外扩一圈的话，
        // 命中区就变成从角向**里**伸进去 20 像素 —— 而图往往本来就不到
        // 40 像素宽（预览框不高），于是**四个手柄的命中区几乎盖满整张图**，
        // 点哪儿都是缩放、根本拖不动图。
        //    用户报的正是这个："现在没办法拖动图片，默认会选到手柄缩放"。
        //
        // 以角为中心则命中区一半在里、一半在外，四个角加起来只占图的四小角，
        // 中间大片区域留给平移。
        const int half = hitSize / 2;
        for (int i = 0; i < kBgHandleCount; ++i) {
            const RECT vis = BgPreviewHandle(imgRect, i, visSize);
            if (empty(vis)) continue;
            const int cx = (vis.left + vis.right) / 2;
            const int cy = (vis.top + vis.bottom) / 2;
            const RECT h{ cx - half, cy - half, cx + half, cy + half };
            if (inside(h)) return kHitBgHandleBase + i;
        }
    }

    if (inside(L.bgPreview))       return kHitBgPreview;
    // ---- 控件配色（D-093）----
    if (inside(L.ctrlModeBtn))  return kHitCtrlMode;
    for (int i = 0; i < kPrefsCtrlColorCount; ++i) {
        if (inside(L.ctrlCards[i])) return kHitCtrlColorBase + i;
    }
    return kHitNone;
}

void CLyricusPrefsDlg::Repaint() {
    if (::IsWindow(m_hWnd)) ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

BOOL CLyricusPrefsDlg::OnInitDialog(HWND, LPARAM) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    m_fontBody  = MakeUiFont(dpi, 9,  false);
    m_fontBold  = MakeUiFont(dpi, 9,  true);
    m_fontSmall = MakeUiFont(dpi, 8,  false);

    DebugLog("首选项页：已初始化（全自绘，alpha=%d）", m_edited.alpha);
    UpdateScrollBar();      // 内部会先确保 WS_VSCROLL 在位（见那边的说明）
    return TRUE;
}

void CLyricusPrefsDlg::OnDestroy() {
    FreeFonts();
}

BOOL CLyricusPrefsDlg::OnEraseBkgnd(CDCHandle) {
    // 返回 TRUE = "我已经处理了" —— 不让系统擦背景。
    // 配合 OnPaint 里的双缓冲，页面切换时不会闪白。
    return TRUE;
}

void CLyricusPrefsDlg::OnPaint(CDCHandle) {
    // 宿主会在布局时把这个页的 WS_VSCROLL 抹掉，而滚动条不在位时
    // SetScrollInfo 全部静默失败。绘制是每帧都走的路径，在这里确认一次
    // 兜得住（真缺的时候还会顺手 UpdateScrollBar 把范围重新报上去）。
    if (EnsureVScrollStyle()) UpdateScrollBar();

    PAINTSTRUCT ps{};
    const HDC dc = BeginPaint(&ps);
    if (dc == nullptr) return;

    RECT rc{};
    ::GetClientRect(m_hWnd, &rc);
    PaintTo(dc, rc);

    EndPaint(&ps);
}

// 把整页画到**给定的 DC** 上。WM_PAINT 和 WM_PRINTCLIENT 都走这里。
void CLyricusPrefsDlg::PaintTo(HDC dc, const RECT& rc) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // 双缓冲：自绘页面直接在窗口 DC 上画会闪（尤其是拖滑块时每帧重绘）
    const HDC mem = CreateCompatibleDC(dc);
    const HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    const HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // ★ 滚动（D-094）：把绘图原点往上移 m_scrollY，之后**所有绘制代码
    //   都不用管滚动** —— 它们本来就在内容坐标系里写，和布局共用同一套坐标。
    //   比在每一处调用上加偏移可靠得多：后者漏一处就是"某个控件不跟着滚"，
    //   而这种漏很难发现（要滚到那个位置才看得见）。
    //
    // ⚠️ 符号是 **+m_scrollY**，不是负的。
    //    SetWindowOrgEx 设的是"窗口原点在逻辑坐标里的位置"，而
    //    **设备坐标 = 逻辑坐标 − 窗口原点**。要让内容坐标 y=scrollY 的点
    //    落在客户区 y=0 上（也就是内容往上走），需要 0 = scrollY − 原点y，
    //    即原点y = +scrollY。
    //    写成负号的话滚下去内容反而往下跑 —— 表现就是"滚了但看不到下面的东西"。
    POINT oldOrg{};
    ::SetWindowOrgEx(mem, 0, m_scrollY, &oldOrg);

    // ⚠️ 传给 DrawPage 的 rc 也要换成**内容坐标**下的客户区矩形 ——
    //    它第一件事就是拿这个矩形铺底。传未偏移的那个的话，
    //    滚下去之后底部会留出一条没铺到的缝（露出上一帧的内容）。
    RECT contentRc = rc;
    contentRc.top    += m_scrollY;
    contentRc.bottom += m_scrollY;

    DrawPage(mem, contentRc, CurrentLayout(), CurrentTheme());

    ::SetWindowOrgEx(mem, oldOrg.x, oldOrg.y, nullptr);
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ★ 让 `PrintWindow` 能抓到自绘内容。
//
// 【为什么必须有】全自绘窗口不处理这条消息时，`PrintWindow` 只会走到
// 默认的背景处理 —— 截图里**内容一片空白，而窗口在屏幕上完全正常**。
// 用户 2026-09-26 看到我那张"滚到底变空白"的截图时说
// 「是你的截图脚本问题，我实际看没有问题」，根因就在这儿。
//
// `PrintWindow` 发的是 `WM_PRINT`，而 `WM_PRINT` 的默认处理会把
// `WM_PRINTCLIENT` 转给窗口 —— 所以只需要接这一条。
// 它给的 DC 需要我们**自己**画上去，这也正是把绘制抽成 PaintTo 的原因。
LRESULT CLyricusPrefsDlg::OnPrintClient(UINT, WPARAM wp, LPARAM, BOOL& bHandled) {
    // ⚠️ WTL 的 MESSAGE_HANDLER 要求末尾那个 BOOL& —— 不设 bHandled
    //    的话消息还会继续往默认处理走，等于白画一遍。
    bHandled = TRUE;
    HDC dc = reinterpret_cast<HDC>(wp);
    if (dc != nullptr) {
        RECT rc{};
        ::GetClientRect(m_hWnd, &rc);
        PaintTo(dc, rc);
    }
    return 0;
}

void CLyricusPrefsDlg::DrawPage(HDC dc, const RECT& rc, const PrefsLayout& L,
                                const PrefsTheme& T) {
    // ---- 底色 ----
    HBRUSH bg = CreateSolidBrush(T.pageBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    // 逐项判空后跳过 —— empty() 现在是文件作用域的共用函数（见开头的说明）。

    // ---- 分组标题 ----
    DrawTextIn(dc, L.titleColors, L"浮动面板配色", T.text, m_fontBold,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, L.titleAlpha, L"整体不透明度", T.text, m_fontBold,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t buf[32];
    swprintf_s(buf, L"%d / 255", m_edited.alpha);
    DrawTextIn(dc, L.alphaValue, buf, T.textDim, m_fontBody,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // ---- 色块 ----
    for (int i = 0; i < kPrefsColorCount; ++i) {
        if (empty(L.cards[i])) continue;
        DrawColorCard(dc, L.cards[i], i);
        DrawTextIn(dc, L.cardLabels[i], kColorSlots[i].label, T.textDim, m_fontBody,
                   DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    // ---- 滑块 ----
    if (!empty(L.slider)) DrawSlider(dc, L.slider, kHitSlider, m_edited.alpha,
                                     kMinAlpha, kMaxAlpha);

    // ---- 底部说明 ----
    DrawTextIn(dc, L.hint,
               L"点色块选颜色；拖滑块调不透明度。\n"
               L"这些设置只影响浮动面板，DUI / CUI 面板跟随宿主主题。",
               T.textDim, m_fontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // ---- 按钮 ----
    if (!empty(L.reset)) DrawResetButton(dc, L.reset);
    if (!empty(L.fontBtn)) DrawFontButton(dc, L.fontBtn);
    DrawPresetArea(dc, L);
    DrawBgArea(dc, L);
    DrawBgPreview(dc, L);
    DrawCtrlColorArea(dc, L);
}

void CLyricusPrefsDlg::DrawColorCard(HDC dc, const RECT& card, int index) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int radius = MulDiv(8, dpi, 96);

    const COLORREF c = m_edited.*(kColorSlots[index].member);
    const bool hot    = (m_hot == index);
    const bool active = (m_active == index);

    RECT r = card;
    if (active) OffsetRect(&r, 0, MulDiv(1, dpi, 96));   // 按下时轻微下沉

    FillRoundRect(dc, r, radius, c);

    // 描边：色块可能和页面底色撞色。悬停时换成强调色，给出"可以点"的反馈。
    const PrefsTheme T = CurrentTheme();
    StrokeRoundRect(dc, r, radius, hot ? MulDiv(2, dpi, 96) : 1,
                    hot ? T.accent : T.border);

    // 十六进制值：按色块自身亮度选黑字还是白字，浅色和深色底上都读得清。
    wchar_t text[16];
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    DrawTextIn(dc, r, text, ColorLuminance(c) > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255),
               m_fontBody, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void CLyricusPrefsDlg::DrawSlider(HDC dc, const RECT& r, int hit, int value,
                                  int minValue, int maxValue) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const PrefsTheme T = CurrentTheme();

    const int trackH = MulDiv(6, dpi, 96);
    const int knobR  = MulDiv(9, dpi, 96);
    const int inset  = knobR;   // 手柄贴到两端时不至于被裁掉

    const int left  = r.left + inset;
    const int right = r.right - inset;
    if (right <= left) return;

    const int cy = (r.top + r.bottom) / 2;

    // 轨道
    RECT track{ left, cy - trackH / 2, right, cy + trackH / 2 };
    FillRoundRect(dc, track, trackH / 2, T.cardBg);

    // 已填充部分 + 手柄位置
    const int span = right - left;
    // ⚠️ 量程由参数决定，**不是**写死的 60..255（那是面板不透明度专用的）。
    //    图片不透明度是 0..100、磨砂 0..40、压暗 0..90 —— 写死的话
    //    后三个滑块的手柄位置全都会算错。
    if (maxValue <= minValue) return;
    if (value < minValue) value = minValue;
    if (value > maxValue) value = maxValue;
    const int pos  = left + MulDiv(span, value - minValue, maxValue - minValue);

    const bool hot = (m_hot == hit) || (m_dragSlider == hit);

    if (pos > left) {
        RECT fill{ left, track.top, pos, track.bottom };
        FillRoundRect(dc, fill, trackH / 2, T.accent);
    }

    // 手柄：悬停/拖动时稍微放大一点，给出反馈
    const int kr = hot ? knobR + MulDiv(1, dpi, 96) : knobR;
    RECT knob{ pos - kr, cy - kr, pos + kr, cy + kr };
    FillRoundRect(dc, knob, kr, hot ? T.accent : T.text);

    // 手柄描边用页面底色，让它和轨道之间有干净的边界
    StrokeRoundRect(dc, knob, kr, MulDiv(2, dpi, 96), T.pageBg);
}

// 自绘按钮。「恢复默认」和「字体...」共用 —— 两个按钮的视觉本来就该一致，
// 各写一份的话圆角、按下位移、文字色判断迟早各改各的，慢慢就看出不一样了。
//
// leftAlign：字体名长短差很多（「跟随」vs「Microsoft YaHei UI」），
// 左对齐 + 省略号才看得清开头；固定文案居中更好看。
void CLyricusPrefsDlg::DrawButton(HDC dc, const RECT& r, const wchar_t* text, int hitId,
                                  const PrefsTheme& T, bool leftAlign) {
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));

    const bool hot    = (m_hot == hitId);
    const bool active = (m_active == hitId);

    RECT box = r;
    if (active) OffsetRect(&box, 0, MulDiv(1, dpi, 96));

    const int radius = MulDiv(6, dpi, 96);
    const COLORREF fill = active ? T.accent : (hot ? T.cardHot : T.cardBg);
    FillRoundRect(dc, box, radius, fill);
    StrokeRoundRect(dc, box, radius, 1, T.border);

    // ★ 文字色按**填充色自己的亮度**选，而不是按状态猜。
    //
    // 这里原本写的是 `active ? 白 : T.text`，而它**当时是对的** ——
    // 因为填充色是从 T.pageBg 推导的、T.text 就是配它的那个前景色，两者同源。
    // 改成按 fill 判断是为了和 color_picker.cpp 保持同一个模式：
    // 那边出过一次「白色按钮配白色文字」（主题标志与实际取到的颜色不同源），
    // 按填充色判断是那种情况下唯一永远正确的做法。
    const COLORREF fg = (ColorLuminance(fill) > 140) ? RGB(20, 20, 24)
                                                     : RGB(255, 255, 255);

    RECT textRc = box;
    UINT flags = DT_VCENTER | DT_SINGLELINE;
    if (leftAlign) {
        textRc.left += MulDiv(10, dpi, 96);
        flags |= DT_LEFT | DT_END_ELLIPSIS;
    } else {
        flags |= DT_CENTER;
    }
    DrawTextIn(dc, textRc, text, fg, m_fontBody, flags);
}

void CLyricusPrefsDlg::DrawResetButton(HDC dc, const RECT& r) {
    DrawButton(dc, r, L"恢复默认", kHitReset, CurrentTheme(), false);
}

void CLyricusPrefsDlg::DrawFontButton(HDC dc, const RECT& r) {    // 显示**正在编辑**的字体，不是已落盘的那个 —— 和色块画 m_edited 一致。
    // 用配置里的值的话，用户选完字体按钮上还是旧名字，看着像没反应。
    std::wstring label = L"字体：";
    if (m_editedFontFace.empty()) {
        label += L"跟随";
    } else {
        label += Utf8ToWide(m_editedFontFace.c_str());
    }
    DrawButton(dc, r, label.c_str(), kHitFontBtn, CurrentTheme(), true);
}

// 外观预设区：标题 + 下拉框 + 四个按钮。
//
// 【为什么下拉用系统菜单而不是自绘列表】自绘列表要就地展开，而这一页只有
// 424 逻辑像素高 —— 列表必然盖住下面几行控件，于是要么被客户区裁掉，
// 要么得再开一个弹窗去处理失焦 / 滚动 / 键盘 / 点到外面关闭。
// TrackPopupMenu 这几件事全是现成的，而且位置它自己会算。
// 控件配色区：标题 + 模式开关 + 4 个基色块（D-093）。
//
// 【自动模式下那 4 个色块画的是什么】画的是**当前实际生效的颜色**
//（从面板底色推出来的那 4 个），而不是结构体里存着的那几个 ——
// 后者在自动模式下**根本不参与绘制**。
// 画实际值 + 标成禁用，用户才能看懂"现在就是这样，想改请切自定义"。
void CLyricusPrefsDlg::DrawCtrlColorArea(HDC dc, const PrefsLayout& L) {
    if (empty(L.titleCtrl) && empty(L.ctrlModeBtn)) return;   // 整体降级了

    const PrefsTheme& T = CurrentTheme();
    const bool custom = (m_edited.ctrlMode == kCtrlCustom);
    const int radius = MulDiv(8, L.dpi, 96);

    if (!empty(L.titleCtrl)) {
        DrawTextIn(dc, L.titleCtrl, L"控件配色", T.text, m_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (!empty(L.ctrlModeBtn)) {
        const bool hot = (m_hot == kHitCtrlMode);
        FillRoundRect(dc, L.ctrlModeBtn, MulDiv(6, L.dpi, 96), hot ? T.cardHot : T.cardBg);
        // 自定义模式下描边用强调色，一眼能看出"现在是你在控制"
        StrokeRoundRect(dc, L.ctrlModeBtn, MulDiv(6, L.dpi, 96), 1,
                        custom ? T.accent : T.border);
        DrawTextIn(dc, L.ctrlModeBtn, custom ? L"自定义" : L"自动推导",
                   custom ? T.text : T.textDim, m_fontBody,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // 自动模式下的实际颜色（禁用态要显示它们，而不是结构体里那 4 个）
    const ControlBaseColors autoCols = DeriveControlColors(m_edited.bg);
    const COLORREF autoShown[kPrefsCtrlColorCount] = {
        autoCols.button, autoCols.icon, autoCols.slider, autoCols.text
    };

    for (int i = 0; i < kPrefsCtrlColorCount; ++i) {
        if (empty(L.ctrlCards[i])) continue;

        const COLORREF c = custom ? (m_edited.*(kCtrlSlots[i].member)) : autoShown[i];

        RECT r = L.ctrlCards[i];
        const bool hot = custom && (m_hot == kHitCtrlColorBase + i);
        const bool active = custom && (m_active == kHitCtrlColorBase + i);
        if (active) OffsetRect(&r, 0, MulDiv(1, L.dpi, 96));

        FillRoundRect(dc, r, radius, c);
        // ⚠️ 文字色按**色块自己的亮度**选，不看模式 —— 和 DrawColorCard 同一条
        //    规则（D-075：自绘界面里颜色永远该由它压在上面的那个颜色决定）。
        StrokeRoundRect(dc, r, radius, hot ? MulDiv(2, L.dpi, 96) : 1,
                        hot ? T.accent : T.border);

        wchar_t text[16];
        swprintf_s(text, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        // 自动模式下把说明文字压暗：它不是可编辑的值，只是"现在长这样"
        const COLORREF txtColor = custom
            ? (ColorLuminance(c) > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255))
            : T.textDim;
        DrawTextIn(dc, r, text, txtColor, m_fontBody,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (!empty(L.ctrlCardLabels[i])) {
            // ⚠️ 标签用 T.textDim，**不要用 T.cardHot**。
            //    cardHot 是"卡片悬停底色"，浅色主题下几乎和页面底色一样白 ——
            //    拿它当文字色等于把标签藏起来（实测截图里「按钮」「图标」
            //    四个字基本看不见）。文字色只能用 textDim / text 这一类。
            DrawTextIn(dc, L.ctrlCardLabels[i], kCtrlSlots[i].label,
                       T.textDim, m_fontBody,
                       DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
    }
}

// 模式开关：自动 <-> 自定义（D-093）。
void CLyricusPrefsDlg::OnCtrlModeToggle() {
    if (m_edited.ctrlMode == kCtrlAuto) {
        // ★ 切到自定义时，把 4 个基色**预填成当前自动推导的结果**。
        //
        // 【为什么这一步不能省】不填的话它们会保持结构体里的默认值 ——
        // 而那未必等于当前底色推出来的东西（用户可能先切了「亮色」预设，
        // 底色早就变了）。结果是一按「自定义」颜色就跳一下，
        // 用户会以为这个按钮坏了。
        //
        // 预填之后切换是**无损**的：他看到的就是刚才那个样子，
        // 只是从这一刻起可以动它了。
        const ControlBaseColors cur = DeriveControlColors(m_edited.bg);
        m_edited.ctrlButton = cur.button;
        m_edited.ctrlIcon   = cur.icon;
        m_edited.ctrlSlider = cur.slider;
        m_edited.ctrlText   = cur.text;
        m_edited.ctrlMode   = kCtrlCustom;
    } else {
        // 切回自动：那 4 个基色**留着不动**（不参与绘制，但下次切回自定义时
        // 会被重新预填，所以留什么值都无所谓 —— 保留着还能让"自动->自定义->
        // 自动->自定义"这条路看到自己上次调的色）。
        m_edited.ctrlMode = kCtrlAuto;
    }
    NotifyChanged();
}

void CLyricusPrefsDlg::PickCtrlColor(int index) {
    if (index < 0 || index >= kPrefsCtrlColorCount) return;
    // 自动模式下不给改 —— 那些值不参与绘制，改了也没有效果，
    // 让它能点只会让人以为"改了没生效"。
    if (m_edited.ctrlMode != kCtrlCustom) return;

    COLORREF* p = &(m_edited.*(kCtrlSlots[index].member));
    if (PickColor(*p)) {          // 复用同一个色环对话框
        NotifyChanged();
    }
}

// 背景图区（D-098）。
void CLyricusPrefsDlg::DrawBgArea(HDC dc, const PrefsLayout& L) {
    if (empty(L.titleBg) && empty(L.bgPick)) return;   // 整区被降级了

    const PrefsTheme& T = CurrentTheme();
    const bool hasImg = !m_edited.bgImage.empty();

    if (!empty(L.titleBg)) {
        DrawTextIn(dc, L.titleBg, L"背景图", T.text, m_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 「清除」只在设了图时才画 —— 没图时它没有意义，画出来只会让人
    // 点一下然后什么都没发生。
    if (hasImg && !empty(L.bgClear)) {
        DrawButton(dc, L.bgClear, L"清除", kHitBgClear, T, false);
    }

    // 「选择图片…」兼任"当前路径"的显示位：单独再放一个只读路径框的话，
    // 窄窗口下两个都会被压扁，不如合成一个。
    //
    // ⚠️ 路径只显示**文件名**（`PathFindFileNameW`），不显示全路径：
    //    全路径的前半段永远是 C:\Users\...\Pictures\ 这种没有信息量的东西，
    //    而按钮宽度有限，显示全路径的结果是文件名被截掉 —— 恰好把
    //    唯一有用的部分丢了。
    if (!empty(L.bgPick)) {
        std::wstring caption;
        if (hasImg) {
            const std::wstring full = Utf8ToWide(m_edited.bgImage.c_str());
            const wchar_t* base = ::PathFindFileNameW(full.c_str());
            caption = std::wstring(L"选择图片…（当前：") +
                      ((base && *base) ? base : full.c_str()) + L"）";
        } else {
            caption = L"选择图片…（当前：无，用纯色底）";
        }
        DrawButton(dc, L.bgPick, caption.c_str(), kHitBgPick, T, false);
    }

    // 适配方式：**点击循环**而不是下拉。几个值，点几下转一圈 ——
    // 比弹菜单少一次交互，也少一份要测的代码。
    if (!empty(L.bgFit)) {
        // ⚠️ 这张表和 kBgFitMax **必须同步**。
        //
        // 这里踩过一次崩溃：加 BgFit::Manual 时把 kBgFitMax 从 3 改成了 4，
        // 却忘了给这个数组补第 5 项 —— 于是 f == 4 时读到数组外的垃圾指针，
        // 传给 DrawButton 之后在 wcslen 里访问违例。
        //
        // ★ 而且下面那句 `if (f > kBgFitMax) f = kBgFitMin;` 是**挡不住**的：
        //   它把 f 夹到"合法的最大值"，而上限本身就已经越界了。
        //   「用常量当边界」只有在常量和表长一致时才成立。
        //
        // 所以再加一条以**表长**为准的检查 —— 两者不一致时不会崩，
        // 最坏是显示错的文字（而那是能一眼看出来的）。
        static const wchar_t* const kFitNames[] = {
            L"填充（裁掉多余）", L"适应（可能留边）", L"拉伸（会变形）", L"平铺",
            L"手动（拖动调整）"
        };
        constexpr int kFitNameCount =
            static_cast<int>(sizeof(kFitNames) / sizeof(kFitNames[0]));
        static_assert(kFitNameCount == kBgFitMax + 1,
                      "kFitNames 的项数和 kBgFitMax 不同步了 —— 加适配方式时两处都要改");

        int f = m_edited.bgFit;
        if (f < 0 || f >= kFitNameCount) f = 0;
        const std::wstring cap = std::wstring(L"适配方式：") + kFitNames[f] + L"（点击切换）";
        DrawButton(dc, L.bgFit, cap.c_str(), kHitBgFit, T, false);
    }

    // 三个参数滑块。
    //
    // ⚠️ 没设图时画成**禁用态**（暗一档）但**仍然可拖** ——
    //    完全不给拖的话，用户想"先把参数调好再选图"就做不到；
    //    而画成亮色又会让人以为已经生效了。暗一档 + 能拖是这两者之间
    //    唯一说得通的做法。
    wchar_t buf[32];
    auto drawOne = [&](const RECT& lab, const RECT& val, const RECT& sl,
                       int hit, int cur, int lo, int hi, const wchar_t* text,
                       const wchar_t* unit) {
        if (empty(sl)) return;
        const COLORREF c = hasImg ? T.text : T.textDim;
        DrawTextIn(dc, lab, text, c, m_fontBody,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        swprintf_s(buf, L"%d%s", cur, unit);
        DrawTextIn(dc, val, buf, c, m_fontBody,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawSlider(dc, sl, hit, cur, lo, hi);
    };

    drawOne(L.bgOpacityLabel, L.bgOpacityValue, L.bgOpacitySlider,
            kHitBgOpacity, m_edited.bgOpacity, kBgOpacityMin, kBgOpacityMax,
            L"图片不透明度", L"%");
    drawOne(L.bgBlurLabel, L.bgBlurValue, L.bgBlurSlider,
            kHitBgBlur, m_edited.bgBlur, kBgBlurMin, kBgBlurMax,
            L"磨砂强度", L"");
    drawOne(L.bgDimLabel, L.bgDimValue, L.bgDimSlider,
            kHitBgDim, m_edited.bgDim, kBgDimMin, kBgDimMax,
            L"压暗（保证歌词可读）", L"%");
}

// 「选择图片…」（D-098）。
void CLyricusPrefsDlg::OnBgPick() {
    wchar_t path[MAX_PATH] = L"";
    if (!m_edited.bgImage.empty()) {
        const std::wstring cur = Utf8ToWide(m_edited.bgImage.c_str());
        wcsncpy_s(path, cur.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hWnd;
    ofn.lpstrFilter = L"图片\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp;*.tif;*.tiff\0所有文件\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"选择面板背景图";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!::GetOpenFileNameW(&ofn)) return;   // 用户取消

    m_edited.bgImage = WideToUtf8(path);
    // ⚠️ 换图必须**显式清缓存**：选了一张新图但四个参数都没变时，
    //    bg_image 那边会因为"路径变了"而重算 —— 那是它该做的。
    //    这里显式清是为了让语义更清楚：换了图就等于换了整个背景，
    //    不该指望下游靠参数比对去发现。
    ClearPanelBackgroundCache();
    DebugLog("背景图：用户选了 %ls", path);
    NotifyChanged();
}

bool CLyricusPrefsDlg::PreviewImageRect(const PrefsLayout& L, const BgManual& mIn,
                                        RECT& out) const {
    out = RECT{ 0, 0, 0, 0 };
    if (m_edited.bgImage.empty()) return false;

    int imgW = 0, imgH = 0;
    if (!GetBgImageSize(Utf8ToWide(m_edited.bgImage.c_str()), imgW, imgH)) return false;

    const int pw = L.bgPreview.right - L.bgPreview.left;
    const int ph = L.bgPreview.bottom - L.bgPreview.top;
    if (pw <= 0 || ph <= 0) return false;

    // ⚠️ 用**预览区的尺寸**算 —— 和 GetPanelBackground 那边是同一个函数、
    //    同一组参数，所以这里算出来的矩形和实际画出来的图**一定一致**。
    //    自己另写一套"图该多大"的算法就会有两份真相，
    //    而它们不一致的表现是"手柄和图错开"，看起来像手柄画歪了。
    const BgManual m = ClampBgManual(mIn);
    const BgPlacement place = ComputeBgPlacement(
        imgW, imgH, pw, ph,
        static_cast<BgFit>(m_edited.bgFit), m);
    if (!place.valid) return false;

    // place.dst 是"预览区坐标系"的 -> 平移到客户区坐标
    out.left   = L.bgPreview.left + place.dst.left;
    out.top    = L.bgPreview.top  + place.dst.top;
    out.right  = L.bgPreview.left + place.dst.right;
    out.bottom = L.bgPreview.top  + place.dst.bottom;
    return true;
}

BgManual CLyricusPrefsDlg::CurrentManual() const {
    BgManual m;
    m.zoomPct    = m_edited.bgZoomPct;
    m.offsetXPct = m_edited.bgOffsetXPct;
    m.offsetYPct = m_edited.bgOffsetYPct;
    return ClampBgManual(m);
}

void CLyricusPrefsDlg::EnsureManualFit() {
    if (m_edited.bgFit == static_cast<int>(BgFit::Manual)) return;
    // 用户一动手就切到"手动" —— 否则他拖了半天、画面却在按 Cover 重算，
    // 根本看不出拖动有效果。切换本身是无损的：手动参数默认就是
    // "铺满 + 居中"，也就是 Cover 的样子。
    m_edited.bgFit = static_cast<int>(BgFit::Manual);
    DebugLog("背景图：用户手动调整构图 -> 切到「手动」适配");
}

// 预览区（D-103）。
void CLyricusPrefsDlg::DrawBgPreview(HDC dc, const PrefsLayout& L) {
    if (empty(L.bgPreview)) return;

    const PrefsTheme& T = CurrentTheme();
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int rad = MulDiv(6, dpi, 96);
    const int w = L.bgPreview.right - L.bgPreview.left;
    const int h = L.bgPreview.bottom - L.bgPreview.top;
    if (w <= 0 || h <= 0) return;

    // ★ 底板用**面板底色**，不是首选项页的主题色（D-118）。
    //
    // 【为什么】预览要"所见即所得"—— 用户在这儿要看的是"我这套配色配这张图
    //    到底什么样"。用主题色的话预览永远是一块浅灰，和面板上真正的效果无关，
    //    那样底板这一层就白画了（还不如不画）。
    FillRoundRect(dc, L.bgPreview, rad, m_edited.bg);

    // ⚠️ 从图开始**裁剪到预览框内**（D-110）。
    //
    // 图放大之后会超出框（那是正常的，用户就是想让局部更大），
    // 但四角手柄是贴在**图的角**上的 —— 图一出框，手柄就跑到框外
    // 压在其他控件上，看着像画错了。
    //
    // 裁剪之后超出部分自然消失，而**部分落在框内**的手柄仍然看得见、
    // 也仍然拖得到（命中测试不裁剪）—— 用户不会因为"图比框大"
    // 就完全失去缩小的入口。
    const int savedDC = ::SaveDC(dc);
    ::IntersectClipRect(dc, L.bgPreview.left, L.bgPreview.top,
                        L.bgPreview.right, L.bgPreview.bottom);

    auto centered = [&](const wchar_t* msg) {
        DrawTextIn(dc, L.bgPreview, msg, T.textDim, m_fontBody,
                   DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    };

    const bool manual = (m_edited.bgFit == static_cast<int>(BgFit::Manual));

    if (m_edited.bgImage.empty()) {
        centered(L"选了图片之后，在这里拖动调整");
    } else {
        // ⚠️ 用**预览区的尺寸**取图，不是面板的 —— bg_image 有两个缓存槽，
        //    所以这两个尺寸不会互相挤掉。（只有一条缓存时它们会交替重算，
        //    表现为"一边好好的、另一边每帧卡"。）
        const int blurPx = MulDiv(m_edited.bgBlur, dpi, 96);
        const BgBitmap* bmp = GetPanelBackground(
            Utf8ToWide(m_edited.bgImage.c_str()), w, h,
            static_cast<BgFit>(m_edited.bgFit), CurrentManual(),
            blurPx, m_edited.bgDim, m_edited.bgOpacity);


        if (bmp == nullptr) {
            // 读不到就**如实说**，而不是画一块空白 —— 后者看起来像"没设图"，
            // 用户会去重新选一遍，而问题其实出在文件本身。
            centered(L"读不到这张图（文件被移走、或格式不支持）");
        } else {
            // ★★ 图要**混合**到底板上，不能直接覆盖（D-125）。
            //
            // 【为什么要绕这一圈】bmp 里"图之外的区域"是**透明像素，RGB = 0**。
            //    而 `StretchDIBits(..., SRCCOPY)` **不看 alpha**，于是那些地方
            //    被画成**纯黑**、把底板整个盖掉。
            //
            //    表现就是：无论把"面板底色"改成什么，预览里那块永远是黑的 ——
            //    用户报"底板颜色还是没变"正是这个。他看到黑色，
            //    以为那是底板，其实是透明区。
            //
            //    面板那边走的是 BlendBgOver（source-over），预览这边漏了 ——
            //    **同一个概念两条路径各写一份**，又一次漏掉了一边。
            //
            // 先在一块临时缓冲里铺底板再叠图，最后一次性贴上去。
            // 底板不透明（alpha=255），所以合成结果也是不透明的，
            // 这正是预览该有的样子（真实面板的不透明度由窗口管，
            // 预览里体现不了，也不该体现）。
            const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
            std::vector<unsigned char> canvas(n * 4);
            const BYTE bb = GetBValue(m_edited.bg);
            const BYTE bg = GetGValue(m_edited.bg);
            const BYTE br = GetRValue(m_edited.bg);
            for (size_t i = 0; i < n; ++i) {
                canvas[i * 4 + 0] = bb;
                canvas[i * 4 + 1] = bg;
                canvas[i * 4 + 2] = br;
                canvas[i * 4 + 3] = 255;
            }
            BlendBgOver(canvas.data(), bmp->bgra.data(), n);

            BITMAPINFO bi{};
            bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth       = w;
            bi.bmiHeader.biHeight      = -h;   // 负 = 自上而下，否则上下颠倒
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            ::StretchDIBits(dc, L.bgPreview.left, L.bgPreview.top, w, h,
                            0, 0, w, h,
                            canvas.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
    }

    // ★ 歌词示意（D-123 / D-126）。
    //
    // 【为什么必须有】预览里只画底板和控件的话，"歌词文字"那一整组颜色
    //（曲名 / 当前行 / 其它行 / 次要 / 警告）改了在预览里**毫无反应** ——
    //    而那恰恰是用户最常调的一组。
    //
    // 【为什么按面板的布局摆】上一版是五行从上往下平铺，曲名和歌词挤在一起、
    //    当前行还被挤出框外。预览的意义就是"看起来和面板一样"，
    //    所以位置要照面板来：**曲名在顶、歌词当前行垂直居中、控制条在底**。
    //    摆错了不如不摆 —— 用户会以为面板上也是那样。
    if (L.bgPreview.bottom - L.bgPreview.top > MulDiv(70, dpi, 96)) {
        const int lineH = MulDiv(19, dpi, 96);
        const int cx0 = L.bgPreview.left;
        const int cx1 = L.bgPreview.right;
        const int ctrlH = MulDiv(30, dpi, 96);   // 底部控制条大致占这么高

        auto line = [&](int cy, const wchar_t* text, COLORREF color, bool bold) {
            RECT r{ cx0, cy - lineH / 2, cx1, cy + lineH / 2 };
            DrawTextIn(dc, r, text, color, bold ? m_fontBold : m_fontBody,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        };

        // 曲名：贴着预览框顶部（面板上也是顶部一行）
        line(L.bgPreview.top + MulDiv(12, dpi, 96) + lineH / 2,
             L"曲名 — 歌手", m_edited.header, true);

        // 歌词：以"控制条上方的区域"的中心为当前行位置 —— 和面板一致
        const int midY = (L.bgPreview.top +
                          (L.bgPreview.bottom - ctrlH)) / 2;
        line(midY - lineH * 3 / 2, L"上一行歌词",   m_edited.dim,    false);
        line(midY,                 L"当前这一行歌词", m_edited.current, true);
        line(midY + lineH * 3 / 2, L"下一行歌词",   m_edited.normal, false);

        // 警告色单独放控制条上方一行 —— 它平时不出现，
        // 但用户调色时要能看到自己挑的是什么。
        line(L.bgPreview.bottom - ctrlH - lineH / 2,
             L"歌词未找到", m_edited.warn, false);
    }

    // 边框画在最后（先画会被图盖住）
    StrokeRoundRect(dc, L.bgPreview, rad, 1, T.border);

    // ★ 把**控件也画出来**（D-118）。
    //
    // 【为什么】选背景图时真正要判断的是"控件压在这张图上还看得清吗" ——
    //    只画一张图完全看不出这一点：一张浅色图配浅色控件，
    //    图本身好看，但控制条会糊成一片。
    //
    // 复用面板那份实现（ControlWindow::DrawControlsPreview），
    // 而不是在这儿重写一遍 —— 两份实现迟早跑偏，
    // 而"预览里看到的和面板上的不是一回事"是这种预览最糟的失败方式。
    ControlWindow::DrawControlsPreview(dc, L.bgPreview, m_edited, dpi);

    // 裁剪到此为止 —— 下面要画手柄，但手柄也得跟着裁
    //（图超出框时它们在框外，正是要裁掉的那部分）。
    ::RestoreDC(dc, savedDC);
    const int savedDC2 = ::SaveDC(dc);
    ::IntersectClipRect(dc, L.bgPreview.left, L.bgPreview.top,
                        L.bgPreview.right, L.bgPreview.bottom);

    // 四角手柄（D-107 / D-108）。**贴在图片的四角上**，不是预览框的四角。
    //
    // 【为什么要贴图】用户拖的是"这张图的角"。贴框的话，图没铺满时
    //（露边、或者手动缩过）手柄会飘在离图很远的框角上，
    // 看起来像"手柄和内容对不上"。而且拖那个角到底在缩什么也不明确。
    RECT imgRect{};
    if (PreviewImageRect(L, imgRect)) {
        const int visSize = MulDiv(12, dpi, 96);
        const int hRad    = MulDiv(3, dpi, 96);
        for (int i = 0; i < kBgHandleCount; ++i) {
            RECT h = BgPreviewHandle(imgRect, i, visSize);
            if (empty(h)) continue;
            const bool hot = (m_hot == kHitBgHandleBase + i) ||
                             (m_dragHandle == kHitBgHandleBase + i);
            // 白块 + 深色描边：这个组合在**任何**底图上都看得见 ——
            // 纯白会在浅色图上消失，纯深色会在暗图上消失，只有带描边的能两头兼顾。
            FillRoundRect(dc, h, hRad, hot ? T.accent : RGB(255, 255, 255));
            StrokeRoundRect(dc, h, hRad, MulDiv(1, dpi, 96), RGB(70, 70, 75));
        }
    }
    ::RestoreDC(dc, savedDC2);

    if (!empty(L.bgPreviewHint)) {
        // 提示里带上**当前缩放百分比**（D-107）。
        // 拖角是连续操作，没有数字的话用户不知道现在放大了多少、
        // 也不知道有没有到上下限 —— 而"拖了没反应"正是上下限时的表现。
        wchar_t hintBuf[128] = L"";
        const wchar_t* hint = L"";
        if (!m_edited.bgImage.empty()) {
            if (manual) {
                swprintf_s(hintBuf, L"拖动移动 · 拖四角缩放 · 当前 %d%%",
                           m_edited.bgZoomPct);
                hint = hintBuf;
            } else {
                hint = L"拖动或拖角会自动切成「手动」适配";
            }
        }
        DrawTextIn(dc, L.bgPreviewHint, hint, T.textDim, m_fontSmall,
                   DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
}

void CLyricusPrefsDlg::OnBgPreviewDrag(int dx, int dy) {
    if (m_edited.bgImage.empty()) return;

    // ⚠️ 用 BgManualRange 算"可移动范围" —— 它和 ComputeBgPlacement 共用同一份，
    //    所以预览里拖到底和面板里拖到底是**同一个位置**。
    //    自己拍一个"拖 N 像素 = 偏移 M%"的话两边会走不一样的距离，
    //    而那种不一致在只看着一边的时候发现不了。
    int imgW = 0, imgH = 0;
    if (!GetBgImageSize(Utf8ToWide(m_edited.bgImage.c_str()), imgW, imgH)) return;

    const PrefsLayout L = CurrentLayout();
    const int pw = L.bgPreview.right - L.bgPreview.left;
    const int ph = L.bgPreview.bottom - L.bgPreview.top;
    if (pw <= 0 || ph <= 0) return;

    BgManual m = CurrentManual();
    int rangeX = 0, rangeY = 0;
    BgManualRange(imgW, imgH, pw, ph, m, rangeX, rangeY);

    // 符号：dstX = -rangeX - rangeX*offset/100，所以**往右拖（dx>0）要让
    // offset 变小**（图往右移）。写反的话拖动方向是反的，
    // 而那用起来像"鼠标抓不住图"。
    if (rangeX > 0) {
        m.offsetXPct -= static_cast<int>(
                            static_cast<long long>(dx) * 100 / rangeX);
    }
    if (rangeY > 0) {
        m.offsetYPct -= static_cast<int>(
                            static_cast<long long>(dy) * 100 / rangeY);
    }

    const BgManual c = ClampBgManual(m);
    if (c.offsetXPct == m_edited.bgOffsetXPct && c.offsetYPct == m_edited.bgOffsetYPct) {
        return;   // 已经贴边了，不用重绘
    }
    m_edited.bgOffsetXPct = c.offsetXPct;
    m_edited.bgOffsetYPct = c.offsetYPct;
    NotifyChanged();
    Repaint();
}

void CLyricusPrefsDlg::BeginBgHandleDrag(int hit, CPoint pt, const PrefsLayout& L) {
    m_dragHandle = hit;

    // ★ 和 OnBgHandleDrag 一样转成**内容坐标**（D-122）。
    //
    // 上一版只在那一边转了，这里漏了 —— 于是 m_dragStartPt 是客户区、
    // pt 是内容坐标，**又**变成两个坐标系。同一个坑踩两次：
    // 凡是"记下来的鼠标位置"和"后来的鼠标位置"必须来自同一套坐标。
    //
    // 具体表现：滚过页面之后拖角，P0 和 P 差一个 scrollY，
    // 起点到锚点的分量全错，ratio 乱跳（日志里 0.000 → 0.209 → 0.032）。
    pt = ContentPoint(pt);

    // ⚠️ 先切到手动模式 —— 下面算"图在哪"要用 m_edited.bgFit，
    //    切之前算出来的是别的适配方式下的位置，锚点就错了。
    EnsureManualFit();

    RECT imgRect{};
    if (!PreviewImageRect(L, imgRect)) return;
    m_dragStartDst = imgRect;

    // ★ 锚点 = **对角**那个手柄（D-115）。
    //   拖右下角时钉住左上角，那才是图片编辑器的标准交互；
    //   围绕框心缩放的话左下和右上也会跟着动，看起来像"图在框里漂"。
    const int corner = hit - kHitBgHandleBase;
    const int anchorCorner = (corner + 2) % kBgHandleCount;
    const int dpi = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const RECT ah = BgPreviewHandle(imgRect, anchorCorner, MulDiv(12, dpi, 96));
    m_dragAnchor.x = (ah.left + ah.right) / 2;
    m_dragAnchor.y = (ah.top + ah.bottom) / 2;



    const BgManual m = CurrentManual();
    m_dragStartZoom = m.zoomPct;
    m_dragStartOffX = m.offsetXPct;
    m_dragStartOffY = m.offsetYPct;
    m_dragStartPt   = pt;
}

void CLyricusPrefsDlg::OnBgHandleDrag(CPoint pt) {
    if (m_dragHandle == kHitNone) return;

    // ★★ 先转成**内容坐标**（D-121）。
    //
    // 【为什么必须转】m_dragAnchor 是从 imgRect 算的，而 imgRect 来自
    //    `L.bgPreview.left + place.dst.left` —— 那是**内容坐标**。
    //    而 pt 是 WM_MOUSEMOVE 给的**客户区坐标**。两者差一个 m_scrollY。
    //
    //    不转的话整套计算都在比较两个不同的坐标系：锚点看起来在
    //    "图的位置"，鼠标却在"屏幕的位置"，于是
    //      · 起点到锚点的那两个分量全是错的；
    //      · 越拖方向越乱，而且滚过页面之后错得更多。
    //    用户报的"拖左下角往下反而缩小"就是这么来的。
    //
    //    HitTest 里早就在做同一件事（`pt.y += m_scrollY`），
    //    这里漏了 —— 凡是拿布局算出来的矩形和鼠标位置比较的地方，
    //    都要先统一到内容坐标。
    pt = ContentPoint(pt);

    // ★ 曼哈顿距离，但**分量带符号**（D-119）。
    //
    // 【为什么不能取绝对值】用户发现的：「以右下角的手柄为例，我把手柄拉到
    //    图片内部，一样会放大，因为曼哈顿距离会抹掉符号」。
    //    `|dx| + |dy|` 在鼠标越过锚点之后仍然变大，于是本该缩小的操作
    //    反而在放大。
    //
    // 【修法】把两个分量**统一到"从锚点指向初始角"这个正向**：
    //      sx0 = sign(P0.x - A.x)      —— 初始角在锚点的哪一侧
    //      d1x = (P.x - A.x) * sx0     —— 越过锚点后变成负数
    //    于是 ratio 会连续地从 >1 走到 <1 再到 <0，而负数由
    //    ClampBgManual 的下限接住（图不会翻转）。
    //
    // 【为什么保留曼哈顿而不是退回欧氏距离】欧氏距离同样要开方、同样要判符号，
    //    但它在"只横着拖"时迟钝（见 D-117）。带符号的曼哈顿两个毛病都没有。
    //
    // 【为什么还要 |dy| 那一项参与】它是"把两个方向的位移加起来"的来源 ——
    //    斜着拖时两个方向都计入，那正是 D-117 要的性质。
    const int sx0 = (m_dragStartPt.x >= m_dragAnchor.x) ? 1 : -1;
    const int sy0 = (m_dragStartPt.y >= m_dragAnchor.y) ? 1 : -1;

    const int d0x = (m_dragStartPt.x - m_dragAnchor.x) * sx0;   // 恒 >= 0
    const int d0y = (m_dragStartPt.y - m_dragAnchor.y) * sy0;
    const int d1x = (pt.x - m_dragAnchor.x) * sx0;              // 可正可负
    const int d1y = (pt.y - m_dragAnchor.y) * sy0;

    // ★ 比例的分母是**那一方向的跨度**（≈ 图的大小），不是"到锚点的距离"（D-122）。
    //
    // 【为什么曼哈顿距离之比太钝】m0 是起点到锚点的**整个行程**
    //（实测 526），拖 100 像素只让 ratio 变成 1.19 —— 手感上几乎没动。
    //    而分量比例的分母是 `|P0.x - A.x|`（图在该方向的跨度，实测 221），
    //    同样拖 100 像素是 1.45，变化明显。
    //
    // 【为什么取两个分量里较大的那个】拖右下角时横向和纵向都在动，
    //    取 max 等于"谁拖得多听谁的"，比固定用某一轴更贴合手感；
    //    而某个方向完全没动时它的比例恒为 1，不会干扰。
    //
    // 符号仍然带（见下），所以越过锚点照样能缩 —— 那是 D-119 修的。
    const double rx = (d0x > 1) ? (static_cast<double>(d1x) / d0x) : 0.0;
    const double ry = (d0y > 1) ? (static_cast<double>(d1y) / d0y) : 0.0;
    const double ratio = (rx > ry) ? rx : ry;

    BgManual m = CurrentManual();
    m.zoomPct = static_cast<int>(std::lround(m_dragStartZoom * ratio));
    const BgManual c = ClampBgManual(m);


    if (c.zoomPct == m_edited.bgZoomPct) return;   // 已经到上下限

    // ⚠️ 用**夹取后**的 zoom 算实际比例。用未夹取的 ratio 的话，
    //    拖到上下限之后图还会继续长/缩，而数字已经不动了 ——
    //    那看起来像"图失控了"。
    const double actual = static_cast<double>(c.zoomPct) / m_dragStartZoom;

    // 围绕锚点缩放按下时那个矩形，得到图**应该**落在哪儿
    const int wantX = m_dragAnchor.x +
        static_cast<int>(std::lround((m_dragStartDst.left - m_dragAnchor.x) * actual));
    const int wantY = m_dragAnchor.y +
        static_cast<int>(std::lround((m_dragStartDst.top - m_dragAnchor.y) * actual));

    // 再把"该在哪儿"反解成 offset。
    //   dstX = -halfW - rangeX * offset / 100
    //     => offset = -(dstX + halfW) * 100 / rangeX
    //
    // ⚠️ halfW/rangeX 必须按**新的**缩放算 —— 图的大小变了，可移动幅度也跟着变。
    const PrefsLayout L = CurrentLayout();
    RECT newRect{};
    if (!PreviewImageRect(L, c, newRect)) return;
    const int pw = L.bgPreview.right - L.bgPreview.left;
    const int ph = L.bgPreview.bottom - L.bgPreview.top;
    const int drawW = newRect.right - newRect.left;
    const int drawH = newRect.bottom - newRect.top;

    const int halfW = (drawW - pw) / 2;
    const int halfH = (drawH - ph) / 2;
    const int rangeX = (halfW >= 0) ? halfW : -halfW;
    const int rangeY = (halfH >= 0) ? halfH : -halfH;

    // wantX/wantY 是客户区坐标，而 dstX/dstY 是预览框坐标系 —— 减掉框的左上角
    const int wantLocalX = wantX - L.bgPreview.left;
    const int wantLocalY = wantY - L.bgPreview.top;

    // range 为 0 表示那个方向没有可移动的余地，保持按下时的 offset
    const int offX = (rangeX > 0) ? -(wantLocalX + halfW) * 100 / rangeX : m_dragStartOffX;
    const int offY = (rangeY > 0) ? -(wantLocalY + halfH) * 100 / rangeY : m_dragStartOffY;

    BgManual m2 = c;
    m2.offsetXPct = offX;
    m2.offsetYPct = offY;
    const BgManual c2 = ClampBgManual(m2);

    m_edited.bgZoomPct    = c2.zoomPct;
    m_edited.bgOffsetXPct = c2.offsetXPct;
    m_edited.bgOffsetYPct = c2.offsetYPct;
    NotifyChanged();
    Repaint();
}

void CLyricusPrefsDlg::DrawPresetArea(HDC dc, const PrefsLayout& L) {
    if (empty(L.titlePreset) && empty(L.presetCombo)) return;   // 整体降级了

    const PrefsTheme& T = CurrentTheme();
    const int r4 = MulDiv(4, L.dpi, 96);

    if (!empty(L.titlePreset)) {
        DrawTextIn(dc, L.titlePreset, L"外观预设", T.text, m_fontBold,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (!empty(L.presetCombo)) {
        const bool hot = (m_hot == kHitPresetCombo);
        FillRoundRect(dc, L.presetCombo, r4, hot ? T.cardHot : T.cardBg);
        StrokeRoundRect(dc, L.presetCombo, r4, 1, T.border);

        RECT textRc = L.presetCombo;
        textRc.left  += MulDiv(8,  L.dpi, 96);
        textRc.right -= MulDiv(22, L.dpi, 96);      // 给 ▼ 留位置

        // 没选过时显示成**暗色**的提示语 —— 用 textDim 而不是 text，
        // 这样"这是一句说明"和"这是一个预设名"一眼能分开。
        const bool hasSel = !m_presetName.empty();
        DrawTextIn(dc, textRc, hasSel ? m_presetName.c_str() : L"（未选择）",
                   hasSel ? T.text : T.textDim, m_fontBody,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // ▼ 实心三角，贴在右边
        const int cx = L.presetCombo.right - MulDiv(13, L.dpi, 96);
        const int cy = (L.presetCombo.top + L.presetCombo.bottom) / 2;
        const int hw = MulDiv(4, L.dpi, 96);
        const int hh = MulDiv(3, L.dpi, 96);
        POINT tri[3] = { { cx - hw, cy - hh }, { cx + hw, cy - hh }, { cx, cy + hh } };

        HBRUSH br = CreateSolidBrush(T.textDim);
        HGDIOBJ oldBr  = SelectObject(dc, br);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBr);
        DeleteObject(br);
    }

    if (!empty(L.presetSave))   DrawButton(dc, L.presetSave,   L"保存", kHitPresetSave,   T, false);
    if (!empty(L.presetDelete)) DrawButton(dc, L.presetDelete, L"删除", kHitPresetDelete, T, false);
    if (!empty(L.presetImport)) DrawButton(dc, L.presetImport, L"导入", kHitPresetImport, T, false);
    if (!empty(L.presetExport)) DrawButton(dc, L.presetExport, L"导出", kHitPresetExport, T, false);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void CLyricusPrefsDlg::OnMouseMove(UINT /*flags*/, CPoint pt) {
    // 拖角缩放（D-107）
    if (m_dragHandle != kHitNone) {
        OnBgHandleDrag(pt);
        return;
    }

    // 拖动预览里的图：按**本次位移**算，不是按"鼠标到哪" ——
    // 后者会在按下的一瞬间把图跳到鼠标位置（那看着像图"弹"了一下）。
    if (m_dragPreview) {
        const int dx = pt.x - m_dragPreviewLast.x;
        const int dy = pt.y - m_dragPreviewLast.y;
        m_dragPreviewLast = pt;
        if (dx != 0 || dy != 0) OnBgPreviewDrag(dx, dy);
        return;
    }

    // 拖动中：直接把 x 喂给那个滑块。注意是**按记录下来的那个 hit**，
    // 不是重新命中测试 —— 拖出滑块范围（甚至拖到窗口外）时仍然要跟着走，
    // 那正是滑块该有的行为。
    if (m_dragSlider != kHitNone) {
        SetSliderFromX(m_dragSlider, pt.x);
        return;
    }

    // 登记一次 TME_LEAVE —— 没有它收不到 WM_MOUSELEAVE，悬停态会一直留着。
    if (!m_tracking) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = m_hWnd;
        if (TrackMouseEvent(&tme)) m_tracking = true;
    }

    const int hit = HitTest(pt);
    // 角手柄用**斜向缩放箭头** —— 光标是唯一的"这里可以拖"的提示，
    // 用普通箭头没人会去试。
    //
    // ★ 配对规则（D-111）：**同一条对角线上的两个角用同一个光标**。
    //
    //   判据是"放大朝外"：
    //     左上角的"外"是 ↖、右下角的是 ↘ -> 两个都要 ↖↘ = IDC_SIZENWSE
    //     右上角的是 ↗、左下角的是 ↙       -> 两个都要 ↗↙ = IDC_SIZENESW
    //
    //   ⚠️ 这里我连着错了两次，值得记下来：
    //     第一次配对了（就是下面这版），用户说"下面两个反了" ——
    //     但那次**图比预览框高，上面两个手柄落在框外看不见**，
    //     他只看到了下面两个，而那两个其实是对的。
    //     我按"下面两个反了"把**四个全换**，于是四个全错。
    //
    //   教训：用户报"某几个不对"时，先确认**他是不是只看到了那几个** ——
    //   可见性是解释"为什么只有一部分不对"的第一候选，
    //   而不是"这两处的逻辑真的不同"。
    HCURSOR cur = LoadCursorW(nullptr, hit == kHitNone ? IDC_ARROW : IDC_HAND);
    if (hit >= kHitBgHandleBase && hit < kHitBgHandleBase + kBgHandleCount) {
        const int corner = hit - kHitBgHandleBase;
        const bool diagNWSE = (corner == 0 || corner == 2);   // 左上 / 右下 = ↖↘
        cur = LoadCursorW(nullptr, diagNWSE ? IDC_SIZENWSE : IDC_SIZENESW);
    }
    SetCursor(cur);
    if (hit != m_hot) { m_hot = hit; Repaint(); }
}

void CLyricusPrefsDlg::OnMouseLeave() {
    m_tracking = false;
    if (m_hot != kHitNone) { m_hot = kHitNone; Repaint(); }
}

void CLyricusPrefsDlg::OnLButtonDown(UINT /*flags*/, CPoint pt) {
    const int hit = HitTest(pt);


    if (hit == kHitNone) return;

    m_active = hit;
    ::SetCapture(m_hWnd);

    // 预览区：开始拖动构图（D-103）
    if (hit == kHitBgPreview && !m_edited.bgImage.empty()) {
        m_dragPreview     = true;
        m_dragPreviewLast = pt;
        EnsureManualFit();
    }

    // 角手柄：开始拖角缩放（D-107）
    if (hit >= kHitBgHandleBase && hit < kHitBgHandleBase + kBgHandleCount) {
        BeginBgHandleDrag(hit, pt, CurrentLayout());
    }

    // 四个滑块都用同一个起点处理：点哪儿跳到哪儿，而不是只响应拖动。
    if (hit == kHitSlider || hit == kHitBgOpacity || hit == kHitBgBlur || hit == kHitBgDim) {
        m_dragSlider = hit;
        SetSliderFromX(hit, pt.x);
    }
    Repaint();
}

void CLyricusPrefsDlg::OnLButtonUp(UINT /*flags*/, CPoint pt) {
    const int hit  = m_active;
    // 松开之前记一下"刚才是不是在拖" —— 拖拽结束时不该再当成一次点击
    // （否则松手会顺带触发按钮动作，滑块拖到一半就把图清了那种）。
    const bool wasDrag = (m_dragSlider != kHitNone) || m_dragPreview ||
                         (m_dragHandle != kHitNone);

    m_active      = kHitNone;
    m_dragSlider  = kHitNone;
    m_dragPreview = false;
    m_dragHandle  = kHitNone;
    if (::GetCapture() == m_hWnd) ::ReleaseCapture();

    if (hit == kHitReset) {
        reset();   // reset() 只改界面，不写存储（见它的说明）
        Repaint();
        return;
    }

    if (hit == kHitFontBtn) {
        PickFont();   // 自己负责写设置 + NotifyChanged
        Repaint();
        return;
    }

    // ---- 外观预设（D-088）----
    // 这几条**立刻生效**，不走"应用 / 取消"：切一套配色就是要马上看到效果。
    // 和色块那种"先在界面上试、点应用才落地"不是一回事。
    if (hit == kHitPresetCombo)  { OnPresetCombo();         Repaint(); return; }
    if (hit == kHitPresetSave)   { OnPresetSaveCurrent();   Repaint(); return; }
    if (hit == kHitPresetDelete) { OnPresetDelete();        Repaint(); return; }
    if (hit == kHitPresetImport) { OnPresetImport();        Repaint(); return; }
    if (hit == kHitPresetExport) { OnPresetExport();        Repaint(); return; }

    // ---- 背景图（D-098）----
    if (hit == kHitBgPick) { OnBgPick(); Repaint(); return; }
    if (hit == kHitBgClear) {
        m_edited.bgImage.clear();
        ClearPanelBackgroundCache();
        NotifyChanged();
        Repaint();
        return;
    }
    if (hit == kHitBgFit) {
        // 循环切换。夹一次是因为配置是文本的，手改可能塞进越界值 ——
        // 不夹的话 (7+1)%4 = 0 会突然跳回第一个，看着像"点了没反应还倒退"。
        int f = m_edited.bgFit;
        if (f < kBgFitMin || f > kBgFitMax) f = kBgFitMin;
        m_edited.bgFit = (f + 1) % (kBgFitMax + 1);
        NotifyChanged();
        Repaint();
        return;
    }

    // ---- 控件配色（D-093）----
    // 基色块的索引从 100 起，和上面那 6 个配色色块（0..5）不重叠，
    // 所以这一段放在它们的判断之前之后都行。
    if (hit == kHitCtrlMode) { OnCtrlModeToggle(); Repaint(); return; }
    if (hit >= kHitCtrlColorBase && hit < kHitCtrlColorBase + kPrefsCtrlColorCount) {
        PickCtrlColor(hit - kHitCtrlColorBase);
        Repaint();
        return;
    }

    if (hit >= 0 && hit < kPrefsColorCount) {
        COLORREF* p = &(m_edited.*(kColorSlots[hit].member));
        if (PickColor(*p)) {
            NotifyChanged();
            Repaint();
        }
        return;
    }

    if (wasDrag) Repaint();
}

// ---------------------------------------------------------------------------
// 外观预设的动作（D-088）
// ---------------------------------------------------------------------------

AppearancePreset CLyricusPrefsDlg::SnapshotAppearance(const std::wstring& name) const {
    AppearancePreset p;
    p.name    = name;
    p.header  = m_edited.header;
    p.current = m_edited.current;
    p.normal  = m_edited.normal;
    p.dim     = m_edited.dim;
    p.warn    = m_edited.warn;
    p.bg      = m_edited.bg;
    p.alpha   = m_edited.alpha;
    // 控件配色（D-093）：模式 + 4 个基色也要进预设，否则"保存当前外观"
    // 会把用户调好的控件色丢掉。
    p.ctrlMode   = m_edited.ctrlMode;
    p.ctrlButton = m_edited.ctrlButton;
    p.ctrlIcon   = m_edited.ctrlIcon;
    p.ctrlSlider = m_edited.ctrlSlider;
    p.ctrlText   = m_edited.ctrlText;
    // 背景图（D-098）：也要进预设，否则"保存当前外观"会把用户挑的图丢掉
    p.bgImage    = m_edited.bgImage;
    p.bgFit      = m_edited.bgFit;
    p.bgBlur     = m_edited.bgBlur;
    p.bgDim      = m_edited.bgDim;
    p.bgOpacity  = m_edited.bgOpacity;
    // 手动构图（D-103）
    p.bgZoomPct    = m_edited.bgZoomPct;
    p.bgOffsetXPct = m_edited.bgOffsetXPct;
    p.bgOffsetYPct = m_edited.bgOffsetYPct;
    p.fontFace = m_editedFontFace;
    // 这两项不在本页上，从配置读当前值
    p.fontPct      = GetLyricDisplayConfig().fontPct;
    p.backdropMode = static_cast<int>(cfg_backdrop_mode.get());
    return p;
}

void CLyricusPrefsDlg::OnPresetCombo() {
    const auto presets = GetAppearancePresets();

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    // id 从 1 起：`TrackPopupMenu` 返回 0 表示"用户点到外面关掉了"。
    const UINT kIdBase = 1;
    const UINT kIdNew  = kIdBase + static_cast<UINT>(presets.size());

    for (size_t i = 0; i < presets.size(); ++i) {
        UINT flags = MF_STRING;
        if (presets[i].name == m_presetName) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, kIdBase + static_cast<UINT>(i), presets[i].name.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdNew, L"另存为新预设");

    const PrefsLayout L = CurrentLayout();
    POINT pt{ L.presetCombo.left, L.presetCombo.bottom };

    // ★ 必须转到**屏幕坐标**再交给 TrackPopupMenu。
    //
    // 布局是按**客户区**算的（和绘制同一套坐标），而 TrackPopupMenu 收的是
    // 屏幕坐标 —— 直接把客户区坐标递过去，菜单会弹到屏幕的另一个位置去
    //（对话框离屏幕原点越远，偏得越离谱）。绘制那边不需要这一步，
    // 所以这个错很容易漏：**同一对坐标，两个 API 要的口径不一样**。
    // ⚠️ 这里必须写 `::` —— 和 MessageBoxW / SetWindowTextW 是同一个坑：
    //    CDialogImpl -> CWindow 有一大批和 Win32 API **同名**的成员函数，
    //    不加 `::` 时编译器优先选成员版。这次参数个数对不上所以报错了，
    //    但**参数个数恰好相同的那些会静默选错**，那才危险。
    ::ClientToScreen(m_hWnd, &pt);

    const UINT cmd = TrackPopupMenu(menu,
                                    TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                    pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 0) return;      // 点到外面

    if (cmd == kIdNew) {
        // 自动起一个不重名的名字。
        //
        // 【为什么不弹输入框】Win32 没有现成的 InputBox，为它动态造一个对话框
        // 的代价远大于收益。名字本来就能靠"导出 → 改 → 导入"来定，
        // 而那条路还顺带支持了分享 —— 所以这里选自动命名。
        for (int n = 2; n < 1000; ++n) {
            wchar_t buf[64];
            swprintf_s(buf, L"预设 %d", n);
            if (FindPreset(presets, buf) == nullptr) { m_presetName = buf; break; }
        }
        SaveAppearancePreset(m_presetName, SnapshotAppearance(m_presetName));
        return;
    }

    const size_t idx = static_cast<size_t>(cmd - kIdBase);
    if (idx >= presets.size()) return;

    m_presetName = presets[idx].name;
    ApplyAppearancePreset(presets[idx]);

    // ★ 应用之后要让**界面上"正在编辑"的那份**跟上，否则色块还画着旧颜色、
    //   而面板已经变了 —— 看起来像"点了没生效"。
    m_edited          = GetPanelAppearance();
    m_applied         = m_edited;
    m_editedFontFace  = GetLyricDisplayConfig().fontFace;
    m_appliedFontFace = m_editedFontFace;
    NotifyChanged();
}

void CLyricusPrefsDlg::OnPresetSaveCurrent() {
    if (m_presetName.empty()) {
        ::MessageBoxW(m_hWnd,
            L"先在左边的下拉里选一套预设。\n\n"
            L"「保存」是**覆盖**选中的那一套；要新建请用下拉菜单里的"
            L"「另存为新预设」。",
            L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }
    SaveAppearancePreset(m_presetName, SnapshotAppearance(m_presetName));
}

void CLyricusPrefsDlg::OnPresetDelete() {
    if (m_presetName.empty()) {
        ::MessageBoxW(m_hWnd, L"先在左边的下拉里选一套预设。",
                    L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool builtin = IsBuiltinPresetName(m_presetName);

    // ★ 删不掉就**如实说明**，不要假装成功。
    //   内置那几套没有落盘，所以对它们必然删不动 —— 但提示里要讲清
    //   "什么情况下才有东西可删"，否则用户会以为这个按钮坏了。
    if (!DeleteAppearancePreset(m_presetName)) {
        wchar_t buf[512];
        swprintf_s(buf,
            L"「%s」是内置预设，而且你没有覆盖过它，所以没有东西可删。\n\n"
            L"如果你改过它并点过「保存」，那时才有一份属于你的副本 —— "
            L"删掉那份就会回到内置的样子。",
            m_presetName.c_str());
        ::MessageBoxW(m_hWnd, buf, L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t buf[256];
    swprintf_s(buf, builtin
        ? L"已删掉你对「%s」的改动，恢复成内置的那一份。"
        : L"已删除预设「%s」。", m_presetName.c_str());
    DebugLog("外观预设：%s", WideToUtf8(buf).c_str());
}

void CLyricusPrefsDlg::OnPresetImport() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hWnd;
    ofn.lpstrFilter = L"Lyricus 预设 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"导入外观预设";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;      // 用户取消

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::MessageBoxW(m_hWnd, L"打不开这个文件。", L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string text;
    char buf[4096];
    DWORD got = 0;
    // 预设就是一行，但别人可能存成了带说明的文本 —— 读到一个上限就够，
    // 避免有人误选了一个几百 MB 的文件把内存吃光。
    constexpr DWORD kMaxRead = 64 * 1024;
    while (text.size() < kMaxRead && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        text.append(buf, got);
    }
    CloseHandle(h);

    AppearancePreset p;
    if (!ImportPreset(text, p)) {
        ::MessageBoxW(m_hWnd,
            L"这个文件里没有可识别的预设。\n\n"
            L"预设文件应该长这样（名字、TAB、然后一串 key=value）：\n"
            L"我的配色\tbg=1C1C1E;current=FFFFFF;alpha=215;fontPct=100;backdrop=4",
            L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }

    // ★ 同名的话先问一声。
    //
    // 【为什么这道确认非有不可】导入是**分享**那条路的主场景 —— 别人发来
    // 一个叫「暗色」的预设，而用户自己多半也有一套叫「暗色」。直接存下去会
    // 静默覆盖他那份，而且**没有任何办法察觉**（下拉里还是同一个名字，
    // 颜色变了也容易以为是自己记错了）。
    // ⚠️ 默认按钮是「否」：这个对话框是**打断**用户的，误按回车不该毁掉东西。
    if (FindPreset(GetAppearancePresets(), p.name) != nullptr) {
        wchar_t ask[512];
        swprintf_s(ask,
            L"已经有一套叫「%s」的预设了。\n\n"
            L"覆盖它？\n\n"
            L"（选「否」会取消这次导入 —— 你可以先把文件里的名字改掉再来。）",
            p.name.c_str());
        if (::MessageBoxW(m_hWnd, ask, L"Lyricus",
                          MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    // 存下来并选中，但**不自动应用** —— 用户可能只是想收着，
    // 不想现在的画面被换掉。
    m_presetName = p.name;
    SaveAppearancePreset(p.name, p);

    wchar_t msg[256];
    swprintf_s(msg, L"已导入「%s」。\n\n它现在是选中状态，但**没有**自动应用 ——"
                    L"想用就再点一次下拉里的它。", p.name.c_str());
    ::MessageBoxW(m_hWnd, msg, L"Lyricus", MB_OK | MB_ICONINFORMATION);
}

void CLyricusPrefsDlg::OnPresetExport() {
    if (m_presetName.empty()) {
        ::MessageBoxW(m_hWnd, L"先在左边的下拉里选一套预设。",
                    L"Lyricus", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto presets = GetAppearancePresets();
    const AppearancePreset* p = FindPreset(presets, m_presetName);
    if (p == nullptr) return;

    wchar_t path[MAX_PATH] = L"";
    // 用预设名做默认文件名，导出多个时不用自己想名字。
    // ⚠️ 名字里的 \ / : 等字符不能进文件名，先换掉。
    std::wstring safe = m_presetName;
    for (wchar_t& c : safe) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?'  || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            c = L'_';
        }
    }
    wcsncpy_s(path, safe.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hWnd;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"导出外观预设";
    ofn.lpstrDefExt = L"txt";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    const std::string out = ExportPreset(*p);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::MessageBoxW(m_hWnd, L"写不了这个文件（权限或路径问题）。",
                    L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(h);

    if (!ok || written != out.size()) {
        ::MessageBoxW(m_hWnd, L"写入没有完成。", L"Lyricus", MB_OK | MB_ICONWARNING);
        return;
    }
    DebugLog("外观预设：已导出「%s」到 %s",
             WideToUtf8(m_presetName).c_str(), WideToUtf8(path).c_str());
}

void CLyricusPrefsDlg::SetSliderFromX(int hit, int x) {
    const PrefsLayout L = CurrentLayout();
    const int dpi   = static_cast<int>(GetDpiForWindowSafe(m_hWnd));
    const int knobR = MulDiv(9, dpi, 96);

    // 每个滑块的：区域、量程、值存在哪
    RECT r{};
    int  lo = 0, hi = 0;
    int* target = nullptr;

    switch (hit) {
    case kHitSlider:
        r = L.slider;          lo = kMinAlpha;     hi = kMaxAlpha;     target = &m_edited.alpha;     break;
    case kHitBgOpacity:
        r = L.bgOpacitySlider; lo = kBgOpacityMin; hi = kBgOpacityMax; target = &m_edited.bgOpacity; break;
    case kHitBgBlur:
        r = L.bgBlurSlider;    lo = kBgBlurMin;    hi = kBgBlurMax;    target = &m_edited.bgBlur;    break;
    case kHitBgDim:
        r = L.bgDimSlider;     lo = kBgDimMin;     hi = kBgDimMax;     target = &m_edited.bgDim;     break;
    default:
        return;
    }
    if (target == nullptr || r.right <= r.left || hi <= lo) return;

    const int left  = r.left + knobR;
    const int right = r.right - knobR;
    if (right <= left) return;

    // 用 long long 作中间量再除 —— 这里要先做 (x - left) 的偏移，
    // 套不进 MulDiv 的形式；而两个 int 相乘在极端 dpi 下有溢出风险。
    int v = lo + static_cast<int>(
                static_cast<long long>(x - left) * (hi - lo) / (right - left));
    if (v < lo) v = lo;
    if (v > hi) v = hi;

    if (v == *target) return;
    *target = v;
    NotifyChanged();
    Repaint();
}

BOOL CLyricusPrefsDlg::OnSetCursor(CWindow, UINT, UINT) {
    // 光标在 OnMouseMove 里按命中目标设过了；这里吃掉默认处理，
    // 免得系统在边框/背景上把它改回箭头造成闪烁。
    return TRUE;
}

UINT CLyricusPrefsDlg::OnGetDlgCode(LPMSG) {
    // 方向键留给不透明度滑块用，别被宿主拿去做页面切换。
    return DLGC_WANTARROWS;
}

void CLyricusPrefsDlg::OnKeyDown(TCHAR key, UINT /*repeat*/, UINT /*flags*/) {
    int delta = 0;
    if (key == VK_LEFT)  delta = -1;
    if (key == VK_RIGHT) delta = +1;
    if (delta == 0) return;

    const int v = ClampAlpha(m_edited.alpha + delta);
    if (v == m_edited.alpha) return;
    m_edited.alpha = v;
    NotifyChanged();
    Repaint();
}

// ---------------------------------------------------------------------------
// 与 preferences_page_instance 的契约
// ---------------------------------------------------------------------------

void CLyricusPrefsDlg::NotifyChanged() {
    if (m_callback.is_valid()) m_callback->on_state_changed();
}

t_uint32 CLyricusPrefsDlg::get_state() {
    t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
    // ★ 字体也要算"改了" —— 它和颜色一样等"应用"才落盘，
    //   不带它的话用户选完字体，"应用"按钮还是灰的。
    if (m_edited != m_applied || m_editedFontFace != m_appliedFontFace)
        state |= preferences_state::changed;
    return state;
}

void CLyricusPrefsDlg::apply() {
    SetPanelAppearance(m_edited);
    m_applied = m_edited;

    // ★ 字体也在这里才落盘（和颜色一致）。
    //   空串是**合法值**（= 跟随宿主），不是"清空错误"。
    SetLyricFontFace(m_editedFontFace);
    m_appliedFontFace = m_editedFontFace;

    DebugLog("首选项页：已应用 曲名=#%02X%02X%02X 当前行=#%02X%02X%02X 普通行=#%02X%02X%02X "
             "暗色=#%02X%02X%02X 警告=#%02X%02X%02X 底色=#%02X%02X%02X alpha=%d",
             GetRValue(m_edited.header),  GetGValue(m_edited.header),  GetBValue(m_edited.header),
             GetRValue(m_edited.current), GetGValue(m_edited.current), GetBValue(m_edited.current),
             GetRValue(m_edited.normal),  GetGValue(m_edited.normal),  GetBValue(m_edited.normal),
             GetRValue(m_edited.dim),     GetGValue(m_edited.dim),     GetBValue(m_edited.dim),
             GetRValue(m_edited.warn),    GetGValue(m_edited.warn),    GetBValue(m_edited.warn),
             GetRValue(m_edited.bg),      GetGValue(m_edited.bg),      GetBValue(m_edited.bg),
             m_edited.alpha);

    // 浮动面板每 250ms 轮询一次外观，这里不主动推它 ——
    // 页面关闭后它自己就会跟上，也就没有"页面销毁后回调到面板"的耦合。
}

void CLyricusPrefsDlg::reset() {
    // reset 只改界面，**不写存储** —— SDK 明确要求这样，
    // 好让用户先看到效果再决定要不要"应用"（preferences_page.h:144）。
    m_edited = PanelAppearance{};

    // ★ 字体回到"跟随宿主"，但同样**只改界面**。
    //
    // 【这里改过一次】从前这一句是直接 `SetLyricFontFace("")`（立刻写存储），
    // 理由是"字体本来就是立即生效的"。但那造出一个当时没想到的坑：
    // **点「恢复默认」再点「取消」= 颜色回来了、字体回不来** ——
    // 因为颜色在 m_edited 里等着应用，而字体已经落盘了。
    // 实测用户就是这么丢掉「方正姚体」的（配置里 lyricus.fontFace 还在，值是空的）。
    // 现在两边同一条路：改内存 -> 点应用才落盘 -> 取消就整页还原。
    m_editedFontFace.clear();

    NotifyChanged();
    Repaint();
}

// ---------------------------------------------------------------------------

bool CLyricusPrefsDlg::PickColor(COLORREF& inOut) {
    // 见声明处的说明：先把自己钉住，防模态期间被释放
    service_ptr_t<preferences_page_instance> self = this;

    // 取色走**自绘的色环取色器**（color_picker.cpp）。
    //
    // 【这一路是怎么走到色环的】
    //   1. 最早用的是 WTL 的 CColorDialog。它**会崩**：2026-09-26 用户报
    //      「preference 里的颜色选项，点击之后会崩溃」，崩溃报告写着
    //          Access violation, write, address 0x20
    //          Crash location: ntdll!RtlEnterCriticalSection  (RCX = 0x18)
    //      —— 在空对象上访问偏移 0x18 的临界区。根因是 WTL 的
    //      CStaticDataInitCriticalSectionLock 在构造函数里直接解引用
    //      ATL::_pAtlModule->m_csStaticDataInitAndTypeInfo，一个空指针检查都没有；
    //      而本组件从没创建过 ATL 模块对象，那个指针一直是空的。
    //   2. 换成纯 Win32 的 ChooseColorW，崩是不崩了。但用户看过之后说
    //      「虽然稍微好点，但我还是想要类似色环的」。
    //   3. 于是有了现在的自绘色环：坐标 <-> 颜色的换算在 color_wheel.cpp
    //      （纯函数、离线可测），窗口与绘制在 color_picker.cpp。
    if (!PromptColorWheel(m_hWnd, inOut, L"选择颜色")) return false;

    // 模态期间页面可能已经被销毁（窗口没了）。self 保证对象还在，
    // 但窗口没了就不该再碰控件。
    if (!::IsWindow(m_hWnd)) return false;
    return true;
}

void CLyricusPrefsDlg::PickFont() {
    // 和 PickColor 一样：模态期间页面可能被关掉、对象被释放
    //（SDK 在 preferences_page.h:133 警告过这件事）。
    service_ptr_t<preferences_page_instance> self = this;

    const LyricDisplayConfig cfg = GetLyricDisplayConfig();

    // 对话框的初始值。用 m_editedFontFace 而不是 cfg —— 后者是**已落盘**的值，
    // 若用户上次选了字体没点应用又来打开，起点该是他看到的那个。
    LOGFONTW lf{};
    if (!m_editedFontFace.empty()) {
        // 用户设过 —— 以它为起点，打开就是当前值
        const std::wstring w = Utf8ToWide(m_editedFontFace.c_str());
        wcsncpy_s(lf.lfFaceName, w.c_str(), _TRUNCATE);
        lf.lfHeight = -MulDiv(12, 96, 72);
    } else {
        // 没设过 —— 拿**系统界面字体**当起点。用户多半只是想微调一下
        // 现在看到的那个字体，从这个起点改最省事。
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            lf = ncm.lfMessageFont;
        } else {
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            lf.lfHeight = -MulDiv(12, 96, 72);
        }
    }

    // ⚠️ 刻意**不用** WTL 的 CFontDialog —— 和 CColorDialog 是同一个坑：
    //    两者都走 CStaticDataInitCriticalSectionLock，而那个锁的构造函数直接
    //    解引用 ATL::_pAtlModule。本组件从没创建过 ATL 模块对象，那指针是空的，
    //    于是 RtlEnterCriticalSection 收到 0x18 —— 正是 2026-09-26 那次崩溃
    //    （failure_00000001.txt，见 D-070）。ChooseFontW 是纯 Win32，
    //    不经过任何 ATL 全局状态，整条路直接没了。
    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = m_hWnd;
    cf.lpLogFont   = &lf;
    cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;

    if (!::ChooseFontW(&cf)) return;   // 用户取消
    if (!::IsWindow(m_hWnd)) return;   // 模态期间页面被关了

    // ★ 只取**字体族**，忽略对话框里挑的字号和粗体。
    //
    // 字号归「字号百分比」那条滑块管，是另一个维度。混在一起的话，
    // 用户挑一次字体就会把辛苦调好的字号一并覆盖掉 —— 而且他多半
    // 根本没注意到自己在字体对话框里也动了字号。
    // ★ 只改**正在编辑**的值，不写存储 —— 和颜色一样，等用户点"应用"。
    //   这样"选了字体又点取消"能真正撤销（从前的立即写盘会把它留下）。
    m_editedFontFace = WideToUtf8(lf.lfFaceName);
    NotifyChanged();

    DebugLog("首选项页：字体 -> 「%s」（待应用）", m_editedFontFace.c_str());
}

// ---------------------------------------------------------------------------

class LyricusPrefsPage : public preferences_page_impl<CLyricusPrefsDlg> {
public:
    const char* get_name() override { return "Lyricus"; }
    GUID        get_guid() override { return guid_prefs_page; }
    GUID        get_parent_guid() override { return preferences_page::guid_display; }
    double      get_sort_priority() override { return 100; }
};

preferences_page_factory_t<LyricusPrefsPage> g_prefs_page_factory;

} // namespace

// ---------------------------------------------------------------------------

namespace lyricus {

void OpenPrefsPage() {
    // show_preferences(guid) = 激活首选项对话框并跳转到这一页（ui.h:117）。
    // ui_control 拿不到时（无 GUI 的场合）什么都不做 —— 菜单项本来也点不到。
    ui_control::ptr ui = ui_control::get();
    if (ui.is_valid()) ui->show_preferences(guid_prefs_page);
}

} // namespace lyricus
