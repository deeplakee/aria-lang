#ifndef ARIA_ARRAY_HPP
#define ARIA_ARRAY_HPP

#include <ranges>

#include "common.hpp"
#include "memory/Allocator.hpp"
#include "memory/Buffer.hpp"

namespace aria {

    // 基于 Trivial 分配器(默认 GC)的可扩容 trivial 数组,在 Buffer 底座上加逻辑长度。 T 必须 trivially-copyable(Value /
    // OpCode / u8 / i32 等 POD,契约见 Allocator.hpp)。持 Buffer<T,Alloc> buf_(收口分配/重分配/释放)+ usize len_(逻辑长
    // 度,<= cap)。不可拷贝/不可移动 (继承自 Buffer,理由见 Buffer 注)。扩容策略固定:初始 8、2 倍几何增长(见
    // ensure_capacity); 走 memcpy 搬迁,故不适合按内容重定位的容器(HashTable / InternPool rehash,见 Buffer 注)。分配器
    // 经 TrivialAllocator concept 解耦(见 Allocator.hpp);Alloc 默认为 GC,实例化点 (调用方 TU)须令 GC 完整可见。 T 元素
    // 类型(POD) Alloc Trivial 分配器(默认 GC)
    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Array {
        static constexpr usize kInitialCapacity = 8; // 首次分配与几何增长起点

        Buffer<T, Alloc> buf_; // 内存块底座(持 alloc_/data_/cap_,收口分配/重分配/释放)
        usize            len_; // 逻辑长度(<= buf_.capacity())

        // 内部扩容原语:确保容量 >= required_capacity,不足则从当前容量(空态落到
        // kInitialCapacity=8)起 2 倍几何增长,一次 reallocate 到位(不触发 GC)。
        // 供内部路径(push/resize)与公开 reserve 共用此实现,使内部不反向依赖公开接口。
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

        // 禁拷贝/禁移动:buf_ 持 GC 堆分配裸指针,浅 move 会 double-free;资源仅经析构释放,
        // 需转移所有权时用指针/就地构造(如 ObjList 持 AriaArray 成员)。
        Array(const Array&)            = delete;
        Array& operator=(const Array&) = delete;
        Array(Array&&)                 = delete;
        Array& operator=(Array&&)      = delete;

        // 追加一个元素;满时经 ensure_capacity 长一档。Array 不持指向缓冲的派生裸指针,
        // 扩容后无需重定位调用方指针。
        void push(const T& value) {
            if (len_ == buf_.capacity()) {
                ensure_capacity(buf_.capacity() + 1);
            }
            buf_.data()[len_++] = value;
        }

        // 整段追加(append 语义,接在 len_ 之后,不改写已有元素):一次扩容 + 单次 memcpy,
        // 替代逐元素 push 循环。参数收 Span<const T> 泛化源:List(std::vector)/裸数组
        // 皆可隐式转换。空 src 直接返回(size 0 的 memcpy 传 nullptr 属无效参数)。
        void copy_from(Span<const T> src) {
            if (src.empty()) {
                return;
            }
            ensure_capacity(len_ + src.size());
            std::memcpy(buf_.data() + len_, src.data(), src.size() * sizeof(T));
            len_ += src.size();
        }

        // 整段倒序追加(append 语义,接在 len_ 之后,不改写已有元素):源段按逆序落位,即把
        // src 反转后接尾,与 copy_from 同族(源段仍以升序 Span 给出,只是消费序相反),一次
        // 扩容 + 逐元素拷(逆序无法 memcpy)。空 src 直接返回。
        void copy_reversed_from(Span<const T> src) {
            if (src.empty()) {
                return;
            }
            ensure_capacity(len_ + src.size());
            for (const T& element: std::views::reverse(src)) {
                buf_.data()[len_++] = element;
            }
        }

        // 位置插入:在 index 之前插入 value(合法域 [0, size()],== size() 即追加,同 push 语义)。
        // 撑长一格后自尾段右移腾位:一次扩容(可能) + 至多 size()-index 次平凡拷贝。
        void insert(const usize index, const T& value) {
            ASSERT(index <= len_, "Array::insert: index out of range");
            ensure_capacity(len_ + 1);
            ++len_;
            for (usize i = len_ - 1; i > index; --i) {
                buf_.data()[i] = buf_.data()[i - 1];
            }
            buf_.data()[index] = value;
        }

        // 公开预分配提示:确保容量 >= capacity(对标 std::vector::reserve);已分配指针可能
        // 改变(Buffer::reserve 内部 reallocate,故调用方持有的 data() 指针随之失效)。
        void reserve(const usize capacity) { ensure_capacity(capacity); }

        // 改变长度;增长部分用 fill 填充(默认 T{})。注意 Value{} 零填充是 f64 0.0 非 nil,
        // 需要空槽当 nil 的场合显式传 Value::nil_val()。
        void resize(const usize count, T fill = T{}) {
            ensure_capacity(count);
            for (usize i = len_; i < count; ++i) {
                buf_.data()[i] = fill;
            }
            len_ = count;
        }

        // 截断到 new_len(new_len <= 当前长度);不释放容量。
        void truncate(const usize new_len) noexcept {
            ASSERT(new_len <= len_, "Array::truncate: new_len exceeds current size");
            len_ = new_len;
        }

        void clear() noexcept { len_ = 0; }

        void pop() noexcept {
            ASSERT(len_ > 0, "Array::pop on empty");
            --len_;
        }

        // 位置移除:移除 index 处元素(合法域 [0, size())),自 index+1 起段左移补位、长度减一;
        // 至多 size()-index-1 次平凡拷贝,容量不变。
        void remove_at(const usize index) noexcept {
            ASSERT(index < len_, "Array::remove_at: index out of range");
            for (usize i = index + 1; i < len_; ++i) {
                buf_.data()[i - 1] = buf_.data()[i];
            }
            --len_;
        }

        [[nodiscard]]
        T& top() noexcept {
            ASSERT(len_ > 0, "Array::top on empty");
            return buf_.data()[len_ - 1];
        }

        [[nodiscard]]
        const T& top() const noexcept {
            ASSERT(len_ > 0, "Array::top on empty");
            return buf_.data()[len_ - 1];
        }

        [[nodiscard]]
        T& operator[](usize index) noexcept {
            ASSERT(index < len_, "Array index out of range");
            return buf_.data()[index];
        }

        [[nodiscard]]
        const T& operator[](usize index) const noexcept {
            ASSERT(index < len_, "Array index out of range");
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

        // 迭代器:存储连续,直接以裸指针为迭代器(兼容 range-for 与 <algorithm>)。
        // 扩容即失效:push/resize/reserve 可能触发 reallocate 搬迁,届时全部迭代器失位
        // (与 std::vector 同语义);此外 mark-sweep GC 不搬迁块且 allocate/reallocate
        // 永不触发 GC(见 GC 核心不变式),故迭代期间发生对象分配/GC 不影响本缓冲。
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
