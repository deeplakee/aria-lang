#include "object/iterator/ObjIterator.hpp"

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/ObjMovement.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjIterator::ObjIterator() : Object{ObjType::ITERATOR} {}

    String ObjIterator::debug_repr() const {
        // 源类型不进文案:语言层单数 Iterator。
        return "<iterator>";
    }

    Opt<Value> ObjIterator::load_field(AriaExeContext& ctx, ObjString* name) {
        return ctx.iterator_class()->load_field(ctx, name);
    }

    Opt<Value> ObjIterator::load_field_bound(AriaExeContext& ctx, ObjString* name) {
        const auto hit = load_field(ctx, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(ctx.gc(), *hit, this));
    }

} // namespace aria
