#ifndef ARIA_STRING_BUILDER_HPP
#define ARIA_STRING_BUILDER_HPP

#include <cstring>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

namespace aria {

    // 可增长原始字节缓冲:buffer 经 GC 分配器家族分配(bytes_allocated_ 自动记账),append 直写字节;
    // take_string 把 buffer 零拷贝移交 ObjString(免中间层累积与最终整串拷贝),内容哈希在铸造口
    // 按当时内容一次全算(唯一消费点) -- 增长期间的就地改写(迭代器)不涉任何哈希失效协议。
    // 容量策略:builder 是一次性构建 -> 定型移交,空余空间是纯负资产 -- 扩容一律精确按需(无倍增
    // 无空余;超容 append 逐次 realloc 搬迁一次旧内容,碎片化流式追加须先量后填,如 join 预扫),
    // 多发 append 的总长可预知消费方先 reserve 一次定容免逐段重配,单发 append 自身即精确按需
    // 定容免 reserve。分配口径恒含终态 '\0' 位:append 交付后 data_[size_] 恒为 '\0',定容路径的
    // take 长臂即同尺寸 realloc(零拷贝零新分配)。
    // GC 纪律:allocate/reallocate 永不触发 GC(GC 核心不变式),本类全程零 GC 点、无需守卫;唯一
    // GC 点在 take_string 委托的 new_string 接管重载内部 new_object 顶部,buffer 是 raw 内存不受
    // sweep 影响。take 后空态可复用。
    class StringBuilder {
    public:
        explicit StringBuilder(GC& gc) : gc_{&gc}, data_{nullptr}, size_{0}, cap_{0} {}

        // 带种串构造(等价 gc 构造 + append(src) 一体):单发 append 形态的消费方直接用,免 reserve。
        StringBuilder(GC& gc, StringView src) : StringBuilder{gc} { append(src); }

        ~StringBuilder() {
            gc_->deallocate<char>(data_, cap_); // 判空安全,空态(含已 take)为零释放
        }

        StringBuilder(const StringBuilder&)            = delete;
        StringBuilder& operator=(const StringBuilder&) = delete;
        StringBuilder(StringBuilder&&)                 = delete;
        StringBuilder& operator=(StringBuilder&&)      = delete;

        void append(const StringView src) noexcept {
            if (src.empty()) {
                return;
            }
            reserve(size_ + src.size());
            std::memcpy(data_ + size_, src.data(), src.size());
            size_ += src.size();
            data_[size_] = '\0';
        }

        // 唯一扩容路径(append 的隐式扩容走同口):确保此后追加至总长 needed 不再分配(分配口径含
        // 终态 '\0' 位,精确按需、无倍增无空余);只扩不缩,已足即空操作。
        void reserve(const usize needed) noexcept {
            if (cap_ >= needed + 1) {
                return;
            }
            data_ = gc_->reallocate<char>(data_, cap_, needed + 1);
            cap_  = needed + 1;
        }

        [[nodiscard]]
        StringView view() const noexcept {
            return StringView{data_, size_};
        }

        [[nodiscard]]
        usize size() const noexcept {
            return size_;
        }

        // 裸指针可变迭代器:range-for 与 STL 算法直用(空态 begin == end == nullptr);const 读经 view()。
        char* begin() noexcept { return data_; }

        char* end() noexcept { return data_ + size_; }

        // 移交铸串:驻留判定(命中零拷贝返回)与接管/SSO 化收在 new_string 接管重载,buffer 释放
        // 责任随三臂移交;data_/size_/cap_ 取走归空态,本对象可继续 append 复用,三个 take 各自
        // 独立字段、实参求值顺序无关(串哈希按当时内容算出)。调用方已持 hash_str(内容)终态时
        // 用 hash 重载,免重算。
        ObjString* take_string() { return take_string(util::hash_str(view())); }

        ObjString* take_string(const u32 hash) {
            return new_string(*gc_, util::take(data_), util::take(size_), util::take(cap_), hash);
        }

    private:
        GC*   gc_;
        char* data_;
        usize size_;
        usize cap_;
    };

} // namespace aria

#endif // ARIA_STRING_BUILDER_HPP
