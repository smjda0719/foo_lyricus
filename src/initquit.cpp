#include "stdafx.h"

#include "control_window.h"
#include "config.h"
#include "debug_log.h"
#include "version.h"

// ---------------------------------------------------------------------------
// 组件生命周期。
//
// on_init 时如果上次退出前面板是开着的，就自动恢复显示 —— 让它像一个
// 常驻的迷你播放器，而不是每次都要去菜单里点。
// ---------------------------------------------------------------------------

namespace {

// 把「我现在跑的是哪一份 DLL」写进日志。
//
// 【为什么需要】热安装换掉 DLL 之后，foobar2000 组件列表里显示的那个版本号
// 是**编译时写死的一串字符串**，磁盘上的文件换了它也不会变 —— 于是
// "到底装上没有"完全没法确认。2026-09-26 就是据此误判了「好像没安上」，
// 其实 DLL 早就是新的（不过那次也真查出了别的东西，见 D-058）。
//
// 打的是 **DLL 自己的文件写入时间**，不是 __DATE__ / __TIME__：
// 后者是「编译这个 .cpp 的时刻」，增量编译下它可能停在好几天前不动，
// 而能回答"跑的是哪一份"的，只有磁盘上那个文件被写下的时间。
void LogBuildIdentity() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(core_api::get_my_instance(), path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        lyricus::DebugLog("构建标识: 版本=%s（拿不到自身路径，构建时间未知）",
                          LYRICUS_VERSION);
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        lyricus::DebugLog("构建标识: 版本=%s（读自身文件信息失败，构建时间未知）",
                          LYRICUS_VERSION);
        return;
    }

    FILETIME local{};
    SYSTEMTIME st{};
    if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) &&
        FileTimeToSystemTime(&local, &st)) {
        lyricus::DebugLog("构建标识: 版本=%s  DLL 写入于 %04u-%02u-%02u %02u:%02u:%02u"
                          "  大小=%lu 字节",
                          LYRICUS_VERSION,
                          st.wYear, st.wMonth, st.wDay,
                          st.wHour, st.wMinute, st.wSecond,
                          fad.nFileSizeLow);
    } else {
        lyricus::DebugLog("构建标识: 版本=%s  大小=%lu 字节（时间转换失败）",
                          LYRICUS_VERSION, fad.nFileSizeLow);
    }
}

class LyricusInitQuit : public initquit {
public:
    void on_init() override {
        // 第一件事就报身份：这份日志里后面的每一行都属于这个构建，
        // 排查时不必再猜"你看的是哪一版"。
        LogBuildIdentity();

        // 先把老配置换算到新基准，再显示面板 —— 否则第一帧会按老值画，
        // 250ms 后才自己纠正过来，看起来像闪了一下。
        lyricus::MigrateCurrentRatioToPanelBase();

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
