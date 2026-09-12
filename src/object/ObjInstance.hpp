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
    //
    //   - class_:所属类(实例方法派发 / 字段未命中沿类链查静态表的起点),构造注入、不可变,
    //     **恒非空**(构造期 ASSERT)。
    //   - fields_:实例字段表(`init` 内 `this.x = ...` 落此,无字段预声明、动态)。键 intern
    //     ObjString*,值为绑定 Value。**bound-method 缓存同居此表**(M5 决策 4:与真字段同表
    //     同 keyspace;fields 命中优先即真字段遮蔽同名方法与缓存项,三铁则见 M5 计划 §2.4)。
    //     惰性分配(首次 set 才建表)。**私有,不对外暴露**:成员读写一律走
    //     load_field/store_field 协议(LOAD_FIELD/STORE_FIELD/LOAD_THIS_FIELD/STORE_THIS_FIELD
    //     与缓存回填 set 均在对象内完成),外部无整表访问器。
    //
    //   地址哈希型可变对象(走 Object{ObjType::INSTANCE} ctor);equals 保持默认地址相等--
    //     实例按身份判等,无内容相等语义。final,不再派生;AriaHashTable 成员自身禁拷贝/禁移动。
    //   trace():标 class_ + 委托 fields_.trace(gc)(遍历占用槽 mark_value key+value;缓存
    //     的 bound-method 经此级联标,GC 侧零额外负担)。
    //   to_string():`<Foo instance>`。
    class ObjInstance final : public Object {
    public:
        explicit ObjInstance(GC& gc, ObjClass* cls);
        ~ObjInstance() override = default; // fields_ 持 GC* 级联自释放;class_ 是 GC 对象,不归本类释放

        ObjInstance(const ObjInstance&)            = delete;
        ObjInstance& operator=(const ObjInstance&) = delete;
        ObjInstance(ObjInstance&&)                 = delete;
        ObjInstance& operator=(ObjInstance&&)      = delete;

        // 所属类(构造注入、不可变:实例不换类,故无 setter)。
        [[nodiscard]]
        ObjClass* cls() const noexcept {
            return class_;
        }

        // 命名成员读取协议 override(LOAD_FIELD / LOAD_THIS_FIELD 统一语义):fields 命中优先
        //(真字段遮蔽同名方法与缓存项,铁则 3)-> **委托类协议** ObjClass::load_field(沿类链
        // 读穿透直读;miss 已按类措辞就地 fail,nullopt ⟺ 已 fail,本 override 只透传信号 --
        // 成员表在类链上,文案随宿主)。
        //
        // 命中处理:可调用值(闭包/原生)现场绑 this 并回填 fields 缓存(铁则 1:只缓存绑定
        // 方法,快照语义),非可调用静态值直读不缓存。分配点(new_bound_method)的 GC 安全与
        // 回填细节见 .cpp 实现注。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 命名成员写入协议 override:实例字段动态创建(无预声明),set 即写入,
        // 永不失败(恒 true;false ⟺ 已 fail)。
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

        // 标 class_ + fields_(key+value;缓存 bound/method 闭包经值级联)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(fields_ 的 ctrl/entries 两块由 ~HashTable 自释放,不计入壳)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjInstance);
        }

        // 调试渲染:`<Foo instance>`(class_ 恒非空);显示同文案(to_string 经基类默认委托)。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ObjClass*     class_;  // 所属类(恒非空,ctor ASSERT;构造注入不可变)
        AriaHashTable fields_; // 实例字段表 + bound-method 缓存(惰性分配)
    };

    // 工厂:分配 ObjInstance(fields_ 空态)。shell 单次分配、无内部二级分配 ==> 工厂内无中间
    //     GC 点;工厂不替调用方守卫入参(「每方只守自己创建的」),调用方须在调用前自行根化
    //     cls(实例化路径 cls 在栈根化)。返回对象白色无根,建成即写栈(值栈根)。
    [[nodiscard]]
    ObjInstance* new_instance(GC& gc, ObjClass* cls);

} // namespace aria

#endif // ARIA_OBJ_INSTANCE_HPP
