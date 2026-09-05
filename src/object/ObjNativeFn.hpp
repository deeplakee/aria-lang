#ifndef ARIA_OBJ_NATIVE_FN_HPP
#define ARIA_OBJ_NATIVE_FN_HPP

#include "common.hpp" // Span / u8 / String (经 type.hpp)
#include "object/Object.hpp"
#include "value/Value.hpp" // Value

namespace aria {

    class GC;
    class ObjString;
    class AriaVM; // 前向声明:NativeFn 形参取引用,本头不依赖其完整定义

    // 原生函数的 C++ 调用签名(宿主侧实现,供 VM 在 CALL 命中 ObjNativeFn 时同步直接调用,
    // 不经字节码帧)。builtins(print / len / type / assert ...)与未来嵌入 API 皆包成此类型。
    //
    //   返回 bool、错误走侧信道、返回值写槽 0 -- 三者配套设计,把冷路径错误踢出返回类型:
    //
    //   - vm:宿主句柄(对标 Lua lua_State* / Wren WrenVM* / N-API env -- 单一状态句柄)。
    //       供:报错--vm.fail(code, fmt, ...)/vm.raise(code, detail)(写入当前上下文的挂起错误寄存器,
    //       见下"错误");分配对象--vm.gc()(run() 期值栈/帧已接 GC 根,内建内可经 vm.gc().new_string 等
    //       分配;跨分配持有的裸 Obj* 须 make_guard 根化);未来回调 aria 函数 / 反射(待 vm 暴露相应访问器)。
    //       M6 协程期 vm 路由到当前协程,故原生函数无需也不持 VMContext 引用 -- 单句柄即可,且自动随当前协程。
    //   - slots:调用区可写视图,指向值栈上 [callee, a1..aN] 的连续 argc+1 个槽:
    //       slots[0]  = 槽 0(callee / **返回槽**--原生函数把返回值直接写于此,省去 drop+push);
    //       slots[1..argc] = 实参 a1..aN(a1 = slots[1], aN = slots[argc]);
    //       argc = slots.size() - 1。
    //     原生函数天然变参(不存 arity 字段),元数自查 args.size();固定元数内建在体内自查即可。
    //
    //   返回值:bool。true = 成功(把返回值写到 slots[0],原地覆盖 callee);false = 失败(已调
    //   vm.fail/vm.raise 置寄存器)。惯用法--成功路径 `slots[0] = ...; return true;`,失败路径
    //   一行 `return vm.fail(code, fmt, ...);`(vm.fail 置寄存器的同时返回 false,故 return 即报错
    //   即返失败)。VM 调用后 drop(argc) 弹掉 a1..aN,slots[0] 升至栈顶即为返回值 -- 比
    //   「drop(argc+1)+push(result)」省一压,且原生函数直接掌控返回槽。
    //
    //   错误(侧信道):冷路径错误不该编进每次调用的返回类型。原生函数调 vm.fail(code, fmt, ...)
    //   或 vm.raise(code, detail) 写入 VMContext 的挂起错误寄存器(Movement::pending_error_,载荷
    //   类型 Value),然后 return false;AriaVM::raise/fail 把 code+detail 一步烘齐**完整烘焙消息**
    //   (Error::make_message:位置前缀取自调用方帧 last_ip 反推 offset 查行号表 -- 原生不进帧,顶帧即 caller,
    //   位置恰为 CALL 站点;合成模块退化 "<name>:line";帧栈空即 run 外直调则无位置)装箱
    //   ObjException{code, 消息} 后存入,用户 throw(M3)的原值路由另走 Movement::raise(Value)。
    //   VM 在 CALL 后以**返回的 bool 为成败信号**--true 走成功路径(drop argc,slots[0] 升至栈顶),
    //   false 经 take_error() 取出寄存器中的载荷、ObjException 经其 to_error(Error::from_baked
    //   原样回传)还原为 Error 沿 runtime_err 路径传播(M1 无 try/catch 即作未捕获错误从 run()
    //   返回;M3 raise/unwind 落地后供 catch)。故 Error 仅在出错时构造,不进每次调用的返回值。
    //   分配安全:vm.fail 内 new_exception 可能触发 GC,值栈/帧/builtins_ 已接根,载荷入寄存器后
    //   经 VM 根 tracer 标 pending_error 保命。
    //
    //   bool 与寄存器的同步:bool 是成败信号,寄存器是错误载荷容器,二者须一致。VM 据 bool 分支:成功
    //   路径仅 debug 断言 !has_error() 验证契约(寄存器本就空 -- 进场已守、原生未 raise,无需 clear_error;
    //   若违约 debug 暴露,release 不静默清掉掩盖),失败路径 take_error() 取载荷(空则 * 解引用空 Opt
    //   属 UB,debug 断言先暴露)。debug 断言 ok == !has_error() 捕捉两类违约--「调了 vm.fail 却
    //   return true」(忘 return false:release 下不再 clear_error 掩盖,残留错误随寄存器泄漏至下次调用,
    //   违约属实现 bug,任其表面化胜于吞掉)、「return false 却没调 raise」(声明失败无载荷:release 下
    //   take_error() 取空、* 解引用空 Opt 属 UB)。**契约:return false ⟺ 已调 vm.fail/vm.raise;
    //   用 `return vm.fail(...)` 即自动满足。**
    //
    //   契约:
    //   1. 原生函数是**叶子调用**--不得操作 VM 值栈(push/pop/drop),否则 slots 视图失效。只读
    //      slots[1..]、写 slots[0]、经 vm.fail/raise 报错。需回调 aria 函数属未来机制(vm 提供,自管栈)。
    //   2. 读即时值(数字/布尔/nil/既有对象指针)无 GC,任意阶段安全;写新生对象到 slots[0] 需
    //      GC 解锁(M6+)且分配的中间对象须 Guard 入临时根(slots[0] 写入后即随值栈为根)。
    //   3. 错误经 vm.raise 侧信道(寄存器),不抛 C++ 异常(通道 3 仅限 VM 外),不用 longjmp。
    using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);

    // 原生函数对象:把一个 C++ NativeFn 包成 aria Value。
    //
    //   - name_:函数名(intern 驻留,同指针),供报错渲染与 to_string;**恒非空**(构造期 ASSERT)--
    //     无具名需求者用 kAnonymousName("<anonymous>")作匿名名,与 ObjFunction 的 lambda 命名一致,
    //     不可传 nullptr。new_native_fn(GC&, NativeFn) 重载即以 kAnonymousName 建名并自守。
    //   - fn_:C++ 函数指针(恒非空,构造断言)。不存 arity(原生天然变参,fn 自查 slots.size())。
    //
    //   地址哈希型可变对象(走 Object{ObjType::NATIVE_FN} ctor);equals 保持默认地址相等--
    //     原生函数无"内容相等"语义(同名的 C++ 实现可不同)。final,不再派生。
    //   trace():标 name_(fn_ 是 C++ 指针,非 GC 对象,不标)。name_ 恒非空,mark_object 无需容 nullptr。
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

        // 标 name_(fn_ 非 GC 对象)。mark_object 容 nullptr 仅防御。
        void trace(GC& gc) const noexcept override;

        // 壳定长,无外挂子内存。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjNativeFn);
        }

        // 可读描述:`<fn name>`(与 ObjFunction 一致--用户侧不区分 native / user 函数;
        //   native 身份经 ObjType::NATIVE_FN / type() 反射可见,不靠 to_string 区分)。
        //   name_ 恒非空(ctor ASSERT),匿名原生函数渲染 `<fn <anonymous>`(name_ = kAnonymousName)。
        [[nodiscard]]
        String to_string() const override;

    private:
        ObjString* name_;
        NativeFn   fn_;
    };

    // 匿名原生函数名(`<anonymous>`):与 ObjFunction 的 lambda 命名一致(`<>` 是正常标识符中不可用
    //   的符号,具独特辨识度)。无具名需求的原生函数用本常量作 name_,经 new_native_fn(GC&, NativeFn)
    //   重载自动 intern 驻留;亦可由调用方 intern 后传 new_native_fn(GC&, ObjString*, NativeFn) 显式构造。
    inline constexpr StringView kAnonymousName = "<anonymous>";

    // 工厂:分配 ObjNativeFn。工厂不替调用方守卫入参--name 经 intern 是 weak root,new_object 顶
    //   maybe_collect 可能回收,但工厂只做一次 new_object、无内部新建对象,故**调用方须在调用前自行
    //   根化 name**(跨 new_object),与 new_function 同理。fn 是标量,无需入根。name 须非空(构造期 ASSERT)。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, NativeFn fn);

    // 工厂重载(匿名):name 取 kAnonymousName("<anonymous>"),工厂内部 new_string 驻留并自行守卫
    //   (工厂守「自己创建的」),调用方无需手动建串根化。委托 new_native_fn(GC&, ObjString*, NativeFn)。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, NativeFn fn);

} // namespace aria

#endif // ARIA_OBJ_NATIVE_FN_HPP
