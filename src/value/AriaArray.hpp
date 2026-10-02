#ifndef ARIA_ARIA_ARRAY_HPP
#define ARIA_ARIA_ARRAY_HPP

#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "value/Value.hpp"

namespace aria {

    // 绑定 Value 的 aria 数组:继承 Array<Value> 全部存储与接口,加 trace(GC&)(遍历元素 mark_value)与
    // find/contains/remove(按 value_equal,嵌套容器按内容递归)。基类 dtor 非虚但本子类不作多态基,故安全;不可拷贝/移动。
    class AriaArray : public Array<Value> {
    public:
        using Array<Value>::Array; // 继承 explicit Array(GC*) ctor

        // trace 期无 push 不扩容,range-for 迭代器恒有效。
        void trace(GC& gc) const noexcept {
            for (const Value& v: *this) {
                gc.mark_value(v);
            }
        }

        // 首个 value_equal 命中元素的下标,未命中 nullopt。value_equal 无分配,GC-pure。
        [[nodiscard]]
        Opt<usize> find(const Value& target) const noexcept {
            for (usize index = 0; index < size(); ++index) {
                if (value_equal(data()[index], target)) {
                    return index;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        bool contains(const Value& target) const noexcept {
            return find(target).has_value();
        }

        // 移除全部 value_equal 命中元素(保序,一趟 O(n));未命中零操作,返回是否命中。
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
