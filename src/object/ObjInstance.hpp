#ifndef ARIA_OBJ_INSTANCE_HPP
#define ARIA_OBJ_INSTANCE_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjClass;
    class ObjString;

    // 实例对象:类经实例化(call_value CLASS 分支)的产物(ObjType::INSTANCE)。
    //   - class_:所属类(字段未命中沿类链查静态表的起点),构造注入、不可变、恒非空(ctor ASSERT)。
    //   - fields_:实例字段表(无字段预声明、动态;惰性分配)。**纯字段** -- 只存真字段,不缓存
    //     方法绑定:缓存会让「类/父类改写方法」对既有实例陈旧、且与新建实例不一致(monkey patch
    //     半可用且难解释)。故读路径每次访问现场绑定新 bound,调用路径经 `load_field_unbound` 走
    //     不绑定形态(零分配 + 每次按当前类链解析)。**私有不对外暴露**,读写一律走协议。
    //   地址哈希型、final。trace 标 class_ + fields_(字段里的可调用值经值级联标)。
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

        // 命名成员读取协议 override(方法值**读取**路径):fields 命中优先(真字段遮蔽类链同名
        // 成员)-> **委托类协议** ObjClass::load_field 沿链读穿透(miss 的类措辞随协议传播)。命中
        // 方法戳闭包即现场绑 this(每次访问一个新 bound,不缓存),其余原值直读;分配点 GC 安全见 .cpp。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 命名成员读取的不绑定形态 override(PREPARE_METHOD 与算子钩子取实现共用):**不绑定**,
        // 返回字段/类链里的原值,交 VM 以 receiver 占调用区槽 0 直调(方法体从槽 0 读 this)。
        // 零分配,且每次按当前类链解析(改类/父类方法立即生效,与读路径同一份可见性)。
        [[nodiscard]]
        Opt<Value> load_field_unbound(AriaVM& vm, ObjString* name) override;

        // 命名成员写入协议 override:实例字段动态创建(无预声明),set 即写入,永不失败。
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

        // 算子与调用协议的 11 个实现(基类默认直接 fail,故参与该协议须显式实现):各自按钩子名(VM 常量串表,
        // 注册表 runtime/str_table.hpp)经 load_field_unbound 到实例 fields(字段可遮蔽类链钩子)再类链取。
        // 即「实例上一个叫 `__add__` 的字段/方法就是它的 `+`」。
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

    // 工厂:分配 ObjInstance(fields_ 空态),单次分配无内部二级分配。守卫纪律见 Object.hpp;
    //     调用方须自行根化 klass;建成即写栈(值栈根)。
    [[nodiscard]]
    ObjInstance* new_instance(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_OBJ_INSTANCE_HPP
