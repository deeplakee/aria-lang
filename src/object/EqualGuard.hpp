#ifndef ARIA_EQUAL_GUARD_HPP
#define ARIA_EQUAL_GUARD_HPP

#include <algorithm>

#include "common.hpp"

namespace aria {

    class Object;

    // 比较链守卫:is_cycle 查自引用/互环(重遇同对视为相等,余归纳),再挂守卫入栈;
    // thread_local 朴素栈,RAII 配对;分配不触 GC -- EQUAL 弹栈后操作数无根,风险仅比较中途。
    class EqualGuard {
    public:
        EqualGuard(const Object* lhs, const Object* rhs) { active_pairs().emplace_back(lhs, rhs); }

        ~EqualGuard() { active_pairs().pop_back(); }

        EqualGuard(const EqualGuard&)            = delete;
        EqualGuard& operator=(const EqualGuard&) = delete;

        // 该对是否已在比较链上:线性扫,栈深 = 比较嵌套深度,极小。
        static bool is_cycle(const Object* lhs, const Object* rhs) {
            const auto& pairs = active_pairs();
            return std::ranges::find_if(pairs, [lhs, rhs](const auto& pair) {
                       return pair.first == lhs && pair.second == rhs;
                   }) != pairs.end();
        }

    private:
        // 当前比较链(每线程独立)。
        static List<Pair<const Object*, const Object*>>& active_pairs() {
            static thread_local List<Pair<const Object*, const Object*>> pairs;
            return pairs;
        }
    };

} // namespace aria

#endif // ARIA_EQUAL_GUARD_HPP
