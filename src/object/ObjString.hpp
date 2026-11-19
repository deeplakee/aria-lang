#ifndef ARIA_OBJ_STRING_HPP
#define ARIA_OBJ_STRING_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjRange;

    // 不可变、内容哈希；短串内联，等价内容经驻留池共享对象。
    class ObjString final : public Object {
    public:
        static constexpr usize kShortCapacity = 15;

        ObjString(GC& gc, StringView src, u32 hash);

        // 接管 data 的释放责任；长串沿用缓冲，短串转入内联存储。data 为空指针时 size 须为 0。
        ObjString(GC& gc, char* data, usize size, usize cap, u32 hash);

        // hash 须为 hash_str(hash_str(lhs), rhs) 的续算结果。
        ObjString(GC& gc, StringView lhs, StringView rhs, u32 hash);

        ~ObjString() override;

        // 视图仅在对象存活期有效；跨 collect 须保活对象。
        [[nodiscard]]
        StringView view() const noexcept;

        [[nodiscard]]
        usize length() const noexcept {
            return length_;
        }

        [[nodiscard]]
        bool is_long() const noexcept {
            return length_ > kShortCapacity;
        }

        void trace(GC&) const noexcept override {}

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjString);
        }

        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 下标按字节计，支持负数；Range 键走切片。
        [[nodiscard]]
        Opt<Value> load_index(AriaExeContext& ctx, Value key) override;

        [[nodiscard]]
        bool store_index(AriaExeContext& ctx, Value key, Value value) override;

    private:
        // 切片按字节处理，倒序多字节文本可能产生非法 UTF-8。
        [[nodiscard]]
        Opt<Value> slice(AriaExeContext& ctx, const ObjRange* range) const;

        GC* gc_; // 供 ~ObjString 释放 long_chars_(仅长串用到)
        union {
            char  short_chars_[kShortCapacity + 1]; // 16 字节,与 char* 取大
            char* long_chars_;
        };
        usize length_;
    };

    // 返回驻留串，等价内容共享同一对象；显式 hash 须与内容哈希一致。
    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView src);

    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView src, u32 hash);

    // hash 须为两段的续算结果；lhs/rhs 的宿主串须跨分配保活。
    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView lhs, StringView rhs, u32 hash);

    // 接管缓冲的释放责任；驻留命中时释放缓冲并返回已有串。
    // data 为空且 size 为 0 表示空串。
    [[nodiscard]]
    ObjString* new_string(GC& gc, char* data, usize size, usize cap, u32 hash);

    [[nodiscard]]
    ObjString* new_string(GC& gc, char ch);

} // namespace aria

#endif // ARIA_OBJ_STRING_HPP
