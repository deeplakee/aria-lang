#ifndef ARIA_SYS_HPP
#define ARIA_SYS_HPP

namespace aria {
#if defined(__linux__)
    #if defined(__ANDROID__)
        #define SYS_ANDROID
    #else
        #define SYS_LINUX
    #endif
#elif defined(__APPLE__)
    #include "TargetConditionals.h"
    #if TARGET_OS_IOS
        #define SYS_IOS
    #elif TARGET_OS_MAC
        #define SYS_MACOS
    #else
        #error "unknown apple platform"
    #endif
#elif defined(_WIN32) || defined(_WIN64)
    #define SYS_WINDOWS
#elif defined(__FreeBSD__)
    #define SYS_FREEBSD
#else
    #error "unknown platform"
#endif

#if defined(SYS_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif

} // namespace aria

#endif // ARIA_SYS_HPP
