#ifndef ARIA_BUFFER_HPP
#define ARIA_BUFFER_HPP

#include "common.hpp"
#include "memory/Allocator.hpp"

namespace aria {

    // Trivial 内存块底座:仅持 {Alloc* alloc_, T* data_, usize cap_},收口「分配 / 重分配 / 释放」,扩容策略不内置
    // (调用方算好 new_cap 调 reserve)。不可拷贝/不可移动:持堆分配裸指针(data_),浅 move 会 double-free。
    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Buffer {
        T*     data_;
        usize  cap_;
        Alloc* alloc_;

    public:
        // 空态:不分配,延迟到首次 reserve 才分配。
        explicit Buffer(Alloc* alloc) noexcept : data_{nullptr}, cap_{0}, alloc_{alloc} {}

        // 分配 initial_cap 个 T(initial_cap > 0)。
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

        // 扩容到 new_cap(仅 new_cap > cap 才 reallocate,可能原地扩展也可能搬迁)。旧基址在返回后一律失效、不
        // 返回基址差;持派生裸指针的调用方须调前把到旧基址的偏移记成整数、调后以新基址加偏移重建(原址即刷新)。
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
