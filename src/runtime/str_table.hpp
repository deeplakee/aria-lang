#ifndef ARIA_STR_TABLE_HPP
#define ARIA_STR_TABLE_HPP

#include "common.hpp"

namespace aria::str_table {

    // VM 常量串注册表(常量串表 = str 的表,故名单 str_table):VM 自己在**运行期按名取用**、须恒久
    // 存活的字符串常量,单一事实源即本表。常量与查下标方法同居一命名空间。字面量形态经 AriaVM::str
    // <"__add__">() 取用:键经 index_of 编译期定下标,访问口以 static_assert 校验,不在表内的键编不过。
    // 运行期键(kOperatorFns 表键、coroutine status)不走本表查下标,一律 new_string 驻留命中取串 --
    // 条目恒在驻留池且随本表标根,命中即注册表对象、零分配。
    // **成员判据**:只收「VM 自己按名取用」的串。代码里写下的常量名(字段名/方法名/函数名等)不进本表 -- 它们编进
    // 常量池,经 ObjFunction::trace -> CodeUnit::trace 已随函数可达。
    // **为什么须恒久存活**:驻留池(InternPool)是 weak root,串无可达引用则下轮 collect 即被摘除,下次按名取用重铸
    // 一个新对象。算子钩子名正是这种串(实例算子派发每次都要一个稳定的 ObjString* 去查表),如今只被「恰好把它们
    // 当键的类表」间接保命 -- 那是巧合不是契约。故 AriaVM bootstrap 期一趟 new_string 把本表填进 string_constants_,
    // 并随 registers_ 一起在 VM 根 tracer 里标根:表在则串在。
    // **命名**:算子/调用钩子取前后双下划线形 -- 普通 aria 标识符不会这么命名,故与用户自己的方法名不撞、一眼可辨是
    // 协议名;十一个名字与 Object::op_*_impl 虚函数族一一对应(那族回答「本对象上该算子/调用对应的可调用值」,
    // 内建类型直给自身实现)。`/` 是 aria 唯一的除法算子(无 // 形态),故取 __div__。本表无字节码消费者
    //(对照 kValueRegisterNames 那种给反汇编打印用的名表),故只此一张表。
    // 第二类成员是 coroutine.status 的 5 个状态串(与 ObjMovement 的 to_string(ExecState) 同拼写;进表
    // 是为随表标根保命,status 返参经 new_string(to_string(state)) 驻留命中取回同一条)。
    // 第三类成员是 Exception face 的字段键 _message/_code:默认 init 落字段与 face 实例腿读据都按名取用;字段键
    // 只活在实例 fields,无任何类表键锚定,weak root 下无实例存活即被摘除重铸,故入表保命。
    inline constexpr StringView kConstants[] = {
            "__add__",   // binary +(十一个钩子名与 Object::op_*_impl 虚函数族一一对应)
            "__sub__",   // binary -
            "__mul__",   // binary *
            "__div__",   // binary /(aria 唯一的除法算子,无 // 形态)
            "__mod__",   // binary %
            "__lt__",    // comparison <
            "__le__",    // comparison <=
            "__gt__",    // comparison >
            "__ge__",    // comparison >=
            "__neg__",   // unary negation
            "__call__",  // call hook
            "_message",  // Exception face 字段键
            "_code",     // Exception face 字段键
            "suspended", // coroutine status 串
            "running",   // coroutine status 串
            "normal",    // coroutine status 串
            "done",      // coroutine status 串
            "failed",    // coroutine status 串
    };

    // 表长:常量串表条数(供 VM 预置 string_constants_ 格位,同 kValueRegisterCount)。
    inline constexpr usize kCount = std::size(kConstants);

    // 按键查表下标:consteval 逐条比对,命中返下标,查不到返 nullopt(纯查询无副作用)。消费者
    //(AriaVM::str 字面量形态)对返回值做 static_assert,不在表内的键在编译期直接报错。
    [[nodiscard]]
    consteval Opt<usize> index_of(const StringView key) {
        for (usize index = 0; index < kCount; ++index) {
            if (kConstants[index] == key) {
                return index;
            }
        }
        return std::nullopt;
    }

} // namespace aria::str_table

#endif // ARIA_STR_TABLE_HPP
