#ifndef ARIA_OBJ_STRING_HPP
#define ARIA_OBJ_STRING_HPP

#include "common.hpp"
#include "object/Object.hpp"

namespace aria {

    class GC;
    class ObjRange;

    // 字符串对象:SSO(短串内联 / 长串独立 buffer)。
    //
    //   - 长度 <= kShortCapacity(15):内联 short_chars_[16](15 字符 + NUL),无额外分配。
    //   - 长度  > kShortCapacity:long_chars_ 指向 gc.allocate<char>(length_+1) 的独立 buffer,
    //     ~ObjString 时 gc_->deallocate<char> 释放。壳本身定长(sizeof(ObjString))。
    //
    //   is_long() 由 length_ > kShortCapacity 派生(不存标志位,省 1 字节 + 填充)。
    //   trace() 空(纯字节)。哈希(FNV-1a 32-bit)构造时算出,存 Object::hash_。
    //   持 GC* gc_ 供 ~ObjString 释放 long_chars_(非 Array 子内存的释放统一走虚析构)。
    //
    //   Phase 2 起接 intern 驻留池:new_string 先查 GC 的 InternPool,命中返回已有串,
    //   未命中才 new_object + insert。等价内容的串共享同一 ObjString*。
    //
    //   下标读经 load_index 协议 override:整数键 = 字节语义(计划 D5,与 len 同域),产出
    //   单字节 1-char string;多字节序列中间字节取该字节自身(字节契约的自然结果)。下标
    //   写恒报错(string 不可变,TypeMismatch 定向文案)。命名成员经 load_field 委托 VM 的
    //   String bootstrap 类方法表恒绑定(两步,同 ObjList/ObjMap 形)。
    class ObjString final : public Object {
    public:
        static constexpr usize kShortCapacity = 15;

    private:
        GC* gc_; // 供 ~ObjString 释放 long_chars_(仅长串用到)
        union {
            char  short_chars_[kShortCapacity + 1]; // 16 字节,与 char* 取大
            char* long_chars_;
        };
        usize length_;

    public:
        ObjString(GC& gc, StringView src);
        ~ObjString() override;

        // 借出内容视图:指向 SSO 内联 buffer 或 long_chars_。非移动 GC 对象地址与缓冲恒定,
        // view 在 ObjString 存活期内有效;但串被 sweep 回收后 view 即悬垂,勿长期持有跨 collect
        // 的 view(经 intern 持串则随持串者保命)。
        [[nodiscard]]
        StringView view() const noexcept;

        // 内容相等(==):先比指针(intern 命中快速路径),再比 view() 字符内容。
        // override Object::equals(默认地址相等)。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 可读描述(显示位):字符内容原文(无引号),如 hello。override Object::to_string
        // (基类默认委托 debug_repr,本类型显示与调试分叉故两者都 override)。
        [[nodiscard]]
        String to_string() const override;

        // 调试渲染(repr 位):字面量形式 `"<转义内容>"`--util::escape_string 转义内部、外层
        // 补双引号(反汇编常量池等调试上下文的字符串约定形态);显示与调试分叉故两者都 override。
        [[nodiscard]]
        String debug_repr() const override;

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

        // 下标读取:整数键(字节域,D5),产出单字节 1-char string;负数从尾计数、归一化后
        // 越界 IndexOutOfBounds、非整数 TypeMismatch;多字节序列中间字节取该字节自身(字节
        // 契约的自然结果)。Range 键走切片(见 slice)。查读含一次 new_string(intern)分配:
        // receiver 经调用方值栈为根。
        [[nodiscard]]
        Opt<Value> load_index(AriaVM& vm, Value key) override;

        // 切片(Range 键):段解析收口 ObjRange.cpp 的 resolve_slice_bounds(有上界与无上界两形态
        // 统一),与 list 切片同口径 -- 端点从尾计数、无上界 i.. 允许空段、越界/空串 nullopt(报
        // IndexOutOfBounds "slice index out of range",与 list 同串)。域仍是字节(与 s[i]/len
        // 同域):倒序段产出**字节逆序**串,多字节输入下不是合法 UTF-8 -- 与 s[i] 能取到续接字节
        // 同属字节域契约(按码点反转需另立码点域口径,不在切片内)。
        [[nodiscard]]
        Opt<Value> slice(AriaVM& vm, const ObjRange* range) const;

        // 下标写入:string 不可变,恒 TypeMismatch 定向文案。
        [[nodiscard]]
        bool store_index(AriaVM& vm, Value key, Value value) override;

        // 算术协议 override(算术族唯一接线者:ADD 指令经 AriaVM::run_binary_add 在对象左值
        // 上派发到此):两侧均为 String 即拼接,结果经 new_string 驻留(同内容必同指针);
        // 否则 TypeMismatch 定向文案。不做隐式转字符串,显式转换走内置 str()。
        [[nodiscard]]
        Opt<Value> op_add(AriaVM& vm, Value rhs) const override;

        // 比较算子 override(四个比较指令经 AriaVM::run_binary_compare 在对象左值上派发):
        // 两侧均为 String 即按**字节序**(unsigned/memcmp 语义,string_view::compare)比较,返回
        // 装箱 Bool;否则 TypeMismatch 定向文案。字节序与 len/s[i] 的字节域同域,且对 s[i] 切出的
        // 非法单字节串仍全序;UTF-8 保序,故合法文本上结果与按码点比较一致(无 locale/collation)。
        [[nodiscard]]
        Opt<Value> op_less(AriaVM& vm, Value rhs) const override;

        [[nodiscard]]
        Opt<Value> op_less_equal(AriaVM& vm, Value rhs) const override;

        [[nodiscard]]
        Opt<Value> op_greater(AriaVM& vm, Value rhs) const override;

        [[nodiscard]]
        Opt<Value> op_greater_equal(AriaVM& vm, Value rhs) const override;

        // 命名成员读取协议 override:内置侧两步,与实例路径同构(同 ObjList::load_field
        // 形)--先委托 VM 的 String bootstrap 类协议(ObjClass::load_field 沿链查表,miss
        // 类措辞 fail 随协议透传),命中即自持 new_bound_method 恒绑定 this。store_field 不
        // override:基类默认「does not support field access」即内置类型的正确行为。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 方法调用解析协议 override:与 load_field 同一趟类表查找,命中直取类表原生值交 VM 调用
        // -- 恒绑定但不铸 ObjBoundMethod(内置侧 bound 无缓存可回填,每取一次白铸一个;forIn 每迭代
        // 两个,见集合计划 §4.4 基线)。调用区槽 0 保持 receiver 原样,正是原生要的 this。
        [[nodiscard]]
        Opt<Value> resolve_invoke(AriaVM& vm, ObjString* name) override;
    };

    // 工厂:返回内容等于 src 的 ObjString*。经 GC 驻留池:命中返回已有串,未命中分配+驻留。
    [[nodiscard]]
    ObjString* new_string(GC& gc, StringView src);

    // 单字节串便捷重载(下标读产出形态):委托 StringView 版,同样经驻留池。
    [[nodiscard]]
    ObjString* new_string(GC& gc, char ch);

} // namespace aria

#endif // ARIA_OBJ_STRING_HPP
