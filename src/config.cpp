#include "stdafx.h"
#include "config.h"

// 显式包含，不指望 stdafx.h 顺带带上 —— 单测台那边的 shim 没有那份间接包含，
// 这一点在 lyrics_view.h 上刚吃过一次（C2039，见 D-086）。
#include "lyric.h"       // WideToUtf8 / Utf8ToWide
#include "debug_log.h"

namespace lyricus {

const char* BackdropModeName(BackdropMode mode) {
    switch (mode) {
        case BackdropMode::None:    return "无（不透明）";
        case BackdropMode::Mica:    return "Mica";
        case BackdropMode::Acrylic: return "Acrylic（毛玻璃）";
        case BackdropMode::MicaAlt:     return "Mica Alt";
        case BackdropMode::Translucent: return "半透明（自绘）";
    }
    return "未知";
}

// GUID 统一用 1A7C3E90-2B41-4C58-9D6E-0F1A2B3C4Dxx 段，便于识别归属。
cfg_var_modern::cfg_int  cfg_panel_x        ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x01}}, -1);
cfg_var_modern::cfg_int  cfg_panel_y        ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x02}}, -1);
cfg_var_modern::cfg_int  cfg_panel_w        ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x03}}, 460);
cfg_var_modern::cfg_int  cfg_panel_h        ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x04}}, 150);
cfg_var_modern::cfg_bool cfg_panel_visible  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x05}}, false);
cfg_var_modern::cfg_int  cfg_backdrop_mode  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x06}},
                                             static_cast<int64_t>(BackdropMode::Translucent));

cfg_var_modern::cfg_string cfg_manual_lyric_map({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0d}}, "");

// 按文件夹指定的歌词线索（0x38）。见 folder_hint.h。
cfg_var_modern::cfg_string cfg_folder_hints({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x38}}, "");

// 「当前行位置」基准迁移标记（0x39）。见 config.h 与 D-043。
cfg_var_modern::cfg_int cfg_ratio_base_ver({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x39}}, 0);

// 逐曲目的歌词时间偏移（0x3A）。见 config.h 与 D-048。
cfg_var_modern::cfg_string cfg_lyric_offset_map({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x3a}}, "");

// 在线歌词源的顺序与启用状态（0x3B）。见 config.h 与 source_order.h。
cfg_var_modern::cfg_string cfg_lyric_source_order({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x3b}}, "");

// 用户存的外观预设。**内置那 4 套不在里面** —— 它们是 BuiltinPresets()
// 现生成的，不落盘。这样升级版本时内置预设能跟着更新，
// 而用户改过的同名条目会以"覆盖"的形式存在这张表里。
// GUID 末字节用 0x3c（0x3b 被源顺序占了，其余到 0x3a 为止都在用）。
cfg_var_modern::cfg_string cfg_appearance_presets({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x3c}}, "");

// ---- 浮动面板外观（0x30-0x36）------------------------------------------------
//
// ⚠️ 分配新 GUID 前先 grep 全工程的 `0x4d,0x`（见 D-022）。
// 当前占用：0x01-0x06 / 0x0D / 0x38 / 0x39 = 本文件，0x07-0x0C / 0x0F / 0x12-0x14 = menu.cpp，
//          0x10 = dui_element，0x11 = cui_panel，0x20-0x27 = settings.cpp，
//          0x30-0x36 = 本文件的外观项，0x37 = prefs_page。
cfg_var_modern::cfg_int cfg_app_header ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x30}}, RGB(235,235,240));
cfg_var_modern::cfg_int cfg_app_current({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x31}}, RGB(255,255,255));
cfg_var_modern::cfg_int cfg_app_normal ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x32}}, RGB(172,172,180));
cfg_var_modern::cfg_int cfg_app_dim    ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x33}}, RGB(150,150,158));
cfg_var_modern::cfg_int cfg_app_warn   ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x34}}, RGB(205,165,165));
cfg_var_modern::cfg_int cfg_app_bg     ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x35}}, RGB(28,28,30));
cfg_var_modern::cfg_int cfg_app_alpha  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x36}}, 215);

