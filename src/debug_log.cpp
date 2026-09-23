#include "stdafx.h"
#include "debug_log.h"

#include <cstdarg>
#include <cstdio>

namespace lyricus {
namespace {

// 日志跟在 DLL 旁边（即 user-components-xxx\foo_lyricus\）。
// 不用 core_api::pathInProfile —— 实测那份路径下文件从未生成，
// 原因未查明；写在 DLL 自己所在目录是确定可写、也确定找得到的。
pfc::string8 ResolveLogPath() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(core_api::get_my_instance(), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return pfc::string8("lyricus-debug.log");   // 兜底：相对路径
    }

    // 手工做宽字符 -> UTF-8，不依赖 pfc 的转换 API 名字
    char utf8[MAX_PATH * 3]{};
    const int written = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                            utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (written <= 0) {
        return pfc::string8("lyricus-debug.log");
    }
    utf8[written] = '\0';

    pfc::string8 path(utf8);
    const char* slash = strrchr(path.get_ptr(), '\\');
    if (slash != nullptr) {
        path.truncate(slash - path.get_ptr() + 1);
    }
    path += "lyricus-debug.log";
    return path;
}

} // namespace

void DebugLog(const char* fmt, ...) {
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    static const pfc::string8 path = ResolveLogPath();

    FILE* f = nullptr;
    if (fopen_s(&f, path.get_ptr(), "a") == 0 && f != nullptr) {
        fprintf(f, "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        fclose(f);
    }

    // 同时进 foobar2000 控制台（View -> Console），方便现场看
    console::printf("Lyricus: %s", body);
}

} // namespace lyricus
