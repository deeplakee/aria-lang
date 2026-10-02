#ifndef ARIA_NANBOXING_HPP
#define ARIA_NANBOXING_HPP

#include <bit>
#include <type_traits>
#include "common.hpp"

namespace aria {
    class Object;
}

namespace aria::nanboxing {

    // NaN-boxing 值表示(C++23):真 double 原样存储,其余类型藏进 quiet NaN 未用位型。只认领 top-2 mantissa
    // bits 置位的 kQNan 子集,避开硬件 canonical quiet NaN(0x7ff8...),使算术产生的 NaN 落回 is_f64 判定、
    // 绝不与 boxed 值冲突。非 f64 位布局:sign==1 -> 低 48 位为指针(x86-64/ARM64 用户态);sign==0 -> tag 在
    // bits 48..49:1 = nil、2 = bool(真值在 bit 0)、3 = int(48 位补码,bits 0..47)。
    class Value {
        using Obj = Object*;

    public:
        enum class Type { Nil, Bool, F64, Int, Obj };

        static constexpr u64 kQNan    = 0x7ffc000000000000ull; // sign=0, exp=all-1, mant top 2 bits set
        static constexpr u64 kSign    = 0x8000000000000000ull; // sign=1, else=all-0
        static constexpr u64 kPayload = 0x0000ffffffffffffull; // low 48 bits all-1
        static constexpr u64 kTagMask = 0x0003000000000000ull; // bits 48..49
        static constexpr u64 kTagNil  = 0x0001000000000000ull;
        static constexpr u64 kTagBool = 0x0002000000000000ull;
        static constexpr u64 kTagInt  = 0x0003000000000000ull;

        static constexpr u64 kNilBits   = kQNan | kTagNil;
        static constexpr u64 kFalseBits = kQNan | kTagBool | 0u;
        static constexpr u64 kTrueBits  = kQNan | kTagBool | 1u;

        // 默认构造为 trivial(= default,POD 可 memcpy/入 FrameStack):默认初始化 Value v; 的 bits_ 为不定值;
        // 值初始化 Value{} 是 f64 0.0 而非 nil(与 TagValue 的 Value{} 恰为合法 nil 互为镜像),需要 nil 用 nil_val()。
        constexpr Value() noexcept = default;

        [[nodiscard]]
        static constexpr Value nil_val() noexcept {
            return Value{kNilBits};
        }

        [[nodiscard]]
        static constexpr Value true_val() noexcept {
            return Value{kTrueBits};
        }

        [[nodiscard]]
        static constexpr Value false_val() noexcept {
            return Value{kFalseBits};
        }

        [[nodiscard]]
        static constexpr Value from_bool(const bool value) noexcept {
            return Value{value ? kTrueBits : kFalseBits};
        }

        [[nodiscard]]
        static constexpr Value from_f64(const f64 value) noexcept {
            // 把任意 NaN 规范化为单一位型(硬件 canonical quiet NaN),回出时绝不会被误判为
            // boxed 值;与 TagValue 的同款契约互为对齐点(=== 按位比较下 NaN===NaN 的前提)。
            if (value != value) {
                return Value{0x7ff8000000000000ull};
            }
            return Value{f64_to_u64(value)};
        }

        [[nodiscard]]
        static constexpr Value from_int(int64_t value) noexcept {
            // 按 48 位补码整数存储(值域 [-2^47, 2^47),越界 ASSERT)。
            ASSERT(value >= -(static_cast<int64_t>(1) << 47) && value < (static_cast<int64_t>(1) << 47),
                   "integer does not fit in 48-bit NaN-box payload");
            return Value{kQNan | kTagInt | (static_cast<u64>(value) & kPayload)};
        }

        [[nodiscard]]
        static constexpr Value from_i32(const i32 value) noexcept {
            return from_int(value);
        }

        [[nodiscard]]
        static Value from_obj(Obj object) noexcept {
            u64 bits = ptr_to_u64(object);
            ASSERT((bits & ~kPayload) == 0, "pointer exceeds 48-bit payload");
            return Value{kSign | kQNan | (bits & kPayload)};
        }

        [[nodiscard]]
        constexpr bool is_nil() const noexcept {
            return bits_ == kNilBits;
        }

        [[nodiscard]]
        constexpr bool is_bool() const noexcept {
            return is_box() && !(bits_ & kSign) && (bits_ & kTagMask) == kTagBool;
        }

        [[nodiscard]]
        constexpr bool is_f64() const noexcept {
            return (bits_ & kQNan) != kQNan;
        }

        [[nodiscard]]
        constexpr bool is_int() const noexcept {
            return is_box() && !(bits_ & kSign) && (bits_ & kTagMask) == kTagInt;
        }

        [[nodiscard]]
        constexpr bool is_box() const noexcept {
            return !is_f64();
        }

        [[nodiscard]]
        constexpr bool is_obj() const noexcept {
            return is_box() && (bits_ & kSign);
        }


        [[nodiscard]]
        constexpr Type type() const noexcept {
            if (is_f64())
                return Type::F64;
            if (bits_ & kSign)
                return Type::Obj;
            switch (bits_ & kTagMask) {
                case kTagNil:
                    return Type::Nil;
                case kTagBool:
                    return Type::Bool;
                default:
                    return Type::Int;
            }
        }

        [[nodiscard]]
        constexpr const char* type_name() const noexcept {
            return type_name(type());
        }

        // 取值族(debug 下 ASSERT 类型匹配)
        [[nodiscard]]
        bool as_bool() const noexcept {
            ASSERT(is_bool(), "value is not Bool");
            return (bits_ & 1u) != 0;
        }

        [[nodiscard]]
        f64 as_f64() const noexcept {
            ASSERT(is_f64(), "value is not F64");
            return u64_to_f64(bits_);
        }

        [[nodiscard]]
        int64_t as_int() const noexcept {
            ASSERT(is_int(), "value is not Int");
            // 48 位载荷符号扩展:先把 bit 47 左移进符号位,再算术右移回来(signed >> 在 C++20 为算术移位)。
            return static_cast<int64_t>(bits_ << 16) >> 16;
        }
        [[nodiscard]]
        i32 as_i32() const noexcept {
            return static_cast<i32>(as_int());
        }

        [[nodiscard]]
        Obj as_obj() const noexcept {
            ASSERT(is_obj(), "value is not Obj");
            return u64_to_ptr(bits_ & kPayload);
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
        explicit constexpr Value(const u64 bits) noexcept : bits_(bits) {}

        static u64 f64_to_u64(const f64 value) { return std::bit_cast<u64>(value); }
        static f64 u64_to_f64(const u64 bits) { return std::bit_cast<f64>(bits); }
        static u64 ptr_to_u64(Obj object) { return static_cast<u64>(reinterpret_cast<uintptr_t>(object)); }
        static Obj u64_to_ptr(const u64 bits) { return reinterpret_cast<Obj>(bits); }

        u64 bits_;
    };

    static_assert(sizeof(Value) == 8, "Value must be exactly 64 bits");
    static_assert(std::is_trivial_v<Value>, "Value must be a trivial class");
    static_assert(std::is_standard_layout_v<Value>, "Value must be a standard-layout (POD) class");

} // namespace aria::nanboxing

#endif // ARIA_NANBOXING_HPP
