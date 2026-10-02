#ifndef ARIA_STR_TABLE_HPP
#define ARIA_STR_TABLE_HPP

#include "common.hpp"

namespace aria::str_table {

    // VM 常量串注册表:VM 运行期按名取用、须恒久存活的字符串常量。
    inline constexpr StringView kConstants[] = {
            "__add__",   // binary +
            "__sub__",   // binary -
            "__mul__",   // binary *
            "__div__",   // binary /
            "__mod__",   // binary %
            "__lt__",    // comparison <
            "__le__",    // comparison <=
            "__gt__",    // comparison >
            "__ge__",    // comparison >=
            "__neg__",   // unary negation
            "__call__",  // call hook
            "_message",  // Exception face 字段键
            "_code",     // Exception face 字段键
            "suspended", // coroutine status 串
            "running",   // coroutine status 串
            "normal",    // coroutine status 串
            "done",      // coroutine status 串
            "failed",    // coroutine status 串
    };

    // 表长:常量串表条数(供 VM 预置 string_constants_ 格位,同 kValueRegisterCount)。
    inline constexpr usize kCount = std::size(kConstants);

    // 按键查表下标(consteval 逐条比对;查不到返 nullopt,消费者 static_assert 拒表外键)。
    [[nodiscard]]
    consteval Opt<usize> index_of(const StringView key) {
        for (usize index = 0; index < kCount; ++index) {
            if (kConstants[index] == key) {
                return index;
            }
        }
        return std::nullopt;
    }

} // namespace aria::str_table

#endif // ARIA_STR_TABLE_HPP
