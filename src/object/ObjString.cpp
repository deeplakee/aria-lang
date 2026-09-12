#include "object/ObjString.hpp"

#include <format>

#include "memory/GC.hpp"
#include "util/util.hpp"

namespace aria {

    ObjString::ObjString(GC& gc, const StringView src) :
        Object{util::hash_str(src), ObjType::STRING}, gc_{&gc}, length_{src.size()} {
        if (is_long()) {
            long_chars_ = gc.allocate<char>(length_ + 1);
            std::memcpy(long_chars_, src.data(), length_);
            long_chars_[length_] = '\0';
        } else {
            std::memcpy(short_chars_, src.data(), length_);
            short_chars_[length_] = '\0';
        }
    }

    ObjString::~ObjString() {
        if (is_long()) {
            gc_->deallocate<char>(long_chars_, length_ + 1);
        }
    }

    StringView ObjString::view() const noexcept {
        const char* chars = is_long() ? long_chars_ : short_chars_;
        return StringView{chars, length_};
    }

    String ObjString::to_string() const { return std::format("{}", view()); }

    String ObjString::debug_repr() const {
        // 字面量形式:转义 + 双引号包裹。
        return std::format("\"{}\"", util::escape_string(view()));
    }

    bool ObjString::equals(const Object* other) const noexcept {
        if (this == other)
            return true; // intern 命中:同指针同内容
        if (!is<ObjString>(other))
            return false;
        return view() == as<ObjString>(other)->view();
    }

    ObjString* new_string(GC& gc, const StringView src) {
        if (const auto found = gc.intern_find(src)) {
            return found; // 命中驻留池:返回已有串,不分配、不 GC
        }
        // s 此刻白色无根,但 intern_insert -> InternPool::insert -> allocate<ObjString*> 走 trivial
        // 分配(不触发 GC,见 GC.hpp 核心不变式),故 s 跨 insert 不会被回收,无需守卫。
        const auto s = gc.new_object<ObjString>(gc, src); // 顶部 maybe_collect 在 s 诞生前完成
        gc.intern_insert(s);
        return s;
    }

} // namespace aria
