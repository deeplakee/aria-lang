#ifndef ARIA_EVAL_HPP
#define ARIA_EVAL_HPP

#include "aria.hpp"
#include "common.hpp"
#include "value/Value.hpp"

namespace aria {

    class AriaVM;
    class ObjBoundMethod;
    class ObjClass;
    class ObjClosure;
    class ObjNativeFn;
    class ObjString;
    class Object;

    // 前置声明(run_binary_* 模板形参用):免头文件拖入 bytecode 树,定义处 .cpp 已含。
    enum class OpCode : u8;

    // eval 层驱动引擎:主循环、指令执行体、调用机器与异常派发。全静态方法宿主,宿主内部
    //(current_ 与各服务表)经 AriaVM 友元到达,循环状态一律自 vm.current_ 取(单循环切换模型)。
    class Eval {
    public:
        // 主循环:驱动 *current_ 至顶层返回/未捕获(出口协议返 bool:成功 = 返回值已写回调用区
        // 槽 0 即栈顶;失败 = 载荷留寄存器,失败出口均就地善后到边界)。
        static bool dispatch_loop(AriaVM& vm);

        // 闭包调用进帧单点(宿主执行入口 run 与重入执行体共用):元数检查 -> 帧余量检查 -> 实参
        // 整形后进帧。失败 WrongArity/StackOverflow。
        static bool call_closure(AriaExeContext& ctx, ObjClosure* obj, u8 argc);

        // 重入执行体(ObjMovement 重入口 execute_closure/execute_method 的共用体):压 slot0 与
        // args(栈形 [slot0, a1..aN],argc = 声明参数数,不含槽 0)后同步驱动至结算。成功 = 栈顶
        // 收取闭包返回值;失败 nullopt 且载荷留寄存器、残留帧已截净,调用方按 bool 契约 return
        // false 交外层续搜;递归深度达 kMaxRecursionDepth 报 StackOverflow,实参超 u8 域报
        // WrongArity。ctx 恒为正在执行的上下文(契约,断言钉)。args 不得指进本上下文值栈;若传
        // 栈上段,压档余量须足(扩容会搬迁旧块使视角悬垂,断言复核之)。
        [[nodiscard]]
        static Opt<Value> execute_call(AriaExeContext& ctx, Value slot0, ObjClosure* closure, Span<const Value> args);

    private:
        // CALL 分发:栈顶形如 [callee, a1..aN],按 callee 类型分派 call_* 子例程;其余对象按调用
        // 钩子 __call__ 取实现后递归分发,非对象直接报 CallNonCallable。**call_* 族统一契约:
        // false/nullptr ⟺ 错误载荷已 raise 进 *current_ 挂起错误寄存器**,调用方 take_error 传播。
        static bool call_value(AriaExeContext& ctx, Value callee, u8 argc);

        // 类实例化:new_instance 为唯一 GC 点,建成即槽 0 原位换实例(即新帧 this),余下交
        // call_value 通用分发。
        static bool call_class(AriaExeContext& ctx, ObjClass* obj, u8 argc);

        // 绑定方法调用:槽 0 原位覆写为 receiver(this 替代 callee,实参槽位不动),余下交
        // call_value 分发。方法值无需守卫:覆写后经类表槽/缓存可达。
        static bool call_bound_method(AriaExeContext& ctx, const ObjBoundMethod* obj, u8 argc);

        // 原生函数调用:同步调 obj->fn(),不进帧;bool 契约透传。
        static bool call_native(AriaExeContext& ctx, const ObjNativeFn* obj, u8 argc);

        // 按 Op 取本对象的算子实现(编译期分发到 Object::op_*_impl,Op 由调用点穷举);nullopt ⟺
        // 已 fail(措辞随宿主)。模板定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        static Opt<Value> get_obj_binary_op_impl(AriaExeContext& ctx, Object* obj);

        // 弹 2 算 1 的二元数值运算入口(9 个算术/比较指令共用,Op 由调用点穷举实例化):类型守卫后
        // 按域分流,双 Int 走 run_binary_int、任一 F64 升浮点走 run_binary_f64(两域失败语义不同);
        // 定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        static bool run_binary_numeric(AriaExeContext& ctx);

