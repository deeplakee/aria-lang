#include "runtime/CallFrame.hpp"

#include "bytecode/CodeUnit.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"

namespace aria {

    void CallFrame::init(CallFrame& frame, ObjClosure* closure, Value* stack_top, const u8 argc) {
        const auto fn = closure->function();
        frame.slots   = stack_top - argc - 1;
        frame.closure = closure;
        frame.unit    = &fn->unit();
        frame.module  = fn->module();
        frame.ip      = fn->unit().code.data();
        // 位置锚点占位(= code 起始,等价 offset 0),主循环取指前即覆写。不用 nullptr:
        // 与 data() 相减是 UB。(无 NSDMI,保 trivial 聚合。)
        frame.last_ip = frame.ip;
    }

} // namespace aria
