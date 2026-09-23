#include "stdafx.h"

// ---------------------------------------------------------------------------
// Lyricus - foobar2000 歌词显示 + 独立操作面板
//
// M0 里程碑：先让 DLL 能被 foobar2000 正常加载并在组件列表里报出身份。
// 后续里程碑见 docs/plan.md。
// ---------------------------------------------------------------------------

// 组件版本声明。
// 注意：每个 DLL 只允许有一个 DECLARE_COMPONENT_VERSION —— 声明多个会被
// foobar2000 当成版本 0，进而认为组件过期（官方注释明确警告过）。
DECLARE_COMPONENT_VERSION(
    "Lyricus",
    "0.1.0",
    "Lyricus - lyrics display and standalone control panel for foobar2000.\n"
    "独立操作面板（置顶 / Mica / Acrylic）+ 歌词显示面板。"
);

// 防止用户改名 DLL，或在同一进程里重复加载本组件。
// 参数必须与实际输出的文件名完全一致。
VALIDATE_COMPONENT_FILENAME("foo_lyricus.dll");

// 启用 cfg_var 的降级功能（仅当 FOOBAR2000_TARGET_VERSION 由新往旧切换时才相关）。
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
