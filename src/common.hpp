#ifndef ARIA_COMMON_HPP
#define ARIA_COMMON_HPP

static_assert(sizeof(void*) == 8, "This program requires a 64-bit system.");

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include "sys.hpp"
#include "type.hpp"

namespace aria {

// 值表示选择：默认 NaN-boxing（8B）；定义 ARIA_USE_TAGVALUE 改用 TagValue（tag+union，16B）。
#if !defined(ARIA_USE_TAGVALUE)
    #define USING_NANBOXING
#endif


#ifdef NDEBUG
    #define UNREACHABLE() std::unreachable()
#else
    #define UNREACHABLE()                                                                                            \
        do {                                                                                                         \
            std::fprintf(stderr, "[%s:%d] This code should not be reached in %s()\n", __FILE__, __LINE__, __func__); \
            std::abort();                                                                                            \
        } while (false)
#endif

// 编译期不可达:收口 if constexpr 白名单分派链的末臂,漏臂(白名单外的模板实参)实例化时编译报错。
// abort 不入任何实例化路径,只为给流分析器 [[noreturn]] 事实;message 包括号会触发 C++26 扩展告警。
#define STATIC_UNREACHABLE(message)    \
    do {                               \
        static_assert(false, message); \
        std::abort();                  \
    } while (false)

#ifdef NDEBUG
    #define ASSERT(condition, message) ((void) 0)
#else
    #define ASSERT(condition, message)                                                                              \
        do {                                                                                                        \
            if (!(condition)) {                                                                                     \
                std::fprintf(stderr, "[%s:%d] Assert failed in %s(): %s\n", __FILE__, __LINE__, __func__, message); \
                std::abort();                                                                                       \
            }                                                                                                       \
        } while (false)
#endif

// 禁止内联：热/冷路径分离--把冷路径移出热函数的内联展开，控制热路径代码体积。
#if defined(_MSC_VER)
    #define ARIA_NOINLINE __declspec(noinline)
#else
    #define ARIA_NOINLINE __attribute__((noinline))
#endif

} // namespace aria

#endif // ARIA_COMMON_HPP
