#ifndef ARIA_OBJ_CLASS_HPP
#define ARIA_OBJ_CLASS_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;

    // 类对象:def 类的运行期载体(ObjType::CLASS)。def 类 = 类级静态表 + superclass 单链,实例字段在 ObjInstance 的
    // fields 表。成员读写取 Python/JS 式**读穿透、写遮蔽**。
    //   - name_:类名(intern;**指针恒非空**,构造期 ASSERT)。
    //   - superclass_:父类,链式查找终止于 bootstrap 的 Object 根类(唯一 super 为空者);运行期经 MAKE_CLASS 注入,此后不
    //     可变。
    //   - field_:类级成员表(单数命名区分 ObjInstance.fields_):静态变量 + 静态/实例方法同表,方法性区分在 defining class
    //     戳(表内无 tag)。全部写路径经 set_field,沿链查找见 find_field。
    //   - init_:构造器方法值。写点全在对象内:构造期自 super 派生(快照语义,父 init 此后变更不传导;Object 根出厂 nil 由
    //     bootstrap 经 set_field 设)+ set_field 命中 "init" 同步。地址哈希型、final。trace 标 name_ + superclass_(容
    //     nullptr)+ init_ + field_。
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

        // 构造器方法值(实例化取用,call_value 的 CLASS 分支消费 -- 非字段协议职责,故不经
        // load/store_field;写点与快照语义见类注释)。
        [[nodiscard]]
        Value init() const noexcept {
            return init_;
        }

        // 写入本类自身表的 name 槽(创建或更新,**不沿链** -- 继承名/新名新建键遮蔽、父表不动)。
        // 类成员**创建路径的唯一公开写入口**(MAKE_STATIC/MAKE_METHOD、bootstrap 设 init、
        // store_field 三路共用);name=="init" 时同步 init_。永不失败;set 走 trivial 分配不触 GC。
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

        // 命名成员读取协议 override:沿链 find_field 读穿透直读 -- 静态值/方法闭包/原生原样
        // 取出,**不绑定不缓存**(类路径无 this,不经 ObjBoundMethod)。全链 miss 以类措辞
        // fail UndefinedProperty;查找纯查询,fail 装箱是唯一分配点。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 命名成员写入协议 override:类上赋值落本类自身表恒成功(动态新增允许,"init" 命中同步
        // init_);vm 为签名统一保留(本 override 无 fail 路径)。
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

    private:
        // 沿 super 链查(self 起,逐父向上),命中返回首个命中值的拷贝、未命中 nullopt。
        // **纯查询,无分配、不 fail**;私有,仅本类型 override 内部使用(组合方一律委托
        // load_field 协议)。field_ 惰性未建表时直接返 nullopt。
        [[nodiscard]]
        Opt<Value> find_field(ObjString* name) noexcept;

        ObjString*    name_;       // 类名(intern 驻留;显示名;指针恒非空)
        ObjClass*     superclass_; // 父类(唯 Object 根为 nullptr;运行期注入后不可变)
        AriaHashTable field_;      // 类级成员表(惰性分配;单数 field_ 区分 ObjInstance.fields_)
        Value         init_;       // 构造器方法值(闭包/原生)
    };

    // 工厂:分配 ObjClass(init_ 由构造函数自 super 派生,工厂纯分配)。守卫纪律见 Object.hpp:
    // **调用方须自行根化 name 与 super**(MAKE_CLASS 的 peek-不弹栈纪律)。建成须立即发布进根。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super);

    // 工厂重载(StringView 名):name 经工厂内部 intern 并自行守卫;super 根化约定同上。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, StringView name, ObjClass* super);

} // namespace aria

#endif // ARIA_OBJ_CLASS_HPP
