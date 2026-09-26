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
