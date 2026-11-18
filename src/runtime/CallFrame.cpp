#include "runtime/CallFrame.hpp"

#include "bytecode/CodeUnit.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjFunction.hpp"

namespace aria {

    void CallFrame::init(CallFrame& frame, ObjFunction* function, Value* stack_top, const u8 argc) {
        ASSERT(!function->is_native(), "bytecode frame requires an aria function");
        const auto proto = function->proto();
        frame.slots      = stack_top - argc - 1;
        frame.function   = function;
        frame.unit       = &proto->unit();
        frame.module     = proto->module();
        frame.ip         = proto->unit().code.data();
        // 先锚定 code 起点；nullptr 无法与 code.data() 相减求偏移。
        frame.last_ip = frame.ip;
    }

} // namespace aria
