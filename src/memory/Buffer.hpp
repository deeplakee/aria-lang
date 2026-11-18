#ifndef ARIA_BUFFER_HPP
#define ARIA_BUFFER_HPP

#include "common.hpp"
#include "memory/Allocator.hpp"

namespace aria {

    // 管理平凡类型内存块；容量策略由调用方决定。
    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Buffer {
        T*     data_;
        usize  cap_;
        Alloc* alloc_;

    public:
        explicit Buffer(Alloc* alloc) noexcept : data_{nullptr}, cap_{0}, alloc_{alloc} {}

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

        // 返回后旧基址视为失效；派生指针须在调用前转为偏移，调用后以新基址重建。
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