        // 整数域九算子(结果压栈):除/模零是域特有失败,就地 fail;% 为 C++ 语义。
        template<OpCode Op>
        [[nodiscard]]
        static bool run_binary_int(AriaExeContext& ctx, i64 lhs, i64 rhs);

        // 浮点域九算子(结果压栈):按 IEEE,除零得 inf/nan、% 走 fmod,无失败路径。
        template<OpCode Op>
        [[nodiscard]]
        static bool run_binary_f64(AriaExeContext& ctx, f64 lhs, f64 rhs);

        // 九个二元算子的执行体(算术/比较 block 唯一入口):两侧均 String 走 run_string_binary 就地
        // 完成;其余非对象左值委托 run_binary_numeric,对象左值取算子实现后交 call_value(调用区
        // [lhs, rhs] 即 [this, arg1])。peek 不弹 -- receiver 占调用区槽 0(栈即根)跨实现内分配。
        template<OpCode Op>
        static bool run_binary_operator(AriaExeContext& ctx);

        // string 快路径派发门(lhs 非空,调用方已判 is_obj;白名单收在本方法:ADD 与四个字序比较,
        // 其余算子恒 false):两侧均 String 交 run_string_binary 就地完成;返回 true = 已派发,
        // false = 不适用落原路(非失败)。定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        static bool try_string_fast_dispatch(AriaExeContext& ctx, Object* lhs);

        // string 算子快路径(白名单收在 try_string_fast_dispatch):[lhs, rhs] -> [r],就地拼接/按
        // 无符号字节序比较,语义与 StringClass 对应钩子逐位一致;定义在 .cpp(实例化点全在本 TU)。
        template<OpCode Op>
        static bool run_string_binary(AriaExeContext& ctx, ObjString* lhs, ObjString* rhs);

        // NEGATE 执行体:[v] -> [r]:整数/浮点就地取负,对象左值取 __neg__ 实现后调用(一元恒零
        // 实参,调用区 [v] 即 [this]);其余类型报 InvalidOperand。契约同上。
        static bool run_negate(AriaExeContext& ctx);

        // field 族指令执行体;bool 契约同 call_value(对象协议失败由 override 内 ctx.fail 就地烘焙,
        // 非对象守卫由执行体 fail),peek 不弹 -- 协议内分配跨 GC 须 obj 在栈(栈即根)。

        // LOAD_FIELD 执行体:[obj] -> [v],经 load_field_bound 协议解析写回原槽;非对象守卫文案
        // 留执行体,对象 miss 文案由协议 override 烘焙。
        static bool run_load_field(AriaExeContext& ctx, ObjString* name);

        // STORE_FIELD 执行体:[obj, v] -> [v](完成时单槽下移留 v -- 赋值表达式约定)。经
        // Object::store_field 协议。
        static bool run_store_field(AriaExeContext& ctx, ObjString* name);

        // THIS 对执行体:this 取顶帧槽 0,this 不经值栈与 obj.m 同走 field 协议。槽 0 非实例 = this
        // 被自由调用/类直调掏空(经 call_closure 的写法不可达),响亮 TypeMismatch 兜底(帧形
        // 不变式只对 call_closure 成立)。
        static bool run_load_this_field(AriaExeContext& ctx, ObjString* name);

        // 写入执行体:[v] -> [v](peek-store 经 this 的 store_field,实例字段动态即创建)。非实例
        // 兜底报错同读取执行体,文案用 field assignment 形态。
        static bool run_store_this_field(AriaExeContext& ctx, ObjString* name);

        // PREPARE_METHOD 执行体(两段式第一段):[recv] -> [recv, target]。接收者在栈顶、解析
        // 先于实参求值;待调值压栈跨指令存活(栈即根),由 CALL_METHOD 收口。
        static bool run_prepare_method(AriaExeContext& ctx, ObjString* name);

        // CALL_METHOD 执行体(两段式第二段):[recv, target, a1..aN] -> [r],实参整体下移一格补掉
        // 待调值占的那格得调用区 [recv, a1..aN](槽 0 = receiver),交 call_value 统一分发;
        // 纯调用,不再解析。
        static bool run_call_method(AriaExeContext& ctx, u8 argc);

