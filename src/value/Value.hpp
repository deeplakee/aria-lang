#ifndef ARIA_VALUE_HPP
#define ARIA_VALUE_HPP

#include "common.hpp"

#ifdef USING_NANBOXING
    #include "value/NanBoxing.hpp"
namespace aria {
    using namespace nanboxing;
}
#else
    #include "value/TagValue.hpp"
namespace aria {
    using namespace tagvalue;
}
#endif

namespace aria {

    class ObjString;

    // Value 操作(哈希 / 相等)
    //
    //   值语义收口于 Value 层:声明在此,定义在 Value.cpp。AriaHashTable.hpp 中的
    //   ValueHash/ValueEq 仿函数(HashTable 模板参数)包装这些自由函数;VM 的
    //   OpCode::EQUAL(==)/STRICT_EQUAL(===) 直接调用。基于两表示共有的 type()/as_*() API,
    //   **不**依赖 NanBoxing 专属的 bits()/same_bits()(TagValue 未提供,故两表示都编译)。
    //
    //   双相等体系(语言 == 与 === 的语义来源):
    //   - value_equal(== 内容相等):Nil/Bool 类型严格按值;Int/F64 **跨类型 IEEE 数值**
    //     (1==1.0 true、-0.0==0.0 true、NaN!=NaN);Obj 调 Object::equals 虚函数
    //     (ObjString 比内容,其它默认地址)。不做 JS 全套强制转换(bool/字符串不与数值互比)。
    //   - value_identical(=== 严格相等):类型严格(1===1.0 false);f64 按位(-0.0!==0.0、
    //     NaN 规范化后 NaN===NaN true);Obj 指针相等。
    //
    //   哈希表键用 ===(value_identical):对象按引用做键,字符串靠 intern 等价内容同指针 ->
    //   按内容查到。value_hash 已与 === 自洽(a===b => hash(a)==hash(b)),无需改。
    [[nodiscard]]
    inline bool is_num(const Value v) noexcept {
        return v.is_int() || v.is_f64();
    }

    // 真值(Lua 风格):nil 与 false 为假,其余(含 0/"")为真。
    [[nodiscard]]
    inline bool is_truthy(const Value v) noexcept {
        return !(v.is_nil() || (v.is_bool() && !v.as_bool()));
    }

    // 值的精确类型名(PascalCase,全项目统一约定):原语走 Value::type_name() constexpr 成员
    // (Nil/Bool/Int/F64/Obj);Obj 走 obj->type_name() 取对象子类型(String/Function/NativeFn/...)。
    // 区别于 Value::type_name() 成员--后者为 constexpr 粗分类,Obj 一律返 "Obj" 丢子类型;
    // 本自由函数是**精确类型名**的统一入口,供错误消息类型名打印用。定义在 Value.cpp(需 Object 完整类型)。
    [[nodiscard]]
    StringView type_name(Value v) noexcept;

    [[nodiscard]]
    u32 value_hash(Value v) noexcept;

    // 内容相等(==):见上。语言 == 与 match 的语义来源。
    [[nodiscard]]
    bool value_equal(Value a, Value b) noexcept;

    // 严格相等(===):见上。哈希表键相等;语言 === 的语义来源。
    [[nodiscard]]
    bool value_identical(Value a, Value b) noexcept;

    // 值的字符串渲染
    //
    //   f64 可读化:保证含 `.`/`e`/`E`(整值补 `.0`),与 Int 区分;inf/nan 直出。
    //   收口于 Value 层供多处复用(VM 的 PRINT 渲染、反汇编器常量池小节等),避免逻辑散落重复。
    [[nodiscard]]
    String format_f64(f64 d);

    // 值的可读渲染(PRINT / REPL 回显等用):nil/true/false/整数/浮点/对象描述。
    //   Obj 统一走虚函数 to_string()(显示位:多数内建类型经基类默认委托 debug_repr;ObjString
    //   返回原文无引号)。区别于调试渲染的带引号字面量形式(居 ObjString::debug_repr)。
    [[nodiscard]]
    String format_value(Value v);

    // 值的**非重入**调试渲染(执行跟踪 / 反汇编常量池等调试上下文用):内置原语与 format_value
    //   一致,Obj 走虚函数 debug_repr() 而非可重载的 to_string()(后者是未来用户类 __str__ 的
    //   挂载点,可重载为运行 aria 字节码,调试上下文若在 dispatch_loop 内调用会重入 VM 致无限递归)。
    //   debug_repr 的 override 契约是纯 C++ 惰性渲染(绝不重入 VM / 不触 GC 回收,见 Object.hpp),
    //   语言层无法新增 C++ 子类型、override 集合编译期封闭,故虚分派绝不触用户重载;各类型 debug
    //   文案由各子类型自己实现(ObjString 字面量带引号转义 / ObjFunction/ObjClosure/ObjNativeFn
    //   渲染 `<fn name>` / ObjUpvalue `<upvalue>` 稳定短文案 / 其余内建类型同各自显示文案)。
    //   原语 nil/bool/int/f64 同 format_value。
    [[nodiscard]]
    String format_value_debug(Value v);

} // namespace aria

#endif // ARIA_VALUE_HPP
