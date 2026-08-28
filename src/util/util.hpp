#ifndef ARIA_UTIL_HPP
#define ARIA_UTIL_HPP

#include <format>
#include <type_traits>
#include "common.hpp"
#include "io.hpp"

namespace aria::util {

    namespace detail {

        inline void emit_bytes(const u8* p, const int n, const int group_bits) {
            const int  total_bits = n * 8;
            const bool sep        = group_bits > 0;                // 是否插入分隔符
            const int  step       = sep ? group_bits : total_bits; // 不分组时当作一整组

            for (int i = total_bits - 1; i >= 0; --i) { // 高位先行
                const u8  b   = p[(i / 8)];             // 第 i 位所在字节
                const int bit = (b >> (i % 8)) & 1;
                io::print("{}", static_cast<char>('0' + bit));
                // 在当前组的最后一位之后插空格（末尾不留）
                if (sep && i % step == 0 && i != 0) {
                    io::print(" ");
                }
            }
            io::print("\n");
        }

        // splitmix64 mixing step(Vigna lowbias32 变体):64 位值雪崩成 32 位散列。
        // 常数/移位(30/27/31)专为 32 位输出低偏置调优--高位差异充分传播到低 32 位,
        // 利于 Swiss Table 取低 7 位作 h2。供数值/地址等 64 位标量哈希共用(见 hash_num/hash_addr)。
        [[nodiscard]]
        inline u32 splitmix64_mix32(u64 x) noexcept {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebull;
            x ^= x >> 31;
            return static_cast<u32>(x);
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

    // 以“高位在前、每 group_bits 位一组、空格分隔”的形式打印 n 个字节。
    // group_bits 控制多少位为一组用空格分隔，默认 8；传 0 或负数则不分隔。
    template<typename T>
    void print_binary(const T& value, const int group_bits = 8) {
        if constexpr (std::is_pointer_v<T> || std::is_same_v<T, std::nullptr_t>) {
            const auto addr = reinterpret_cast<uintptr_t>(value);
            u8         buf[sizeof(addr)];
            std::memcpy(buf, &addr, sizeof(addr));
            detail::emit_bytes(buf, sizeof(addr), group_bits);
        } else {
            // 整型、浮点及其它可平凡复制的对象：直接打印对象表示
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

    // 把 u32 按"低位在前"(小端序)拆成 4 字节:索引 0 为最低字节。
    [[nodiscard]]
    inline Vector<u8, 4> split_dword(const u32 word) noexcept {
        return {
            static_cast<u8>(word & 0xFF),
            static_cast<u8>((word >> 8) & 0xFF),
            static_cast<u8>((word >> 16) & 0xFF),
            static_cast<u8>((word >> 24) & 0xFF)};
    }
} // namespace aria::util

#endif // ARIA_UTIL_HPP
