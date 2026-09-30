#ifndef ARIA_LIST_CLASS_HPP
#define ARIA_LIST_CLASS_HPP

#include "common.hpp"
#include "runtime/value_register.hpp" // kList*FnOffset(寄存器实现格偏移)

namespace aria {

    class GC;
    class ObjClass;

    // list 方法面(List bootstrap 类)的自持单元:**公有面 = make_class 加算子实现缓存清单**。
    // 方法体与方法表仍住 ListClass.cpp 匿名命名空间(方法面是 VM 侧语言面,object 层保持纯表示)。
    // VM 只在 bootstrap_list_class 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入寄存器/入格
    // 即根)。
    class ListClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面,以裸指针返回;发布进寄存器 ListClass 格由
        // 调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);

        // 算子实现缓存清单:两个算子钩子名 -> AriaVM 寄存器实现格,bootstrap_list_class 装载类表后
        // 据此逐条把实现拷进 ListAddFn/ListMulFn 格(算子派发热路径直读,免每次过类表查找;类表
        // 仍是规范家,bootstrap 后无写点故两份恒一致,DEBUG 缺格即断言)。键为钩子拼写,bootstrap
        // 消费端经 new_string 驻留命中取串(皆注册表条目,零分配)。
        static constexpr Pair<StringView, u8> kOperatorFns[] = {
                {"__add__", kListAddFnOffset},
                {"__mul__", kListMulFnOffset},
        };
    };

} // namespace aria

#endif // ARIA_LIST_CLASS_HPP
