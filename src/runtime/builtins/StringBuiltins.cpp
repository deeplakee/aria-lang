#include "runtime/builtins/StringBuiltins.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjStringIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
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
            // 视图自身收缩(remove_prefix/remove_suffix):两轮空判即首尾边界,无须下标账。
            auto trimmed = Object::as<ObjString>(slots[0].as_obj())->view();
            while (!trimmed.empty() && is_ascii_space(trimmed.front())) {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && is_ascii_space(trimmed.back())) {
                trimmed.remove_suffix(1);
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), trimmed));
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
            const auto sep_view = sep->view();
            if (sep_view.empty()) {
                return vm.fail(ErrorCode::EmptyPattern, "split separator must not be empty");
            }
            // receiver 留在 slots[0] 由栈标根到切割结束(槽 0 是它唯一的栈根,临时 receiver 只有
            // intern 弱根,覆写即悬垂),故新 list 挂临时根跨段串铸造的 GC 点保命,循环结束才写回
            // 槽 0 发布。sep 经 slots[1] 恒为根,视图可留。
            const auto src   = Object::as<ObjString>(slots[0].as_obj())->view();
            const auto list  = new_list(vm.gc());
            const auto guard = vm.gc().make_guard(list);
            usize      begin = 0;
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
            slots[0] = Value::from_obj(list);
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

        // contains(sub) -> Bool:子串包含判定(与 find 同域:按字节子串判,非字符集合);未命中
        // 返 false 不报错,与 find 未命中返 nil 同族。空串参数恒真(空串在任何位置都算包含)。
        bool contains_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "contains expects 1 argument, got {}", argc);
            }
            const auto sub = try_obj<ObjString>(slots[1]);
            if (sub == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "contains argument must be a string, got {}",
                               type_name(slots[1]));
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(str->view().contains(sub->view()));
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
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "substring index must be an integer, got {}",
                               type_name(slots[1]));
            }
            if (argc == 2 && !slots[2].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "substring index must be an integer, got {}",
                               type_name(slots[2]));
            }
            const auto str    = Object::as<ObjString>(slots[0].as_obj());
            const i64  begin  = slots[1].as_int();
            const auto length = static_cast<i64>(str->length());
            const i64  end    = argc == 2 ? slots[2].as_int() : length;
            // 前两个析取短路后 end >= begin >= 0 已成立,上界比较按 i64 即可(不借 u64 窄化)。
            if (begin < 0 || end < begin || end > length) {
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

        // size() -> 整数:UTF-8 字节数(len(s) 的方法形态,与 s[i] 同域,计划 D5)。码点数不是本
        // 方法 --那是 len(chars())。
        bool size_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "size expects no arguments, got {}", argc);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_int(static_cast<i64>(str->length()));
            return true;
        }

        // is_empty() -> Bool:空串判定(size() == 0 的谓词形;与 list/map 同名)。
        bool is_empty_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "is_empty expects no arguments, got {}", argc);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(str->length() == 0);
            return true;
        }

        // chars() -> list<string>:逐码点切出的 1-char string 快照 -- 码点域访问口,产出形态与
        // 迭代、s[i] 同族;码点数即 len(chars()),与字节域的 len(s)/size() 相对。非法字节序列产出
        // 替换码点串,口径同 ObjStringIterator::next(只吞一个坏字节)。GC 时序:receiver 留在
        // slots[0] 由栈标根,新 list 白色须跨逐字符铸造的 GC 点,故挂临时根保命,循环结束才发布。
        bool chars_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "chars expects no arguments, got {}", argc);
            }
            const auto src   = Object::as<ObjString>(slots[0].as_obj())->view();
            const auto list  = new_list(vm.gc());
            const auto guard = vm.gc().make_guard(list);
            for (usize offset = 0; offset < src.size();) {
                const auto [cp, width] = utf8::decode_one(src, offset);
                offset += width;
                // 串铸后立即 push(中间无 GC 点);串白色期间经 list 可达。
                list->elements().push(Value::from_obj(new_string(vm.gc(), utf8::encode(cp))));
            }
            slots[0] = Value::from_obj(list);
            return true;
        }

        // codepoint_at(i) -> 整数:第 i 个码点的码点值(码点序号索引,区别于字节域 s[i]);
        // 越界 IndexOutOfBounds--负数与越过末码点同走循环走空后的同一处报错(负数不早退,多扫
        // 一遍串换单出口)。逐码点扫描定位(O(i),无偏移索引表,v1 接受)。
        bool codepoint_at_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "codepoint_at expects 1 argument, got {}", argc);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "codepoint index must be an integer, got {}",
                               type_name(slots[1]));
            }
            const i64  index  = slots[1].as_int();
            const auto src    = Object::as<ObjString>(slots[0].as_obj())->view();
            usize      offset = 0;
            for (i64 seen = 0; offset < src.size(); ++seen) {
                const auto [cp, width] = utf8::decode_one(src, offset);
                if (seen == index) {
                    slots[0] = Value::from_int(cp); // u32 码点(<= 0x10FFFF)加宽,值域安全
                    return true;
                }
                offset += width;
            }
            return vm.fail(ErrorCode::IndexOutOfBounds, "codepoint index {} out of range", index);
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

        // string 方法表:注册进 String bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp)。
        constexpr builtins::BuiltinEntry kStringBuiltins[] = {
                {"upper", upper_fn},
                {"lower", lower_fn},
                {"trim", trim_fn},
                {"split", split_fn},
                {"find", find_fn},
                {"contains", contains_fn},
                {"replace", replace_fn},
                {"substring", substring_fn},
                {"starts_with", starts_with_fn},
                {"ends_with", ends_with_fn},
                {"size", size_fn},
                {"is_empty", is_empty_fn},
                {"chars", chars_fn},
                {"codepoint_at", codepoint_at_fn},
                {"iter", iter_fn},
        };

    } // namespace

    void register_string_builtins(GC& gc, ObjClass* klass) { register_builtin_methods(gc, klass, kStringBuiltins); }

} // namespace aria
