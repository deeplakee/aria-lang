#ifndef ARIA_ARIA_ARRAY_HPP
#define ARIA_ARIA_ARRAY_HPP

#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "value/Value.hpp"

namespace aria {

    // 绑定 Value 的 aria 数组:继承 Array<Value> 的存储与接口(push/[]/data/begin/end/size...),
    //        加 trace(GC&)(遍历元素 mark_value)。
    //
    //        分层:src/memory/ 的 Array<T> 对 T 完全通用(不知 Value 为何物);本类绑成 Value
    //        并补 GC trace,ObjList 持其作成员、trace 委托 arr.trace(gc)。
    //        继承而非组合:直接复用全部公开接口;基类 dtor 非虚但本子类不作多态基,故安全。
    //        不可拷贝/不可移动(继承自 Array)。
    class AriaArray : public Array<Value> {
    public:
        using Array<Value>::Array; // 继承 explicit Array(GC*) ctor

        // GC 标记:遍历所有元素 mark_value(nil/int/f64 无对象子节点)。由 owner(ObjList)
        // 在 collect 的 trace 阶段调用;trace 期无 push 不扩容,range-for 迭代器恒有效。
        void trace(GC& gc) const noexcept {
            for (const Value& v: *this) {
                gc.mark_value(v);
            }
        }
    };

} // namespace aria

#endif // ARIA_ARIA_ARRAY_HPP