// 控件配色（D-093）。GUID 末字节 0x40~0x44 —— 0x02~0x0d 和 0x30~0x3c 都已被占用。
cfg_var_modern::cfg_int cfg_app_ctrl_mode  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x40}}, kCtrlAuto);
cfg_var_modern::cfg_int cfg_app_ctrl_btn   ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x41}}, RGB(58,62,72));
cfg_var_modern::cfg_int cfg_app_ctrl_icon  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x42}}, RGB(200,200,208));
cfg_var_modern::cfg_int cfg_app_ctrl_slider({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x43}}, RGB(206,210,220));
cfg_var_modern::cfg_int cfg_app_ctrl_text  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x44}}, RGB(190,190,198));

// 背景图（D-098）。GUID 末字节 0x50~0x54（0x01~0x0d / 0x30~0x3c / 0x40~0x44 都已占用）。
cfg_var_modern::cfg_string cfg_app_bg_image ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x50}}, "");
cfg_var_modern::cfg_int    cfg_app_bg_fit   ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x51}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_blur  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x52}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_dim   ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x53}}, 0);

// 鼠标穿透（D-130）。存 int 而不是 bool —— cfg_var_modern 那套按整数存，
// 用 int 省得为"0/1"再包一层。读回来按 != 0 判。
cfg_var_modern::cfg_int    cfg_app_click_through({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x58}}, 0);

// 锁定显示尺寸（D-132）。和 clickThrough 一样用 int 存 0/1。
cfg_var_modern::cfg_int    cfg_app_bg_locked  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x59}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_lockedw ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5A}}, 0);

// 镜像与旋转（D-133）。
cfg_var_modern::cfg_int    cfg_app_bg_fliph  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5C}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_flipv  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5D}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_rotate ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5E}}, 0);

// 预览框的宽高比 × 100（D-132）。0 = 跟随实际面板尺寸。
//
// ⚠️ 它**不进 PanelAppearance** —— 那是"浮动面板长什么样"的快照，
//    而这个是"首选项页的预览框怎么画"，只有这一个页面用得上。
//    混进去会让每次改预览比例都触发一轮面板重绘。
cfg_var_modern::cfg_int    cfg_prefs_preview_aspect({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5B}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_opacity({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x54}}, 100);
// 手动构图（D-103）。GUID 末字节 0x55~0x57。
cfg_var_modern::cfg_int    cfg_app_bg_zoom  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x55}}, 100);
cfg_var_modern::cfg_int    cfg_app_bg_offx  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x56}}, 0);
cfg_var_modern::cfg_int    cfg_app_bg_offy  ({0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x57}}, 0);

int ClampAlpha(int v) {
    if (v < kMinAlpha) return kMinAlpha;
    if (v > kMaxAlpha) return kMaxAlpha;
    return v;
}

// 通用整数夹取。原先这里只有 ClampAlpha 一个专用版本，加控件配色（D-093）
// 时又需要夹 ctrlMode，就抽了这个 —— 各写一遍迟早会不一致。
int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

