#pragma once

// ---------------------------------------------------------------------------
// 单测用的 SDK/cfg_var.h 替身。
//
// src/config.h 需要 cfg_var_modern 这几个类型才能编译，
// 但搜索算法一个配置项都不读。所以这里只给**能编译的声明**：
// 单测不链接 config.cpp，因此这些函数永远不需要有实现。
//
// 为什么是替身 SDK 头而不是替身 config.h：
// 头文件用引号包含时会**优先在包含者所在目录**找，
// 所以 src/config.h 无论如何都会赢过 -I 路径 —— 替身 config.h 是塞不进去的。
// 而 <SDK/cfg_var.h> 用的是尖括号，-I 能生效。
// 这样单测编译的 config.h 与生产**完全同一份**，只有它下面的 SDK 依赖被换掉。
// ---------------------------------------------------------------------------

#include <cstdint>

namespace cfg_var_modern {

struct cfg_int {
    std::int64_t get() const;
};

struct cfg_bool {
    bool get() const;
};

struct cfg_string {
    const char* get() const;
};

} // namespace cfg_var_modern
