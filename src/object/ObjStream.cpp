#include "object/ObjStream.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaExeContext.hpp"

namespace aria {

    ObjStream::ObjStream(ObjClass* klass, std::FILE* stream) : Object{ObjType::STREAM}, class_{klass}, stream_{stream} {
        ASSERT(klass != nullptr, "class must not be null");
        ASSERT(stream != nullptr, "stream must not be null");
    }

    void ObjStream::trace(GC& gc) const noexcept { gc.mark_object(class_); }

    Opt<Value> ObjStream::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        if (const auto value = class_->find_field(name)) {
            return value;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", class_->repr(), name->view());
    }

} // namespace aria
