#include "object/iterator/ObjIterator.hpp"

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/ObjMovement.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjIterator::ObjIterator() : Object{ObjType::ITERATOR} {}

    String ObjIterator::repr() const {
        // 源类型不进文案:语言层单数 Iterator。
        return "<iterator>";
    }

    Opt<Value> ObjIterator::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.iterator_class();
        if (const auto v = klass->find_field(name)) {
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    Opt<Value> ObjIterator::load_field(AriaExeContext& ctx, ObjString* name) {
        return load_field_from_class(ctx, name);
    }

    Opt<Value> ObjIterator::load_field_bound(AriaExeContext& ctx, ObjString* name) {
        const auto hit = load_field(ctx, name);
        if (!hit) {
            return std::nullopt;
        }
        return Value::from_obj(new_bound_method(ctx.gc(), *hit, this));
    }

} // namespace aria
