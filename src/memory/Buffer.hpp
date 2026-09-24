#ifndef ARIA_BUFFER_HPP
#define ARIA_BUFFER_HPP

#include "common.hpp"
#include "memory/Allocator.hpp"

namespace aria {

    // 基于 Trivial 分配器(默认 GC)的 trivial 内存块底座:仅持 {Alloc* alloc_, T* data_, usize cap_}, 收口「分配 / 重分
    // 配 / 释放」三件事(Array 与 ObjMovement 值栈在其上构建)。扩容策略不内置: 调用方按自己的需要算好 new_cap 后调 reserve
    // 。reserve 走分配器 reallocate(memcpy 旧块到新块), 不返回基址差(见 reserve 注释)。T 须 trivially-copyable:按内容
    // 重定位的容器(HashTable / InternPool rehash 要按新容量重算元素位置)不走本类,直接用分配器的 allocate / deallocate
    // 自管 bucket 数组。分配器经 TrivialAllocator concept 解耦(见 Allocator.hpp);Alloc 默认为 GC,实例化点 (调用方 TU)
    // 须令 GC 完整可见。不可拷贝/不可移动:持分配器堆分配裸指针(data_),浅 move 会 double-free。
    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Buffer {
        T*     data_;
        usize  cap_;
        Alloc* alloc_;

    public:
        // 空态:不分配,延迟到首次 reserve 才分配(Array 用此重载)。
        explicit Buffer(Alloc* alloc) noexcept : data_{nullptr}, cap_{0}, alloc_{alloc} {}

        // 分配 initial_cap 个 T(initial_cap > 0;ObjMovement 值栈用此重载,初始定容)。
        Buffer(Alloc* alloc, const usize initial_cap) noexcept :
            data_{alloc->template allocate<T>(initial_cap)}, cap_{initial_cap}, alloc_{alloc} {}

        ~Buffer() {
            if (data_) {
                alloc_->template deallocate<T>(data_, cap_);
            }
        }

        Buffer(const Buffer&)            = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&&)                 = delete;
        Buffer& operator=(Buffer&&)      = delete;

        // 扩容到 new_cap:仅当 new_cap > 当前 cap 才真正 reallocate(memcpy 旧块到新块);reallocate 内部释放旧块、返回新
        // 块基址。本方法**不返回新旧基址差**:旧块在 reallocate 内已释放,新/旧两指针分属不同数组且旧块已不在生存期,对
        // 它们做指针减法(new - old)是 UB([expr.add] p5)。需要重定位派生裸指针的调用方(如 ObjMovement 值栈)须自行以整数维
        // 护偏移:在调本方法**前**把派生指针到旧基址(data())的偏移算成整数(此时旧块存活,指针减法有定义),调**后**用新基
        // 址(data())+ 偏移重建(新块存活,偏移在范围内,有定义)。详见 ObjMovement::grow_stack_。
        void reserve(const usize new_cap) noexcept {
            if (new_cap <= cap_) {
                return;
            }
            data_ = alloc_->template reallocate<T>(data_, cap_, new_cap);
            cap_  = new_cap;
        }

        [[nodiscard]]
        T* data() noexcept {
            return data_;
        }

        [[nodiscard]]
        const T* data() const noexcept {
            return data_;
        }

        [[nodiscard]]
        usize capacity() const noexcept {
            return cap_;
        }
    };

} // namespace aria

#endif // ARIA_BUFFER_HPP
