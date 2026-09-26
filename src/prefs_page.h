#pragma once

namespace lyricus {

// 打开本组件的首选项页（Preferences → Display → Lyricus）。
//
// 【为什么要这么一层】菜单项只该说"打开设置"，不该知道那个页面长什么样、
// 用什么 GUID、由哪个类实现 —— 那些都是 prefs_page.cpp 的实现细节。
// 所以这里只暴露一个动作，GUID 留在原处。
//
// 实现走 ui_control::show_preferences(guid)：SDK 说它会"激活首选项对话框
// 并跳转到指定页"（ui.h:117）。用户 2026-09-26 提这个需求的原话是
// 「这个面板其实也应该在 view 里有。那个地方更好找」—— 从
// File → Preferences → Display → Lyricus 一路点进去确实太深了。
void OpenPrefsPage();

} // namespace lyricus
