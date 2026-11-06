#ifndef ARIA_VALUE_HPP
#define ARIA_VALUE_HPP

#include "common.hpp"

#ifdef USING_NANBOXING
    #include "value/NanBoxing.hpp"
namespace aria {
    using namespace nanboxing;
}
#else
    #include "value/TagValue.hpp"
namespace aria {
    using namespace tagvalue;
}
#endif
#include <functional>

namespace aria {

    class ObjString;

    // Value 的哈希与相等判定函数;只用两表示共有的 type()/as_*() API,两表示下都能编译。
    // 双相等体系(语言 == 与 === 的语义来源):== = value_equal,内容相等--Int/F64 跨类型 IEEE 数值,Obj 按
    // 内容虚分派相等(不做 JS 式强制转换);=== = value_identical,类型严格--f64 按位,Obj 指针相等;哈希表键用 ===。
    [[nodiscard]]
    inline bool is_num(const Value value) noexcept {
        return value.is_int() || value.is_f64();
    }

    // is_num 前提下取数值的 f64 视图(Int 升 f64 / F64 原样);非 num 值由 as_int 的 debug ASSERT 拦截。
    [[nodiscard]]
    inline f64 as_num(const Value value) noexcept {
        return value.is_f64() ? value.as_f64() : static_cast<f64>(value.as_int());
    }

    // 真值(Lua 风格):nil 与 false 为假,其余(含 0/"")为真。
    [[nodiscard]]
    inline bool is_truthy(const Value value) noexcept {
        return !(value.is_nil() || (value.is_bool() && !value.as_bool()));
    }

    // 精确类型名统一入口(区别于 Value::type_name 的粗分类:对 Obj 取子类型精确拼写,后者返 "Obj")。
    [[nodiscard]]
    StringView type_name(Value value) noexcept;

    [[nodiscard]]
    u32 value_hash(Value value) noexcept;

    // 内容相等(==):语言 == 与 match 的语义来源。
    [[nodiscard]]
    bool value_equal(Value lhs, Value rhs) noexcept;

    // 严格相等(===):语言 === 与哈希表键判等的语义来源。
    [[nodiscard]]
    bool value_identical(Value lhs, Value rhs) noexcept;

    // 自然序小于:数值域双 Int 整数序、混合升 f64,NaN 排在一切数值之前(保严格弱序,否则排序形式上 UB);
    // 字符串域按无符号字节序(string_view::compare 的 memcmp 语义)。域外组合(非数值/字符串或混居)未定义,
    // 调用方须先域检;noexcept 无分配,可直接作排序比较器。
    [[nodiscard]]
    bool value_less(Value lhs, Value rhs) noexcept;

    // f64 可读化:保证含 `.`/`e`/`E`(整值补 `.0`),与 Int 区分;inf/nan 直出。
    [[nodiscard]]
    String format_f64(f64 value);

    // 显示位渲染(纯 C++,与 VM 无关):字符串原文直取,其余与 value_repr 同口径;不执行用户代码。
    [[nodiscard]]
    String value_str(Value value);

    // 调试渲染(执行跟踪/反汇编常量池/未捕获兜底等上下文):Obj 走 repr();调试渲染
    // 永不执行用户代码,故在 VM 主循环内与无 VM 上下文处皆可安心调用。
    [[nodiscard]]
    String value_repr(Value value);

    // 实参表形态的只读值段;非实参语义的值段仍写字面 Span<const Value>。
    using ArgList = Span<const Value>;

} // namespace aria

// std 容器以 Value 作键所需的两个特化:哈希转发 value_hash、判等转发 value_identical(===)
// 而非 ==(value_equal),二者按类型/位型自洽;否则 int 1 与 f64 1.0、-0.0 与 0.0 会合并成同键,改变语义。
namespace std {

    template<>
    struct hash<aria::Value> {
        [[nodiscard]]
        aria::usize operator()(const aria::Value value) const noexcept {
            return aria::value_hash(value);
        }
    };

    template<>
    struct equal_to<aria::Value> {
        [[nodiscard]]
        bool operator()(const aria::Value lhs, const aria::Value rhs) const noexcept {
            return aria::value_identical(lhs, rhs);
        }
    };

} // namespace std

#endif // ARIA_VALUE_HPP
