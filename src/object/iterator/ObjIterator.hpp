#ifndef ARIA_OBJ_ITERATOR_HPP
#define ARIA_OBJ_ITERATOR_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class AriaVM;
    class ObjString;

    // 迭代器基类(ObjType::ITERATOR,type(it) 恒 "Iterator"):迭代协议的引擎缝。每源一个小子类、
    // 各持自然游标(list 下标 / string 字节偏移 / map 槽位 / range 区间当前值,源码同目录),基类
    // 只钉三件契约:has_next 纯查询、next 越界 fail、trace 标各自的源(纯虚钉住,忘标 = 编译错)。
    // 语言方法面(has_next/next 经 Iterator bootstrap 类表恒绑定)住 runtime/builtins/IteratorClass;
    // load_field / load_field_bound override 基类一次,全子类共享。debug_repr 渲染 "<iterator>";equals 默认地址判等。
    class ObjIterator : public Object {
    public:
        ~ObjIterator() override = default; // 子类成员是 GC 对象/标量,不归本类释放

        ObjIterator(const ObjIterator&)            = delete;
        ObjIterator& operator=(const ObjIterator&) = delete;
        ObjIterator(ObjIterator&&)                 = delete;
        ObjIterator& operator=(ObjIterator&&)      = delete;

        // 各子类标各自的源(子类的被遍历者成员),忘标 = 编译错。
        void trace(GC& gc) const noexcept override = 0;

        // 各子类返回 sizeof(自身)。
        [[nodiscard]]
        usize size() const noexcept override = 0;

        // 调试渲染:"<iterator>"(源类型不进文案);显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

        // 裸读 override(基类一次、全子类共享):同一趟类表查找但不铸 ObjBoundMethod,直取类表原生值。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:查 Iterator bootstrap 类表,命中自持 new_bound_method 恒绑 this(权威注见
        // Object.hpp)。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

        // 引擎缝:是否还有下一个元素。纯查询 --无分配、无 fail,故不收 vm。
        [[nodiscard]]
        virtual bool has_next() const noexcept = 0;

        // 引擎缝:取下一元素并推进游标。越界 `return vm.fail(IterationExhausted)` 一行
        //(FailSignal 哨兵,nullopt ⟺ 已 fail);返回值是元素拷贝。
        [[nodiscard]]
        virtual Opt<Value> next(AriaVM& vm) = 0;

    protected:
        // 仅子类可造;地址哈希型。
        ObjIterator();
    };

} // namespace aria

#endif // ARIA_OBJ_ITERATOR_HPP
