#ifndef ARIA_OBJ_CLASS_HPP
#define ARIA_OBJ_CLASS_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;

    // 类对象:def 类的运行期载体,由静态成员表与 superclass 链组成(读穿透、写遮蔽);
    // init_ 在构造期从父类快照,之后父类的 init 变更不再传导。
    class ObjClass final : public Object {
    public:
        ObjClass(GC& gc, ObjString* name, ObjClass* super);
        // field_ 持 GC* 级联自释放;name_/superclass_/init_ 是 GC 对象,不归本类释放
        ~ObjClass() override = default;

        ObjClass(const ObjClass&)            = delete;
        ObjClass& operator=(const ObjClass&) = delete;
        ObjClass(ObjClass&&)                 = delete;
        ObjClass& operator=(ObjClass&&)      = delete;

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        // 父类:唯一 nullptr 者是 Object 根类(VM bootstrap)。
        [[nodiscard]]
        ObjClass* superclass() const noexcept {
            return superclass_;
        }

        // 构造器方法值(实例化取用;不经 load/store_field 协议)。
        [[nodiscard]]
        Value init() const noexcept {
            return init_;
        }

        // 类成员创建路径的唯一公开写入口:落本类自身表不沿链(继承名/新建键遮蔽);"init" 命中同步
        // init_。永不失败,set 走 trivial 分配不触 GC。
        void set_field(ObjString* name, Value value);

        // 标 name_ + superclass_(容 nullptr)+ mark_value(init_)+ field_(key+value)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(field_ 的 ctrl/entries 两块由 ~HashTable 自释放,不计入壳)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjClass);
        }

        // 调试渲染:`<class Foo>`(name_ 恒非空,ctor ASSERT);显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

        // 命名成员读取 override:沿链读穿透直读,不绑定不缓存(类路径无 this);全链 miss 以类措辞
        // fail,该 fail 装箱是唯一分配点。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 命名成员写入 override:类上赋值落本类自身表恒成功(动态新增允许);本 override 无 fail 路径。
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

    private:
        // 沿 super 链查,链上首个命中拷出;纯查询无分配不 fail(私有,组合方一律走 load_field 协议)。
        [[nodiscard]]
        Opt<Value> find_field(ObjString* name) noexcept;

        ObjString*    name_;       // 类名(intern 驻留;显示名;指针恒非空)
        ObjClass*     superclass_; // 父类(唯 Object 根为 nullptr;运行期注入后不可变)
        AriaHashTable field_;      // 类级成员表(惰性分配;单数 field_ 区分 ObjInstance.fields_)
        Value         init_;       // 构造器方法值(闭包/原生)
    };

    // 工厂:分配 ObjClass(init_ 由 ctor 自 super 派生);name 与 super 皆白色,调用方须自行根化,
    // 建成须立即发布进根。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super);

    // 工厂重载(StringView 名):name 经工厂内部 intern 并自行守卫;super 根化约定同上。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, StringView name, ObjClass* super);

} // namespace aria

#endif // ARIA_OBJ_CLASS_HPP
