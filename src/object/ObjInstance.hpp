#ifndef ARIA_OBJ_INSTANCE_HPP
#define ARIA_OBJ_INSTANCE_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjClass;
    class ObjString;

    // 实例对象:字段动态、无需预声明,存 fields_ 表;方法不落 fields_,读路径按当前类链每次现场绑定,
    // 保证类或父类改写方法后对既有实例立即生效。
    class ObjInstance final : public Object {
    public:
        explicit ObjInstance(GC& gc, ObjClass* klass);
        ~ObjInstance() override = default; // fields_ 持 GC* 级联自释放;class_ 是 GC 对象,不归本类释放

        ObjInstance(const ObjInstance&)            = delete;
        ObjInstance& operator=(const ObjInstance&) = delete;
        ObjInstance(ObjInstance&&)                 = delete;
        ObjInstance& operator=(ObjInstance&&)      = delete;

        // 所属类(构造注入、不可变:实例不换类,故无 setter)。
        [[nodiscard]]
        ObjClass* klass() const noexcept {
            return class_;
        }

        // 标 class_ + fields_(key+value;字段里存的可调用值经值级联)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(fields_ 的 ctrl/entries 两块由 ~HashTable 自释放,不计入壳)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjInstance);
        }

        // 调试渲染:`<Foo instance>`(class_ 恒非空);显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

        // 裸查找 override(调用路径与算子钩子取实现共用):不绑定,返回字段/类链里的原值交 VM 直调
        // (方法体从槽 0 读 this)。零分配,每次按当前类链解析。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override(方法值读取路径):fields 命中优先(真字段遮蔽类链),否则委托类协议沿链
        // 读穿透;命中方法戳闭包现场绑 this(每次新 bound,不缓存),其余原值直读。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

        // 命名成员写入协议 override:实例字段动态创建(无预声明),set 即写入,永不失败。
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

        // 算子/调用协议 11 实现:按钩子名(VM 常量串表)经裸查找 load_field 取,实例 fields 可遮蔽类链钩子
        // --「实例上一个叫 `__add__` 的字段/方法就是它的 `+`」。
        [[nodiscard]]
        Opt<Value> op_add_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_sub_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_mul_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_div_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_mod_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_less_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_less_equal_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_greater_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_greater_equal_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_negate_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_call_impl(AriaVM& vm) override;

    private:
        ObjClass*     class_;  // 所属类(恒非空,ctor ASSERT;构造注入不可变)
        AriaHashTable fields_; // 实例字段表(惰性分配;纯字段,方法绑定不缓存)
    };

    // 工厂:分配 ObjInstance(fields_ 空态,单次分配);调用方须自行根化 klass,建成即写栈(值栈根)。
    [[nodiscard]]
    ObjInstance* new_instance(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_OBJ_INSTANCE_HPP
