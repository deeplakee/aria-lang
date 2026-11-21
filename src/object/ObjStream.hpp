#ifndef ARIA_OBJ_STREAM_HPP
#define ARIA_OBJ_STREAM_HPP

#include <cstdio>

#include "object/Object.hpp"

namespace aria {

    class ObjStream final : public Object {
    public:
        // klass 和 stream 均非空。stream 由宿主持有，须活过本对象。
        ObjStream(ObjClass* klass, std::FILE* stream);

        [[nodiscard]]
        ObjClass* klass() const noexcept {
            return class_;
        }

        [[nodiscard]]
        std::FILE* stream() const noexcept {
            return stream_;
        }

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjStream);
        }

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

    private:
        ObjClass*  class_;
        std::FILE* stream_;
    };

} // namespace aria

#endif // ARIA_OBJ_STREAM_HPP
