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

    bool ObjString::equals(const Object* other) const noexcept {
        if (this == other)
            return true; // intern 命中:同指针同内容
        if (!is<ObjString>(other))
            return false;
        return view() == as<ObjString>(other)->view();
    }

    String ObjString::debug_repr() const {
        // 字面量形式:转义 + 双引号包裹。
        return std::format("\"{}\"", util::escape_string(view()));
    }

    String ObjString::to_string() const { return std::format("{}", view()); }

    Opt<Value> ObjString::load_field(AriaVM& vm, ObjString* name) {
        const auto hit = vm.string_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjString::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 不铸 ObjBoundMethod,命中直取类表原生值(契约见 Object.hpp);本体是纯透传。
        return vm.string_class()->load_field(vm, name);
    }

    Opt<Value> ObjString::load_index(AriaVM& vm, const Value key) {
        // Range 键 = 切片(流程见 slice,与 list 同口径)。
        if (const auto range = try_obj<ObjRange>(key)) {
            return slice(vm, range);
        }
        // 整数键 = 字节域(与 len 同域):产出单字节 1-char string;多字节序列
        // 中间字节取该字节自身(字节契约的自然结果,非完整字符)。非整数 TypeMismatch;
        // 负下标从尾计数、归一化后越界 IndexOutOfBounds(文案报原始键值,同 list)。
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
        // string 不可变:下标写恒报错(协议族文案;键值检查无意义,先拒操作本身)。
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support subscript assignment", aria_type_name());
    }

    Opt<Value> ObjString::op_add_impl(AriaVM& vm) { return vm.register_value(kStringAddFnOffset); }

    Opt<Value> ObjString::op_mul_impl(AriaVM& vm) { return vm.register_value(kStringMulFnOffset); }

    Opt<Value> ObjString::op_less_impl(AriaVM& vm) { return vm.register_value(kStringLtFnOffset); }

    Opt<Value> ObjString::op_less_equal_impl(AriaVM& vm) { return vm.register_value(kStringLeFnOffset); }

    Opt<Value> ObjString::op_greater_impl(AriaVM& vm) { return vm.register_value(kStringGtFnOffset); }

    Opt<Value> ObjString::op_greater_equal_impl(AriaVM& vm) { return vm.register_value(kStringGeFnOffset); }

    Opt<Value> ObjString::slice(AriaVM& vm, const ObjRange* range) const {
        // 切片段解析收口 resolve_slice_bounds:nullopt = 无法形成合法区间(文案与 list 切片同串)。
        // 段内容先拷进非 GC 的 C++ String 再铸串:receiver 与 range 经调用方值栈为根,new_string
        // 顶部 maybe_collect 时安全;倒序段在拷贝上按字节反转(字节域,多字节输入下产出非法 UTF-8)。
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

    ObjString* new_string(GC& gc, const char ch) {
        // 委托 StringView 版(驻留池同一入口);&ch 取局部地址仅同步使用,无悬垂窗口。
        return new_string(gc, StringView{&ch, 1});
    }

} // namespace aria
