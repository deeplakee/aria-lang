#ifndef ARIA_OBJ_INSTANCE_HPP
#define ARIA_OBJ_INSTANCE_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjClass;
    class ObjString;

    // 字段动态创建并遮蔽类成员；方法读取时现场绑定，不缓存，以反映类链变更。
    class ObjInstance final : public Object {
    public:
        explicit ObjInstance(GC& gc, ObjClass* klass);
        ~ObjInstance() override = default; // fields_ 持 GC* 级联自释放;class_ 是 GC 对象,不归本类释放

        [[nodiscard]]
        ObjClass* klass() const noexcept {
            return class_;
        }

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjInstance);
        }

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 字段优先，缺席时查类链，返回原值。
        [[nodiscard]]
        Opt<Value> load_field(AriaExeContext& ctx, ObjString* name) override;

        // 仅绑定类链中的方法，字段中的函数保持原值。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaExeContext& ctx, ObjString* name) override;

        bool store_field(AriaExeContext& ctx, ObjString* name, Value value) override;

    private:
        ObjClass*     class_;  // 所属类(恒非空,ctor ASSERT;构造注入不可变)
        AriaHashTable fields_; // 实例字段表(惰性分配;纯字段,方法绑定不缓存)
    };

    // 调用方须根化 klass。
    [[nodiscard]]
    ObjInstance* new_instance(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_OBJ_INSTANCE_HPP
