#ifndef ARIA_OBJ_CLASS_HPP
#define ARIA_OBJ_CLASS_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;

    // 类成员沿父链读取、在本类写入遮蔽；init 在构造时取父类快照，后续不随父类变更。
    class ObjClass final : public Object {
    public:
        ObjClass(GC& gc, ObjString* name, ObjClass* super);
        ~ObjClass() override = default;

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        // 仅 Object 根类的父类为空。
        [[nodiscard]]
        ObjClass* superclass() const noexcept {
            return superclass_;
        }

        [[nodiscard]]
        Value init() const noexcept {
            return init_;
        }

        // 只写本类表；写入 init 时同步构造器快照。
        void set_field(ObjString* name, Value value);

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjClass);
        }

        [[nodiscard]]
        String repr() const override;

        // 类值的隐式钩子查 Class 元类链，不查自身声明表。
        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 普通成员仅沿声明链查询，不回退元类，不绑定。
        [[nodiscard]]
        Opt<Value> load_field(AriaExeContext& ctx, ObjString* name) override;

        [[nodiscard]]
        Opt<Value> load_field_bound(AriaExeContext& ctx, ObjString* name) override;

        // 沿父链返回首个命中值；未命中不挂起错误。
        [[nodiscard]]
        Opt<Value> find_field(ObjString* name) noexcept;

        bool store_field(AriaExeContext& ctx, ObjString* name, Value value) override;

    private:
        ObjString*    name_;       // 类名(intern 驻留;显示名;指针恒非空)
        ObjClass*     superclass_; // 父类(唯 Object 根为 nullptr;运行期注入后不可变)
        AriaHashTable field_;      // 类级成员表(惰性分配;单数 field_ 区分 ObjInstance.fields_)
        Value         init_;       // 构造器方法值(闭包/原生)
    };

    // 调用方须根化 name 与 super；StringView 重载内部根化新建名称。
    [[nodiscard]]
    ObjClass* new_class(GC& gc, ObjString* name, ObjClass* super);

    [[nodiscard]]
    ObjClass* new_class(GC& gc, StringView name, ObjClass* super);

} // namespace aria

#endif // ARIA_OBJ_CLASS_HPP
