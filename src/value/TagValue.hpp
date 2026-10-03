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

        // 默认构造为 trivial(= default,POD 可 memcpy/入 FrameStack):默认初始化为不定值,值初始化 Value{} 零填充
        // 恰为合法 nil(tag_=0=Type::Nil 首枚举、union 零位 nullptr_t)。与 NanBoxing 互为镜像:那边 Value{} = f64 0.0 非
        // nil。
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
        static constexpr Value from_bool(const bool value) noexcept {
            return Value{value};
        }

        [[nodiscard]]
        static constexpr Value from_f64(const f64 value) noexcept {
            // 规范化 NaN 到单一 bit pattern(0x7ff8...,与 NanBoxing 一致),消除两表示分叉:
            // value_identical(===) 按位比较,规范化后任意两个 NaN bit 相等 -> NaN===NaN true。
            if (value != value) {
                return Value{std::bit_cast<f64>(0x7ff8000000000000ull)};
            }
            return Value{value};
        }

        [[nodiscard]]
        static constexpr Value from_int(const i64 value) noexcept {
            return Value{value};
        }

        [[nodiscard]]
        static constexpr Value from_i32(const i32 value) noexcept {
            return Value{static_cast<i64>(value)};
        }

        [[nodiscard]]
        static Value from_obj(Obj object) noexcept {
            ASSERT(object != nullptr, "null object pointer");
            return Value{object};
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

        static constexpr const char* type_name(const Type t) noexcept {
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
        // 底层构造仅供工厂方法内部使用,外部创建 Value 统一走工厂(private 收口与 NanBoxing 一致)。
        explicit Value(const bool val) : tag_{Type::Bool} { bool_val_ = val; }

        explicit Value(const f64 val) : tag_{Type::F64} { f64_val_ = val; }

        explicit Value(const i64 val) : tag_{Type::Int} { int_val_ = val; }

        explicit Value(Obj val) : tag_{Type::Obj} { ptr_val_ = val; }

        Type tag_;
        union {
            // 首成员占位:与 tag_=0 配对,保证零填充(Value{})恰为合法 nil 位型;读取恒经 tag_ 分派,本成员不可读。
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
