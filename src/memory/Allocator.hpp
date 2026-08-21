#ifndef ARIA_ALLOCATOR_HPP
#define ARIA_ALLOCATOR_HPP

#include <type_traits>

#include "common.hpp"

namespace aria {

    // GC 前向声明:供 Buffer/Array/HashTable 的分配器模板参数以 GC 为默认值。默认值只需
    // 类型名已声明即可,完整定义在实例化点(即使用 GC 作分配器的调用方 TU)可见就行,故
    // 容器头不必 include memory/GC.hpp。
    class GC;

    // 元素类型契约:T 必须 trivially-copyable(无非平凡拷贝/移动/析构,可 memcpy / 逐字节赋值)。
    //   供 Array / Buffer / HashTable 约束 K/V/元素:这些容器靠 memcpy 搬迁元素(Buffer::reserve
    //   reallocate / HashTable rehash / Array push 扩容),非 trivially-copyable 的 T 会破坏对象
    //   语义(浅拷贝丢资源 / 漏析构)。封装为具名 concept 而非体里 static_assert:
    //   (1) 令三容器统一走「约束型模板参数」(template<TriviallyCopyable T, ...>),头里每个参数
    //       都带约束,体里不再散落 static_assert;
    //   (2) 具名 concept 在诊断里自文档化(`does not satisfy 'TriviallyCopyable'`),可读性不输
    //       自定义 static_assert 消息,且胜过裸 `requires is_trivially_copyable_v<T>`(后者诊断
    //       只冒一串 trait 模板);
    //   (3) 可作复合约束的积木(如未来 PodValue = TriviallyCopyable<T> && ...);
    //   (4) C++23 标准库无 std::trivially_copyable concept(C++26 或加),故自管一行。
    template<typename T>
    concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

    // Trivial 分配器契约:持「分配 / 重分配 / 释放」三件字节级模板方法的对象。
    //
    //   GC 满足本概念(allocate<T>/reallocate<T>/deallocate<T>);未来可供给 mock 或 arena
    //   分配器做测试 / 隔离。concept 仅以 u8 为代表类型校验成员模板存在且签名相符;实际以
    //   其它 trivially-copyable T 实例化同一成员模板,由分配器保证对所有 T 行为一致
    //   (GC 实现为 ::operator new(count*sizeof(T)),与 T 无关)。
    //
    //   Buffer<T,Alloc> / Array<T,Policy,Alloc> / HashTable<K,V,Hash,Eq,Alloc> 经此概念
    //   与具体分配器解耦:容器头不 include GC.hpp,故不传递地拖入 object/value 树;使用 GC
    //   作分配器的具体类(AriaArray/AriaHashTable/CodeUnit/Movement)自行 include GC.hpp
    //   (显式依赖,而非经容器传递)。这是 enter_frame 同族的依赖反转:低层(容器)依赖抽象
    //   契约(本 concept),高层(GC)实现契约并被注入。
    template<typename A>
    concept TrivialAllocator = requires(A* a, u8* p, usize old_n, usize new_n) {
        { a->template allocate<u8>(new_n) } -> std::same_as<u8*>;
        { a->template reallocate<u8>(p, old_n, new_n) } -> std::same_as<u8*>;
        { a->template deallocate<u8>(p, old_n) } noexcept;
    };

} // namespace aria

#endif // ARIA_ALLOCATOR_HPP
