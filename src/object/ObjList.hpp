#ifndef ARIA_OBJ_LIST_HPP
#define ARIA_OBJ_LIST_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaArray.hpp"

namespace aria {

    class GC;

    // list 对象(ObjType::LIST):`[...]` 字面量的运行期载体,元素为任意 Value、按下标顺序
    // 存 AriaArray(MAKE_LIST 一次整段拷入;下标读写经 load_index/store_index 协议 override,
    // 待后续批接线)。
    //
    //   - 地址哈希型可变对象(可变故作 map 键按身份);equals 按内容递归:长度相等且逐元素
    //     value_equal(嵌套 list 经各自 equals 递归),value_equal 无分配、GC-pure 契约保持;
    //     `===` 恒指针(value_identical,不经本类)。
    //   - trace():委托 elements_.trace(遍历元素 mark_value)。
    //   - debug_repr():`[1, "ab"]` 式,元素走 format_value_debug(嵌套字符串带引号,避免
    //     `[1, ab]` 歧义;嵌套 list 递归);显示同文案(to_string 经基类默认委托)。
    class ObjList final : public Object {
    public:
        // 元素表惰性增长,ctor 只绑分配器(首字节预留由 copy_from/push 的扩容路径自理)。
        explicit ObjList(GC& gc);

        ~ObjList() override = default; // 元素是 GC 值,不归本类释放

        ObjList(const ObjList&)            = delete;
        ObjList& operator=(const ObjList&) = delete;
        ObjList(ObjList&&)                 = delete;
        ObjList& operator=(ObjList&&)      = delete;

        // 元素表(非常量供填充:MAKE_LIST 的 copy_from 与后续 push/pop 方法批;对标
        // ObjModule::globals 的容器成员直曝)。
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

        // 下标读取:整数键,越界/负数 IndexOutOfBounds、非整数键 TypeMismatch(越界值就地
        // 拼进文案);查读无分配。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 下标写入:键检查同读,不自动增长(越界即报,追加走 push 方法);写已存槽恒成功。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

        // 调试渲染:`[1, "ab"]`;显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        AriaArray elements_; // 元素表(GC 分配器绑定;push/copy_from 惰性增长,trivial 分配不触 GC)
    };

    // 工厂:分配空 ObjList。只做一次 new_object、无内部新建对象,无入参对象可守;返回对象
    //     白色无根,调用方建成即发布进根(MAKE_LIST:元素自值栈整段拷入走 trivial 分配不触
    //     GC,随即 drop+push 入值栈根,窗口内无 GC 点)。
    [[nodiscard]]
    ObjList* new_list(GC& gc);

} // namespace aria

#endif // ARIA_OBJ_LIST_HPP
