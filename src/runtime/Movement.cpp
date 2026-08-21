#include "runtime/Movement.hpp"

#include "object/ObjFunction.hpp"

namespace aria {

    // 进帧 = acquire 空帧 + 就位该帧。定义在此(.cpp)而非头文件:init_frame_ 解引用 fn
    // 需 ObjFunction 完整类型(unit/module/code),头文件仅前向声明 ObjFunction 即可,
    // 避免 Movement.hpp 拖入 object/bytecode 树。
    void Movement::enter_frame(ObjFunction* fn, const u8 argc) {
        CallFrame& f = frames_.acquire(); // 取空帧(现为栈顶)
        init_frame_(f, fn, argc);         // 就位该帧:slots + VM 专有字段
    }

    // 修改栈顶帧(刚 acquire 的空帧)为对 fn 的调用。slots 按不变量设为 top - argc - 1
    // (栈顶 [callee, a1..aN]:slots 指向槽 0 callee);VM 专有字段从 fn 解引用填充。
    // module 缓存 fn->module(),供 LOAD/STORE/DEF_GLOBAL 定位当前模块 globals;
    // ip 指向 fn 字节码起始(RETURN/异常时按 ip 算 offset)。
    void Movement::init_frame_(CallFrame& f, ObjFunction* fn, const u8 argc) const {
        f.slots    = top_ - argc - 1;
        f.function = fn;
        f.unit     = &fn->unit();
        f.module   = fn->module();
        f.ip       = fn->unit().code.data();
    }

} // namespace aria
