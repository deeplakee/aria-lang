#include "runtime/builtins/StringClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "memory/StringBuilder.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjStringIterator.hpp"
#include "runtime/Eval.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 除 codepoint_at 的码点序号外，下标和长度均按字节计。

        bool fn_init(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto str = Eval::stringify(ctx, info[0]);
            if (!str) {
                return false;
            }
            info.set_return_val(Value::from_obj(str));
            return true;
        }

        // 大小写转换仅处理 ASCII，其余字节保持原样。
        bool fn_upper(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            auto out = StringBuilder{ctx.gc(), self->view()};
            for (char& c: out) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            info.set_return_val(Value::from_obj(out.take_string()));
            return true;
        }

        bool fn_lower(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            auto out = StringBuilder{ctx.gc(), self->view()};
            for (char& c: out) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            info.set_return_val(Value::from_obj(out.take_string()));
            return true;
        }

        [[nodiscard]]
        bool is_ascii_space(const char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        }

        bool fn_trim(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
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
            info.set_return_val(Value::from_obj(new_string(ctx.gc(), trimmed)));
            return true;
        }

        // 显式分隔符保留空段；结果列表须跨逐段铸串保活。
        ObjList* split_by_sep(const AriaExeContext& ctx, const StringView src, const StringView sep) {
            const auto list  = new_list(ctx.gc());
            const auto guard = ctx.gc().make_guard(list);
            usize      begin = 0;
            while (true) {
                const usize hit = src.find(sep, begin);
                if (hit == StringView::npos) {
                    break;
                }
                list->elements().push(Value::from_obj(new_string(ctx.gc(), src.substr(begin, hit - begin))));
                begin = hit + sep.size();
            }
            list->elements().push(Value::from_obj(new_string(ctx.gc(), src.substr(begin))));
            return list;
        }

        // 无分隔符时按连续 ASCII 空白切分并丢弃空段，不拆多字节字符。
        ObjList* split_on_space(const AriaExeContext& ctx, const StringView src) {
            const auto list  = new_list(ctx.gc());
            const auto guard = ctx.gc().make_guard(list);
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
                    list->elements().push(Value::from_obj(new_string(ctx.gc(), src.substr(begin, end - begin))));
                }
                begin = end;
            }
            return list;
        }

        bool fn_split(const NativeCallInfo& info) {
            auto&      ctx  = info.ctx();
            const auto argc = info.size();
            if (argc != 0 && argc != 1) {
                return ctx.arity_error_range(argc, 0, 1);
            }
            ObjString* sep = nullptr;
            if (argc == 1) {
                sep = try_as_obj<ObjString>(info[0]);
                if (sep == nullptr) {
                    return ctx.fail(ErrorCode::TypeMismatch, "split separator must be a string, got {}",
                                    type_name(info[0]));
                }
                if (sep->view().empty()) {
                    return ctx.fail(ErrorCode::EmptyPattern, "split separator must not be empty");
                }
            }
            // 分割期间不能覆盖 receiver 槽，否则 src 借用的内容可能被回收。
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const auto src  = self->view();
            const auto list = sep == nullptr ? split_on_space(ctx, src) : split_by_sep(ctx, src, sep->view());
            info.set_return_val(Value::from_obj(list));
            return true;
        }

        bool fn_find(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto sub = try_as_obj<ObjString>(info[0]);
            if (sub == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "find argument must be a string, got {}", type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const usize hit = self->view().find(sub->view());
            info.set_return_val(hit == StringView::npos ? Value::nil_val() : Value::from_int(static_cast<i64>(hit)));
            return true;
        }

        bool fn_contains(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto sub = try_as_obj<ObjString>(info[0]);
            if (sub == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "contains argument must be a string, got {}",
                                type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().contains(sub->view())));
            return true;
        }

        bool fn_replace(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 2) {
                return ctx.arity_error(argc, 2);
            }
            const auto pattern     = try_as_obj<ObjString>(info[0]);
            const auto replacement = try_as_obj<ObjString>(info[1]);
            if (pattern == nullptr || replacement == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "replace arguments must be strings, got {} and {}",
                                type_name(info[0]), type_name(info[1]));
            }
            if (pattern->length() == 0) {
                return ctx.fail(ErrorCode::EmptyPattern, "replace pattern must not be empty");
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            // 先统计不重叠匹配的输出总长，再一次定容填充。
            const StringView src     = self->view();
            const StringView pat     = pattern->view();
            const StringView rep     = replacement->view();
            usize            matches = 0;
            for (usize hit = 0; (hit = src.find(pat, hit)) != StringView::npos; hit += pat.size()) {
                ++matches;
            }
            auto out = StringBuilder{ctx.gc()};
            out.reserve(src.size() - matches * pat.size() + matches * rep.size());
            usize begin = 0;
            for (usize hit = 0; (hit = src.find(pat, begin)) != StringView::npos; begin = hit + pat.size()) {
                out.append(src.substr(begin, hit - begin));
                out.append(rep);
            }
            out.append(src.substr(begin));
            info.set_return_val(Value::from_obj(out.take_string()));
            return true;
        }

        // 字节区间 [start, end)，不支持负下标，不钳制越界。
        bool fn_substring(const NativeCallInfo& info) {
            auto&      ctx  = info.ctx();
            const auto argc = info.size();
            if (argc != 1 && argc != 2) {
                return ctx.arity_error_range(argc, 1, 2);
            }
            if (!info[0].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "substring index must be an integer, got {}",
                                type_name(info[0]));
            }
            if (argc == 2 && !info[1].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "substring index must be an integer, got {}",
                                type_name(info[1]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const i64  begin  = info[0].as_int();
            const auto length = static_cast<i64>(self->length());
            const i64  end    = argc == 2 ? info[1].as_int() : length;
            if (begin < 0 || end < begin || end > length) {
                return ctx.fail(ErrorCode::IndexOutOfBounds, "substring range {}..{} out of range", begin, end);
            }
            info.set_return_val(Value::from_obj(new_string(ctx.gc(), self->view().substr(begin, end - begin))));
            return true;
        }

        bool fn_starts_with(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto prefix = try_as_obj<ObjString>(info[0]);
            if (prefix == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "starts_with argument must be a string, got {}",
                                type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().starts_with(prefix->view())));
            return true;
        }

        bool fn_ends_with(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto suffix = try_as_obj<ObjString>(info[0]);
            if (suffix == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "ends_with argument must be a string, got {}",
                                type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().ends_with(suffix->view())));
            return true;
        }

        bool fn_size(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_int(static_cast<i64>(self->length())));
            return true;
        }

        bool fn_is_empty(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->length() == 0));
            return true;
        }

        // 按码点切出原字节，非法字节替换为 U+FFFD；结果列表须跨铸串保活。
        bool fn_chars(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const auto src   = self->view();
            const auto list  = new_list(ctx.gc());
            const auto guard = ctx.gc().make_guard(list);
            for (usize offset = 0; offset < src.size();) {
                const usize start      = offset;
                const auto [cp, width] = utf8::decode_one(src, offset);
                offset += width;
                // 单字节 ASCII 直接取缓存，非法字节转替换码点，多字节字符保留原编码。
                if (width == 1) {
                    if (cp >= 0x80) {
                        list->elements().push(Value::from_obj(new_string(ctx.gc(), utf8::encode(cp))));
                    } else {
                        list->elements().push(Value::from_obj(ctx.char_string(static_cast<u8>(cp))));
                    }
                } else {
                    list->elements().push(Value::from_obj(new_string(ctx.gc(), src.substr(start, width))));
                }
            }
            info.set_return_val(Value::from_obj(list));
            return true;
        }

        bool fn_codepoint_at(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            if (!info[0].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "codepoint index must be an integer, got {}",
                                type_name(info[0]));
            }
            const i64  index = info[0].as_int();
            const auto self  = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const auto src    = self->view();
            usize      offset = 0;
            for (i64 seen = 0; offset < src.size(); ++seen) {
                const auto [cp, width] = utf8::decode_one(src, offset);
                if (seen == index) {
                    info.set_return_val(Value::from_int(cp)); // u32 码点(<= 0x10FFFF)加宽,值域安全
                    return true;
                }
                offset += width;
            }
            return ctx.fail(ErrorCode::IndexOutOfBounds, "codepoint index {} out of range", index);
        }

        bool fn_to_int(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const auto parsed = util::parse_int_text(self->view());
            info.set_return_val(parsed ? Value::from_int(*parsed) : Value::nil_val());
            return true;
        }

        bool fn_to_float(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            const auto parsed = util::parse_float_text(self->view());
            info.set_return_val(parsed ? Value::from_f64(*parsed) : Value::nil_val());
            return true;
        }

        bool fn_iter(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_obj(new_string_iterator(ctx.gc(), self)));
            return true;
        }

        // 用无符号字节序比较，避免平台 char 的符号性影响非 ASCII 排序。
        bool fn___lt__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjString>(info[0]);
            if (rhs == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "__lt__ requires two strings, got {} and {}",
                                type_name(info.self()), aria::type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().compare(rhs->view()) < 0));
            return true;
        }

        bool fn___le__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjString>(info[0]);
            if (rhs == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "__le__ requires two strings, got {} and {}",
                                type_name(info.self()), aria::type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().compare(rhs->view()) <= 0));
            return true;
        }

        bool fn___gt__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjString>(info[0]);
            if (rhs == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "__gt__ requires two strings, got {} and {}",
                                type_name(info.self()), aria::type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().compare(rhs->view()) > 0));
            return true;
        }

        bool fn___ge__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjString>(info[0]);
            if (rhs == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "__ge__ requires two strings, got {} and {}",
                                type_name(info.self()), aria::type_name(info[0]));
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->view().compare(rhs->view()) >= 0));
            return true;
        }

        bool fn___add__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjString>(info[0]);
            if (rhs == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "__add__ requires two strings, got {} and {}",
                                type_name(info.self()), type_name(info[0]));
            }
            const auto lhs = info.receiver<ObjString>();
            if (lhs == nullptr) {
                return false;
            }
            // 左串哈希是 FNV 终态，可直接续算右串，无须重扫前缀。
            const auto hash = util::hash_str(lhs->hash(), rhs->view());
            const auto out  = new_string(ctx.gc(), lhs->view(), rhs->view(), hash);
            info.set_return_val(Value::from_obj(out));
            return true;
        }

        bool fn___mul__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            if (!info[0].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "__mul__ requires a string and an integer, got {} and {}",
                                type_name(info.self()), type_name(info[0]));
            }
            const i64 count = info[0].as_int();
            if (count < 0) {
                return ctx.fail(ErrorCode::TypeMismatch, "__mul__ requires a non-negative integer, got {}", count);
            }
            const auto self = info.receiver<ObjString>();
            if (self == nullptr) {
                return false;
            }
            if (count == 0) {
                info.set_return_val(Value::from_obj(new_string(ctx.gc(), StringView{}))); // basis 全量,驻留空串共享
            } else if (count == 1) {
                info.set_return_val(Value::from_obj(self)); // 等内容必命中驻留,即 self 本体
            } else {
                const auto src    = self->view();
                const auto copies = static_cast<usize>(count);
                auto       out    = StringBuilder{ctx.gc()};
                out.reserve(src.size() * copies);
                for (usize i = 0; i < copies; ++i) {
                    out.append(src);
                }
                const auto rest = out.view().substr(src.size());
                info.set_return_val(Value::from_obj(out.take_string(util::hash_str(self->hash(), rest))));
            }
            return true;
        }

        // 显示返回原文，调试 repr 保留引号与转义。
        bool fn___str__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            if (info.receiver<ObjString>() == nullptr) {
                return false;
            }
            info.set_return_val(info.self());
            return true;
        }

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
                {"__str__", fn___str__},
        };

    } // namespace

    ObjClass* StringClass::make_class(GC& gc, ObjClass* super) {
        const auto klass = new_class(gc, "String", super);
        Builtin::register_class_methods(gc, klass, kStringBuiltins);
        return klass;
    }

} // namespace aria
