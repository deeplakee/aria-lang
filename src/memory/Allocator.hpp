#ifndef ARIA_ALLOCATOR_HPP
#define ARIA_ALLOCATOR_HPP

#include <type_traits>

#include "common.hpp"

namespace aria {

    // GC 前向声明:供 Buffer/Array/HashTable 的分配器模板参数以 GC 为默认值(完整定义在实例化点
    // 即使用 GC 作分配器的调用方 TU 可见就行,故容器头不必 include memory/GC.hpp)。
    class GC;

    // 元素类型契约:T 必须 trivially-copyable--这些容器靠 memcpy 搬迁元素(reallocate / rehash /
    //   扩容),非 trivially-copyable 的 T 会破坏对象语义(浅拷贝丢资源 / 漏析构)。
    template<typename T>
    concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

    // Trivial 分配器契约:持「分配 / 重分配 / 释放」三件字节级模板方法的对象(GC 满足本概念)。
    //   concept 仅以 u8 为代表类型校验成员模板存在且签名相符;实际以其它 trivially-copyable T
    //   实例化同一成员模板,由分配器保证对所有 T 行为一致。
    //   容器经此概念与具体分配器解耦:容器头不 include GC.hpp;使用 GC 作分配器的具体类自行
    //   include GC.hpp(显式依赖,而非经容器传递)。
    template<typename A>
    concept TrivialAllocator = requires(A* a, u8* p, usize old_n, usize new_n) {
        { a->template allocate<u8>(new_n) } -> std::same_as<u8*>;
        { a->template reallocate<u8>(p, old_n, new_n) } -> std::same_as<u8*>;
        { a->template deallocate<u8>(p, old_n) } noexcept;
    };

} // namespace aria

#endif // ARIA_ALLOCATOR_HPP
