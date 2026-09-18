#ifndef ARIA_EQUAL_GUARD_HPP
#define ARIA_EQUAL_GUARD_HPP

#include <algorithm>

#include "common.hpp"

namespace aria {

    class Object;

    // 在比路径守卫:递归比较子值的 equals(list 等容器)入口先 is_cycle 查自引用/互环
    // (命中即返 true,余归纳闭合:同对重遇视为相等,正则树同构判等),再挂本守卫把
    // (lhs, rhs) 对入栈。thread_local 栈即当前比较链(每线程独立),RAII 出栈由逆序析构
    // 与调用栈严格配对。分配走 std::allocator,不触发 GC collect,GC-pure 契约守得住
    //(EQUAL 弹栈后操作数无根,风险仅在比较中途发生 collect)。非递归类型(字符串/函数
    // 等)不递归比较子值,无需挂。
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
