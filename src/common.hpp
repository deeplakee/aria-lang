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
// 宏由根 CMakeLists.txt 的 option(ARIA_USE_TAGVALUE) 注入（cmake -DARIA_USE_TAGVALUE=ON，
// 供 TagValue 路径全量构建 + ctest 验证），亦可编译命令行手工 -D；两实现 API 一致
// （见 value/Value.hpp 的 #ifdef 分派），仅 sizeof 与 Value{} 零填充语义不同。
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

} // namespace aria

#endif // ARIA_COMMON_HPP
