#ifndef ARIA_OBJ_LIST_HPP
#define ARIA_OBJ_LIST_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaArray.hpp"

namespace aria {

    class GC;
    class ObjRange;

    // list 对象(ObjType::LIST):`[...]` 字面量的运行期载体,元素为任意 Value、按下标顺序存 AriaArray(MAKE_LIST 一次整段
    // 拷入)。下标读写经 load_index/store_index 协议 override;命名成员(push/pop 等)经 load_field 委托 VM 的 List
    // bootstrap 类方法表恒绑定。
    //   - 地址哈希型可变对象(可变故作 map 键按身份);equals 按内容递归:长度相等且逐元素 value_equal(嵌套 list 经各自
    //     equals 递归),value_equal 无分配、GC-pure 契约保持;入口挂 EqualGuard 防环(重遇同对视为相等,正则树同构判等);
    //     `===` 恒指针(value_identical,不经本类)。
    //   - trace():委托 elements_.trace(遍历元素 mark_value)。
    //   - debug_repr():`[1, "ab"]` 式,元素走 format_value_debug(嵌套字符串带引号,避免 `[1, ab]` 歧义;嵌套 list 递归);
    //     入口挂 PrintGuard 防环(自引用/互环截断 `[...]`,Python 同款);显示同文案(to_string 经基类默认委托)。
    class ObjList final : public Object {
    public:
        // 元素表惰性增长,ctor 只绑分配器(首字节预留由 copy_from/push 的扩容路径自理)。
        explicit ObjList(GC& gc);

        ~ObjList() override = default; // 元素是 GC 值,不归本类释放

        ObjList(const ObjList&)            = delete;
        ObjList& operator=(const ObjList&) = delete;
        ObjList(ObjList&&)                 = delete;
        ObjList& operator=(ObjList&&)      = delete;

        // 元素表(非常量供 MAKE_LIST 的 copy_from 与 push/pop 方法填充;容器成员直曝)。
        [[nodiscard]]
        AriaArray& elements() noexcept {
            return elements_;
        }

        [[nodiscard]]
        const AriaArray& elements() const noexcept {
            return elements_;
        }

        void trace(GC& gc) const noexcept override;

        // 壳定长(elements_ 内联在壳内,元素缓冲由其 Buffer 自持)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjList);
        }

        // 内容相等:同为 list 且长度相等且逐元素 value_equal;其余(含跨类型)恒 false。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 调试渲染:`[1, "ab"]`;显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

        // 命名成员读取协议 override:查 List bootstrap 类表,命中自持 new_bound_method 恒绑 this
        //(两步形态权威注见 Object.hpp)。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 方法调用解析 override:同一趟类表查找但不铸 ObjBoundMethod,直取类表原生值(见 Object.hpp)。
        [[nodiscard]]
        Opt<Value> load_field_unbound(AriaVM& vm, ObjString* name) override;

        // 下标读取:Range 键 = 切片(产出新 list,端点从尾计数、越界 fail-fast、倒序 range 产
        // 出倒序段、只读;见 slice,切片路径有分配);整数键:负数从尾计数、归一化后越界
        // IndexOutOfBounds、非整数键 TypeMismatch(越界值就地拼进文案);整数键查读无分配。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 下标写入:键检查同读,不自动增长(越界即报,追加走 push 方法);写已存槽恒成功。
        // Range 键不特殊对待,落整数键检查的统一文案(切片写只读)。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

        // 算子协议 override(内建实现直给,**不经成员查找**):返回 List 类表里对应钩子的原生
        // 函数值(`__add__`/`__mul__` 在 bootstrap 期注册进类表并同时拷进实现格 kList*Fn,这里
        // 读格即得;类表仍是规范家)。其余算子不 override -> 基类默认报「本类型不支持该算子」。
        [[nodiscard]]
        Opt<Value> op_add_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_mul_impl(AriaVM& vm) override;

    private:
        // 切片(Range 键):段解析收口 ObjRange.cpp 的 resolve_slice_bounds(有上界与无上界两形态
        // 皆在内;从尾计数,无上界给后缀、空后缀以两端相等+不含上界表示),本函数只管按方向折算
        // count 与铸新 list 段拷。
        [[nodiscard]]
        Opt<Value> slice(AriaVM& vm, const ObjRange* range);

        AriaArray elements_; // 元素表(GC 分配器绑定;push/copy_from 惰性增长,trivial 分配不触 GC)
    };

    // 工厂:分配空 ObjList(单次分配,无入参对象可守);返回对象白色无根,调用方建成即发布进根。
    [[nodiscard]]
    ObjList* new_list(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_LIST_HPP
