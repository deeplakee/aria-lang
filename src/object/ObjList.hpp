#ifndef ARIA_OBJ_LIST_HPP
#define ARIA_OBJ_LIST_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaArray.hpp"

namespace aria {

    class GC;
    class ObjRange;

    // list 对象:元素任意 Value 的顺序容器;地址哈希型(map 键按对象身份)。
    class ObjList final : public Object {
    public:
        // 元素表惰性增长,ctor 只绑分配器(首字节预留由 copy_from/push 的扩容路径自理)。
        explicit ObjList(GC& gc);

        ~ObjList() override = default; // 元素是 GC 值,不归本类释放

        ObjList(const ObjList&)            = delete;
        ObjList& operator=(const ObjList&) = delete;
        ObjList(ObjList&&)                 = delete;
        ObjList& operator=(ObjList&&)      = delete;

        // 元素表(非常量供 MAKE_LIST 的 copy_from 与 push/pop 方法填充;容器成员直曝)。
        [[nodiscard]]
        AriaArray& elements() noexcept {
            return elements_;
        }

        [[nodiscard]]
        const AriaArray& elements() const noexcept {
            return elements_;
        }

        void trace(GC& gc) const noexcept override;

        // 壳定长(elements_ 内联在壳内,元素缓冲由其 Buffer 自持)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjList);
        }

        // 内容相等:同为 list 且长度相等且逐元素 value_equal;其余(含跨类型)恒 false。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 调试渲染:`[1, "ab"]`;显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

        // 裸读 override:委托 List bootstrap 类表直取原生值。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:同一查找命中恒绑 this。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

        // 整数键:负数从尾计数、归一化后越界即报(越界值就地拼进文案),非整数键 TypeMismatch,查读无分配。
        // Range 键 = 切片:产出新 list、只读(路径有分配),段解析收口 resolve_slice_bounds。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 下标写入:键检查同读,不自动增长(追加走 push 方法),写已存槽恒成功;Range 键落统一文案(切片写只读)。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

        // 算子协议 override(内建直给,不经成员查找):读 bootstrap 期注册进类表并拷进实现格的钩子原生值
        //(类表仍是规范家);其余算子不 override -> 基类默认报「本类型不支持该算子」。
        [[nodiscard]]
        Opt<Value> op_add_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_mul_impl(AriaVM& vm) override;

    private:
        // 切片(Range 键):段解析收口 ObjRange.cpp 的 resolve_slice_bounds(从尾计数、无上界给后缀、
        // 空段以两端相等+不含上界表示),本函数只按方向折算 count、铸新 list 段拷。
        [[nodiscard]]
        Opt<Value> slice(AriaVM& vm, const ObjRange* range);

        AriaArray elements_; // 元素表(GC 分配器绑定;push/copy_from 惰性增长,trivial 分配不触 GC)
    };

    // 工厂:分配空 ObjList(单次分配,无入参对象可守);返回对象白色无根,调用方建成即发布进根。
    [[nodiscard]]
    ObjList* new_list(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_LIST_HPP
