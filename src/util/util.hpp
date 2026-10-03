#ifndef ARIA_UTIL_HPP
#define ARIA_UTIL_HPP

#include <charconv>
#include <cstring>
#include <format>
#include <system_error>
#include <type_traits>
#include "aria.hpp"
#include "common.hpp"
#include "io.hpp"

namespace aria::util {

    namespace detail {

        inline void emit_bytes(const u8* p, const int n, const int group_bits) {
            const int  total_bits = n * 8;
            const bool sep        = group_bits > 0;
            const int  step       = sep ? group_bits : total_bits;

            for (int i = total_bits - 1; i >= 0; --i) {
                const u8  b   = p[(i / 8)];
                const int bit = (b >> (i % 8)) & 1;
                io::print("{}", static_cast<char>('0' + bit));
                // 在当前组的最后一位之后插空格（末尾不留）
                if (sep && i % step == 0 && i != 0) {
                    io::print(" ");
                }
            }
            io::print("\n");
        }

        // splitmix64 mixing step(Vigna lowbias32 变体):64 位雪崩成 32 位散列;常数/移位为其
        // 32 位输出低偏置调优,高位差异充分传播到低 32 位(Swiss Table 取低位作 h2)。
        [[nodiscard]]
        inline u32 splitmix64_mix32(u64 value) noexcept {
            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27;
            value *= 0x94d049bb133111ebull;
            value ^= value >> 31;
            return static_cast<u32>(value);
        }

        // FNV-1a 32-bit 字节哈希核心,自 state 起逐字节处理(空始传 basis,续算传前缀终态)。
        inline constexpr u32 kFnv1aBasis = 2166136261u; // FNV-1a offset basis

        [[nodiscard]]
        inline u32 fnv1a_32(const u32 state, const u8* data, const usize len) noexcept {
            u32 h = state;
            for (usize i = 0; i < len; ++i) {
                h ^= data[i];
                h *= 16777619u; // FNV-1a prime
            }
            return h;
        }

    } // namespace detail

    // 数值哈希:Int/F64 的 64 位位模式经 splitmix64 混合。
    [[nodiscard]]
    inline u32 hash_num(const u64 bits) noexcept {
        return detail::splitmix64_mix32(bits);
    }

    // 地址哈希:指针位模式经 splitmix64 混合。
    [[nodiscard]]
    inline u32 hash_addr(const void* p) noexcept {
        return detail::splitmix64_mix32(static_cast<u64>(reinterpret_cast<uintptr_t>(p)));
    }

    // 续算:state 为前缀终态(hash(a) 续算 b 即 hash(a+b))。
    [[nodiscard]]
    inline u32 hash_str(const u32 state, const StringView s) noexcept {
        return detail::fnv1a_32(state, reinterpret_cast<const u8*>(s.data()), s.size());
    }

    // 字符串哈希:FNV-1a 32-bit。
    [[nodiscard]]
    inline u32 hash_str(const StringView s) noexcept {
        return hash_str(detail::kFnv1aBasis, s);
    }

