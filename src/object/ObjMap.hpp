#ifndef ARIA_OBJ_MAP_HPP
#define ARIA_OBJ_MAP_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;

    // 键按 === 判等，可变对象按身份；迭代序不作保证。
    class ObjMap final : public Object {
    public:
        explicit ObjMap(GC& gc);

        ~ObjMap() override = default; // 键值是 GC 值,不归本类释放

        ObjMap(const ObjMap&)            = delete;
        ObjMap& operator=(const ObjMap&) = delete;
        ObjMap(ObjMap&&)                 = delete;
        ObjMap& operator=(ObjMap&&)      = delete;

        [[nodiscard]]
        AriaHashTable& table() noexcept {
            return table_;
        }

        [[nodiscard]]
        const AriaHashTable& table() const noexcept {
            return table_;
        }

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjMap);
        }

        // 键按 === 匹配，值按 == 比较。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 缺键报错，与 get 返回 nil 的语义不同。
        [[nodiscard]]
        Opt<Value> load_index(AriaExeContext& ctx, Value key) override;

        [[nodiscard]]
        bool store_index(AriaExeContext& ctx, Value key, Value value) override;

    private:
        AriaHashTable table_; // 键值表(GC 分配器绑定;set 惰性首分配,trivial 分配不触 GC)
    };

    [[nodiscard]]
    ObjMap* new_map(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_MAP_HPP
