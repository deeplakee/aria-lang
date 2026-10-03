#include "object/ObjString.hpp"

#include <algorithm>
#include <format>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjRange.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"

namespace aria {

    ObjString::ObjString(GC& gc, const StringView src, const u32 hash) :
        Object{hash, ObjType::STRING}, gc_{&gc}, length_{src.size()} {
        ASSERT(hash == util::hash_str(src), "hash must be hash_str(src)");
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

    bool ObjString::equals(const Object* other) const noexcept {
        ASSERT(other != nullptr, "null object pointer");
        if (this == other)
            return true; // intern 命中:同指针同内容
        if (!other->is<ObjString>())
            return false;
        return view() == other->as<ObjString>()->view();
    }

    String ObjString::debug_repr() const { return std::format("\"{}\"", util::escape_string(view())); }

    String ObjString::to_string() const { return std::format("{}", view()); }

    Opt<Value> ObjString::load_field(AriaVM& vm, ObjString* name) { return vm.string_class()->load_field(vm, name); }

    Opt<Value> ObjString::load_field_bound(AriaVM& vm, ObjString* name) {
        const auto hit = load_field(vm, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjString::load_index(AriaVM& vm, const Value key) {
        if (const auto range = try_as_obj<ObjRange>(key)) {
            return slice(vm, range);
        }
        // 整数键 = 字节域,负下标从尾计数归一化;多字节序列中间字节取该字节自身。
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "string index must be an integer, got {}", aria::type_name(key));
        }
        const i64 raw = key.as_int();
        if (const auto slot = util::resolve_index(raw, length_)) {
            return Value::from_obj(new_string(vm.gc(), view()[*slot]));
        }
        return vm.fail(ErrorCode::IndexOutOfBounds, "string index {} out of range", raw);
    }

    bool ObjString::store_index(AriaVM& vm, const Value key, const Value value) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support subscript assignment", type_name());
    }

    Opt<Value> ObjString::op_add_impl(AriaVM& vm) { return vm.register_value(kStringAddFnOffset); }

    Opt<Value> ObjString::op_mul_impl(AriaVM& vm) { return vm.register_value(kStringMulFnOffset); }

    Opt<Value> ObjString::op_less_impl(AriaVM& vm) { return vm.register_value(kStringLtFnOffset); }

    Opt<Value> ObjString::op_less_equal_impl(AriaVM& vm) { return vm.register_value(kStringLeFnOffset); }

    Opt<Value> ObjString::op_greater_impl(AriaVM& vm) { return vm.register_value(kStringGtFnOffset); }

    Opt<Value> ObjString::op_greater_equal_impl(AriaVM& vm) { return vm.register_value(kStringGeFnOffset); }

    Opt<Value> ObjString::slice(AriaVM& vm, const ObjRange* range) const {
        // 段内容先拷进非 GC 的 C++ String 再铸串:receiver 与 range 经调用方值栈为根,new_string 顶部
        // maybe_collect 时安全;倒序段在拷贝上按字节反转(字节域)。段解析收口 resolve_slice_bounds。
        const auto segment = resolve_slice_bounds(range, length_);
        if (!segment) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "slice range {} out of range", range->debug_repr());
        }
        String buffer{view().substr(segment->start, segment->count)};
        if (segment->is_reversed) {
            std::ranges::reverse(buffer);
        }
        return Value::from_obj(new_string(vm.gc(), buffer));
    }

    ObjString* new_string(GC& gc, const StringView src) { return new_string(gc, src, util::hash_str(src)); }

    ObjString* new_string(GC& gc, const StringView src, const u32 hash) {
        if (const auto found = gc.intern_find(src, hash)) {
            return found; // 命中驻留池:返回已有串,不分配、不 GC
        }
        // s 此刻白色无根,但 intern_insert 走 trivial 分配不触发 GC,跨 insert 不会被回收,无需守卫;
        // 唯一 GC 触发点是下方 new_object 顶部的 maybe_collect,发生在 s 诞生前。
        const auto s = gc.new_object<ObjString>(gc, src, hash);
        gc.intern_insert(s);
        return s;
    }

    ObjString* new_string(GC& gc, const StringView lhs, const StringView rhs, const u32 hash) {
        // 先查后拼:命中驻留池零拷贝返回;未命中才把两段拼进非 GC 的 C++ String 再铸造。
        if (const auto found = gc.intern_find(lhs, rhs, hash)) {
            return found; // 命中驻留池:返回已有串,不分配、不 GC
        }
        const auto out = util::concat_string(lhs, rhs);
        // GC 走查同上:唯一 GC 点是 new_object 顶部 maybe_collect,发生在 s 诞生前;out 非 GC 对象,
        // lhs/rhs 宿主串由调用方保活(调用点两侧均栈根)。
        const auto s = gc.new_object<ObjString>(gc, out, hash);
        gc.intern_insert(s);
        return s;
    }

    ObjString* new_string(GC& gc, const char ch) {
        // 委托 StringView 版(驻留池同一入口);&ch 取局部地址仅同步使用,无悬垂窗口。
        return new_string(gc, StringView{&ch, 1});
    }

} // namespace aria
