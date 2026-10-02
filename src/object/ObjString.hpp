#ifndef ARIA_OBJ_STRING_HPP
#define ARIA_OBJ_STRING_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjRange;

    // 字符串对象:SSO 表示(短串内联,长串独立缓冲经 gc_ 释放);内容哈希,等价内容经驻留池共享同一对象。
    class ObjString final : public Object {
    public:
        static constexpr usize kShortCapacity = 15;

        ObjString(GC& gc, StringView src, u32 hash);

        ~ObjString() override;

        // 借出内容视图:非移动 GC 对象地址与缓冲恒定,存活期有效;sweep 回收后即悬垂,勿跨 collect 持有。
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

        // 无 Value 子节点。
        void trace(GC&) const noexcept override {}

        // 壳定长(长串 buffer 不计入壳,由 ~ObjString 单独释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjString);
        }

        // 内容相等(==):先比指针(intern 命中快速路径),再比 view() 字符内容。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 调试渲染(repr 位):字面量形式 `"<转义内容>"`(显示与调试分叉,与 to_string 成对 override)。
        [[nodiscard]]
        String debug_repr() const override;

        // 可读描述(显示位):字符内容原文(无引号)。
        [[nodiscard]]
        String to_string() const override;

        // 裸读 override:委托 String bootstrap 类表直取原生值。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 绑定读 override:同一查找命中恒绑 this。
        [[nodiscard]]
        Opt<Value> load_field_bound(AriaVM& vm, ObjString* name) override;

        // 整数键 = 字节域(与 len 同域):产出单字节 1-char string,负数从尾计数、归一化后越界即报;
        // 多字节序列中间字节取该字节自身。Range 键走切片。查读含一次 intern 分配:receiver 经值栈为根。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 下标写入:string 不可变,恒 TypeMismatch 定向文案。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

        // 算子协议 override(内建直给,不经成员查找):读 bootstrap 期注册进类表并拷进实现格的钩子原生值
        //(类表仍是规范家);其余算子不 override -> 基类默认报「本类型不支持该算子」。
        [[nodiscard]]
        Opt<Value> op_add_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_mul_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_less_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_less_equal_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_greater_impl(AriaVM& vm) override;

        [[nodiscard]]
        Opt<Value> op_greater_equal_impl(AriaVM& vm) override;

    private:
        // 切片(Range 键):段解析收口 resolve_slice_bounds(报错文案与 list 切片同串)。域是字节(与 s[i]/len
        // 同域):倒序段产出字节逆序串,多字节输入下不是合法 UTF-8 -- 与 s[i] 取续接字节同属字节域契约。
        [[nodiscard]]
        Opt<Value> slice(AriaVM& vm, const ObjRange* range) const;

        GC* gc_; // 供 ~ObjString 释放 long_chars_(仅长串用到)
        union {
            char  short_chars_[kShortCapacity + 1]; // 16 字节,与 char* 取大
            char* long_chars_;
        };
        usize length_;
    };

    // 工厂:返回内容等于 src 的 ObjString*。经 GC 驻留池:命中返回已有串,未命中分配+驻留。
    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView src);

    // 调用方已持有 hash_str(src) 之值时用本重载,免重算。
    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView src, u32 hash);

    // 单字节串便捷重载(下标读产出形态):委托 StringView 版,同样经驻留池。
    [[nodiscard]]
    ObjString* new_string(GC& gc, char ch);

} // namespace aria

#endif // ARIA_OBJ_STRING_HPP
