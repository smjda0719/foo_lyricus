#include "stdafx.h"

#include "control_window.h"
#include "config.h"

// ---------------------------------------------------------------------------
// 组件生命周期。
//
// on_init 时如果上次退出前面板是开着的，就自动恢复显示 —— 让它像一个
// 常驻的迷你播放器，而不是每次都要去菜单里点。
// ---------------------------------------------------------------------------

namespace {

class LyricusInitQuit : public initquit {
public:
    void on_init() override {
        if (lyricus::cfg_panel_visible.get()) {
            lyricus::ControlWindow::Get().Show();
        }
    }

    void on_quit() override {
        lyricus::ControlWindow::Get().Shutdown();
    }
};

initquit_factory_t<LyricusInitQuit> g_initquit;

} // namespace