PanelAppearance GetPanelAppearance() {
    PanelAppearance a;
    a.header  = static_cast<COLORREF>(cfg_app_header .get());
    a.current = static_cast<COLORREF>(cfg_app_current.get());
    a.normal  = static_cast<COLORREF>(cfg_app_normal .get());
    a.dim     = static_cast<COLORREF>(cfg_app_dim    .get());
    a.warn    = static_cast<COLORREF>(cfg_app_warn   .get());
    a.bg      = static_cast<COLORREF>(cfg_app_bg     .get());
    // 兜底夹取：配置是文本的，手工编辑或跨版本残留都可能塞进越界值
    a.alpha   = ClampAlpha(static_cast<int>(cfg_app_alpha.get()));

    // 控件配色（D-093）。ctrlMode 也夹一下 —— 手改配置写个 7 进来的话，
    // 绘制侧的两分支判断会两个都不成立，控件就整个不画了。
    a.ctrlMode   = ClampInt(static_cast<int>(cfg_app_ctrl_mode.get()), kCtrlMin, kCtrlMax);
    a.ctrlButton = static_cast<COLORREF>(cfg_app_ctrl_btn.get());
    a.ctrlIcon   = static_cast<COLORREF>(cfg_app_ctrl_icon.get());
    a.ctrlSlider = static_cast<COLORREF>(cfg_app_ctrl_slider.get());
    a.ctrlText   = static_cast<COLORREF>(cfg_app_ctrl_text.get());

    // 背景图（D-098）。四个数值都夹一下 —— 手改配置写个 999 进来的话，
    // 模糊会跑到几十秒（半径越大越慢，而盒式的复杂度虽然与半径无关，
    // 迭代次数乘上去仍然是实打实的开销），压暗过头会让图全黑。
    a.bgImage = cfg_app_bg_image.get();
    a.bgFit     = ClampInt(static_cast<int>(cfg_app_bg_fit.get()),     kBgFitMin,     kBgFitMax);
    a.bgBlur    = ClampInt(static_cast<int>(cfg_app_bg_blur.get()),    kBgBlurMin,    kBgBlurMax);
    a.bgDim     = ClampInt(static_cast<int>(cfg_app_bg_dim.get()),     kBgDimMin,     kBgDimMax);
    a.clickThrough = (cfg_app_click_through.get() != 0);
    a.bgLocked  = (cfg_app_bg_locked.get() != 0);
    a.bgFlipH    = (cfg_app_bg_fliph.get() != 0);
    a.bgFlipV    = (cfg_app_bg_flipv.get() != 0);
    a.bgRotate90 = ClampInt(static_cast<int>(cfg_app_bg_rotate.get()), 0, kBgRotateMax);
    a.bgLockedW = ClampInt(static_cast<int>(cfg_app_bg_lockedw.get()), 0, kBgLockedWMax);
    a.bgOpacity = ClampInt(static_cast<int>(cfg_app_bg_opacity.get()), kBgOpacityMin, kBgOpacityMax);

    // 手动构图（D-103）。用 ClampBgManual 而不是逐个 ClampInt ——
    // 三个值是一组语义（"图必须盖住区域"），分开夹容易漏掉某一个，
    // 而漏掉的那个会让画面露出一条底色边。
    {
        BgManual m;
        m.zoomPct    = static_cast<int>(cfg_app_bg_zoom.get());
        m.offsetXPct = static_cast<int>(cfg_app_bg_offx.get());
        m.offsetYPct = static_cast<int>(cfg_app_bg_offy.get());
        const BgManual c = ClampBgManual(m);
        a.bgZoomPct    = c.zoomPct;
        a.bgOffsetXPct = c.offsetXPct;
        a.bgOffsetYPct = c.offsetYPct;
    }
    return a;
}

void SetPanelAppearance(const PanelAppearance& a) {
    cfg_app_header  = static_cast<int64_t>(a.header);
    cfg_app_current = static_cast<int64_t>(a.current);
    cfg_app_normal  = static_cast<int64_t>(a.normal);
    cfg_app_dim     = static_cast<int64_t>(a.dim);
    cfg_app_warn    = static_cast<int64_t>(a.warn);
    cfg_app_bg      = static_cast<int64_t>(a.bg);
    cfg_app_alpha   = ClampAlpha(a.alpha);

    cfg_app_ctrl_mode   = ClampInt(a.ctrlMode, kCtrlMin, kCtrlMax);
    cfg_app_ctrl_btn    = static_cast<int64_t>(a.ctrlButton);
    cfg_app_ctrl_icon   = static_cast<int64_t>(a.ctrlIcon);
    cfg_app_ctrl_slider = static_cast<int64_t>(a.ctrlSlider);
    cfg_app_ctrl_text   = static_cast<int64_t>(a.ctrlText);

    cfg_app_bg_image   = a.bgImage.c_str();
    cfg_app_bg_fit     = ClampInt(a.bgFit,     kBgFitMin,     kBgFitMax);
    cfg_app_bg_blur    = ClampInt(a.bgBlur,    kBgBlurMin,    kBgBlurMax);
    cfg_app_bg_dim     = ClampInt(a.bgDim,     kBgDimMin,     kBgDimMax);
    cfg_app_click_through = a.clickThrough ? 1 : 0;
    cfg_app_bg_locked  = a.bgLocked ? 1 : 0;
    cfg_app_bg_fliph   = a.bgFlipH ? 1 : 0;
    cfg_app_bg_flipv   = a.bgFlipV ? 1 : 0;
    cfg_app_bg_rotate  = ClampInt(a.bgRotate90, 0, kBgRotateMax);
    cfg_app_bg_lockedw = ClampInt(a.bgLockedW, 0, kBgLockedWMax);
    cfg_app_bg_opacity = ClampInt(a.bgOpacity, kBgOpacityMin, kBgOpacityMax);

    // 手动构图（D-103）。同样走 ClampBgManual，让"三点一组"的约束只有一份实现。
    {
        BgManual m;
        m.zoomPct    = a.bgZoomPct;
        m.offsetXPct = a.bgOffsetXPct;
        m.offsetYPct = a.bgOffsetYPct;
        const BgManual c = ClampBgManual(m);
        cfg_app_bg_zoom = c.zoomPct;
        cfg_app_bg_offx = c.offsetXPct;
        cfg_app_bg_offy = c.offsetYPct;
    }
}

