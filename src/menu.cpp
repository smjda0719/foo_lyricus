#include "stdafx.h"

#include "control_window.h"
#include "config.h"
#include "playback_state.h"
#include "debug_log.h"

#include <commdlg.h>

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

const GUID guid_menu_group   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x07}};
const GUID guid_cmd_toggle   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x08}};
const GUID guid_cmd_backdrop = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x09}};
const GUID guid_cmd_reset    = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0a}};
const GUID guid_cmd_reload_lyric = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0b}};
const GUID guid_cmd_pick_lyric   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0c}};
const GUID guid_cmd_auto_lyric   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0f}};

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
        default: uBugCheck();
        }
    }

    void get_name(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_toggle:       out = "显示 / 隐藏操作面板"; break;
        case cmd_backdrop:     out = "切换背景材质"; break;
        case cmd_reset:        out = "重置面板位置与材质"; break;
        case cmd_reload_lyric: out = "重新加载歌词"; break;
        case cmd_pick_lyric:   out = "选择歌词文件..."; break;
        case cmd_auto_lyric:   out = "恢复自动匹配歌词"; break;
        default: uBugCheck();
        }
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_toggle:
            out = "显示或隐藏 Lyricus 独立操作面板（放在副屏用，置顶显示）。";
            return true;
        case cmd_backdrop:
            out = "在 无(不透明) / Mica / Acrylic(毛玻璃) / Mica Alt / 半透明(自绘) 之间循环切换。";
            return true;
        case cmd_reset:
            out = "把面板位置、尺寸、背景材质恢复成默认值。";
            return true;
        case cmd_reload_lyric:
            out = "重新按当前曲目路径查找同目录同名的 .lrc 文件。";
            return true;
        case cmd_pick_lyric:
            out = "手动指定一个歌词文件。该选择会绑定到当前曲目并在重启后保留。";
            return true;
        case cmd_auto_lyric:
            out = "解除手动指定的歌词，回到「同目录同名 .lrc」的自动匹配。";
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
            const int count = 5;  // BackdropMode 的取值个数
            const int next  = (static_cast<int>(lyricus::cfg_backdrop_mode.get()) + 1) % count;
            lyricus::cfg_backdrop_mode = next;
            lyricus::ControlWindow::Get().ApplyBackdrop();
            lyricus::DebugLog("菜单：切换背景材质 -> %s",
                              lyricus::BackdropModeName(static_cast<lyricus::BackdropMode>(next)));
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

        default:
            uBugCheck();
        }
    }
};

mainmenu_commands_factory_t<LyricusMenu> g_menu_commands;

} // namespace
