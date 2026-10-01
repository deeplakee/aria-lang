#ifndef ARIA_PRINT_GUARD_HPP
#define ARIA_PRINT_GUARD_HPP

#include <algorithm>

#include "common.hpp"

namespace aria {

    class Object;

    // 在印路径守卫:与 EqualGuard 成对(机制论证见其头注:thread_local 朴素栈、RAII 逆序析构
    // 严格配对、线性扫极小、分配不触 GC)。差异:记录单位是**单对象**而非 (lhs, rhs) 对--
    // 渲染只需「在印对象重遇」即截断 "[...]"(比较侧同对重遇视为相等才有余归纳闭合问题)。
    // 非递归类型(字符串/函数等)不递归渲染子值,无需挂。
    class PrintGuard {
    public:
        explicit PrintGuard(const Object* object) { active_path().push_back(object); }

        ~PrintGuard() { active_path().pop_back(); }

        PrintGuard(const PrintGuard&)            = delete;
        PrintGuard& operator=(const PrintGuard&) = delete;

        // 对象是否已在当前渲染路径上:线性扫,路径长度 = 渲染嵌套深度,极小。
        static bool is_cycle(const Object* object) {
            const auto& path = active_path();
            return std::ranges::find(path, object) != path.end();
        }

    private:
        // 当前渲染链(每线程独立)。
        static List<const Object*>& active_path() {
            static thread_local List<const Object*> path;
            return path;
        }
    };

} // namespace aria

#endif // ARIA_PRINT_GUARD_HPP
