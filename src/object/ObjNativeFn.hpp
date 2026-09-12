#ifndef ARIA_OBJ_NATIVE_FN_HPP
#define ARIA_OBJ_NATIVE_FN_HPP

#include "aria.hpp"   // kAnonymousName
#include "common.hpp" // Span / u8 / String (经 type.hpp)
#include "object/Object.hpp"
#include "value/Value.hpp" // Value

namespace aria {

    class GC;
    class ObjString;
    class AriaVM; // 前向声明:NativeFn 形参取引用,本头不依赖其完整定义

    // 原生函数的 C++ 调用签名(宿主侧实现,供 VM 在 CALL 命中 ObjNativeFn 时同步直接调用,
    // 不经字节码帧)。builtins(type/len/str/assert)与未来嵌入 API 皆包成此类型。
    //
    //   - vm:宿主句柄(对标 lua_State* / WrenVM* / N-API env)。供报错(vm.fail/vm.raise,写入
    //     当前上下文的挂起错误寄存器,见下「错误」)、分配对象(vm.gc();跨分配持有的裸 Obj*
    //     须 make_guard 根化)。M6 协程期 vm 路由到当前协程,原生函数无需持 VMContext 引用。
    //   - slots:调用区可写视图,指向值栈上 [callee, a1..aN] 的连续 argc+1 个槽:
    //       slots[0]  = callee / **返回槽**(返回值直接写于此,省去 drop+push);
    //       slots[1..argc] = 实参;argc = slots.size() - 1。
    //     原生函数天然变参(不存 arity 字段),元数自查。
    //
    //     方法调用形态(经 ObjBoundMethod 绑定):调用区槽 0 为 bound 对象,VM 调用前覆写为
    //     receiver -- 原生收到的 slots[0] = this,同时仍是返回槽;实参槽位与自由调用一致。
    //     类路径/静态访问(`Foo.m`)取出裸原生值不绑定,slots[0] = 原生自身,与自由调用无异。
    //
    //   返回值:bool。true = 成功(返回值已写 slots[0]);false = 失败(已调 vm.fail/vm.raise)。
    //   惯用法--成功 `slots[0] = ...; return true;`,失败一行 `return vm.fail(code, fmt, ...);`。
    //   VM 调用后 drop(argc) 弹掉实参,slots[0] 升至栈顶即为返回值。
    //
    //   错误(侧信道):冷路径错误不编进每次调用的返回类型。AriaVM::raise/fail 把 code+detail
    //   一步烘齐**完整烘焙消息**(Error::make_message:位置取自调用方帧 -- 原生不进帧,顶帧即
    //   caller,位置恰为 CALL 站点;帧栈空即 run 外直调则无位置)装箱 ObjException 存入寄存器,
    //   用户 throw 的原值路由另走 Movement::raise(Value)。VM 在 CALL 后以返回的 bool 为成败
    //   信号:成功 drop argc;失败载荷留寄存器,调用方走 unwind 查异常记录表,全未命中物化为
    //   Error 从 run() 返回。故 Error 仅在出错时构造。
    //
    //   分配安全:vm.fail 内 new_exception 可能触发 GC,值栈/帧/builtins_ 已接根,载荷入寄存器
    //   后经 VM 根 tracer 标根。
    //
    //   bool 与寄存器的同步:**契约:return false ⟺ 已调 vm.fail/vm.raise;用
    //   `return vm.fail(...)` 即自动满足。**VM 据 bool 分支:成功路径仅 debug 断言 !has_error()
    //   验证契约(release 不静默清掉掩盖);失败路径 take_error() 取载荷(空则解引用空 Opt 属
    //   UB,debug 断言先暴露)。
    //
    //   契约:
    //   1. 原生函数是**叶子调用**--不得操作 VM 值栈(push/pop/drop),否则 slots 视图失效。
    //      只读 slots[1..]、写 slots[0]、经 vm.fail/raise 报错。需回调 aria 函数属未来机制。
    //   2. 读即时值(数字/布尔/nil/既有对象指针)无 GC,任意阶段安全;写新生对象到 slots[0] 需
    //      GC 解锁(M6+)且分配的中间对象须 Guard 入临时根(slots[0] 写入后即随值栈为根)。
    //   3. 错误经 vm.raise 侧信道(寄存器),不抛 C++ 异常(通道 3 仅限 VM 外),不用 longjmp。
    using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);

    // 原生函数对象:把一个 C++ NativeFn 包成 aria Value。
    //
    //   - name_:函数名(intern 驻留,同指针),供报错渲染与 to_string;**恒非空**(构造期
    //     ASSERT)-- 无具名需求者用 kAnonymousName 作匿名名,不可传 nullptr。
    //   - fn_:C++ 函数指针(恒非空,构造断言)。不存 arity(原生天然变参,fn 自查)。
    //
    //   地址哈希型可变对象;equals 保持默认地址相等 -- 原生函数无「内容相等」语义。final,不再派生。
    //   trace():标 name_(fn_ 是 C++ 指针,非 GC 对象,不标);name_ 恒非空,mark_object 无需容 nullptr。
    class ObjNativeFn final : public Object {
    public:
        ObjNativeFn(ObjString* name, NativeFn fn);
        ~ObjNativeFn() override = default; // 无 GC 子内存,壳定长

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        [[nodiscard]]
        NativeFn fn() const noexcept {
            return fn_;
        }

        // mark_object 容 nullptr 仅防御(本应恒非空,见类注)。
        void trace(GC& gc) const noexcept override;

        // 壳定长,无外挂子内存。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjNativeFn);
        }

        // 调试渲染:`<fn name>`(与 ObjFunction 一致 -- 用户侧不区分 native / user 函数,
        //   native 身份经 ObjType::NATIVE_FN / type() 反射可见)。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ObjString* name_;
        NativeFn   fn_;
    };

    // 匿名原生函数名用 aria.hpp 的 kAnonymousName("<anonymous>" -- `<>` 不可作标识符,具辨识度):
    // 无具名需求者经下方 NativeFn-only 重载自动 intern 驻留。

    // 工厂:分配 ObjNativeFn。**调用方须在调用前自行根化 name**(intern 串是 weak root,跨
    //   new_object 顶 maybe_collect 可能被回收;工厂只做一次 new_object,不替调用方守卫),
    //   与 new_function 同理。fn 是标量,无需入根。name 须非空(构造期 ASSERT)。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, NativeFn fn);

    // 工厂重载(匿名):name 取 kAnonymousName,工厂内部 new_string 驻留并自行守卫。委托上者。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, NativeFn fn);

    // 工厂重载(StringView 名):name 经工厂内部 intern 并自行守卫(bootstrap/builtins 注册等
    //   无现成 intern 串的站点免去 new_string+guard 两步)。委托上者。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, StringView name, NativeFn fn);

} // namespace aria

#endif // ARIA_OBJ_NATIVE_FN_HPP
