#include "object/ObjString.hpp"

#include <algorithm>
#include <format>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "memory/StringBuilder.hpp"
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

    ObjString::ObjString(GC& gc, char* data, const usize size, const usize cap, const u32 hash) :
        Object{hash, ObjType::STRING}, gc_{&gc}, length_{size} {
        // 接管移交 buffer,表示形态自分流(与拷贝版同构):长串收缩接管,短串拷入 SSO 槽后释放;
        // data[size] 处的终态 '\0' 由本构造安置。
        if (is_long()) {
            // 长串臂的 realloc 是记账对齐(cap 口径 -> ~ObjString 的 length_+1 口径),不是再分配:
            // reserve 精确路径为同尺寸、倍增兜底路径为收缩,均不超当前块可用尺寸,后端原地返回
            // 原指针,零拷贝零新分配;buffer 自 reserve 起全程只有那一次 malloc。
            const auto shrunk = gc.reallocate<char>(data, cap, length_ + 1);
            shrunk[length_]   = '\0';
            long_chars_       = shrunk;
        } else {
            std::memcpy(short_chars_, data, length_);
            short_chars_[length_] = '\0';
            gc.deallocate<char>(data, cap);
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
            return Value::from_obj(vm.char_string(static_cast<u8>(view()[*slot])));
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
        // 段内容拷进 GC 记账 builder(带种串构造单发定容),倒序段在 builder 上按字节反转(字节域);
        // receiver 与 range 经调用方值栈为根,唯一 GC 点在 take_string。段解析收口 resolve_slice_bounds。
        const auto segment = resolve_slice_bounds(range, length_);
        if (!segment) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "slice range {} out of range", range->debug_repr());
        }
        auto buffer = StringBuilder{vm.gc(), view().substr(segment->start, segment->count)};
        if (segment->is_reversed) {
            std::ranges::reverse(buffer);
        }
        return Value::from_obj(buffer.take_string());
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

    ObjString* new_string(GC& gc, char* const data, const usize size, const usize cap, const u32 hash) {
        ASSERT(hash == util::hash_str(StringView{data, size}), "hash must be hash_str(content)");
        // 先查驻留:命中免铸造,移交来的 buffer 即刻释放。
        if (const auto found = gc.intern_find(StringView{data, size}, hash)) {
            gc.deallocate<char>(data, cap);
            return found;
        }
        // GC 走查:唯一 GC 点是 new_object 顶部 maybe_collect,buffer 为 raw 内存不受影响;
        // intern_insert 走 trivial 分配不触发 GC。表示形态(SSO/长串)由接管构造自分流。
        const auto s = gc.new_object<ObjString>(gc, data, size, cap, hash);
        gc.intern_insert(s);
        return s;
    }

    ObjString* new_string(GC& gc, const char ch) {
        // 委托 StringView 版(驻留池同一入口);&ch 取局部地址仅同步使用,无悬垂窗口。
        return new_string(gc, StringView{&ch, 1});
    }

} // namespace aria
