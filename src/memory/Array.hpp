#ifndef ARIA_ARRAY_HPP
#define ARIA_ARRAY_HPP

#include "common.hpp"
#include "memory/Allocator.hpp"
#include "memory/Buffer.hpp"

namespace aria {

    // 基于 Trivial 分配器(默认 GC)的可扩容 trivial 数组,在 Buffer 底座上加逻辑长度。
    //        T 必须 trivially-copyable(无析构/可 memcpy / 可逐字节赋值):
    //        Value / OpCode / u8 / i32 等 POD。
    //        持 Buffer<T,Alloc> buf_(收口分配/重分配/释放)+ usize len_(逻辑长度,<= cap)。
    //        不可拷贝/不可移动(继承自 Buffer)。
    //
    //        扩容策略固定:初始 8、2 倍几何增长(见 ensure_capacity)。
    //
    //        用途:**顺序**增长的可扩容数组(ObjList 元素 / CodeUnit 字节码与常量池)。
    //        扩容走 Buffer::reserve -> reallocate(memcpy 旧数据到新块),故不适合 HashTable --
    //        HashTable 的 rehash 要按新容量重算每个元素位置(memcpy 会放错),它该直接用
    //        分配器的 allocate/deallocate 自管 bucket 数组,intern 驻留池同理。
    //
    //        GC 自身的 scratch(gray_stack_ / temp_roots_)用 List(std::vector),
    //        不走 Array,不混入 managed heap 计数。
    //
    //        分配器经 TrivialAllocator concept 解耦(见 Allocator.hpp):本头不 include
    //        GC.hpp,故不传递地拖入 object/value 树;Alloc 默认为 GC,实例化点(调用方 TU)
    //        须令 GC 完整可见。
    // T     元素类型(POD)
    // Alloc Trivial 分配器(默认 GC)
    template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
    class Array {
        static constexpr usize kInitialCapacity = 8; // 首次分配与几何增长起点

        Buffer<T, Alloc> buf_; // 内存块底座(持 alloc_/data_/cap_,收口分配/重分配/释放)
        usize            len_; // 逻辑长度(<= buf_.capacity())

        // 内部扩容原语:确保容量 >= n,不足则 2 倍几何增长(从当前容量起翻倍直到 >= n)
        // 一次 reallocate 到位(Buffer::reserve -> reallocate<T>,不触发 GC);n <= 当前容量时
        // 无操作。供内部路径(push/resize)与公开 reserve 共用此实现,使内部不反向依赖公开接口。
        // cap_=0(空态未分配)时落到 kInitialCapacity=8,push 首元素顺带完成首次分配。
        void ensure_capacity(const usize n) noexcept {
            if (n <= buf_.capacity()) {
                return;
            }
            usize new_cap = buf_.capacity() < kInitialCapacity ? kInitialCapacity : buf_.capacity();
            while (new_cap < n) {
                new_cap *= 2;
            }
            buf_.reserve(new_cap);
        }

    public:
        explicit Array(Alloc* alloc) noexcept : buf_{alloc}, len_{0} {}

        ~Array() = default; // buf_ 自释放;len_ 为标量。

        // 禁拷贝/禁移动:buf_ 持 GC 堆分配裸指针,浅 move 会 double-free;资源仅经析构释放,
        // 需转移所有权时用指针/就地构造(如 ObjList 持 AriaArray 成员)。
        Array(const Array&)            = delete;
        Array& operator=(const Array&) = delete;
        Array(Array&&)                 = delete;
        Array& operator=(Array&&)      = delete;

        // 追加一个元素;满栈时经 ensure_capacity 长一档(Buffer::reserve -> reallocate<T>,
        // 不触发 GC)。Array 无指进缓冲的派生裸指针,扩容后无需重定位。
        void push(const T& v) {
            if (len_ == buf_.capacity()) {
                ensure_capacity(buf_.capacity() + 1);
            }
            buf_.data()[len_++] = v;
        }

        // 公开预分配提示:确保容量 >= n(对标 std::vector::reserve)。已分配指针可能改变
        // (Buffer::reserve 内部 reallocate)。薄封装内部 ensure_capacity,使内部路径
        // (push/resize)不反向依赖本公开接口。
        void reserve(const usize n) { ensure_capacity(n); }

        // 改变长度;增长部分用 fill 填充(默认 T{})。注意 Value{} 零填充是 f64 0.0 非 nil,
        // 需要空槽当 nil 的场合显式传 Value::nil_val()。
        void resize(const usize n, T fill = T{}) {
            ensure_capacity(n);
            for (usize i = len_; i < n; ++i) {
                buf_.data()[i] = fill;
            }
            len_ = n;
        }

        // 截断到 n(n <= 当前长度);不释放容量。
        void truncate(const usize n) noexcept {
            ASSERT(n <= len_, "Array::truncate: n exceeds current size");
            len_ = n;
        }

        void clear() noexcept { len_ = 0; }

        void pop() noexcept {
            ASSERT(len_ > 0, "Array::pop on empty");
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
        T& operator[](usize i) noexcept {
            ASSERT(i < len_, "Array index out of range");
            return buf_.data()[i];
        }

        [[nodiscard]]
        const T& operator[](usize i) const noexcept {
            ASSERT(i < len_, "Array index out of range");
            return buf_.data()[i];
        }

        [[nodiscard]]
        T* data() noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        const T* data() const noexcept {
            return buf_.data();
        }

        [[nodiscard]]
        Span<T> span() noexcept {
            return {buf_.data(), len_};
        }

        [[nodiscard]]
        Span<const T> span() const noexcept {
            return {buf_.data(), len_};
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
