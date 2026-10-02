#ifndef ARIA_ALLOCATOR_HPP
#define ARIA_ALLOCATOR_HPP

#include <type_traits>

#include "common.hpp"

namespace aria {

    // GC 前向声明:完整定义在实例化点(调用方 TU)可见即可,容器头不必 include memory/GC.hpp。
    class GC;

    // 契约理由:容器靠 memcpy 搬迁元素(reallocate / rehash / 扩容),非 trivially-copyable 的 T 会浅拷贝
    // 丢资源 / 漏析构。
    template<typename T>
    concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

    // Trivial 分配器契约:持「分配 / 重分配 / 释放」三件字节级模板方法的对象(GC 满足本概念)。
    //   concept 仅以 u8 为代表类型校验成员模板存在且签名相符,分配器保证对其它 trivially-copyable
    //   T 行为一致。
    template<typename A>
    concept TrivialAllocator = requires(A* a, u8* p, usize old_n, usize new_n) {
        { a->template allocate<u8>(new_n) } -> std::same_as<u8*>;
        { a->template reallocate<u8>(p, old_n, new_n) } -> std::same_as<u8*>;
        { a->template deallocate<u8>(p, old_n) } noexcept;
    };

} // namespace aria

#endif // ARIA_ALLOCATOR_HPP
