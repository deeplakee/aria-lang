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

#define USING_NANBOXING


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
