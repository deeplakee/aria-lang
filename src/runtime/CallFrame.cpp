#include "runtime/CallFrame.hpp"

#include "bytecode/CodeUnit.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjFunction.hpp"

namespace aria {

    void CallFrame::init(CallFrame& frame, ObjFunction* function, Value* stack_top, const u8 argc) {
        ASSERT(!function->is_native(), "bytecode frame requires an aria function");
        const auto fn  = function->proto();
        frame.slots    = stack_top - argc - 1;
        frame.function = function;
        frame.unit     = &fn->unit();
        frame.module   = fn->module();
        frame.ip       = fn->unit().code.data();
        // 位置锚点占位(= code 起始,等价 offset 0),主循环取指前即覆写。不用 nullptr:
        // 与 data() 相减是 UB。(无 NSDMI,保 trivial 聚合。)
        frame.last_ip = frame.ip;
    }

} // namespace aria
