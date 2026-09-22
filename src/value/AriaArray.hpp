#ifndef ARIA_ARIA_ARRAY_HPP
#define ARIA_ARIA_ARRAY_HPP

#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "value/Value.hpp"

namespace aria {

    // 绑定 Value 的 aria 数组:继承 Array<Value> 的存储与接口(push/[]/data/begin/end/size...),
    //        加 trace(GC&)(遍历元素 mark_value)与值相等原语 find/contains/remove。
    //
    //        分层:src/memory/ 的 Array<T> 对 T 完全通用(不知 Value 为何物);本类绑成 Value
    //        并补 GC trace 与 == 语义判等(value_equal,嵌套容器按内容递归、数值跨型相等
    //        --int 1 == f64 1.0),ObjList 持其作成员、trace 委托 arr.trace(gc)。语言面
    //        list 的 find/contains/remove 方法体即这三件的薄壳。
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

        // 首个 value_equal 命中元素的下标,未命中 nullopt。value_equal 无分配,GC-pure。
        // 元素访问走 data()[i](同 Array 自有方法的内访风格,循环域结构性保证下标)。
        [[nodiscard]]
        Opt<usize> find(const Value& target) const noexcept {
            for (usize index = 0; index < size(); ++index) {
                if (value_equal(data()[index], target)) {
                    return index;
                }
            }
            return std::nullopt;
        }

        // value_equal 成员判定。
        [[nodiscard]]
        bool contains(const Value& target) const noexcept {
            return find(target).has_value();
        }

        // 移除**全部** value_equal 命中元素(保序一趟压缩:未命中元素前移补位后截断),
        // 返回是否命中(留存数与原元素数之差即判);未命中零操作。较反复单点移除一趟
        // O(n),不受命中次数放大。
        bool remove(const Value& target) noexcept {
            const usize old_size = size();
            usize       kept     = 0;
            for (usize index = 0; index < old_size; ++index) {
                if (!value_equal(data()[index], target)) {
                    data()[kept] = data()[index];
                    ++kept;
                }
            }
            truncate(kept);
            return kept != old_size;
        }
    };

} // namespace aria

#endif // ARIA_ARIA_ARRAY_HPP
