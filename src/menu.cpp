#include "stdafx.h"

#include "control_window.h"
#include "config.h"
#include "folder_hint.h"
#include "playback_state.h"
#include "debug_log.h"

#include <commdlg.h>
#include <SDK/playlist.h>       // playlist_manager（退路：播放列表里选中的那首）
#include <SDK/popup_message.h>   // 失败要弹出来，不能只写日志
#include "lyric.h"            // Utf8ToWide
#include <string>

// ---------------------------------------------------------------------------
// 主菜单命令。
//
// 在标准菜单的 "View" 分组下挂一个 "Lyricus" 子菜单，放两条命令：
//   1. 显示 / 隐藏操作面板
//   2. 切换背景材质（无 / Mica / Acrylic / Mica Alt）—— 方便现场对比效果
//
// GUID 段与 config.cpp 保持一致（...4Dxx），便于识别归属。
// ---------------------------------------------------------------------------

namespace {

// 本文件后面新增的代码直接用无限定名（PanelAppearance / GetPanelAppearance 等），
// 它们都在 lyricus 里。已有的 lyricus:: 前缀写法继续有效，不必回头改。
using namespace lyricus;

const GUID guid_menu_group   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x07}};
const GUID guid_cmd_toggle   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x08}};
const GUID guid_cmd_backdrop = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x09}};
const GUID guid_cmd_reset    = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0a}};
const GUID guid_cmd_reload_lyric = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0b}};
const GUID guid_cmd_pick_lyric   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0c}};
const GUID guid_cmd_auto_lyric   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0f}};
// 0x12-0x14：三个「显示设置」快捷调整命令。
// ⚠️ 分配新 GUID 前先 grep 全工程的 `0x4d,0x` ——
//    撞了既不会报错也不会警告，编译链接一路绿灯（已经踩过一次）。
const GUID guid_cmd_font  = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x12}};
const GUID guid_cmd_span  = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x13}};
const GUID guid_cmd_shift = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x14}};
// 0x15：为文件夹指定歌词线索（无标签曲目用）
const GUID guid_cmd_hint = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x15}};

// 歌词时间偏移（0x16-0x18）。见 config.h 的 cfg_lyric_offset_map 和 D-048。
const GUID guid_cmd_off_earlier = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x16}};
const GUID guid_cmd_off_later   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x17}};
const GUID guid_cmd_off_reset   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x18}};

// 每次点半秒。0.5 秒是"听得出来"和"不用点十几次"之间的折中：
// 整首偏移通常是几秒（剪辑版本不同），0.5 秒大概点 4~14 次到位。
constexpr double kOffsetStepSec = 0.5;

// 显示设置用「点一下换下一档」而不是弹子菜单：
// 和已有的「切换背景材质」一个路子，代码少一截，而且改完立刻能在面板上看到效果 ——
// 比在子菜单里先找到当前项再点，反馈直接得多。
// 需要精确值时去 首选项 -> 高级 -> Lyricus 里填，两边读写的是同一份存储。
const int kFontSteps[]  = {80, 100, 125, 150, 200};
const int kSpanSteps[]  = {0, 2, 3, 5, 7, 9};      // 0 = 自适应
const int kShiftSteps[] = {30, 40, 50, 60, 70};    // 当前行的垂直位置 %

// 背景通透度档位（alpha 0-255，255 = 完全不透明）。
//
// 菜单里只给几档确定的值 —— 它本来就只能"点"，让人盲点着找手感不如给档位。
// 想要精确值就去 首选项 → 显示 → Lyricus 拖滑块（那里也有实时预览）。
const int kAlphaSteps[] = {255, 242, 217, 179};    // 100% / 95% / 85% / 70%

int AlphaPercent(int alpha) {
    // 四舍五入到整数百分比
    return (alpha * 100 + 127) / 255;
}

