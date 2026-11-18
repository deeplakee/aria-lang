#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjFunction;
    class ObjString;

    class ObjBoundMethod final : public Object {
    public:
        ObjBoundMethod(ObjFunction* method, Value receiver);
        ~ObjBoundMethod() override = default;

        ObjBoundMethod(const ObjBoundMethod&)            = delete;
        ObjBoundMethod& operator=(const ObjBoundMethod&) = delete;
        ObjBoundMethod(ObjBoundMethod&&)                 = delete;
        ObjBoundMethod& operator=(ObjBoundMethod&&)      = delete;

        [[nodiscard]]
        ObjFunction* method() const noexcept {
            return method_;
        }

        [[nodiscard]]
        Value receiver() const noexcept {
            return receiver_;
        }

        [[nodiscard]]
        ObjString* name() const noexcept;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjBoundMethod);
        }

        // 内容相等(==):receiver 同一 && method 同一;`===` 仍按身份。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

    private:
        ObjFunction* method_;   // 被绑定的 Aria 或原生函数,恒非空
        Value        receiver_; // 绑定接收者,调用时写入槽 0
    };

    // 调用方须根化 method 与 receiver 中的对象。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, ObjFunction* method, Value receiver);

    // 指针重载的 receiver 须非空。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, ObjFunction* method, Object* receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
