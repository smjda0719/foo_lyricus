#include "stdafx.h"
#include "config.h"

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

int ClampAlpha(int v) {
    if (v < kMinAlpha) return kMinAlpha;
    if (v > kMaxAlpha) return kMaxAlpha;
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
}

} // namespace lyricus
