#pragma once

#include <SDK/cfg_var.h>

// ---------------------------------------------------------------------------
// Lyricus 配置项
//
// 每个 cfg_var 都必须有一个全局唯一的 GUID —— 撞了会读到别的组件的配置。
// 命名空间用 cfg_var_modern（SDK/config/cfg_var.h 里的现代实现，
// 而不是 cfg_var_legacy 那套）。
// ---------------------------------------------------------------------------

namespace lyricus {

// 背景材质模式
enum class BackdropMode : int {
    None        = 0,   // 不使用系统背景（自绘不透明底色，最稳的兜底）
    Mica        = 1,   // 采样桌面壁纸，不透明
    Acrylic     = 2,   // 毛玻璃，透出后面的窗口
    MicaAlt     = 3,   // Mica 的强着色变体
    Translucent = 4,   // 分层窗口整体半透明（自绘，不依赖 DWM 材质）
};

const char* BackdropModeName(BackdropMode mode);

// 独立操作面板：位置与尺寸。-1 表示"从未保存过"，此时用默认值。
extern cfg_var_modern::cfg_int  cfg_panel_x;
extern cfg_var_modern::cfg_int  cfg_panel_y;
extern cfg_var_modern::cfg_int  cfg_panel_w;
extern cfg_var_modern::cfg_int  cfg_panel_h;

// 关闭时是否记住"面板是开着的"，下次启动自动恢复。
extern cfg_var_modern::cfg_bool cfg_panel_visible;

// 背景材质模式（存 BackdropMode 的整数值）
extern cfg_var_modern::cfg_int  cfg_backdrop_mode;

// 手动指定的歌词，**按曲目记住**。
//
// 存储格式：每条一行，「曲目 URL <TAB> 歌词路径」。
// 特例：URL 为空的那条表示「选文件时没在播放」，会被下一首播放的曲目收养。
//
// 之所以做成一张表而不是单个键值对：早期的实现只存一对，一旦播放别的歌
// 就把绑定整个丢弃 —— 结果是「给 A 指定的歌词，换到 B 再换回 A 就忘了」。
extern cfg_var_modern::cfg_string cfg_manual_lyric_map;

} // namespace lyricus
