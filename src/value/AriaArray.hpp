#ifndef ARIA_ARIA_ARRAY_HPP
#define ARIA_ARIA_ARRAY_HPP

#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "value/Value.hpp"

namespace aria {

    // 绑定 Value 的 aria 数组:继承 Array<Value> 的存储与接口(push/[]/span/size...),
    //        加 trace(GC&)(遍历元素 mark_value)。
    //
    //        分层:src/memory/ 的 Array<T> 对 T 完全通用(不知 Value 为何物);src/value/ 的
    //        AriaArray 把 T 绑成 Value 并补上 GC trace。Phase 3 的 ObjList(Object 子类型)
    //        持 AriaArray 作成员,ObjList::trace 委托 arr.trace(gc)。
    //
    //        继承而非组合:直接复用 Array<Value> 全部公开接口,免转发样板。Array dtor 非虚,
    //        但本子类不作多态基(不会拿 Array<Value>* 指向 AriaArray 再 delete),故安全;
    //        未新增资源持有成员,隐式 dtor -> ~Array<Value> 自释放。不可拷贝/不可移动(继承自 Array)。
    class AriaArray : public Array<Value> {
    public:
        using Array<Value>::Array; // 继承 explicit Array(GC*) ctor

        // GC 标记:遍历所有元素,mark_value(对象元素被标根;nil/int/f64 无对象子节点)。
        // 由 owner(未来 ObjList)在 collect 的 trace 阶段调用。range-for:trace 期无 push
        // 不扩容,迭代器恒有效;ObjList 元素与各函数常量池(CodeUnit::trace 委托)均经此。
        void trace(GC& gc) const noexcept {
            for (const Value& v: *this) {
                gc.mark_value(v);
            }
        }
    };

} // namespace aria

#endif // ARIA_ARIA_ARRAY_HPP