        // LOAD_SUPER_FIELD 执行体:defining class 取顶帧 closure 直读(编译器不变式,ASSERT 把关),
        // 自父类沿链读穿透、不含 defining 自身;命中方法闭包则绑 this=帧槽 0 压栈,其余原值直读。
        // 全链 miss 为语言可达错误,经协议 fail。
        static bool run_load_super_field(AriaExeContext& ctx, ObjString* name);

        // 下标族指令执行体(LOAD/STORE_INDEX):契约同 field 族

        // list 下标快路径派发门(target 非空,调用方已判 is_obj):接收者 tag 为 LIST 且键为整数时
        // 免 load_index 虚派发直取,键解析与虚路径同走 resolve_index;返回 true = 已完成,false =
        // 不适用落原路(非失败),报错文案单一事实源留 ObjList::load_index。无分配无 GC 点。
        static bool try_list_load_fast_dispatch(AriaExeContext& ctx, Object* target, Value idx);

        // list 下标写快路径派发门:契约同 try_list_load_fast_dispatch,界内写槽后值下移两格
        // ([obj, idx, v] -> [v]);报错文案单一事实源留 ObjList::store_index。
        static bool try_list_store_fast_dispatch(AriaExeContext& ctx, Object* target, Value idx, Value value);

        // LOAD_INDEX 执行体:[obj, idx] -> [v],经 load_index 协议写回 obj 槽再弹 idx;非对象守卫
        // 文案留执行体,对象侧越界/键类型文案由 override 就地烘焙。
        static bool run_load_index(AriaExeContext& ctx);

        // STORE_INDEX 执行体:[obj, idx, v] -> [v](值下移两格留 v,peek-store --赋值表达式约定),
        // 经 store_index 协议。
        static bool run_store_index(AriaExeContext& ctx);

        // range 构造指令执行体(MAKE_RANGE):契约同 field 族

        // MAKE_RANGE 执行体:有界 [from, to] -> [range]、无上界 [from] -> [range]。端点须为整数,
        // 非整数 fail TypeMismatch(静态文案不插端点值);端点 peek 在栈跨 new_range 的
        // maybe_collect(栈即根),铸完 drop 再 push。
        static bool run_make_range(AriaExeContext& ctx, u8 flags);

        // MAKE_CLASS 执行体:[super] -> [class] 原地写回。super 须类值、且非内建容器五类
        //(iterator/list/map/string/range,Exception 与 Object 可作 super),否则 fail TypeMismatch;
        // super peek 在栈跨 new_class 的 maybe_collect(栈即根),建成原地写回槽发布。
        static bool run_make_class(AriaExeContext& ctx, ObjString* name);

        // IMPORT 执行体:resolve_module -> intern 后 guard 根化查 modules_(命中复用即循环导入),
        // 未命中 vm.load_module 后包闭包进帧 run-once;两分支栈效应统一 [..., module]。bool 契约同
        // call_* 族(false ⟺ 载荷已 raise:解析/加载/进帧)。私有性即「仅限驱动期调用」的强制。
        static bool run_import(AriaExeContext& ctx, const ObjString* path);

        // RETURN 执行体:[..., ret] -> 取返回值后弹帧(关本帧区间开指,值迁入各自 upvalue 自持),
        // 按弹后帧深分流(出口边界 = current_->ret_frame())。返回 true = 本循环结束(递归出口值
        // 落调用区槽 0 / 主顶层返回值留栈顶);false = 循环继续(帧间返回写回 callee 槽 / 协程
        // 完成切换)。
        static bool run_return(AriaExeContext& ctx);

        // 异常派发:自最内帧向下按 last_ip 遍查本上下文各帧 CodeUnit 异常记录表,命中回退派发
        //(不动帧栈/值栈)。搜索边界 = current_->ret_frame()(随上下文走)。返回 true = 已派发;
        // false = 边界之上全帧未命中,载荷留寄存器(嵌套边界交外层续搜 / 链上已转投恢复者 /
        // 链根未捕获善后归调用方)。命中可能在多跳之后,current_ 已非进入时的上下文。
        static bool unwind(AriaVM& vm);
    };

} // namespace aria

#endif // ARIA_EVAL_HPP
