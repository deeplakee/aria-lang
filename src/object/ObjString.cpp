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

    bool ObjString::equals(const Object* other) const noexcept {
        if (this == other)
            return true; // intern 命中:同指针同内容
        if (!is<ObjString>(other))
            return false; // 不同 Obj 类型
        return view() == as<ObjString>(other)->view();
    }

    ObjString* new_string(GC& gc, const StringView src) {
        if (ObjString* found = gc.intern_find(src)) {
            return found; // 命中驻留池:返回已有串,不分配、不 GC
        }
        auto s     = gc.new_object<ObjString>(gc, src); // 顶部 maybe_collect 在分配前完成
        auto guard = gc.make_guard(s);                  // 防御性:保护新串直至 insert 完成
        gc.intern_insert(s);
        return s;
    }

} // namespace aria
