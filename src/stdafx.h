#pragma once

// Lyricus - foobar2000 歌词与操作面板组件
//
// 统一预编译头。与官方 foo_sample 保持一致，直接拉 SDK 的 "全量 + ATL" 入口：
//   helpers/foobar2000+atl.h
//     -> helpers/foobar2000-lite+atl.h   (ATL / WTL / pfc)
//     -> SDK/foobar2000-all.h            (完整 foobar2000 SDK 接口)
//
// 因此工程的包含目录必须同时有：
//   3rdparty/foobar2000   (让 <helpers/...> 与 <SDK/...> 可解析)
//   3rdparty              (让 <pfc/pfc.h> 可解析)
//   3rdparty/WTL/Include  (让 <atlapp.h> 等 WTL 头可解析 —— SDK 不自带 WTL)

#ifdef __cplusplus
#include <helpers/foobar2000+atl.h>
#endif
