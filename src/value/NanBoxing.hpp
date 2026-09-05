#ifndef ARIA_NANBOXING_HPP
#define ARIA_NANBOXING_HPP

#include <bit>
#include <type_traits>
#include "common.hpp"

namespace aria {
    class Object;
}

namespace aria::nanboxing {

    // NaN-boxed value for the aria interpreter (C++23).
    //
    // Every value fits in a single 64-bit IEEE-754 double. Real doubles are stored
    // verbatim; all other types hide inside the unused bit patterns of a quiet NaN.
    //
    // IEEE-754 binary64 layout:
    //   [sign:1][exponent:11][mantissa:52]
    // A NaN has exponent == 0x7FF and a non-zero mantissa. We claim the subset of
    // NaN space whose top two mantissa bits are set (the "box" pattern below); this
    // dodges the hardware canonical quiet NaN (0x7ff8...), so arithmetic NaNs stay
    // classified as f64 and never collide with a boxed value.
    //
    // Box layout (when the value is NOT a f64):
    //   sign == 1  -> pointer, payload = low 48 bits (x86-64 / ARM64 user space)
    //   sign == 0  -> tag in bits 48..49:
    //                   1 = nil
    //                   2 = bool   (truth value in bit 0)
    //                   3 = int    (48-bit two's-complement payload in bits 0..47)
    // Pointers claim the whole sign==1 half: a 48-bit pointer needs no tag slot,
    // which leaves both tag slots free for Nil/Bool/Int.
    class Value {
        using Obj = Object*;

    public:
        enum class Type { Nil, Bool, F64, Int, Obj };

        // --- bit-pattern constants -------------------------------------------
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

        // --- construction -----------------------------------------------------
        // 默认构造为 trivial（= default）：默认初始化 Value v; 时 bits_ 为不定值；
        // 值初始化 Value{} 零填充（0 即 f64 0.0，并非 nil）。需要 nil 请用 nil_val()。
        // 这样 Value 满足 is_trivial + is_standard_layout（POD），可 memcpy、可入 FrameStack。
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
        static constexpr Value from_bool(bool b) noexcept {
            return Value{b ? kTrueBits : kFalseBits};
        }

        [[nodiscard]]
        static constexpr Value from_f64(f64 d) noexcept {
            // Canonicalize any NaN to the hardware quiet NaN so it can never be
            // mistaken for a boxed value on the way back out.
            if (d != d) {
                return Value{0x7ff8000000000000ull};
            }
            return Value{f64_to_u64(d)};
        }

        [[nodiscard]]
        static constexpr Value from_int(int64_t i) noexcept {
            // Stored as a 48-bit two's-complement integer (range +/- 2^47).
            ASSERT(i >= -(static_cast<int64_t>(1) << 47) && i < (static_cast<int64_t>(1) << 47),
                   "integer does not fit in 48-bit NaN-box payload");
            return Value{kQNan | kTagInt | (static_cast<u64>(i) & kPayload)};
        }

        [[nodiscard]]
        static constexpr Value from_i32(i32 i) noexcept {
            return from_int(i); // i32 always fits in the 48-bit payload
        }

        [[nodiscard]]
        static Value from_obj(Obj p) noexcept {
            u64 u = ptr_to_u64(p);
            ASSERT((u & ~kPayload) == 0, "pointer exceeds 48-bit payload");
            return Value{kSign | kQNan | (u & kPayload)};
        }

        // --- type tests -------------------------------------------------------
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
                    return Type::Int; // kTagInt
            }
        }

        [[nodiscard]]
        constexpr const char* type_name() const noexcept {
            return type_name(type());
        }

        // --- extraction (assert the matching type in debug builds) ------------
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
            // Sign-extend the 48-bit payload: shift bit 47 into the sign position,
            // then arithmetic-shift back (signed >> is arithmetic in C++20).
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

        // --- raw access & equality -------------------------------------------
        [[nodiscard]]
        constexpr u64 bits() const noexcept {
            return bits_;
        }

        [[nodiscard]]
        static constexpr Value from_bits(u64 b) noexcept {
            return Value{b};
        }

        // Bitwise equality. Note: two f64 NaNs compare unequal under `==` but here
        // identical bit patterns are equal, and -0.0 != 0.0 in bits. Use as_f64()
        // and float comparison if you need IEEE numeric semantics.
        [[nodiscard]]
        constexpr bool same_bits(Value o) const noexcept {
            return bits_ == o.bits_;
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
        explicit constexpr Value(u64 bits) noexcept : bits_(bits) {}

        static u64 f64_to_u64(f64 d) { return std::bit_cast<u64>(d); }
        static f64 u64_to_f64(u64 u) { return std::bit_cast<f64>(u); }
        static u64 ptr_to_u64(Obj p) { return static_cast<u64>(reinterpret_cast<uintptr_t>(p)); }
        static Obj u64_to_ptr(u64 u) { return reinterpret_cast<Obj>(u); }

        u64 bits_;
    };

    static_assert(sizeof(Value) == 8, "Value must be exactly 64 bits");
    static_assert(std::is_trivial_v<Value>, "Value must be a trivial class");
    static_assert(std::is_standard_layout_v<Value>, "Value must be a standard-layout (POD) class");

} // namespace aria::nanboxing

#endif // ARIA_NANBOXING_HPP
