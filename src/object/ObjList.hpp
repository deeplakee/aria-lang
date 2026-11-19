#ifndef ARIA_OBJ_LIST_HPP
#define ARIA_OBJ_LIST_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaArray.hpp"

namespace aria {

    class GC;
    class ObjRange;

    // 元素任意；作为 map 键时按对象身份判等。
    class ObjList final : public Object {
    public:
        explicit ObjList(GC& gc);

        ~ObjList() override = default; // 元素是 GC 值,不归本类释放

        [[nodiscard]]
        AriaArray& elements() noexcept {
            return elements_;
        }

        [[nodiscard]]
        const AriaArray& elements() const noexcept {
            return elements_;
        }

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjList);
        }

        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 整数键支持负下标；Range 键返回新切片。
        [[nodiscard]]
        Opt<Value> load_index(AriaExeContext& ctx, Value key) override;

        // 下标写入不自动增长，不支持切片赋值。
        [[nodiscard]]
        bool store_index(AriaExeContext& ctx, Value key, Value value) override;

    private:
        // 切片(Range 键):负端点从尾计数,无上界取后缀,倒序区间产出倒序元素;返回新 list。
        [[nodiscard]]
        Opt<Value> slice(AriaExeContext& ctx, const ObjRange* range);

        AriaArray elements_; // 元素表(GC 分配器绑定;push/copy_from 惰性增长,trivial 分配不触 GC)
    };

    [[nodiscard]]
    ObjList* new_list(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_LIST_HPP
