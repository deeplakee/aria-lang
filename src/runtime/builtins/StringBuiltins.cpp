#include "runtime/builtins/StringBuiltins.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjStringIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // ---- string 方法实现(NativeFn 方法调用形态:slots[0] = receiver 兼返回槽,读 slots[1..]) ----
        //
        // 下标域:除 codepoint_at(码点序号)外全部字节域(计划 D5,与 len/s[i] 同域)。string
        // 不可变,全部产出新串;receiver 在 slots[0] 覆写前经栈根存活,单输出方法直接构造,
        // split 先拷内容进 C++ String(非 GC 内存)再逐段铸造。

        // upper() -> 新串:ASCII 范围(A-Z/a-z)逐字节转大写,其余字节原样(v1 ASCII only,
        // Unicode casing 需 case 映射表,后续批按需)。
        bool upper_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "upper expects no arguments, got {}", argc);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            String     out{str->view()};
            for (char& c: out) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), out));
            return true;
        }

        // lower() -> 新串:ASCII 逐字节转小写,同 upper 口径。
        bool lower_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "lower expects no arguments, got {}", argc);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            String     out{str->view()};
            for (char& c: out) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), out));
            return true;
        }

        // ASCII 空白六字符(trim 的空白集,与 upper/lower 的 ASCII 口径一致)。
        [[nodiscard]]
        bool is_ascii_space(const char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        }

        // trim() -> 新串:去首尾 ASCII 空白;全空白返空串。
        bool trim_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "trim expects no arguments, got {}", argc);
            }
            const auto src   = Object::as<ObjString>(slots[0].as_obj())->view();
            usize      begin = 0;
            usize      end   = src.size();
            while (begin < end && is_ascii_space(src[begin])) {
                ++begin;
            }
            while (end > begin && is_ascii_space(src[end - 1])) {
                --end;
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), src.substr(begin, end - begin)));
            return true;
        }

        // split(sep) -> list<string>:按分隔符切开,保留空段("a,,b" -> ["a","","b"]);
        // 空串输入切出 [""]。分隔符须非空 string(EmptyPattern)。
        bool split_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "split expects 1 argument, got {}", argc);
            }
            const auto sep = try_obj<ObjString>(slots[1]);
            if (sep == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "split separator must be a string, got {}",
                               type_name(slots[1]));
            }
            if (sep->length() == 0) {
                return vm.fail(ErrorCode::EmptyPattern, "split separator must not be empty");
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            // 内容拷进 C++ String(非 GC 内存):list 建成即覆写 slots[0],receiver 此后不再
            // 经栈存活,切割全程只依赖拷贝与栈上 list。
            const StringView src      = str->view();
            const StringView sep_view = sep->view();
            const auto       list     = new_list(vm.gc());
            slots[0]                  = Value::from_obj(list); // 先发布:list 白色建成即入栈根,段串铸造期间保命
            usize begin               = 0;
            while (true) {
                const usize hit = src.find(sep_view, begin);
                if (hit == StringView::npos) {
                    break;
                }
                // 段串铸后立即 push(中间无 GC 点);段串白色期间经 list 可达。
                list->elements().push(Value::from_obj(new_string(vm.gc(), src.substr(begin, hit - begin))));
                begin = hit + sep_view.size();
            }
            list->elements().push(Value::from_obj(new_string(vm.gc(), src.substr(begin))));
            return true;
        }

        // find(sub) -> 整数或 nil:子串首现字节下标,未命中 nil(下标永不为 nil 故无歧义,
        // Ruby 同款 --aria 有负下标,-1 是合法下标,miss 时 s[s.find(x)] 会静默取末字符)。
        bool find_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "find expects 1 argument, got {}", argc);
            }
            const auto sub = try_obj<ObjString>(slots[1]);
            if (sub == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "find argument must be a string, got {}", type_name(slots[1]));
            }
            const auto  str = Object::as<ObjString>(slots[0].as_obj());
            const usize hit = str->view().find(sub->view());
            slots[0]        = hit == StringView::npos ? Value::nil_val() : Value::from_int(static_cast<i64>(hit));
            return true;
        }

        // replace(old, new) -> 新串:全部替换(Python/JS replaceAll 同款);匹配串为空报
        // EmptyPattern。拼接在 C++ String(非 GC 内存),末尾一次铸造。
        bool replace_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 2) {
                return vm.fail(ErrorCode::WrongArity, "replace expects 2 arguments, got {}", argc);
            }
            const auto old_str = try_obj<ObjString>(slots[1]);
            const auto new_str = try_obj<ObjString>(slots[2]);
            if (old_str == nullptr || new_str == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "replace arguments must be strings, got {} and {}", //
                               type_name(slots[1]), type_name(slots[2]));
            }
            if (old_str->length() == 0) {
                return vm.fail(ErrorCode::EmptyPattern, "replace pattern must not be empty");
            }
            const auto       str   = Object::as<ObjString>(slots[0].as_obj());
            const StringView src   = str->view();
            const StringView old_v = old_str->view();
            const StringView new_v = new_str->view();
            String           out;
            usize            begin = 0;
            while (true) {
                const usize hit = src.find(old_v, begin);
                if (hit == StringView::npos) {
                    out += src.substr(begin);
                    break;
                }
                out += src.substr(begin, hit - begin);
                out += new_v;
                begin = hit + old_v.size();
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), out));
            return true;
        }

        // substring(start[, end]) -> 新串:字节区间 [start, end);end 省略到尾;越界/负数/
        // end<begin 报 IndexOutOfBounds,不静默钳制、无负下标归一(与 list 下标不同口径)。
        bool substring_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1 && argc != 2) {
                return vm.fail(ErrorCode::WrongArity, "substring expects 1 or 2 arguments, got {}", argc);
            }
            if (!slots[1].is_int() || (argc == 2 && !slots[2].is_int())) {
                return vm.fail(ErrorCode::TypeMismatch, "substring index must be an integer, got {}",
                               type_name(argc == 2 ? slots[2] : slots[1]));
            }
            const auto str   = Object::as<ObjString>(slots[0].as_obj());
            const i64  begin = slots[1].as_int();
            const i64  end   = argc == 2 ? slots[2].as_int() : static_cast<i64>(str->length());
            if (begin < 0 || end < begin || static_cast<u64>(end) > str->length()) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "substring range {}..{} out of range", begin, end);
            }
            slots[0] = Value::from_obj(new_string(
                    vm.gc(), str->view().substr(static_cast<usize>(begin), static_cast<usize>(end - begin))));
            return true;
        }

        // starts_with(prefix) -> bool:字节前缀判定;空串前缀恒真。ends_with 同款尾缀。
        bool starts_with_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "starts_with expects 1 argument, got {}", argc);
            }
            const auto prefix = try_obj<ObjString>(slots[1]);
            if (prefix == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "starts_with argument must be a string, got {}",
                               type_name(slots[1]));
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(str->view().starts_with(prefix->view()));
            return true;
        }

        bool ends_with_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "ends_with expects 1 argument, got {}", argc);
            }
            const auto suffix = try_obj<ObjString>(slots[1]);
            if (suffix == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "ends_with argument must be a string, got {}",
                               type_name(slots[1]));
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(str->view().ends_with(suffix->view()));
            return true;
        }

        // codepoint_at(i) -> 整数:第 i 个码点的码点值(码点序号索引,区别于字节域 s[i]);
        // 越界(含负数)IndexOutOfBounds。逐码点扫描定位(O(i),无偏移索引表,v1 接受)。
        bool codepoint_at_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "codepoint_at expects 1 argument, got {}", argc);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "codepoint index must be an integer, got {}",
                               type_name(slots[1]));
            }
            const i64 index = slots[1].as_int();
            if (index < 0) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "codepoint index {} out of range", index);
            }
            const auto str    = Object::as<ObjString>(slots[0].as_obj());
            const auto src    = str->view();
            i64        seen   = 0;
            usize      offset = 0;
            while (true) {
                if (offset >= src.size()) {
                    return vm.fail(ErrorCode::IndexOutOfBounds, "codepoint index {} out of range", index);
                }
                const auto [cp, width] = utf8::decode_one(src, offset);
                if (seen == index) {
                    slots[0] = Value::from_int(cp); // u32 码点(<= 0x10FFFF)加宽,值域安全
                    return true;
                }
                offset += width;
                ++seen;
            }
        }

        // iter() -> 迭代器:铸造 ObjStringIterator(string 与其迭代器成对,铸造口按类型解开
        // receiver)。GC 时序同 ListBuiltins::iter_fn:str 在 slots[0] 于栈根,迭代器白色建成
        // 先写回槽发布再返回,中间无 GC 点;此后 str 经迭代器 trace 可达。
        bool iter_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "iter expects no arguments, got {}", argc);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_obj(new_string_iterator(vm.gc(), str));
            return true;
        }

        struct StringBuiltinEntry {
            StringView name;
            NativeFn   fn;
        };

        // string 方法表:注册进 String bootstrap 类(kListBuiltins 同款循环)。注册名经
        // new_native_fn 的 StringView 重载 intern,与 CodeGen LOAD_FIELD 发射的同名常量同
        // 指针,查表按指针命中。
        constexpr StringBuiltinEntry kStringBuiltins[] = {
                {"upper", upper_fn},         {"lower", lower_fn},
                {"trim", trim_fn},           {"split", split_fn},
                {"find", find_fn},           {"replace", replace_fn},
                {"substring", substring_fn}, {"starts_with", starts_with_fn},
                {"ends_with", ends_with_fn}, {"codepoint_at", codepoint_at_fn},
                {"iter", iter_fn},
        };

    } // namespace

    void register_string_builtins(GC& gc, ObjClass* klass) {
        for (const auto& [name, fn]: kStringBuiltins) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            klass->set_field(fn_obj->name(), Value::from_obj(fn_obj));
        }
    }

} // namespace aria
