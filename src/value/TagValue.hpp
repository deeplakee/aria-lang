#ifndef ARIA_TAGVALUE_HPP
#define ARIA_TAGVALUE_HPP

#include <bit>
#include <type_traits>
#include "common.hpp"

namespace aria {
    class Object;
}


namespace aria::tagvalue {

    class Value {
        using Obj = Object*;

    public:
        enum class Type : u8 { Nil, Bool, F64, Int, Obj };

        // 默认构造为 trivial（= default）：默认初始化 Value v; 时为不定值；
        // 值初始化 Value{} 零填充 -> tag_=0=Type::Nil（Nil 为首枚举值 0）、union 零位置 nil，
        // 故 Value{} 仍为合法 nil。这样 Value 满足 is_trivial + is_standard_layout（POD），
        // 可 memcpy、可入 FrameStack。若日后 Type 枚举顺序变动需同步 nil_val()。
        constexpr Value() noexcept = default;

        [[nodiscard]]
        static constexpr Value nil_val() noexcept {
            return Value{};
        }

        [[nodiscard]]
        static constexpr Value true_val() noexcept {
            return Value{true};
        }

        [[nodiscard]]
        static constexpr Value false_val() noexcept {
            return Value{false};
        }

        [[nodiscard]]
        static constexpr Value from_bool(bool b) noexcept {
            return Value{b};
        }

        [[nodiscard]]
        static constexpr Value from_f64(f64 d) noexcept {
            // 规范化 NaN 到单一 bit pattern(0x7ff8...,与 NanBoxing 一致),消除两表示分叉:
            // value_identical(===) 按位比较,规范化后任意两个 NaN bit 相等 -> NaN===NaN true。
            if (d != d) {
                return Value{std::bit_cast<f64>(0x7ff8000000000000ull)};
            }
            return Value{d};
        }

        [[nodiscard]]
        static constexpr Value from_int(i64 i) noexcept {
            return Value{i};
        }

        [[nodiscard]]
        static constexpr Value from_i32(i32 i) noexcept {
            return Value{static_cast<i64>(i)};
        }

        [[nodiscard]]
        static Value from_obj(Obj p) noexcept {
            return Value{p};
        }

        [[nodiscard]]
        constexpr bool is_nil() const noexcept {
            return tag_ == Type::Nil;
        }

        [[nodiscard]]
        constexpr bool is_bool() const noexcept {
            return tag_ == Type::Bool;
        }

        [[nodiscard]]
        constexpr bool is_f64() const noexcept {
            return tag_ == Type::F64;
        }

        [[nodiscard]]
        constexpr bool is_int() const noexcept {
            return tag_ == Type::Int;
        }

        [[nodiscard]]
        constexpr bool is_obj() const noexcept {
            return tag_ == Type::Obj;
        }


        [[nodiscard]]
        constexpr Type type() const noexcept {
            return tag_;
        }

        [[nodiscard]]
        bool as_bool() const noexcept {
            ASSERT(is_bool(), "value is not Bool");
            return bool_val_;
        }

        [[nodiscard]]
        f64 as_f64() const noexcept {
            ASSERT(is_f64(), "value is not F64");
            return f64_val_;
        }

        [[nodiscard]]
        i64 as_int() const noexcept {
            ASSERT(is_int(), "value is not Int");
            return int_val_;
        }
        [[nodiscard]]
        i32 as_i32() const noexcept {
            return static_cast<i32>(as_int());
        }

        [[nodiscard]]
        Obj as_obj() const noexcept {
            ASSERT(is_obj(), "value is not Obj");
            return ptr_val_;
        }

        [[nodiscard]]
        constexpr const char* type_name() const noexcept {
            return type_name(tag_);
        }

        static constexpr const char* type_name(Type t) noexcept {
            switch (t) {
                case Type::Nil:
                    return "Nil";
                case Type::Bool:
                    return "Bool";
                case Type::F64:
                    return "F64";
                case Type::Int:
                    return "Int";
                case Type::Obj:
                    return "Obj";
            }
            return "?";
        }

    private:
        // 底层构造仅供工厂方法（nil_val/true_val/false_val/from_*）内部使用，
        // 外部创建 Value 统一走工厂，与 NanBoxing 的 private 构造收口一致。
        explicit Value(const bool val) : tag_{Type::Bool} { bool_val_ = val; }

        explicit Value(const f64 val) : tag_{Type::F64} { f64_val_ = val; }

        explicit Value(const i64 val) : tag_{Type::Int} { int_val_ = val; }

        explicit Value(Obj val) : tag_{Type::Obj} { ptr_val_ = val; }

        Type tag_;
        union {
            nullptr_t nil_val_;
            bool      bool_val_;
            f64       f64_val_;
            i64       int_val_;
            Obj       ptr_val_;
        };
    };

    static_assert(sizeof(Value) == 16, "Value must be 16 bytes (1 tag + padding + 8-byte union)");
    static_assert(std::is_trivial_v<Value>, "Value must be a trivial class");
    static_assert(std::is_standard_layout_v<Value>, "Value must be a standard-layout (POD) class");
} // namespace aria::tagvalue

#endif // ARIA_TAGVALUE_HPP
