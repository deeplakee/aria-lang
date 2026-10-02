#ifndef ARIA_RAWALLOC_HPP
#define ARIA_RAWALLOC_HPP

#include <cstdlib>

#include "common.hpp"

#if defined(ARIA_USE_MIMALLOC)
    #include <mimalloc.h>
#endif

namespace aria::mem {
    // GC 底层字节分配原语：分配、扩容、释放三口必须同族（混用 new/delete 与 malloc 族是 UB）。
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
