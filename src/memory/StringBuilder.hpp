#ifndef ARIA_STRING_BUILDER_HPP
#define ARIA_STRING_BUILDER_HPP

#include <cstring>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

namespace aria {

    // 可增长原始字节缓冲:buffer 经 GC 分配器家族分配(bytes_allocated_ 自动记账),append 直写字节;
    // take_string 把 buffer 零拷贝移交 ObjString(免中间层累积与最终整串拷贝),内容哈希在铸造口
    // 按当时内容一次全算(唯一消费点) -- 增长期间的就地改写(char 访问)不涉任何哈希失效协议。
    // 容量策略:builder 是一次性构建 -> 定型移交,空余空间是纯负资产 -- 总长可预知的消费方应先
    // reserve 精确预分配(零倍增零空余,take 收缩成 no-op);ensure_ 倍增仅供不可预知的流式追加
    // 兜底。GC 纪律:allocate/reallocate 永不触发 GC(GC 核心不变式),本类全程零 GC 点、无需守卫;
    // 唯一 GC 点在 take_string 委托的 new_string 接管重载内部 new_object 顶部,buffer 是 raw 内存不受
    // sweep 影响。take 后空态可复用。
    class StringBuilder {
    public:
        explicit StringBuilder(GC& gc) : gc_{&gc}, data_{nullptr}, size_{0}, cap_{0} {}

        ~StringBuilder() {
            gc_->deallocate<char>(data_, cap_); // 判空安全,空态(含已 take)为零释放
        }

        StringBuilder(const StringBuilder&)            = delete;
        StringBuilder& operator=(const StringBuilder&) = delete;
        StringBuilder(StringBuilder&&)                 = delete;
        StringBuilder& operator=(StringBuilder&&)      = delete;

        void append(const StringView src) noexcept {
            ensure_(size_ + src.size());
            std::memcpy(data_ + size_, src.data(), src.size());
            size_ += src.size();
        }

        void append(const char ch) noexcept {
            ensure_(size_ + 1);
            data_[size_] = ch;
            size_ += 1;
        }

        // 预分配:保证此后追加至总长 needed 不再分配(reallocate 记账契约一致);只扩不缩,已足即空操作。
        void reserve(const usize needed) noexcept {
            if (cap_ >= needed) {
                return;
            }
            data_ = gc_->reallocate<char>(data_, cap_, needed);
            cap_  = needed;
        }

        [[nodiscard]]
        StringView view() const noexcept {
            return StringView{data_, size_};
        }

        // 移交铸串:驻留判定(命中零拷贝返回)与接管/SSO 化收在 new_string 接管重载,buffer 释放责任
        // 随三臂移交;data_/size_/cap_ 取走归空态,本对象可继续 append 复用,三个 take 各自独立
        // 字段、实参求值顺序无关(hash 在其前按内容算出)。
        ObjString* take_string() {
            using namespace util;
            const auto hash = hash_str(StringView{data_, size_});
            return new_string(*gc_, take(data_), take(size_), take(cap_), hash);
        }

    private:
        // 保证后续追加至总长 needed 不再分配:未达则倍增(懒分配首块 kInitialCap)。
        void ensure_(const usize needed) noexcept {
            if (cap_ >= needed) {
                return;
            }
            usize new_cap = cap_ != 0 ? cap_ : kInitialCap;
            while (new_cap < needed) {
                new_cap *= 2;
            }
            data_ = gc_->reallocate<char>(data_, cap_, new_cap);
            cap_  = new_cap;
        }

        static constexpr usize kInitialCap = 64;

        GC*   gc_;
        char* data_;
        usize size_;
        usize cap_;
    };

} // namespace aria

#endif // ARIA_STRING_BUILDER_HPP
