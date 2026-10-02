#ifndef ARIA_OBJ_MAP_HPP
#define ARIA_OBJ_MAP_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;

    // map 对象:键值均为任意 Value 的哈希表;键判等用 ===,可变对象按对象身份;迭代序 unspecified。
    class ObjMap final : public Object {
    public:
        // 哈希表惰性分配,ctor 只绑分配器(首分配由 set 的扩容路径自理)。
        explicit ObjMap(GC& gc);

        ~ObjMap() override = default; // 键值是 GC 值,不归本类释放

        ObjMap(const ObjMap&)            = delete;
        ObjMap& operator=(const ObjMap&) = delete;
        ObjMap(ObjMap&&)                 = delete;
        ObjMap& operator=(ObjMap&&)      = delete;

        // 哈希表(非常量供 MAKE_MAP 的逐对 set 与 store_index 填充;容器成员直曝)。
        [[nodiscard]]
        AriaHashTable& table() noexcept {
            return table_;
        }

        [[nodiscard]]
        const AriaHashTable& table() const noexcept {
            return table_;
        }

        void trace(GC& gc) const noexcept override;

        // 壳定长(table_ 内联在壳内,entries/ctrl 缓冲由其分配器自持)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjMap);
        }

        // 内容相等:同为 map 且 size 相等且逐键命中同键、值 value_equal;其余(含跨类型)恒 false。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 调试渲染:{"a": 1} 式。
        [[nodiscard]]
        String debug_repr() const override;

        // 裸读 override:委托 Map bootstrap 类表直取原生值。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:同一查找命中恒绑 this。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

        // 下标读取:任意键,miss KeyError(键走 debug 形入文案);find 纯查询无分配。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 下标写入:恒成功,命中覆写、未命中新增;键值经调用方值栈为根(set/rehash 不触 GC)。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

    private:
        AriaHashTable table_; // 键值表(GC 分配器绑定;set 惰性首分配,trivial 分配不触 GC)
    };

    // 工厂:分配空 ObjMap(单次分配,无入参对象可守);返回对象白色无根,调用方建成即发布进根。
    [[nodiscard]]
    ObjMap* new_map(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_MAP_HPP
