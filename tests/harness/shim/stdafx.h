#pragma once

// ---------------------------------------------------------------------------
// 单测用的 stdafx 替身。
//
// 真实工程里 src/stdafx.h 引 <helpers/foobar2000+atl.h>，把整个 SDK 拖进来。
// 而 lyric_search.cpp 实际上只用到 windows.h 和标准库 ——
// 所以离线单测只需要这么薄薄一层。
//
// 为什么要费这个劲：FindLyricFile() 本质是个**对文件系统的纯函数**，
// 拿真实文件直接调它就能验证全部搜索策略，**不用播放任何音频**。
// 这比"造静音 WAV 夹具再让播放器去播"干净得多，也不再打断用户听歌。
// ---------------------------------------------------------------------------

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SDK 门面替身
//
// 真实工程里这些来自 <helpers/foobar2000+atl.h>。
// online_lyric.cpp 整个文件只用到下面这几个符号，所以替身也就这么点：
//   core_api::get_my_instance()      —— 取 DLL 路径（单测不写缓存目录，用不上）
//   core_api::is_main_thread()       —— 主线程断言
//   core_api::are_services_available()—— 关闭过程中的守卫
//   fb2k::splitTask / inMainThread   —— 后台线程与回主线程
//
// 后两个刻意**什么都不做**：单测只验纯逻辑（JSON 解析、百分号编码、缓存键），
// 不验线程投递。真起线程反而会让单测变得不确定。
// ---------------------------------------------------------------------------

namespace core_api {

inline HINSTANCE get_my_instance() { return nullptr; }
inline bool      is_main_thread() { return false; }
inline bool      are_services_available() { return true; }

} // namespace core_api

namespace fb2k {

template <typename F> void splitTask(F&&) {}
template <typename F> void inMainThread(F&&) {}

} // namespace fb2k

// Release 构建里 PFC_ASSERT 本来就是空操作（pfc-lite.h 的 `#elif !PFC_DEBUG` 分支）
#ifndef PFC_ASSERT
#define PFC_ASSERT(x) ((void)0)
#endif
