#ifndef ARIA_OBJ_MOVEMENT_HPP
#define ARIA_OBJ_MOVEMENT_HPP

#include <format>

#include "common.hpp"
#include "object/Object.hpp"
#include "runtime/AriaExeContext.hpp"

namespace aria {

    class AriaVM;

    // 将执行上下文包装为 GC 对象，主上下文与协程共用此类型。
    class ObjMovement final : public Object, public AriaExeContext {
    public:
        explicit ObjMovement(GC* gc, AriaVM* host) noexcept : Object{ObjType::MOVEMENT}, AriaExeContext{gc, host} {}

        ~ObjMovement() override = default;

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjMovement);
        }

        [[nodiscard]]
        String repr() const override {
            return std::format("<coroutine {}>", to_string(state_));
        }

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;
    };

    // bootstrap 期间宿主服务尚未就位，不可调用上下文服务。
    [[nodiscard]]
    ObjMovement* new_movement(GC& gc, AriaVM& host);

} // namespace aria

#endif // ARIA_OBJ_MOVEMENT_HPP
