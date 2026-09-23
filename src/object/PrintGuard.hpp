#ifndef ARIA_PRINT_GUARD_HPP
#define ARIA_PRINT_GUARD_HPP

#include <algorithm>

#include "common.hpp"

namespace aria {

    class Object;

    // 在印路径守卫:递归渲染子值的 debug_repr(list 等容器)入口先 is_cycle 查自引用/互环
    // (命中即返 "[...]" 截断,防无限递归栈溢出),再挂本守卫把自身入栈。thread_local 栈
    // 即当前渲染链(每线程独立);RAII 出栈由逆序析构与调用栈严格配对,中途 bad_alloc 亦
    // 不漏 pop。非递归类型(字符串/函数等)不递归渲染子值,无需挂。
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
        static List<const Object*>& active_path() {
            static thread_local List<const Object*> path;
            return path;
        }
    };

} // namespace aria

#endif // ARIA_PRINT_GUARD_HPP
