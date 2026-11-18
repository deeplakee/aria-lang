#ifndef ARIA_ARRAY_HPP
#define ARIA_ARRAY_HPP

#include <ranges>

#include "common.hpp"
#include "memory/Allocator.hpp"
#include "memory/Buffer.hpp"

namespace aria {

    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Array {
        static constexpr usize kInitialCapacity = 8; // 首次分配与几何增长起点

        Buffer<T, Alloc> buf_;
        usize            len_; // 逻辑长度(<= buf_.capacity())

        void ensure_capacity(const usize required_capacity) noexcept {
            if (required_capacity <= buf_.capacity()) {
                return;
            }
            usize new_cap = buf_.capacity() < kInitialCapacity ? kInitialCapacity : buf_.capacity();
            while (new_cap < required_capacity) {
                new_cap *= 2;
            }
            buf_.reserve(new_cap);
        }

    public:
        explicit Array(Alloc* alloc) noexcept : buf_{alloc}, len_{0} {}

        ~Array() = default;

        Array(const Array&)            = delete;
        Array& operator=(const Array&) = delete;
        Array(Array&&)                 = delete;
        Array& operator=(Array&&)      = delete;

        void push(const T& value) {
            if (len_ == buf_.capacity()) {
                ensure_capacity(buf_.capacity() + 1);
            }
            buf_.data()[len_++] = value;
        }

        // src 不得因本次扩容而失效；空段不调用 memcpy。
        void copy_from(Span<const T> src) {
            if (src.empty()) {
                return;
            }
            ensure_capacity(len_ + src.size());
            std::memcpy(buf_.data() + len_, src.data(), src.size() * sizeof(T));
            len_ += src.size();
        }

        void copy_reversed_from(Span<const T> src) {
            if (src.empty()) {
                return;
            }
            ensure_capacity(len_ + src.size());
            for (const T& element: std::views::reverse(src)) {
                buf_.data()[len_++] = element;
            }
        }

        void insert(const usize index, const T& value) {
            ASSERT(index <= len_, "index out of range");
            ensure_capacity(len_ + 1);
            ++len_;
            for (usize i = len_ - 1; i > index; --i) {
                buf_.data()[i] = buf_.data()[i - 1];
            }
            buf_.data()[index] = value;
        }

        void reserve(const usize capacity) { ensure_capacity(capacity); }

        // 新增元素以 fill 填充；Value{} 在 NaN-boxing 下是 0.0，需要 nil 时须显式传入。
        void resize(const usize count, T fill = T{}) {
            ensure_capacity(count);
            for (usize i = len_; i < count; ++i) {
                buf_.data()[i] = fill;
            }
            len_ = count;
        }

        void truncate(const usize new_len) noexcept {
            ASSERT(new_len <= len_, "new_len exceeds current size");
            len_ = new_len;
        }

        void clear() noexcept { len_ = 0; }

        void pop() noexcept {
            ASSERT(len_ > 0, "empty array");
            --len_;
        }

        void remove_at(const usize index) noexcept {
            ASSERT(index < len_, "index out of range");
            for (usize i = index + 1; i < len_; ++i) {
                buf_.data()[i - 1] = buf_.data()[i];
            }
            --len_;
        }

        [[nodiscard]]
        T& top() noexcept {
            ASSERT(len_ > 0, "empty array");
            return buf_.data()[len_ - 1];
        }

        [[nodiscard]]
        const T& top() const noexcept {
            ASSERT(len_ > 0, "empty array");
            return buf_.data()[len_ - 1];
        }

        [[nodiscard]]
        T& operator[](usize index) noexcept {
            ASSERT(index < len_, "index out of range");
            return buf_.data()[index];
        }

        [[nodiscard]]
        const T& operator[](usize index) const noexcept {
            ASSERT(index < len_, "index out of range");
            return buf_.data()[index];
        }

        [[nodiscard]]
        T* data() noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        const T* data() const noexcept {
            return buf_.data();
        }

        // 扩容使指针与迭代器失效；mark-sweep GC 本身不搬迁此缓冲。
        [[nodiscard]]
        T* begin() noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        T* end() noexcept {
            return buf_.data() + len_;
        }

        [[nodiscard]]
        const T* begin() const noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        const T* end() const noexcept {
            return buf_.data() + len_;
        }

        [[nodiscard]]
        const T* cbegin() const noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        const T* cend() const noexcept {
            return buf_.data() + len_;
        }

        [[nodiscard]]
        usize size() const noexcept {
            return len_;
        }

        [[nodiscard]]
        usize capacity() const noexcept {
            return buf_.capacity();
        }

        [[nodiscard]]
        bool empty() const noexcept {
            return len_ == 0;
        }
    };

} // namespace aria

#endif // ARIA_ARRAY_HPP
