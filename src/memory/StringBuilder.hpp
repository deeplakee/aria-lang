#ifndef ARIA_STRING_BUILDER_HPP
#define ARIA_STRING_BUILDER_HPP

#include <cstring>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "util/util.hpp"

namespace aria {

    // GC 记账的字节缓冲，容量按需精确增长；分段追加宜先预留总长。
    // 仅 take_string 可能触发 GC；移交后恢复为空态，可复用。
    class StringBuilder {
    public:
        explicit StringBuilder(GC& gc) : gc_{&gc}, data_{nullptr}, size_{0}, cap_{0} {}

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

        // needed 不含末尾 NUL，内部额外保留一字节。
        void reserve(const usize needed) noexcept {
            if (cap_ >= needed + 1) {
                return;
            }
            data_ = gc_->reallocate<char>(data_, cap_, needed + 1);
            cap_  = needed + 1;
        }

        // 先 reserve 并直写内容，再提交长度和末尾 NUL；不初始化新增字节。
        void resize(const usize size) noexcept {
            ASSERT(cap_ >= size + 1, "resize beyond reserved capacity");
            size_        = size;
            data_[size_] = '\0';
        }

        [[nodiscard]]
        StringView view() const noexcept {
            return StringView{data_, size_};
        }

        [[nodiscard]]
        usize size() const noexcept {
            return size_;
        }

        char* begin() noexcept { return data_; }

        char* end() noexcept { return data_ + size_; }

        // 缓冲释放责任移交给 new_string；hash 重载要求传入当前内容的哈希。
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
