#include "runtime/builtins/StringClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjStringIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // string 方法实现(NativeFn 方法调用形态见 Builtin.hpp)下标域:除 codepoint_at(码点
        // 序号)外全部字节域(与 len/s[i] 同域)。string 不可变,全部产出新串;receiver 在 slots[0]
        // 覆写前经栈根存活,单输出方法直接构造,split 先拷内容进 C++ String(非 GC 内存)再逐段铸造。

        // upper() -> 新串:ASCII 范围(A-Z/a-z)逐字节转大写,其余字节原样。
        bool fn_upper(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
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
        bool fn_lower(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
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
        bool fn_trim(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
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

        // 单参形态:按 sep 逐段切开,保留空段("a,,b" -> ["a","","b"]);空串输入切出 [""]。建并返回
        // 新 list -- 白色对象跨逐段铸造的 GC 点,由本函数内的守卫保命;src 是 receiver 的视图,
        // receiver 由调用方全程留在槽 0 为根。
        ObjList* split_by_sep(AriaVM& vm, const StringView src, const StringView sep) {
            const auto list  = new_list(vm.gc());
            const auto guard = vm.gc().make_guard(list);
            usize      begin = 0;
            while (true) {
                const usize hit = src.find(sep, begin);
                if (hit == StringView::npos) {
                    break;
                }
                // 段串铸后立即 push(中间无 GC 点);段串白色期间经 list 可达。
                list->elements().push(Value::from_obj(new_string(vm.gc(), src.substr(begin, hit - begin))));
                begin = hit + sep.size();
            }
            list->elements().push(Value::from_obj(new_string(vm.gc(), src.substr(begin))));
            return list;
        }

        // 无参形态:按 ASCII 空白**连续段**切开并丢弃空段;全空白与空串返 []。空白集与 trim 同源
        // (is_ascii_space),故只在 ASCII 域判定 -- 0x80 以上的字节一律非空白,多字节字符不会被劈开。
        ObjList* split_on_space(AriaVM& vm, const StringView src) {
            const auto list  = new_list(vm.gc());
            const auto guard = vm.gc().make_guard(list);
            usize      begin = 0;
            while (begin < src.size()) {
                while (begin < src.size() && is_ascii_space(src[begin])) {
                    ++begin;
                }
                usize end = begin;
                while (end < src.size() && !is_ascii_space(src[end])) {
                    ++end;
                }
                if (end > begin) {
                    list->elements().push(Value::from_obj(new_string(vm.gc(), src.substr(begin, end - begin))));
                }
                begin = end;
            }
            return list;
        }

        // split([sep]) -> list<string>:1 参按分隔符切(保留空段,分隔符须非空 string = EmptyPattern);
        // 0 参按 ASCII 空白连续段切、丢空段(见 split_on_space)。
        bool fn_split(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0 && argc != 1) {
                return vm.arity_error_range(argc, 0, 1);
            }
            ObjString* sep = nullptr;
            if (argc == 1) {
                sep = try_obj<ObjString>(slots[1]);
                if (sep == nullptr) {
                    return vm.fail(ErrorCode::TypeMismatch, "split separator must be a string, got {}",
                                   type_name(slots[1]));
                }
                if (sep->view().empty()) {
                    return vm.fail(ErrorCode::EmptyPattern, "split separator must not be empty");
                }
            }
            // receiver 全程留在 slots[0] 由栈标根(槽 0 是它唯一的栈根,临时 receiver 只有 intern
            // 弱根,覆写即悬垂),切割流程读的 src 即其视图。产出的 list 白色,在下一句写回槽 0
            // 发布 -- 两句之间无 GC 点,故无须在此再挂守卫。sep 经 slots[1] 恒为根,视图可留。
            const auto src  = Object::as<ObjString>(slots[0].as_obj())->view();
            const auto list = sep == nullptr ? split_on_space(vm, src) : split_by_sep(vm, src, sep->view());
            slots[0]        = Value::from_obj(list);
            return true;
        }

        // find(sub) -> 整数或 nil:子串首现字节下标,未命中 nil(下标永不为 nil 故无歧义,miss 时
        // s[s.find(x)] 会静默取末字符)。
        bool fn_find(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
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

        // contains(sub) -> Bool:子串包含判定(与 find 同域:按字节子串判);未命中返 false 不报错。
        // 空串参数恒真。
        bool fn_contains(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
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

        // replace(old, new) -> 新串:全部替换;匹配串为空报 EmptyPattern。拼接在 C++ String(非
        // GC 内存),末尾一次铸造。
        bool fn_replace(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 2) {
                return vm.arity_error(argc, 2);
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
        bool fn_substring(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1 && argc != 2) {
                return vm.arity_error_range(argc, 1, 2);
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
        bool fn_starts_with(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
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

        bool fn_ends_with(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
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

        // size() -> 整数:UTF-8 字节数(与 s[i] 同域)。码点数那是 chars().size()。
        bool fn_size(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_int(static_cast<i64>(str->length()));
            return true;
        }

        // is_empty() -> Bool:空串判定(size() == 0 的谓词形;与 list/map 同名)。
        bool fn_is_empty(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(str->length() == 0);
            return true;
        }

        // chars() -> list<string>:逐码点切出的 1-char string 快照(码点域访问口;码点数即
        // chars().size(),与字节域的 size() 相对)。非法字节序列产出替换码点串,口径同
        // ObjStringIterator::next(只吞一个坏字节)。GC 约束:receiver 留在 slots[0] 由栈标根,新
        // list 白色须跨逐字符铸造的 GC 点,故挂临时根保命,循环结束才发布。
        bool fn_chars(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
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

        // codepoint_at(i) -> 整数:第 i 个码点的码点值(码点序号索引,区别于字节域 s[i]);越界
        // IndexOutOfBounds(负数与越过末码点同走循环走空后的同一处报错)。
        bool fn_codepoint_at(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
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

        // to_int() -> 整数或 nil:整串十进制解析(util::parse_int_text,语法与域见其注);失败返
        // nil(miss 返 nil 与 find/get 同族,nil 永不与合法整数二义)。
        bool fn_to_int(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto parsed = util::parse_int_text(Object::as<ObjString>(slots[0].as_obj())->view());
            slots[0]          = parsed ? Value::from_int(*parsed) : Value::nil_val();
            return true;
        }

        // to_float() -> 浮点或 nil:整串十进制解析(util::parse_float_text),失败同 to_int 返 nil。
        bool fn_to_float(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto parsed = util::parse_float_text(Object::as<ObjString>(slots[0].as_obj())->view());
            slots[0]          = parsed ? Value::from_f64(*parsed) : Value::nil_val();
            return true;
        }

        // iter() -> 迭代器:string 与其迭代器成对(铸造口按类型解开 receiver)。GC 约束:str 在
        // slots[0] 于栈根,迭代器白色建成**先写回槽发布再返回**,中间无 GC 点。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto str = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_obj(new_string_iterator(vm.gc(), str));
            return true;
        }

        // 运算符重载方法(函数名与 runtime/str_table.hpp 的注册表拼写一一对应,经 AriaVM::run_binary_operator
        // 取用;也是"算子 = 方法"的唯一实现处)
        // 名字与失败文案都是**就地字面量**(与方法名同形):文案打方法名,与注册键同处一文件、golden
        // 钉住拼写。两侧均为 String 才成立(无隐式转换);元数不符走 vm.arity_error(措辞家族唯一口)。
        // string 只有 `+`/`*` 与四个比较。

        // 四个比较钩子 -> Bool:两侧须皆 String,按**无符号字节序**比较。必须走 string_view::compare
        // (char_traits 的 memcmp 语义)--char 在多数平台有符号,手写逐 char 比较会把 0x80 以上的字节排到
        // ASCII 之前(`"é" < "z"` 会反过来)。纯读零分配(GC-pure),无 GC 点。
        bool fn___lt__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_obj<ObjString>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__lt__ requires two strings, got {} and {}",
                               type_name(slots[0]), aria::type_name(slots[1]));
            }
            const auto lhs = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(lhs->view().compare(rhs->view()) < 0);
            return true;
        }

        bool fn___le__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_obj<ObjString>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__le__ requires two strings, got {} and {}",
                               type_name(slots[0]), aria::type_name(slots[1]));
            }
            const auto lhs = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(lhs->view().compare(rhs->view()) <= 0);
            return true;
        }

        bool fn___gt__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_obj<ObjString>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__gt__ requires two strings, got {} and {}",
                               type_name(slots[0]), aria::type_name(slots[1]));
            }
            const auto lhs = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(lhs->view().compare(rhs->view()) > 0);
            return true;
        }

        bool fn___ge__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_obj<ObjString>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__ge__ requires two strings, got {} and {}",
                               type_name(slots[0]), aria::type_name(slots[1]));
            }
            const auto lhs = Object::as<ObjString>(slots[0].as_obj());
            slots[0]       = Value::from_bool(lhs->view().compare(rhs->view()) >= 0);
            return true;
        }

        // __add__ -> 新串:拼接,结果经 new_string 驻留(同内容必同指针)。GC 走查:分配点在 intern
        // 未命中时,此刻两侧经调用区槽在栈(receiver 占 slots[0],「栈即根」)。
        bool fn___add__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_obj<ObjString>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__add__ requires two strings, got {} and {}",
                               type_name(slots[0]), type_name(slots[1]));
            }
            const auto lhs = Object::as<ObjString>(slots[0].as_obj());
            String     buffer;
            buffer.reserve(lhs->length() + rhs->length());
            buffer.append(lhs->view()).append(rhs->view());
            slots[0] = Value::from_obj(new_string(vm.gc(), buffer));
            return true;
        }

        // __mul__ -> 新串:整次重复(count 次原样拼接自身,0 次得空串;字节域整段复制,多字节序列
        // 原样成倍)。乘数严格 int(f64 一律拒,同下标访问口径),负数报错不静默得空(空结果易掩盖
        // 调用方 bug,与负数下标报错约定一致)。结果经 new_string 驻留(同 `+`,同内容必同指针)。
        // GC 走查:receiver 占 slots[0]「栈即根」,内容先累积进非 GC 的 C++ String,唯一分配点在
        // 末尾铸造。
        bool fn___mul__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "__mul__ requires a string and an integer, got {} and {}",
                               type_name(slots[0]), type_name(slots[1]));
            }
            const i64 count = slots[1].as_int();
            if (count < 0) {
                return vm.fail(ErrorCode::TypeMismatch, "__mul__ requires a non-negative integer, got {}", count);
            }
            const auto src = Object::as<ObjString>(slots[0].as_obj())->view();
            String     buffer;
            buffer.reserve(src.size() * static_cast<usize>(count));
            for (i64 i = 0; i < count; ++i) {
                buffer.append(src);
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), buffer));
            return true;
        }

        // string 方法表:注册进 String bootstrap 类(注册机制见 runtime/builtins/Builtin.hpp)。
        constexpr BuiltinFnEntry kStringBuiltins[] = {
                {"upper", fn_upper},
                {"lower", fn_lower},
                {"trim", fn_trim},
                {"split", fn_split},
                {"find", fn_find},
                {"contains", fn_contains},
                {"replace", fn_replace},
                {"substring", fn_substring},
                {"starts_with", fn_starts_with},
                {"ends_with", fn_ends_with},
                {"size", fn_size},
                {"is_empty", fn_is_empty},
                {"chars", fn_chars},
                {"codepoint_at", fn_codepoint_at},
                {"to_int", fn_to_int},
                {"to_float", fn_to_float},
                {"iter", fn_iter},
                // 运算符重载方法(String 只有 `+`/`*` 与四个比较;键与函数名对应的钩子名同形,漏改其一时
                // StringClass::kOperatorFns 清单按名查不到、bootstrap 断言即报)
                {"__lt__", fn___lt__},
                {"__le__", fn___le__},
                {"__gt__", fn___gt__},
                {"__ge__", fn___ge__},
                {"__add__", fn___add__},
                {"__mul__", fn___mul__},
        };

    } // namespace

    ObjClass* StringClass::make_class(GC& gc, ObjClass* super) {
        // String bootstrap 类:内置 string 的语言方法面载体,经 ObjString::load_field 查表命中后
        // 恒绑定触达;不入 builtins/模块 globals(用户不可直接取到类对象)。类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "String", super);
        Builtin::register_class_methods(gc, klass, kStringBuiltins);
        return klass;
    }

} // namespace aria
