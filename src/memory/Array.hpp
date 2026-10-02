#ifndef ARIA_ARRAY_HPP
#define ARIA_ARRAY_HPP

#include <ranges>

#include "common.hpp"
#include "memory/Allocator.hpp"
#include "memory/Buffer.hpp"

namespace aria {

    // 基于 Buffer 的可扩容 trivial 数组:Buffer 加逻辑长度(len_ <= cap),2 倍几何增长,memcpy 搬迁;
    // 不可拷贝/不可移动。
    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Array {
        static constexpr usize kInitialCapacity = 8; // 首次分配与几何增长起点

        Buffer<T, Alloc> buf_;
        usize            len_; // 逻辑长度(<= buf_.capacity())

        // 内部扩容原语:确保容量 >= required_capacity,不足则从当前容量(空态落到 kInitialCapacity=8)起 2 倍
        // 几何增长,一次 reallocate 到位(永不触发 GC)。push/resize 与公开 reserve 共用,内部不反向依赖公开接口。
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

        // 追加一个元素;满时经 ensure_capacity 长一档。
        void push(const T& value) {
            if (len_ == buf_.capacity()) {
                ensure_capacity(buf_.capacity() + 1);
            }
            buf_.data()[len_++] = value;
        }

        // 整段追加(接在 len_ 之后,不改写已有元素):一次扩容 + 单次 memcpy。空 src 直接返回
        // (size 0 的 memcpy 传 nullptr 属无效参数)。
        void copy_from(Span<const T> src) {
            if (src.empty()) {
                return;
            }
            ensure_capacity(len_ + src.size());
            std::memcpy(buf_.data() + len_, src.data(), src.size() * sizeof(T));
            len_ += src.size();
        }

        // 整段倒序追加(接在 len_ 之后,不改写已有元素):源段按升序给出,消费时逆序落位,逐元素拷
        // (逆序无法 memcpy)。空 src 直接返回。
        void copy_reversed_from(Span<const T> src) {
            if (src.empty()) {
                return;
            }
            ensure_capacity(len_ + src.size());
            for (const T& element: std::views::reverse(src)) {
                buf_.data()[len_++] = element;
            }
        }

        // 位置插入:在 index 之前插入 value(合法域 [0, size()],== size() 即追加)。
        void insert(const usize index, const T& value) {
            ASSERT(index <= len_, "index out of range");
            ensure_capacity(len_ + 1);
            ++len_;
            for (usize i = len_ - 1; i > index; --i) {
                buf_.data()[i] = buf_.data()[i - 1];
            }
            buf_.data()[index] = value;
        }

        // 公开预分配提示:确保容量 >= capacity(对标 std::vector::reserve);data() 指针可能随 reallocate 失效。
        void reserve(const usize capacity) { ensure_capacity(capacity); }

        // 改变长度;增长部分用 fill 填充(默认 T{})。T = Value 时零填充语义随值表示而变:默认 NanBoxing 下
        // Value{} 是 f64 0.0 非 nil,需要空槽当 nil 的场合显式传 Value::nil_val()。
        void resize(const usize count, T fill = T{}) {
            ensure_capacity(count);
            for (usize i = len_; i < count; ++i) {
                buf_.data()[i] = fill;
            }
            len_ = count;
        }

        // 截断到 new_len(new_len <= 当前长度);不释放容量。
        void truncate(const usize new_len) noexcept {
            ASSERT(new_len <= len_, "new_len exceeds current size");
            len_ = new_len;
        }

        void clear() noexcept { len_ = 0; }

        void pop() noexcept {
            ASSERT(len_ > 0, "empty array");
            --len_;
        }

        // 位置移除:移除 index 处元素(合法域 [0, size())),段左移补位,容量不变。
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

        // 迭代器:存储连续,直接以裸指针为迭代器。扩容即失效(push/resize/reserve 可能触发 reallocate 搬迁,
        // 同 std::vector);mark-sweep GC 不搬迁块且 allocate/reallocate 永不触发 GC,迭代期间的分配/GC 不影响本缓冲。
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
