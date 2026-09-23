#ifndef ARIA_UTIL_HPP
#define ARIA_UTIL_HPP

#include <charconv>
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
            const int  step       = sep ? group_bits : total_bits; // 不分组时当作一整组

            for (int i = total_bits - 1; i >= 0; --i) { // 高位先行
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

        // splitmix64 mixing step(Vigna lowbias32 变体):64 位值雪崩成 32 位散列。常数/移位
        // (30/27/31)专为 32 位输出低偏置调优--高位差异充分传播到低 32 位,利于 Swiss Table 取低 7 位作 h2。
        // 供数值/地址等 64 位标量哈希共用。
        [[nodiscard]]
        inline u32 splitmix64_mix32(u64 value) noexcept {
            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27;
            value *= 0x94d049bb133111ebull;
            value ^= value >> 31;
            return static_cast<u32>(value);
        }

        // FNV-1a 32-bit 字节哈希核心:逐字节 h ^= byte; h *= prime。
        // 供 hash_str(StringView) 等字节序列哈希共用。
        [[nodiscard]]
        inline u32 fnv1a_32(const u8* data, const usize len) noexcept {
            u32 h = 2166136261u; // FNV-1a offset basis
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

    // 地址哈希:指针经 splitmix64 混合(对象身份哈希用)。
    [[nodiscard]]
    inline u32 hash_addr(const void* p) noexcept {
        return detail::splitmix64_mix32(static_cast<u64>(reinterpret_cast<uintptr_t>(p)));
    }

    // 字符串哈希:FNV-1a 32-bit。ObjString 构造与 InternPool 查询共用同一实现，
    // 保证同内容同 hash(驻留一致性所系)。
    [[nodiscard]]
    inline u32 hash_str(const StringView s) noexcept {
        return detail::fnv1a_32(reinterpret_cast<const u8*>(s.data()), s.size());
    }

    // 转义字符串字面量内容(不含外层引号):" \ \n \t \r,其余控制字符(<0x20)-> \x{HH},非 ASCII 透传。
    // 供反汇编器等可读化场景共用。
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

    // 以"高位在前、每 group_bits 位一组、空格分隔"的形式打印 n 个字节。
    // group_bits 控制多少位为一组用空格分隔，默认 8；传 0 或负数则不分隔。
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

    // 任意对象指针 -> void*:const 感知。对 T* 返回 void*,对 const T* 返回 const void*,
    // 保留 const 限定(const 成员里 this 为 const Object*,不能 cast 成非 const void*)。
    // 供 std::format 的 {:p} / 打印对象地址等使用(二者均接受 const void*)。
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

    // 把小端的 low/high 两字节拼成 u16(low 在低位)。与 split_word 互逆。
    [[nodiscard]]
    constexpr u16 make_u16(const u8 low, const u8 high) noexcept {
        return static_cast<u16>(static_cast<u16>(low) | (static_cast<u16>(high) << 8));
    }

    // 把 u16 按"低位在前"(小端序)拆成 2 字节:索引 0 为最低字节。
    [[nodiscard]]
    inline Vector<u8, 2> split_word(const u16 word) noexcept {
        return {static_cast<u8>(word & 0xFF), static_cast<u8>(word >> 8)};
    }

    // 取栈顶元素并弹出:返回栈顶元素(经 move)后 pop。调用方须保证栈非空。
    // Stack (std::stack) 只暴露 top(),无"取出并弹"原子原语,故在此收口一处供共用。
    template<typename T>
    [[nodiscard]]
    T pop_top(Stack<T>& stack) {
        T top = std::move(stack.top());
        stack.pop();
        return top;
    }

    // 取走 optional 当前值并置空:返回被取走的 Opt<T>(可能为空)。Rust Option::take 同名同义;
    // std::optional 无对应成员原语,故收口 std::exchange 习语在此供共用。
    template<typename T>
    [[nodiscard]]
    Opt<T> take(Opt<T>& opt) {
        return std::exchange(opt, std::nullopt);
    }

    // 把 u32 按"低位在前"(小端序)拆成 4 字节:索引 0 为最低字节。
    [[nodiscard]]
    inline Vector<u8, 4> split_dword(const u32 word) noexcept {
        return {static_cast<u8>(word & 0xFF), static_cast<u8>((word >> 8) & 0xFF), static_cast<u8>((word >> 16) & 0xFF),
                static_cast<u8>((word >> 24) & 0xFF)};
    }

    // 两下标距离(绝对值差):取大减小,无符号域恒不下溢。命名随 C++26 std::abs_diff(同义,
    // 标准库就位后可直接替换)。纯换算无分配。
    [[nodiscard]]
    constexpr usize abs_diff(const usize lhs, const usize rhs) noexcept {
        return lhs <= rhs ? rhs - lhs : lhs - rhs;
    }

    // 下标负索引解析(从尾计数约定,list/string 下标与切片共用):-1 = 末元素、-size =
    // 首元素,正数原样;归一化后 < 0 或 >= size 即越界(nullopt)。纯换算无分配无 fail,
    // 报错文案由调用方就地烘焙(报原始键值)。raw 负支加法不下溢(size <= i64max)。
    [[nodiscard]]
    inline Opt<usize> resolve_index(const i64 raw, const usize size) noexcept {
        const auto signed_size = static_cast<i64>(size);
        const i64  index       = raw < 0 ? raw + signed_size : raw;
        if (index < 0 || index >= signed_size) {
            return std::nullopt;
        }
        return index;
    }

    // 位置解析(插入位语义,resolve_index 的姊妹函数:list.insert 消费):-1 = 末元素之前、
    // -size = 首元素之前,负数与下标同式从尾计数归一(负支加法不下溢同上),唯上界放宽到
    // == size --追加位,恰是下标域 [-(size), size) 外多出的一个合法值(负数归一后至多
    // size-1,追加位只有正拼写)。纯换算无分配无 fail,报错文案由调用方就地烘焙(报原始键值)。
    [[nodiscard]]
    inline Opt<usize> resolve_position(const i64 raw, const usize size) noexcept {
        const auto signed_size = static_cast<i64>(size);
        const i64  position    = raw < 0 ? raw + signed_size : raw;
        if (position < 0 || position > signed_size) {
            return std::nullopt;
        }
        return position;
    }

    // 整串十进制整数文本解析(数据文本,不是源码字面量语法):收前导 [+-](from_chars 只认 '-',
    // 故 '+' 先行剥掉)与 [0-9]+,整串消费且落语言 int(i48)域内才为 somed。不跳空白(要修先
    // trim)、不收下划线与进制前缀--那是源码字面量语法(Lexer 的 parse_int 一族),不是数据语法。
    // 域闸设在此而非调用方:越域值经 Value::from_int 会被静默截尾,是调用方不该有忘掉机会的坑。
    // 纯解析无分配无 fail,报错/兜底文案由调用方就地决定(内建方法处兜 nil)。
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

    // 整串十进制浮点文本解析:收整数形/小数形/指数形,亦收 inf/nan(与 str(f64) 的输出往返一致);
    // 整串消费且落 f64 值域内才为 somed--越域(如 1e400)与解析失败同路返 nullopt,不饱和成 inf。
    // 同 parse_int_text:无空白跳过、无下划线、无进制前缀,兜底交调用方。
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

    // 序列化拼接:range 逐元素经 transform 转 String,delimiter 连接(debug_repr 与语言面
    // 集合方法的共用底座;元素序 = range 迭代序)。transform 接收元素、返回可拼进 String
    // 的值(通常 String)。
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
} // namespace aria::util

#endif // ARIA_UTIL_HPP
