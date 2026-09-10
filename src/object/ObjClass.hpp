#ifndef ARIA_OBJ_CLASS_HPP
#define ARIA_OBJ_CLASS_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;
    class ObjClosure;

    // 类对象:def 类的运行期载体(ObjType::CLASS)。语义模型(M5 定调,见 vm-design §6 / M5 计划):
    // def 类 = 类级一张静态表 + superclass 单链;实例字段在 ObjInstance 的 fields 表,不在类上。
    //
    //   - name_:类名(intern 驻留,同指针)。显示名(to_string / 报错渲染)用;
    //     **指针恒非空**(构造期 ASSERT)。
    //   - superclass_:父类。单链查找的链;唯一 super 为空者是 VM bootstrap 的 Object 根类
    //     (链式查找统一终止于它)。运行期经 MAKE_CLASS 从栈上的父类值注入,此后不可变。
    //   - field_:类级成员表:静态变量 + 静态/实例方法同存一张表(M5 决策 2;命名用单数 field_
    //     区分 ObjInstance 的实例字段表 fields_),键 intern
    //     ObjString*,值为绑定 Value(方法槽的值是 ObjClosure,静态变量槽是任意 Value;区分在
    //     值类型本身,表内无 tag)。MAKE_STATIC/MAKE_METHOD 与类上赋值(STORE_FIELD 类路径,
    //     写遮蔽)写此表;沿链查找见 find_field。惰性分配(首次 upsert 才建表)。
    //   - init_:构造器方法值(**Value**,init Value 化 2026-09-10:闭包或原生函数皆可,
    //     实例化统一走 call_value 分发,不再特认闭包;类上赋非可调用值也放行,实例化时
    //     call_value 自然报 CallNonCallable)。ctor 播 nil,MAKE_CLASS 时从父类 seed(super 非空
    //     即继承其 init 值)、MAKE_METHOD("init") 覆盖、类上 init 赋值同步(STORE_FIELD 类路径
    //     特例,经 set_init);不变式「建成的类 init_ 有值且与类表 init 槽一致」
    //     由 VM 建类/写路径保证(Object 根类的由 bootstrap 设)。经类实例化(call_value CLASS 分支)取用。
    //
    //   地址哈希型可变对象(走 Object{ObjType::CLASS} ctor);equals 保持默认地址相等--
    //     类按身份判等,无内容相等语义。final,不再派生;AriaHashTable 成员自身禁拷贝/禁移动。
    //   trace():标 name_ + superclass_(容 nullptr:Object 根)+ init_(出厂 nil,mark_value
    //     容 nil)+ 委托 field_.trace(gc)(遍历占用槽
    //     mark_value key+value;方法闭包的 defining_class 经 ObjClosure::trace 级联标)。
    //   debug_repr():`<class Foo>`;基类 to_string 默认委托本方法,显示同文案。
    class ObjClass final : public Object {
    public:
        // name = 类名(intern,指针恒非空 -- 构造期 ASSERT);super = 父类(唯 Object 根为 nullptr)。
        ObjClass(GC& gc, ObjString* name, ObjClass* super);
        // field_ 持 GC* 级联自释放;name_/superclass_ 是 GC 对象、init_ 是 GC 值,不归本类释放
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

        // 沿 super 链查(self 起,逐父向上):命中返回表内值槽指针,未命中 nullptr。只读与存在性
        // 检查用--类成员读写取 Python/JS class attributes 语义(读穿透、写遮蔽,M5 决策补记):
        // 写不经此槽,STORE_FIELD 类路径先经本函数全链存在性检查、再 upsert_field 落接收类
        // 自身表;super.m 从父类起走链同用本函数。纯查找无分配,GC-pure;field_ 惰性未建表时
        // find 直接返 nullptr。
        [[nodiscard]]
        Value* find_field(ObjString* name) noexcept;

        // 本类自身成员表 upsert(不沿链)--类成员写路径本体(读穿透、写遮蔽:本类已有则原槽更新,
        // 继承来的名字在此新建遮蔽键、父表不动;STORE_FIELD 类路径先沿链存在性检查再落此),
        // 与创建路径 MAKE_STATIC/MAKE_METHOD 共用:命中已有槽保留其 value,未命中插入(V{} 零填充,
        // 调用方负责覆写)。返回本表内值槽指针。可能触发 rehash(调用方取回指针后立即写、勿跨 upsert 持有)。
        [[nodiscard]]
        Value* upsert_field(ObjString* name);

        // 类级成员表(静态变量 + 静态/实例方法同居;LOAD_FIELD 类路径沿链 find_field 读穿透,
        // STORE_FIELD 类路径写遮蔽落自身表)。键 ObjString*(intern,内容语义经 === 同指针),值为绑定 Value。
        [[nodiscard]]
        AriaHashTable& field() noexcept {
            return field_;
        }

        [[nodiscard]]
        const AriaHashTable& field() const noexcept {
            return field_;
        }

        // 构造器方法值(Value:闭包/原生函数;出厂 nil,由调用方 seed --MAKE_CLASS 继承父 init /
        // MAKE_METHOD("init") 覆盖 / 类上 init 赋值同步,三写点见类注释);实例化取用。
        [[nodiscard]]
        Value init() const noexcept {
            return init_;
        }

        void set_init(Value init) noexcept { init_ = init; }

        // 标 name_ + superclass_(容 nullptr:Object 根)+ mark_value(init_)(出厂 nil)+ field_(key+value)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(field_ 的 ctrl/entries 两块由 ~HashTable 自释放,不计入壳)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjClass);
        }

        // 调试渲染:`<class Foo>`(name_ 恒非空,ctor ASSERT);基类 to_string 默认委托本方法,显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ObjString*    name_;       // 类名(intern 驻留;显示名;指针恒非空)
        ObjClass*     superclass_; // 父类(唯 Object 根为 nullptr;运行期注入后不可变)
        AriaHashTable field_; // 类级成员表:静态变量 + 静态/实例方法同表(惰性分配;单数 field_ 区分 ObjInstance.fields_)
        Value         init_;  // 构造器方法值(出厂 nil -> MAKE_CLASS seed -> MAKE_METHOD("init") 覆盖 -> 类上赋值同步)
    };

    // 工厂:分配 ObjClass(field_ 空态、init_ 出厂恒 nil --工厂只分配不 seed,seed 责任在调用方)。
    //     工厂不替调用方守卫入参
    //     (「每方只守自己创建的」)--只做一次 new_object、无内部新建对象,**调用方须在调用前
    //     自行根化 name 与 super**(跨 new_object 顶 maybe_collect;name 经 intern 是 weak root,
    //     super 可能尚未入任何根,如 MAKE_CLASS peek-不弹栈纪律),与 new_function 同理。
    //     返回对象白色无根,调用方须立即发布进根(MAKE_CLASS 原槽写回即经值栈根)。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super);
} // namespace aria

#endif // ARIA_OBJ_CLASS_HPP
