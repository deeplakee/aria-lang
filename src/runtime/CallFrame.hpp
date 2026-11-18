#ifndef ARIA_CALL_FRAME_HPP
#define ARIA_CALL_FRAME_HPP

#include "common.hpp"
#include "value/Value.hpp"

namespace aria {

    class CodeUnit;
    class ObjFunction;
    class ObjModule;

    // 帧存活期间字节码缓冲不可搬迁或修改，ip/last_ip 借用其地址。
    struct CallFrame {
        ObjFunction* function; // 持 aria 函数对象;元数据经其 proto() 取
        CodeUnit*    unit;     // 缓存 function->proto()->unit(),省每条指令一跳
        ObjModule*   module;   // 缓存 function->proto()->module(),供 *_GLOBAL 定位模块 globals
        u8*          ip;       // 指令指针(冷路径按需反推 offset)
        Value*       slots;    // 本帧局部基址(callee 槽 0,参数槽 1..)
        u8*          last_ip;  // 最近取指指令起始(循环顶写;行号/unwind 查表锚点)

        static void init(CallFrame& frame, ObjFunction* function, Value* stack_top, u8 argc);
    };

} // namespace aria

#endif // ARIA_CALL_FRAME_HPP
