#include "stdafx.h"

#include "control_window.h"
#include "config.h"
#include "folder_hint.h"
#include "adjust_dialog.h"
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
const GUID guid_cmd_reset    = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0a}};
const GUID guid_cmd_reload_lyric = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0b}};
const GUID guid_cmd_pick_lyric   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0c}};
const GUID guid_cmd_auto_lyric   = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x0f}};

// ⚠️ 分配新 GUID 前先 grep 全工程的 `0x4d,0x` ——
//    撞了既不会报错也不会警告，编译链接一路绿灯（已经踩过一次）。
//
// 0x09 / 0x12-0x14 **已废弃**（背景通透度 + 三个显示设置快捷档）：
// 它们都被「调节面板」取代了，见下面那段说明。废弃的号不再分配 ——
// 万一有用户把旧命令拖到了工具栏上，至少不会指到别的功能去。

// 0x15：为文件夹指定歌词线索（无标签曲目用）
const GUID guid_cmd_hint = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x15}};

// 「调节面板」（0x16）。见 adjust_dialog.h。
//
// 第一版把这几个量做成了循环档（字号、行位置、通透度）和「点一次提前 0.5 秒」，
// 用户 2026-09-25 连着否掉两次：「这么点太麻烦了」
// 「也把字号，行数，行位置，透明度也这么改」。现在是一条命令开一个
// 五条滑动条的面板，拖动实时生效 —— 那四条旧命令随之删掉。
const GUID guid_cmd_adjust = {0x1a7c3e90,0x2b41,0x4c58,{0x9d,0x6e,0x0f,0x1a,0x2b,0x3c,0x4d,0x16}};

// 【这里原本有四组"点一下换下一档"的档位表】—— 字号 / 行数 / 行位置 / 背景通透度。
//
// 2026-09-25 全部删掉，因为「调节面板」把它们都收进去了：
// 那四个量的共同点是**要一边看面板一边调**，而"点一下换一档"要来回点十几次
// 才找到手感（用户原话：「这么点太麻烦了」）。滑动条一次到位，还能实时预览。
//
// ⚠️ 别再往这里加"循环档"式的显示设置。要加就先问自己：
//    这个量用户是不是要盯着面板调？是的话就该进调节面板。

mainmenu_group_popup_factory g_menu_group(
    guid_menu_group,
    mainmenu_groups::view,
    mainmenu_commands::sort_priority_dontcare,
    "Lyricus");

class LyricusMenu : public mainmenu_commands {
public:
    enum {
        cmd_toggle = 0,
        cmd_reset,
        cmd_reload_lyric,
        cmd_pick_lyric,
        cmd_auto_lyric,
        cmd_hint,
        cmd_adjust,
        cmd_total
    };

    t_uint32 get_command_count() override {
        return cmd_total;
    }

    GUID get_command(t_uint32 index) override {
        switch (index) {
        case cmd_toggle:       return guid_cmd_toggle;
        case cmd_reset:        return guid_cmd_reset;
        case cmd_reload_lyric: return guid_cmd_reload_lyric;
        case cmd_pick_lyric:   return guid_cmd_pick_lyric;
        case cmd_auto_lyric:   return guid_cmd_auto_lyric;
        case cmd_hint:         return guid_cmd_hint;
        case cmd_adjust:       return guid_cmd_adjust;
        default: uBugCheck();
        }
    }

    void get_name(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_toggle:       out = "显示 / 隐藏操作面板"; break;
        case cmd_reset:        out = "重置面板位置与材质"; break;
        case cmd_reload_lyric: out = "重新加载歌词"; break;
        case cmd_pick_lyric:   out = "选择歌词文件..."; break;
        case cmd_auto_lyric:   out = "恢复自动匹配歌词"; break;
        case cmd_hint:
            out = "指定歌词搜索线索...";
            break;

        // 名字里带上关键的两项当前值 —— 菜单每次展开都会重新调 get_name，
        // 不用自己维护勾选状态，扫一眼就知道现在是什么档。
        case cmd_adjust: {
            const lyricus::LyricDisplayConfig c = lyricus::GetLyricDisplayConfig();
            char buf[128];
            sprintf_s(buf, "调节面板...（字号 %d%%  位置 %d%%  偏移 %+.1f 秒）",
                      c.fontPct, c.currentRatio,
                      lyricus::PlaybackState::Get().LyricOffsetSec());
            out = buf;
            break;
        }
        default: uBugCheck();
        }
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_toggle:
            out = "显示或隐藏 Lyricus 独立操作面板（放在副屏用，置顶显示）。";
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

        case cmd_hint:
            out = "给当前曲目**所在的文件夹**补一句搜索线索（歌手 / 专辑），专治没打标签的曲目。"
                  "实测「哀歌」不带歌手时正确答案连前 10 都进不去，"
                  "带上「阿良良木健」后第 1 条就是它。对整个文件夹生效，填一次管十几首。";
            return true;

        case cmd_adjust:
            out = "一个面板调五项：歌词偏移 / 字号 / 显示行数 / 当前行位置 / 面板不透明度。"
                  "**拖动时实时生效**，看着调，满意了点确定；取消会把五项一起还原。\n"
                  "歌词偏移只对当前曲目生效并记住 —— 用在「声音已经唱到下一句、"
                  "面板还停在上一句」那种整首歌的偏移上（多半是因为在线歌词来自"
                  "另一个剪辑版本）。其余四项是全局的。";
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

        // 【这里原本还有三条"点一下换下一档"的显示设置命令】
        //（歌词字号 / 显示行数 / 当前行位置）—— 2026-09-25 删掉，
        // 连同背景通透度那条一起收进了「调节面板」。
        // 理由见文件上方那段说明：这几个量要盯着面板调，循环档点十几次才找得到手感。

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

        // 拖动时实时生效、确定才落盘，所以这里不需要通知任何窗口 ——
        // 面板下一拍（最多 250ms）自己就跟着变了。
        case cmd_adjust:
            lyricus::PromptAdjustPanel(core_api::get_main_window());
            break;

        default:
            uBugCheck();
        }
    }
};

mainmenu_commands_factory_t<LyricusMenu> g_menu_commands;

} // namespace
