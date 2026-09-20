#ifndef ARIA_OBJ_MAP_HPP
#define ARIA_OBJ_MAP_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;

    // map 对象(ObjType::MAP):`{...}` 字面量的运行期载体,键值均任意 Value、存
    // AriaHashTable(Swiss Table;表内键判等 ===,int 1 与 f64 1.0 是不同键;可变对象作
    // 键按身份哈希)。下标读写经 load_index/store_index 协议 override;命名成员经
    // load_field 委托 VM 的 Map bootstrap 类方法表恒绑定。
    //
    //   - 迭代序 unspecified(计划 D4):非定序哈希表,用户不应依赖;map 迭代器产出
    //     [k, v] 二元 list。迭代中变更容器 v1 不承诺(rehash 搬迁槽位),文档明示。
    //   - 地址哈希型可变对象(可变故作 map 键按身份);equals 按内容:size 相等且逐键在
    //     对方命中同键(键按表内语义 ===,find 即 value_identical 匹配)、值 value_equal
    //     (嵌套容器经各自 equals 递归);入口挂 EqualGuard 防环(重遇同对视为相等);
    //     `===` 恒指针(value_identical,不经本类)。
    //   - trace():委托 table_.trace(遍历占用槽 mark_value key+value)。
    //   - debug_repr():`{"a": 1}` 式,键值走 format_value_debug(嵌套字符串带引号,
    //     嵌套容器递归;渲染序随占用槽,同 unspecified);入口挂 PrintGuard 防环
    //     (自引用/互环截断 "{...}",Python 同款);显示同文案(to_string 经基类默认委托)。
    class ObjMap final : public Object {
    public:
        // 哈希表惰性分配,ctor 只绑分配器(首分配由 set 的扩容路径自理)。
        explicit ObjMap(GC& gc);

        ~ObjMap() override = default; // 键值是 GC 值,不归本类释放

        ObjMap(const ObjMap&)            = delete;
        ObjMap& operator=(const ObjMap&) = delete;
        ObjMap(ObjMap&&)                 = delete;
        ObjMap& operator=(ObjMap&&)      = delete;

        // 哈希表(非常量供填充:MAKE_MAP 的逐对 set 与 store_index;对标
        // ObjModule::globals 的容器成员直曝)。
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

        // 下标读取:任意键,miss KeyError(键走 debug 形入文案);find 纯查询无分配。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 下标写入:恒成功,命中覆写、未命中新增键(set 的两条路径均无报错;键值在调用方
        // 值栈为根,set/rehash 走 GC 分配器不触 GC)。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

        // 命名成员读取协议 override:内置侧两步,与实例路径同构(同 ObjList::load_field
        // 形)--先委托 VM 的 Map bootstrap 类协议(ObjClass::load_field 沿链查表,miss 类
        // 措辞 fail 随协议透传),命中即自持 new_bound_method 恒绑定 this。store_field 不
        // override:基类默认「does not support field access」即内置类型的正确行为。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 方法调用解析协议 override:与 load_field 同一趟类表查找,命中直取类表原生值交 VM 调用
        // -- 恒绑定但不铸 ObjBoundMethod(内置侧 bound 无缓存可回填,每取一次白铸一个;forIn 每迭代
        // 两个,见集合计划 §4.4 基线)。调用区槽 0 保持 receiver 原样,正是原生要的 this。
        [[nodiscard]]
        Opt<Value> resolve_invoke(AriaVM& vm, ObjString* name) override;

        // 调试渲染:{"a": 1} 式。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        AriaHashTable table_; // 键值表(GC 分配器绑定;set 惰性首分配,trivial 分配不触 GC)
    };

    // 工厂:分配空 ObjMap。只做一次 new_object、无内部新建对象,无入参对象可守;返回对象
    //     白色无根,调用方建成即发布进根(MAKE_MAP:键值自值栈逐对 set 入表走 trivial 分配
    //     不触 GC,随即 drop+push 入值栈根,窗口内无 GC 点)。
    [[nodiscard]]
    ObjMap* new_map(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_MAP_HPP
