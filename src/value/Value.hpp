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

    // == 按内容比较，Int/F64 可跨类型比较；=== 要求同类型，浮点按位、对象按身份。
    // 哈希表键采用 ===。
    [[nodiscard]]
    inline bool is_num(const Value value) noexcept {
        return value.is_int() || value.is_f64();
    }

    // 仅接受数值；整数提升为 f64。
    [[nodiscard]]
    inline f64 as_num(const Value value) noexcept {
        return value.is_f64() ? value.as_f64() : static_cast<f64>(value.as_int());
    }

    // 只有 nil 和 false 为假。
    [[nodiscard]]
    inline bool is_truthy(const Value value) noexcept {
        return !(value.is_nil() || (value.is_bool() && !value.as_bool()));
    }

    // 对象返回具体类型名，区别于 Value::type_name 的 Obj 粗分类。
    [[nodiscard]]
    StringView type_name(Value value) noexcept;

    [[nodiscard]]
    u32 value_hash(Value value) noexcept;

    [[nodiscard]]
    bool value_equal(Value lhs, Value rhs) noexcept;

    [[nodiscard]]
    bool value_identical(Value lhs, Value rhs) noexcept;

    // 调用前须保证同属数值域或字符串域；NaN 排在其他数值前以维持严格弱序。
    [[nodiscard]]
    bool value_less(Value lhs, Value rhs) noexcept;

    // 整值浮点补 .0，以区别整数。
    [[nodiscard]]
    String format_f64(f64 value);

    // 纯 C++ 显示：字符串不加引号，其余同 value_repr；不执行用户代码。
    [[nodiscard]]
    String value_str(Value value);

    // 调试渲染不执行用户代码，对象走 repr。
    [[nodiscard]]
    String value_repr(Value value);

    using ArgList = Span<const Value>;

} // namespace aria

// 标准容器中的 Value 键也按 === 判等，保持类型与浮点位型差异。
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
