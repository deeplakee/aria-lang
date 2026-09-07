#ifndef ARIA_FRAMESTACK_HPP
#define ARIA_FRAMESTACK_HPP

#include <memory>
#include <type_traits>
#include "common.hpp"

namespace aria {

    // 面向 trivial 帧（CallFrame）的栈式槽位池。
    //        零开销：acquire = 返回槽引用 + count++，pop = count--，truncate = count = n。
    //        T 为 trivially-copyable/destructible -> 无构造/析构；一次分配永不扩容 -> 指针绝对稳定。
    // T        帧类型（纯 POD：聚合或 = default，无用户构造函数）
    // Capacity 最大帧数（编译期常量）
    template<typename T, usize Capacity>
    class FrameStack {
        static_assert(Capacity > 0, "FrameStack capacity must be > 0");
        static_assert(std::is_trivial_v<T>, "FrameStack capacity must be trivial");
        static_assert(std::is_trivially_copyable_v<T>, "FrameStack T must be trivially copyable");
        static_assert(std::is_trivially_destructible_v<T>, "FrameStack T must be trivially destructible");

        // 一次分配、值初始化（zero-fill，确定性），生命周期内永不扩容。
        UPtr<T[]> storage_;
        usize     count_;

    public:
        FrameStack() : storage_{std::make_unique<T[]>(Capacity)}, count_{0} {}
        ~FrameStack() = default; // T trivially-destructible：无元素析构；unique_ptr 释放缓冲

        FrameStack(const FrameStack&)                = delete;
        FrameStack& operator=(const FrameStack&)     = delete;
        FrameStack(FrameStack&&) noexcept            = default;
        FrameStack& operator=(FrameStack&&) noexcept = default;

        // 占用下一个空闲槽并推进计数。不构造--调用方就地填充字段。
        [[nodiscard]] T& acquire() {
            ASSERT(count_ < Capacity, "FrameStack overflow");
            return storage_[count_++];
        }

        void pop() noexcept {
            ASSERT(count_ > 0, "FrameStack underflow");
            --count_;
        }

        // 任意截断到 n（n <= 当前计数）。供异常 unwind 一步跨越多帧。
        void truncate(usize n) noexcept {
            ASSERT(n <= count_, "FrameStack truncate: n exceeds current size");
            count_ = n;
        }

        void clear() noexcept { count_ = 0; }

        [[nodiscard]] T& operator[](usize i) noexcept {
            ASSERT(i < count_, "FrameStack index out of range");
            return storage_[i];
        }
        [[nodiscard]] const T& operator[](usize i) const noexcept {
            ASSERT(i < count_, "FrameStack index out of range");
            return storage_[i];
        }

        [[nodiscard]] T& top() noexcept {
            ASSERT(count_ > 0, "FrameStack empty");
            return storage_[count_ - 1];
        }
        [[nodiscard]] const T& top() const noexcept {
            ASSERT(count_ > 0, "FrameStack empty");
            return storage_[count_ - 1];
        }

        [[nodiscard]]
        usize size() const noexcept {
            return count_;
        }

        [[nodiscard]]
        static constexpr usize capacity() noexcept {
            return Capacity;
        }

        [[nodiscard]]
        bool empty() const noexcept {
            return count_ == 0;
        }

        [[nodiscard]]
        Span<T> span() noexcept {
            return {storage_.get(), count_};
        }

        [[nodiscard]]
        Span<const T> span() const noexcept {
            return {storage_.get(), count_};
        }
    };
} // namespace aria

#endif // ARIA_FRAMESTACK_HPP