// 找最接近的档位下标。当前值不在表里（用户在首选项里拖过滑块）时，
// 找最近的一档 —— 这样"点一下"的落点是可预期的，而不是跳回第一档。
size_t NearestAlphaStep(int alpha) {
    size_t best = 0;
    int bestDiff = 1 << 30;
    for (size_t i = 0; i < _countof(kAlphaSteps); ++i) {
        const int d = (kAlphaSteps[i] > alpha) ? (kAlphaSteps[i] - alpha) : (alpha - kAlphaSteps[i]);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

template <size_t N>
int NextInCycle(const int (&steps)[N], int current) {
    for (size_t i = 0; i < N; ++i) {
        if (steps[i] == current) return steps[(i + 1) % N];
    }
    // 当前值不在表里 —— 用户在高级首选项里手填过一个表外的值。
    // 这时点一下回到第一档，比"什么都不做"更像是响应了。
    return steps[0];
}

mainmenu_group_popup_factory g_menu_group(
    guid_menu_group,
    mainmenu_groups::view,
    mainmenu_commands::sort_priority_dontcare,
    "Lyricus");

class LyricusMenu : public mainmenu_commands {
public:
    enum {
        cmd_toggle = 0,
        cmd_backdrop,
        cmd_reset,
        cmd_reload_lyric,
        cmd_pick_lyric,
        cmd_auto_lyric,
        cmd_font,
        cmd_span,
        cmd_shift,
        cmd_hint,
        cmd_off_earlier,
        cmd_off_later,
        cmd_off_reset,
        cmd_total
    };

    t_uint32 get_command_count() override {
        return cmd_total;
    }

    GUID get_command(t_uint32 index) override {
        switch (index) {
        case cmd_toggle:       return guid_cmd_toggle;
        case cmd_backdrop:     return guid_cmd_backdrop;
        case cmd_reset:        return guid_cmd_reset;
        case cmd_reload_lyric: return guid_cmd_reload_lyric;
        case cmd_pick_lyric:   return guid_cmd_pick_lyric;
        case cmd_auto_lyric:   return guid_cmd_auto_lyric;
        case cmd_font:         return guid_cmd_font;
        case cmd_span:         return guid_cmd_span;
        case cmd_shift:        return guid_cmd_shift;
        case cmd_hint:         return guid_cmd_hint;
        case cmd_off_earlier:  return guid_cmd_off_earlier;
        case cmd_off_later:    return guid_cmd_off_later;
        case cmd_off_reset:    return guid_cmd_off_reset;
        default: uBugCheck();
        }
    }

    void get_name(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_toggle:       out = "显示 / 隐藏操作面板"; break;
        case cmd_backdrop: {
            // 名字里带上当前通透度，和字号那几条一致 —— 一眼就知道现在是什么状态
            const std::string s = "切换背景通透度（当前 " +
                std::to_string(AlphaPercent(GetPanelAppearance().alpha)) + "%）";
            out = s.c_str();
            break;
        }
        case cmd_reset:        out = "重置面板位置与材质"; break;
        case cmd_reload_lyric: out = "重新加载歌词"; break;
        case cmd_pick_lyric:   out = "选择歌词文件..."; break;
        case cmd_auto_lyric:   out = "恢复自动匹配歌词"; break;

        // 下面三条的名字里带当前值：菜单每次展开都会重新调 get_name，
        // 所以不用自己维护"勾选状态"，用户扫一眼就知道现在是什么档。
        case cmd_font: {
            const std::string s = "调整歌词字号（当前 " +
                std::to_string(lyricus::GetLyricDisplayConfig().fontPct) + "%）";
            out = s.c_str();
            break;
        }
        case cmd_span: {
            const int sp = lyricus::GetLyricDisplayConfig().span;
            const std::string s = "调整歌词显示行数（当前 " +
                (sp > 0 ? std::to_string(sp) + " 行）" : std::string("自适应）"));
            out = s.c_str();
            break;
        }
        case cmd_shift: {
            const std::string s = "调整当前行位置（当前 " +
                std::to_string(lyricus::GetLyricDisplayConfig().currentRatio) + "%）";
            out = s.c_str();
            break;
        }
        case cmd_hint:
            out = "指定歌词搜索线索...";
            break;

        // 偏移那三条也把当前值写进名字 —— 菜单每次展开都会重新调 get_name，
        // 所以不用自己维护勾选状态，用户扫一眼就知道现在是几秒。
        case cmd_off_earlier:
        case cmd_off_later: {
            char buf[64];
            sprintf_s(buf, "%s（当前 %+.1f 秒）",
                      index == cmd_off_earlier ? "歌词提前 0.5 秒" : "歌词延后 0.5 秒",
                      lyricus::PlaybackState::Get().LyricOffsetSec());
            out = buf;
            break;
        }
        case cmd_off_reset:
            out = "复位歌词偏移";
            break;
        default: uBugCheck();
        }
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_toggle:
            out = "显示或隐藏 Lyricus 独立操作面板（放在副屏用，置顶显示）。";
            return true;
        case cmd_backdrop:
            out = "在 100% / 95% / 85% / 70% 四档之间循环，控制面板背后的透出程度。"
                  "想要精确值：首选项 → 显示 → Lyricus，那里还能改配色。"
                  "Mica / Acrylic 不可用 —— 它们与 GDI 绘制不兼容（见 D-009）。";
            return true;
        case cmd_reset:
            out = "把面板位置、尺寸、背景材质恢复成默认值。";
            return true;
        case cmd_reload_lyric:
            out = "重新查找当前曲目的歌词文件（精确 / 去前缀 / 标签 / 模糊多策略）。";
            return true;
        case cmd_pick_lyric:
            out = "手动指定一个歌词文件。该选择会绑定到当前曲目并在重启后保留。";
            return true;
        case cmd_auto_lyric:
            out = "解除手动指定的歌词，回到自动匹配。";
            return true;

        case cmd_font:
            out = "在 80 / 100 / 125 / 150 / 200 % 之间循环。"
                  "这一项对独立面板、DUI 元素、CUI 面板同时生效。";
            return true;
        case cmd_span:
            out = "在 自适应 / 2 / 3 / 5 / 7 / 9 之间循环，指当前行上下各显示几行。"
                  "自适应是按面板可用高度算的 —— 独立面板矮就少几行，DUI 里高就多几行。";
            return true;
        case cmd_shift:
            out = "在 30 / 40 / 50 / 60 / 70 % 之间循环，指当前行落在歌词区的什么高度。"
                  "50% 是正中；嫌歌词偏下就往小调。";
            return true;
        case cmd_hint:
            out = "给当前曲目**所在的文件夹**补一句搜索线索（歌手 / 专辑），专治没打标签的曲目。"
                  "实测「哀歌」不带歌手时正确答案连前 10 都进不去，"
                  "带上「阿良良木健」后第 1 条就是它。对整个文件夹生效，填一次管十几首。";
            return true;

        case cmd_off_earlier:
            out = "把当前曲目的歌词提前半秒 —— 用在「声音已经唱到下一句、面板还停在上一句」"
                  "这种**整首歌**的偏移上。设置按曲目记住，重启后仍然有效。";
            return true;
        case cmd_off_later:
            out = "把当前曲目的歌词延后半秒 —— 用在「面板比声音快」的情况。按曲目记住。";
            return true;
        case cmd_off_reset:
            out = "把当前曲目的歌词偏移清零，回到原始时间轴。";
            return true;
        default:
            return false;
        }
    }

    GUID get_parent() override {
        return guid_menu_group;
    }

    void execute(t_uint32 index, service_ptr_t<service_base> callback) override {
        switch (index) {
        case cmd_toggle:
            lyricus::ControlWindow::Get().Toggle();
            break;

        case cmd_backdrop: {
            // 通透度循环。顺带把背景模式拉回「半透明（自绘）」——
            // 只有分层窗口那条路才吃 alpha，留在「无（不透明）」上会显得"点了没反应"。
            //
            // Mica / Acrylic / Mica Alt 不在这条命令里：D-009 查明 DWM 系统背景材质
            // 与 GDI 绘制不兼容 —— 那些 API 全返回 S_OK，但客户区内容根本不被呈现
            //（表现为面板一片空白）。菜单里不该出现明知不可用的选项。
            PanelAppearance ap = GetPanelAppearance();

            const size_t cur = NearestAlphaStep(ap.alpha);
            const int next = kAlphaSteps[(cur + 1) % _countof(kAlphaSteps)];

            ap.alpha = next;
            SetPanelAppearance(ap);
            cfg_backdrop_mode = static_cast<int>(BackdropMode::Translucent);
            ControlWindow::Get().ApplyBackdrop();

            DebugLog("菜单：背景通透度 -> %d%% (alpha=%d)", AlphaPercent(next), next);
            break;
        }

        case cmd_reset: {
            // 尺寸按系统 DPI 缩放后再写入，否则 200% 缩放下窗口会小得装不下内容
            lyricus::ControlWindow::Get().ResetToDefaults();
            lyricus::DebugLog("菜单：面板已重置");
            break;
        }

        case cmd_reload_lyric: {
            lyricus::PlaybackState::Get().ReloadLyrics();
            lyricus::DebugLog("菜单：重新加载歌词");
            break;
        }

        case cmd_pick_lyric: {
            wchar_t buf[MAX_PATH] = L"";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = GetActiveWindow();
            ofn.lpstrFilter = L"歌词文件 (*.lrc)\0*.lrc\0文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrTitle  = L"选择歌词文件";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

            if (GetOpenFileNameW(&ofn)) {
                lyricus::PlaybackState::Get().LoadLyricFile(buf);   // 记住并绑定到当前曲目
                lyricus::DebugLog("菜单：手动选择歌词文件");
            }
            break;
        }

        case cmd_auto_lyric: {
            lyricus::PlaybackState::Get().ClearManualLyric();
            break;
        }

        // 显示设置这三条**不需要**通知任何窗口：
        // 三种宿主都在自己的 250ms 定时器里轮询设置，下一帧自然就变了。
        case cmd_font: {
            const int cur  = lyricus::GetLyricDisplayConfig().fontPct;
            const int next = NextInCycle(kFontSteps, cur);
            lyricus::SetLyricFontPct(next);
            lyricus::DebugLog("菜单：歌词字号 %d%% -> %d%%", cur, next);
            break;
        }

        case cmd_span: {
            const int cur  = lyricus::GetLyricDisplayConfig().span;
            const int next = NextInCycle(kSpanSteps, cur);
            lyricus::SetLyricSpan(next);
            lyricus::DebugLog("菜单：显示行数 %d -> %d（0 = 自适应）", cur, next);
            break;
        }

        case cmd_shift: {
            const int cur  = lyricus::GetLyricDisplayConfig().currentRatio;
            const int next = NextInCycle(kShiftSteps, cur);
            lyricus::SetLyricCurrentRatio(next);
            lyricus::DebugLog("菜单：当前行位置 %d%% -> %d%%", cur, next);
            break;
        }

        case cmd_hint: {
            auto& st = lyricus::PlaybackState::Get();

            // 先看正在播放的曲目；没有就退到播放列表里**焦点所在**的那一项。
            //
            // 【为什么必须有这条退路】用户 2026-09-24 报「输入窗口现在不显示了」，
            // 日志里是「菜单：没在播放，无法指定线索」，点了两次都是这样。
            // 根因：**播放器重启之后是停止状态**（而守候进程现在会自动重启它），
            // 于是这条命令每次都被 `HasTrack()` 挡在门外。
            // 用户显然是想给列表里选中的那首设线索，而不是"必须先播放点什么"。
            std::wstring path = st.TrackPath();
            if (path.empty()) {
                metadb_handle_ptr item;
                auto pm = playlist_manager::get();
                if (pm.is_valid() && pm->activeplaylist_get_focus_item_handle(item) &&
                    !item.is_empty()) {
                    // metadb 给的是 file:// URL，要转成文件系统路径才解析得了文件夹
                    pfc::string8 native;
                    if (filesystem::g_get_native_path(item->get_path(), native)) {
                        path = lyricus::Utf8ToWide(native.get_ptr());
                    }
                }
            }

            if (path.empty()) {
                // ★ 上一次的教训：**不要静默失败**。
                // 上一版这里只写一行日志就退出，用户看到的是"点了没反应"。
                popup_message::g_show(
                    "请先在播放列表里选中一首曲目，或先播放一首。",
                    "Lyricus —— 指定歌词搜索线索");
                break;
            }

            if (lyricus::PromptFolderHint(path, core_api::get_main_window())) {
                // 线索变了 —— 之前用旧查询算出来的结果（包括"没有歌词"那个
                // 负结果缓存）都不算数了，立刻重查一遍。
                //
                // **不用**再单独调 StartOnlineLookup：ReloadLyrics 自己会把在线
                // 查询排上队，而那个方法是私有的，本来也不该从菜单伸手进去。
                lyricus::DebugLog("菜单：歌词线索已更新，重新查询");
                st.ReloadLyrics();
            }
            break;
        }

        // 歌词偏移这三条**不需要**通知任何窗口：把行号置成 npos 之后，
        // 下一次 RefreshPosition（最多 250ms）必然返回 TickChange::Line，
        // 三个宿主都会立刻重绘 —— 和其他显示设置一样靠轮询。
        case cmd_off_earlier:
            lyricus::PlaybackState::Get().NudgeLyricOffset(+kOffsetStepSec);
            break;
        case cmd_off_later:
            lyricus::PlaybackState::Get().NudgeLyricOffset(-kOffsetStepSec);
            break;
        case cmd_off_reset:
            lyricus::PlaybackState::Get().ResetLyricOffset();
            break;

        default:
            uBugCheck();
        }
    }
};

mainmenu_commands_factory_t<LyricusMenu> g_menu_commands;

} // namespace
