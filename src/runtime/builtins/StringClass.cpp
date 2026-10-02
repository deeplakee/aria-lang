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

        // 下标域约定:除 codepoint_at(码点序号)外全部字节域(与 len/s[i] 同域);string 不可变,
        // 方法全部产出新串。

        // init(v) -> string:转换构造,任意值经 format_value 可读渲染(与内建 str 同域同实现),
        // 覆盖槽 0(call_class 预置的临时 instance 被替换)。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), format_value(slots[1])));
            return true;
        }

        // upper() -> 新串:ASCII 范围(A-Z/a-z)逐字节转大写,其余字节原样。
        bool fn_upper(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            String out{self->view()};
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            String out{self->view()};
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            auto trimmed = self->view();
            while (!trimmed.empty() && is_ascii_space(trimmed.front())) {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && is_ascii_space(trimmed.back())) {
                trimmed.remove_suffix(1);
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), trimmed));
            return true;
        }

        // 单参形态:按 sep 逐段切开,保留空段("a,,b" -> ["a","","b"]),空串输入切出 [""]。
        // 新 list 白色须跨逐段铸造的 GC 点,入口挂守卫保命;src 是 receiver 视图,receiver 留槽 0 为根。
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

        // 无参形态:按 ASCII 空白连续段切开并丢弃空段;全空白与空串返 []。
        // 只在 ASCII 域判定空白:0x80 以上字节一律非空白,多字节字符不会被劈开。
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
        // 0 参按 ASCII 空白连续段切、丢空段。
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
            // receiver 全程留槽 0(临时串只有 intern 弱根,覆写即悬垂),src 即其视图;产出 list
            // 白色,下一句写回槽 0 发布,两句间无 GC 点故不再挂守卫;sep 经 slots[1] 恒为根。
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto src  = self->view();
            const auto list = sep == nullptr ? split_on_space(vm, src) : split_by_sep(vm, src, sep->view());
            slots[0]        = Value::from_obj(list);
            return true;
        }

        // find(sub) -> 整数或 nil:子串首现字节下标,未命中 nil。
        bool fn_find(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto sub = try_obj<ObjString>(slots[1]);
            if (sub == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "find argument must be a string, got {}", type_name(slots[1]));
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const usize hit = self->view().find(sub->view());
            slots[0]        = hit == StringView::npos ? Value::nil_val() : Value::from_int(static_cast<i64>(hit));
            return true;
        }

        // contains(sub) -> Bool:按字节子串判定;空串参数恒真,未命中返 false。
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().contains(sub->view()));
            return true;
        }

        // replace(old, new) -> 新串:全部替换;匹配串为空报 EmptyPattern。
        // 拼接在非 GC 的 C++ String,唯一分配点末尾一次铸造(此刻所有实参仍在槽位)。
        bool fn_replace(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 2) {
                return vm.arity_error(argc, 2);
            }
            const auto old_str = try_obj<ObjString>(slots[1]);
            const auto new_str = try_obj<ObjString>(slots[2]);
            if (old_str == nullptr || new_str == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "replace arguments must be strings, got {} and {}",
                               type_name(slots[1]), type_name(slots[2]));
            }
            if (old_str->length() == 0) {
                return vm.fail(ErrorCode::EmptyPattern, "replace pattern must not be empty");
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const StringView src   = self->view();
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

        // substring(start[, end]) -> 新串:字节区间 [start, end),end 省略到尾;越界/负数/end<begin
        // 报 IndexOutOfBounds,不静默钳制、无负下标归一(与 list 下标不同口径)。
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const i64  begin  = slots[1].as_int();
            const auto length = static_cast<i64>(self->length());
            const i64  end    = argc == 2 ? slots[2].as_int() : length;
            if (begin < 0 || end < begin || end > length) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "substring range {}..{} out of range", begin, end);
            }
            slots[0] = Value::from_obj(new_string(vm.gc(), self->view().substr(begin, end - begin)));
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().starts_with(prefix->view()));
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().ends_with(suffix->view()));
            return true;
        }

        // size() -> 整数:UTF-8 字节数(与 s[i] 同域)。码点数那是 chars().size()。
        bool fn_size(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_int(static_cast<i64>(self->length()));
            return true;
        }

        // is_empty() -> Bool:空串判定(size() == 0 的谓词形;与 list/map 同名)。
        bool fn_is_empty(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->length() == 0);
            return true;
        }

        // chars() -> list<string>:逐码点 1-char string 快照(码点域访问口,码点数即 chars().size());
        // 非法字节产出替换码点串(只吞一个坏字节)。GC 约束:receiver 留 slots[0],新 list 挂临时根,循环结束写回槽发布。
        bool fn_chars(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto src   = self->view();
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

        // codepoint_at(i) -> 整数:第 i 个码点的码点值(码点序号,区别于字节域 s[i]);负数与越末同走循环走空后报
        // IndexOutOfBounds。
        bool fn_codepoint_at(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "codepoint index must be an integer, got {}",
                               type_name(slots[1]));
            }
            const i64  index = slots[1].as_int();
            const auto self  = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto src    = self->view();
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

        // to_int() -> 整数或 nil:整串十进制解析,失败返 nil(永不与合法整数二义)。
        bool fn_to_int(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto parsed = util::parse_int_text(self->view());
            slots[0]          = parsed ? Value::from_int(*parsed) : Value::nil_val();
            return true;
        }

        // to_float() -> 浮点或 nil:整串十进制解析,失败返 nil。
        bool fn_to_float(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto parsed = util::parse_float_text(self->view());
            slots[0]          = parsed ? Value::from_f64(*parsed) : Value::nil_val();
            return true;
        }

        // iter() -> 迭代器。GC 约束:str 在 slots[0] 于栈根,迭代器白色建成先写回槽发布再返回,中间无 GC 点。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_obj(new_string_iterator(vm.gc(), self));
            return true;
        }

        // 运算符重载方法:string 只重载 `+`/`*` 与四个比较,两侧均 String 才成立(无隐式转换)。

        // 四个比较钩子 -> Bool:两侧须皆 String,按无符号字节序比较 -- 必须走 string_view::compare
        //(char 多数平台有符号,逐 char 手写会把 0x80+ 字节排到 ASCII 之前);纯读零分配,无 GC 点。
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().compare(rhs->view()) < 0);
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().compare(rhs->view()) <= 0);
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().compare(rhs->view()) > 0);
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->view().compare(rhs->view()) >= 0);
            return true;
        }

        // __add__ -> 新串:拼接,结果经 new_string 驻留(同内容必同指针);分配点在 intern 未命中,两侧经槽在栈。
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
            const auto lhs = receiver<ObjString>(vm, slots[0]);
            if (lhs == nullptr) {
                return false;
            }
            String buffer;
            buffer.reserve(lhs->length() + rhs->length());
            buffer.append(lhs->view()).append(rhs->view());
            // lhs->hash() 即 FNV 终态:续算 rhs 字节得 hash(lhs+rhs),免重扫整段前缀。
            slots[0] = Value::from_obj(new_string(vm.gc(), buffer, util::hash_str(lhs->hash(), rhs->view())));
            return true;
        }

        // __mul__ -> 新串:整次重复(字节域整段复制,多字节序列原样成倍,0 次得空串);乘数严格 int,
        // 负数报错不静默得空。结果经 new_string 驻留(同内容必同指针);累积进非 GC 的 C++ String,
        // 唯一分配点在末尾铸造(receiver 占 slots[0])。
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
            const auto self = receiver<ObjString>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            if (count == 0) {
                slots[0] = Value::from_obj(new_string(vm.gc(), StringView{})); // basis 全量,驻留空串共享
            } else if (count == 1) {
                slots[0] = Value::from_obj(self); // 等内容必命中驻留,即 self 本体
            } else {
                const auto src = self->view();
                String     buffer;
                buffer.reserve(src.size() * static_cast<usize>(count));
                for (i64 i = 0; i < count; ++i) {
                    buffer.append(src);
                }
                // 续算:首份拷贝终态即 hash(a),续算其余各份(count >= 2 已由上方分叉保证)。
                const u32 hash = util::hash_str(self->hash(), StringView{buffer}.substr(src.size()));
                slots[0]       = Value::from_obj(new_string(vm.gc(), buffer, hash));
            }
            return true;
        }

        // string 方法表。
        constexpr BuiltinFnEntry kStringBuiltins[] = {
                {"init", fn_init},
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
                {"__lt__", fn___lt__},
                {"__le__", fn___le__},
                {"__gt__", fn___gt__},
                {"__ge__", fn___ge__},
                {"__add__", fn___add__},
                {"__mul__", fn___mul__},
        };

    } // namespace

    ObjClass* StringClass::make_class(GC& gc, ObjClass* super) {
        // String bootstrap 类:内置 string 的语言方法面载体,类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "String", super);
        Builtin::register_class_methods(gc, klass, kStringBuiltins);
        return klass;
    }

} // namespace aria
