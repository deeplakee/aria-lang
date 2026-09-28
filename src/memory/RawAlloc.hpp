#ifndef ARIA_RAWALLOC_HPP
#define ARIA_RAWALLOC_HPP

#include <cstdlib>

#include "common.hpp"

#if defined(ARIA_USE_MIMALLOC)
    #include <mimalloc.h>
#endif

namespace aria::mem {
    // GC 底层字节分配原语(后端二选一,分配/扩容/释放必须同族):ARIA_USE_MIMALLOC 走 vendored
    // mimalloc -- MI_OVERRIDE=OFF,仅 GC 层显式调用,不接管进程 malloc(macOS 上静态接管不可用,
    // 评估见 bench/lang/mimalloc-2026-09-27.md);关掉该选项走 std::malloc 家族。
    // 两后端都不用 ::operator new:realloc 原语要求三口同族,new 的块喂 realloc 是 UB;GC 内存
    // 是裸字节 + trivially-copyable 容器契约(Buffer/Array 按 memcpy 重定位),malloc 家族安全,
    // 且 mi_malloc 的保证对齐(16 B,64 位)与 std::malloc 的基本对齐都不低于 nothrow new 的口径。
    inline void* alloc(const usize bytes) noexcept {
#if defined(ARIA_USE_MIMALLOC)
        return mi_malloc(bytes);
#else
        return std::malloc(bytes);
#endif
    }

    inline void* realloc(void* p, const usize bytes) noexcept {
#if defined(ARIA_USE_MIMALLOC)
        return mi_realloc(p, bytes);
#else
        return std::realloc(p, bytes);
#endif
    }

    inline void free(void* p) noexcept {
#if defined(ARIA_USE_MIMALLOC)
        mi_free(p);
#else
        std::free(p);
#endif
    }
} // namespace aria::mem

#endif // ARIA_RAWALLOC_HPP