// ---------------------------------------------------------------------------
// 外观预设
// ---------------------------------------------------------------------------

namespace {

// 用户存的那部分**原始文本**（不含内置的）。解析交给 preset.cpp。
std::string ReadPresetText() {
    const pfc::string8 raw = cfg_appearance_presets.get();
    return std::string(raw.get_ptr(), raw.length());
}

void WritePresetText(const std::string& text) {
    cfg_appearance_presets = text.c_str();
}

} // namespace

std::vector<AppearancePreset> GetAppearancePresets() {
    std::vector<AppearancePreset> v = BuiltinPresets();

    for (const auto& u : ParsePresets(ReadPresetText())) {
        // 同名 -> 用户那份**覆盖**内置那份。这样他改完「暗色」存下来，
        // 下次切到「暗色」就是自己的版本，而不是要另起一个名字。
        bool replaced = false;
        for (auto& b : v) {
            if (b.name == u.name) { b = u; replaced = true; break; }
        }
        // 用户自己新增的排在**内置的后面**：内置那 4 套位置固定
        //（「默认」永远是第一个），用户存的不该插到它们中间去。
        if (!replaced) v.push_back(u);
    }
    return v;
}

void SaveAppearancePreset(const std::wstring& name, const AppearancePreset& preset) {
    if (name.empty()) return;

    bool changed = false;
    const std::string next = ApplyPresetEdit(ReadPresetText(), name, &preset, &changed);

    // ★ 没变化就不写盘。
    // 这个判断不是省事 —— 重复保存同一个值会让配置无谓地重写一次，
    // 而歌词线索表那边正是漏了它（重复填同一个歌手会白白作废未命中缓存，见 D-078）。
    if (!changed) return;

    WritePresetText(next);
    DebugLog("外观预设：已保存「%s」", WideToUtf8(name).c_str());
}

bool DeleteAppearancePreset(const std::wstring& name) {
    if (name.empty()) return false;

    bool changed = false;
    const std::string next = ApplyPresetEdit(ReadPresetText(), name, nullptr, &changed);

    // ⚠️ 只有**用户存过**的才删得掉 —— 内置那几套没落盘，所以对它们这里
    //    必然是 false。而"删掉自己的覆盖 = 恢复内置默认"恰恰是这个函数
    //    最有用的地方，调用方要能区分这两种结果，所以返回 bool。
    if (!changed) return false;

    WritePresetText(next);
    DebugLog("外观预设：已删除「%s」（若是内置名字，即恢复成内置默认）",
             WideToUtf8(name).c_str());
    return true;
}

bool IsBuiltinPresetName(const std::wstring& name) {
    for (const auto& b : BuiltinPresets()) {
        if (b.name == name) return true;
    }
    return false;
}

} // namespace lyricus
