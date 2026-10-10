#ifndef ARIA_CALL_FRAME_HPP
#define ARIA_CALL_FRAME_HPP

#include "common.hpp"
#include "value/Value.hpp"

namespace aria {

    class CodeUnit;
    class ObjClosure;
    class ObjModule;

    // 调用帧(trivially-copyable 聚合,满足 FrameStack 约束);ip/last_ip 仅作读取游标/锚点,
    // 运行期从不经其写字节,last_ip 反推 offset 的前提 = 帧存活期间 code 缓冲恒定(非移动 GC +
    // 执行期零 emit)。
    struct CallFrame {
        ObjClosure* closure; // 持闭包(callable 收敛为闭包,顶层入口也闭包);元数据经其 function() 取
        CodeUnit*   unit;    // 缓存 closure->function()->unit(),省每条指令一跳
        ObjModule*  module;  // 缓存 closure->function()->module(),供 *_GLOBAL 定位模块 globals
        u8*         ip;      // 指令指针(冷路径按需反推 offset)
        Value*      slots;   // 本帧局部基址(callee 槽 0,参数槽 1..)
        u8*         last_ip; // 最近取指指令起始(循环顶写;行号/unwind 查表锚点)

        // 就位一帧全部字段:slots = stack_top - argc - 1(栈顶形 [callee, a1..aN],槽 0 兼返回槽),
        // 其余字段自 closure 解引用填充;last_ip 占位 code 起始(取指前即覆写,不用 nullptr -- 与
        // data() 相减是 UB)。定义在 CallFrame.cpp(解引用 closure 需完整类型)。
        static void init(CallFrame& frame, ObjClosure* closure, Value* stack_top, u8 argc);
    };

} // namespace aria

#endif // ARIA_CALL_FRAME_HPP
