#ifndef ARIA_OBJ_CLASS_HPP
#define ARIA_OBJ_CLASS_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;

    // 类对象:def 类的运行期载体(ObjType::CLASS)。语义模型(见 vm-design §6 / M5 计划):
    // def 类 = 类级一张静态表 + superclass 单链;实例字段在 ObjInstance 的 fields 表,不在类上。
    //
    //   - name_:类名(intern 驻留,同指针)。显示名(to_string / 报错渲染)用;
    //     **指针恒非空**(构造期 ASSERT)。
    //   - superclass_:父类,单链查找的链;唯一 super 为空者是 VM bootstrap 的 Object 根类
    //     (链式查找统一终止于它)。运行期经 MAKE_CLASS 从栈上的父类值注入,此后不可变。
    //   - field_:类级成员表:静态变量 + 静态/实例方法同存一张表(M5 决策 2;命名用单数 field_
    //     区分 ObjInstance 的实例字段表 fields_),键 intern ObjString*,值为绑定 Value(方法槽
    //     的值是 ObjClosure,静态变量槽是任意 Value;区分在值类型本身,表内无 tag)。
    //     MAKE_STATIC/MAKE_METHOD 与类上赋值(STORE_FIELD 类路径)统一经 set_field 写此表;
    //     沿链查找见私有 find_field(组合方一律委托 load_field 协议)。
    //     惰性分配(首次 set 才建表)。
    //   - init_:构造器方法值(**Value**:闭包或原生函数皆可;类上赋非可调用值也放行,实例化
    //     时 call_value 报 CallNonCallable 兜底)。写点全在对象内:**构造期自 super 派生**
    //     (super 非空出厂即继承父 init_ 当前值,快照语义 -- 此后父 init 变更不再传导;
    //     Object 根(super==nullptr)出厂 nil,由 bootstrap 经 set_field 设)+
    //     **set_field 落表时同步**(name=="init" 即写 init_,表槽/init_ 一致由对象自维护)。
    //     经类实例化(call_value CLASS 分支)取用。
    //
    //   地址哈希型可变对象(走 Object{ObjType::CLASS} ctor);equals 保持默认地址相等--
    //     类按身份判等,无内容相等语义。final,不再派生;AriaHashTable 成员自身禁拷贝/禁移动。
    //   trace():标 name_ + superclass_(容 nullptr:Object 根)+ init_ + 委托 field_.trace(gc)
    //     (方法闭包的 defining_class 经 ObjClosure::trace 级标)。
    //   to_string():`<class Foo>`。
    class ObjClass final : public Object {
    public:
        // name = 类名(intern,指针恒非空 -- 构造期 ASSERT);super = 父类(唯 Object 根为 nullptr)。
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

        // 写入本类自身表的 name 槽(创建或更新,**不沿链** --继承名/新名新建键遮蔽、本类
        // 已有原槽更新、父表不动)。类成员**创建路径的唯一公开写入口**(落表 set 内联于此,
        // 无独立辅助):MAKE_STATIC/MAKE_METHOD 注册、bootstrap 设 init 与 store_field
        // 落表三路共用;name=="init" 时同步 init_(表槽/init_ 一致性由本写入口自维护,
        // 表槽存在则必与 init_ 同值)。永不失败;set 走 trivial 分配不触 GC,GC-pure。
        void set_field(ObjString* name, Value value);

        // 构造器方法值(闭包/原生;类上赋非可调用值亦放行,实例化时 call_value 报错兜底)。
        // 构造期自 super 派生(快照语义;写点见类注释);实例化取用。**实例化机制接缝,
        // 非字段协议**:init 的消费方是 call_value 的 CLASS 分支(callable 协议),不属
        // load_field/store_field 的成员访问职责,故不经协议;"init" 槽与 init_ 的一致性
        // 由 set_field 同步自维护。
        [[nodiscard]]
        Value init() const noexcept {
            return init_;
        }

        // 命名成员读取协议 override:沿链 find_field 读穿透直读 --静态值/静态方法闭包/原生
        // 原样取出,**不绑定不缓存**(静态方法无 this;类路径不经 ObjBoundMethod)。全链
        // miss:以类措辞 fail UndefinedProperty("<class X> has no member ...",nullopt ⟺
        // 已 fail,渲染经 debug_repr);查找纯查询无分配,fail 装箱是唯一分配点。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 命名成员写入协议 override:类上赋值落本类自身表,恒成功 --本类已有原槽更新、
        // 继承名/新名新建键遮蔽、父表不动(动态新增允许);"init" 命中同步 init_。vm 为
        // 协议签名统一保留(本 override 无 fail 路径);落表 set 走 trivial 分配不触 GC。
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

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

    private:
        // 沿 super 链查(self 起,逐父向上):命中返回链上首个命中值的拷贝,未命中 nullopt
        //(拼写与协议 load 族一致)。**纯查询,无分配、不 fail** --私有,仅本类型
        // load_field override 内部使用;组合方(实例、super 站点)一律委托 load_field 协议。
        // field_ 惰性未建表时直接返 nullopt。
        [[nodiscard]]
        Opt<Value> find_field(ObjString* name) noexcept;

        ObjString*    name_;       // 类名(intern 驻留;显示名;指针恒非空)
        ObjClass*     superclass_; // 父类(唯 Object 根为 nullptr;运行期注入后不可变)
        AriaHashTable field_; // 类级成员表:静态变量 + 静态/实例方法同表(惰性分配;单数 field_ 区分 ObjInstance.fields_)
        Value         init_;  // 构造器方法值(闭包/原生)
    };

    // 工厂:分配 ObjClass(field_ 空态;init_ 由 **ObjClass 构造函数自 super 派生**,工厂纯分配)。
    //
    // 工厂不替调用方守卫入参(「每方只守自己创建的」)--只做一次 new_object、无内部新建对象,
    // **调用方须在调用前自行根化 name 与 super**(跨 new_object 顶 maybe_collect;name 经
    // intern 是 weak root,super 可能尚未入任何根,如 MAKE_CLASS peek-不弹栈纪律),与
    // new_function 同理。
    //
    // 返回对象白色无根,调用方须立即发布进根(MAKE_CLASS 原槽写回即经值栈根)。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super);

    // 工厂重载(StringView 名):name 经工厂内部 intern 并自行守卫,调用方传文本即可;
    //     super 的根化约定同上。委托显式名重载。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, StringView name, ObjClass* super);
} // namespace aria

#endif // ARIA_OBJ_CLASS_HPP
