#ifndef ARIA_TYPE_HPP
#define ARIA_TYPE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aria {
    using i8    = int8_t;
    using u8    = uint8_t;
    using i16   = int16_t;
    using u16   = uint16_t;
    using i32   = int32_t;
    using u32   = uint32_t;
    using i64   = int64_t;
    using u64   = uint64_t;
    using f32   = float;
    using f64   = double;
    using isize = std::ptrdiff_t;
    using usize = std::size_t;

    using StringView = std::string_view;

    using String = std::string;

    template<typename T>
    using List = std::vector<T>;

    template<typename T, usize N>
    using Vector = std::array<T, N>;

    template<typename T>
    using Stack = std::stack<T>;

    template<typename T1, typename T2>
    using HashMap = std::unordered_map<T1, T2>;

    template<typename T>
    using HashSet = std::unordered_set<T>;

    template<typename T1, typename T2>
    using Pair = std::pair<T1, T2>;

    template<typename T>
    using UPtr = std::unique_ptr<T>;

    template<typename T>
    using SPtr = std::shared_ptr<T>;

    template<typename... Args>
    using Tuple = std::tuple<Args...>;

    template<typename T>
    using Span = std::span<T>;

    template<typename T, typename E>
    using Result = std::expected<T, E>;

    template<typename T>
    using Opt = std::optional<T>;

} // namespace aria

#endif // ARIA_TYPE_HPP