    // 转义为字符串字面量内容形态(不含外层引号);控制字符(<0x20)-> \x{HH},非 ASCII 透传。
    [[nodiscard]]
    inline String escape_string(const StringView s) {
        String out;
        out.reserve(s.size() + 2);
        for (const char c: s) {
            switch (const auto uc = static_cast<u8>(c)) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                default:
                    if (uc < 0x20) {
                        out += std::format("\\x{{{:02X}}}", static_cast<u32>(uc));
                    } else {
                        out += static_cast<char>(uc);
                    }
                    break;
            }
        }
        return out;
    }

    // 两段拼接成新串:一次定容,两段 append。
    [[nodiscard]]
    inline String concat_string(const StringView lhs, const StringView rhs) {
        String out;
        out.reserve(lhs.size() + rhs.size());
        out.append(lhs).append(rhs);
        return out;
    }

    // 按"高位在前、每 group_bits 位一组、空格分隔"打印底层字节;group_bits 传 0 或负数则不分隔。
    template<typename T>
    void print_binary(const T& value, const int group_bits = 8) {
        if constexpr (std::is_pointer_v<T> || std::is_same_v<T, std::nullptr_t>) {
            const auto addr = reinterpret_cast<uintptr_t>(value);
            u8         buf[sizeof(addr)];
            std::memcpy(buf, &addr, sizeof(addr));
            detail::emit_bytes(buf, sizeof(addr), group_bits);
        } else {
            u8 buf[sizeof(T)];
            std::memcpy(buf, &value, sizeof(T));
            detail::emit_bytes(buf, sizeof(T), group_bits);
        }
    }

    inline void print_compiler_info() {
#ifdef _MSC_VER
        io::println("MSVC {}", _MSC_VER);
#elif defined(__clang__)
        io::println("Clang {}", __clang_version__);
#elif defined(__GNUC__)
        io::println("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
    }

    // 任意对象指针 -> void*,const 感知:保留 const 限定(const 成员里 this 为 const Object*,不能 cast 非常量 void*)。
    template<typename T>
        requires std::is_pointer_v<T>
    [[nodiscard]]
    auto to_void_ptr(T ptr) noexcept {
        if constexpr (std::is_const_v<std::remove_pointer_t<T>>) {
            return static_cast<const void*>(ptr);
        } else {
            return static_cast<void*>(ptr);
        }
    }

    // 把小端的 low/high 两字节拼成 u16。
    [[nodiscard]]
    constexpr u16 make_u16(const u8 low, const u8 high) noexcept {
        return static_cast<u16>(static_cast<u16>(low) | (static_cast<u16>(high) << 8));
    }

    // 把 u16 按"低位在前"(小端序)拆成 2 字节:索引 0 为最低字节。
    [[nodiscard]]
    inline Vector<u8, 2> split_word(const u16 word) noexcept {
        return {static_cast<u8>(word & 0xFF), static_cast<u8>(word >> 8)};
    }

    // 取栈顶元素并弹出(经 move);调用方须保证栈非空。
    template<typename T>
    [[nodiscard]]
    T pop_top(Stack<T>& stack) {
        T top = std::move(stack.top());
        stack.pop();
        return top;
    }

    // 取走 optional 当前值并置空:返回被取走的 Opt<T>(可能为空)。
    template<typename T>
    [[nodiscard]]
    Opt<T> take(Opt<T>& opt) {
        return std::exchange(opt, std::nullopt);
    }

    // 取走指针当前值并置空:返回被取走的指针(可能为空)。
    template<typename T>
    [[nodiscard]]
    T* take(T*& ptr) {
        return std::exchange(ptr, nullptr);
    }

    // 无符号整数(排除 bool:bool 的归零语义是置 false,自成下方具名重载,不吃归零形态)。
    template<typename T>
    concept UnsignedInteger = std::is_unsigned_v<T> && !std::is_same_v<T, bool>;

    // 取走无符号整数当前值并归零:返回被取走的值。
    template<UnsignedInteger T>
    [[nodiscard]]
    T take(T& value) {
        return std::exchange(value, T{0});
    }

    // 取走 bool 当前值并置 false:与非模板重载分立,精确匹配优先。
    [[nodiscard]]
    inline bool take(bool& flag) {
        return std::exchange(flag, false);
    }

    // 把 u32 按"低位在前"(小端序)拆成 4 字节:索引 0 为最低字节。
    [[nodiscard]]
    inline Vector<u8, 4> split_dword(const u32 word) noexcept {
        return {static_cast<u8>(word & 0xFF), static_cast<u8>((word >> 8) & 0xFF), static_cast<u8>((word >> 16) & 0xFF),
                static_cast<u8>((word >> 24) & 0xFF)};
    }

    // 两下标距离(绝对值差):取大减小,无符号域恒不下溢。
    [[nodiscard]]
    constexpr usize abs_diff(const usize lhs, const usize rhs) noexcept {
        return lhs <= rhs ? rhs - lhs : lhs - rhs;
    }

    // 下标负索引解析(从尾计数约定):-1 = 末元素、-size = 首元素,正数原样;归一化后 < 0 或 >= size
    // 即越界(nullopt)。raw 负支加法不下溢(size <= i64max)。
    [[nodiscard]]
    inline Opt<usize> resolve_index(const i64 raw, const usize size) noexcept {
        const auto signed_size = static_cast<i64>(size);
        const i64  index       = raw < 0 ? raw + signed_size : raw;
        if (index < 0 || index >= signed_size) {
            return std::nullopt;
        }
        return index;
    }

    // 位置解析(插入位语义,负数从尾计数):-1 = 末元素之前、-size = 首元素之前,正数原样;上界放宽到
    // == size(追加位,下标域 [-(size), size) 外多出的唯一合法值);越界返 nullopt。
    [[nodiscard]]
    inline Opt<usize> resolve_position(const i64 raw, const usize size) noexcept {
        const auto signed_size = static_cast<i64>(size);
        const i64  position    = raw < 0 ? raw + signed_size : raw;
        if (position < 0 || position > signed_size) {
            return std::nullopt;
        }
        return position;
    }

    // 整串十进制整数文本解析(数据语法,非源码字面量):收 [+-][0-9]+,整串消费且落 i48 值域内才 somed。
    // 不跳空白、不收下划线/进制前缀(那是源码字面量语法);域闸设此:越域值经 Value::from_int 会被静默截尾。
    [[nodiscard]]
    inline Opt<i64> parse_int_text(const StringView text) {
        const auto body  = text.starts_with('+') ? text.substr(1) : text;
        const auto first = body.data();
        const auto last  = body.data() + body.size();
        i64        value = 0;
        if (const auto [end, ec] = std::from_chars(first, last, value, 10);
            ec != std::errc{} || end != last || value < kIntMin || value > kIntMax) {
            return std::nullopt;
        }
        return value;
    }

    // 整串十进制浮点文本解析:收整数形/小数形/指数形与 inf/nan;整串消费,越域(如 1e400)与解析失败同返 nullopt。
    // 数据语法同 parse_int_text:不跳空白、不收下划线/进制前缀。
    [[nodiscard]]
    inline Opt<f64> parse_float_text(const StringView text) {
        const auto body  = text.starts_with('+') ? text.substr(1) : text;
        const auto first = body.data();
        const auto last  = body.data() + body.size();
        f64        value = 0.0;
        if (const auto [end, ec] = std::from_chars(first, last, value); ec != std::errc{} || end != last) {
            return std::nullopt;
        }
        return value;
    }

    // 序列化拼接:range 逐元素经 transform 转 String 后以 delimiter 连接(元素序 = range 迭代序)。
    template<typename Range, typename Fn>
    [[nodiscard]]
    String join(const Range& range, const StringView delimiter, Fn&& transform) {
        String out;
        bool   first = true;
        for (const auto& element: range) {
            if (!first) {
                out += delimiter;
            }
            first = false;
            out += transform(element);
        }
        return out;
    }

    // 定长字符串载体,供字符串字面量作 NTTP:C++20 类类型 NTTP 须为结构化类型(数据成员全 public 非 mutable),
    // string_view 私有成员当不了、const char* 收不了字面量。存储含 '\0'(data_ 即 C 串),view() 不含,比较才恒命中。
    template<usize capacity>
    class FixedString {
    public:
        char data_[capacity]{}; // public 是结构化类型的硬条件,非风格选择

        consteval FixedString(const char (&s)[capacity]) {
            for (usize i = 0; i < capacity; ++i) {
                data_[i] = s[i]; // 连 '\0' 一起拷
            }
        }

        [[nodiscard]]
        constexpr StringView view() const noexcept {
            return StringView{data_, capacity - 1};
        }
    };
} // namespace aria::util

#endif // ARIA_UTIL_HPP
