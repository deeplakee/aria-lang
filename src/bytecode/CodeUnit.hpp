#ifndef ARIA_CODEUNIT_HPP
#define ARIA_CODEUNIT_HPP

#include "bytecode/code.hpp"
#include "common.hpp"
#include "memory/Array.hpp"
#include "value/AriaArray.hpp"

namespace aria {

    class GC;

    // 操作数位宽上限(字节码编码格式事实源):u8 操作数最大 255、u16 操作数最大 65535。
    // 发射侧(CodeGen 的 arity/实参/常量池索引/局部槽号检查)与本类编码侧(POP_N 分块/短槽号/
    // 跳转偏移/池容量)共享,勿在别处重写字面量;各语义上限常量以之为源(见 CodeUnit.cpp/CodeGen.cpp)。
    constexpr u32 kU8OperandMax  = 0xFF;   // u8 操作数最大值
    constexpr u32 kU16OperandMax = 0xFFFF; // u16 操作数最大值(u16 索引可寻址 0..65535,容量 = +1)

    // 行号表条目(RLE):从 `offset` 起、直到下一条 entry 的 offset 之前,字节码均属 `line` 行。
    //        简单聚合(类内默认成员初始化),trivially-copyable 满足 Array<T> 约束。
    struct LineEntry {
        u32 offset = 0;
        u32 line   = 0;
    };

    // 异常记录表条目:一个 try 块的受保护区间 [begin, end)、catch handler 入口与 unwind 栈深。
    //        begin/end/handle 均为 code 字节流中的 offset;stack_depth 为编译期 try 入口局部数快照
    //        (相对 frame.slots,非全局栈基址),unwind 据此截断值栈,并把异常值 push 落到 catch 参数槽
    //        (恒 == stack_depth,值填槽无 STORE_LOCAL)。不存 frame_depth(运行时量,由 unwind 遍历
    //        帧链隐式决定)、不存 catch_slot(恒等 stack_depth)。
    //        简单聚合(类内默认成员初始化, 同 LineEntry), trivially-copyable 满足 Array<T> 约束。
    struct TryRecord {
        u32 begin       = 0; // try 受保护区间起始 offset (含)
        u32 end         = 0; // try 受保护区间结束 offset (不含); [begin, end) 内的 ip 命中此记录
        u32 handle      = 0; // catch handler 入口 offset; unwind 后跳此
        u32 stack_depth = 0; // try 入口局部数(编译期快照); unwind 截值栈至 frame.slots + 此值
    };

    // 字节码容器:一个编译单元(函数/模块顶层)的字节流 + 常量池 + 行号表 + 异常记录表。
    //
    //   底层数据结构,四个 Array 字段(code/constants/lines/try_records)直接 public 裸露,VM/编译器/
    //   反汇编器直接操作(`cu.code.push(...)`/`cu.constants[i]`/`cu.lines` 等)。只保留有
    //   "不可散落逻辑"的方法:
    //     - emit_*:写字节同时按 RLE 记行号表(去重逻辑收口于此,不散到调用方);
    //     - emit_pop_n/emit_jump/patch_jump/emit_jump_back/emit_load_local/emit_store_local:
    //       跳转编码 / 回填 / 分块 POP / 槽位短长变体等字节码编码逻辑收口于此(编译器不再自持薄包装);
    //     - line_for_offset:RLE 二分查行(二分逻辑不重复实现);
    //     - trace:GC 委托入口(ObjFunction::trace 调)。
    //   跳转回填的越界(超 64KB)以 bool 返回交回调用方翻译为 Error -- 本类不持有 Error 语义。
    //
    //   **行号模型**:emit 一律带 `line` 参数(无状态、无重载)-- 调用方(编译器)自己跟踪
    //   当前行号,每次 emit 传入。RLE 去重在 record_line_ 内做(与末条同行则不追加)。
    //
    //   注:本类**不是 Object**,是 `ObjFunction` 的值成员。四个 Array 持 GC* 自释放,
    //   ~CodeUnit -> ~Array 级联释放(同 ObjString long_chars_)。非拷贝/非移动。
    class CodeUnit {
    public:
        Array<u8>        code;        // 字节流: opcode + 内联操作数 (小端)
        AriaArray        constants;   // 常量池 (Array<Value> + trace)
        Array<LineEntry> lines;       // RLE 行段表 (offset -> line)
        Array<TryRecord> try_records; // 异常记录表 (按 begin 单调; find_try_handler 二分查)

        explicit CodeUnit(GC* gc) noexcept;

        ~CodeUnit() = default;

        CodeUnit(const CodeUnit&)            = delete;
        CodeUnit& operator=(const CodeUnit&) = delete;
        CodeUnit(CodeUnit&&)                 = delete;
        CodeUnit& operator=(CodeUnit&&)      = delete;

        // ---- emit (带 line; RLE 行号记录收口于此) ----
        void emit_byte(u8 byte, u32 line);
        void emit_word(u16 word, u32 line); // 小端: 低字节先
        void emit_op(OpCode op, u32 line);

        // 字节码编码逻辑(收口于此,编译器不再自持薄包装)。偏移基准 = 读 u16 操作数后的 ip。
        // 分块 emit POP_N(每块<=255);chunk==1 时降级为 POP(1B,免操作数),与 load/store 局部槽短/长分流同思路。
        void emit_pop_n(u32 count, u32 line);
        // 发 op + 占位 u16,返回占位偏移 src_off(供 patch_jump 回填)。无越界。
        usize emit_jump(OpCode op, u32 line);
        // 前向回填 src_off 处占位为 target_off - base_off(base_off = 读完 u16 操作数后的 ip,即偏移基准);
        //   越界(>65535)返 false(不写),成功返 true。
        bool patch_jump(usize src_off);
        // 后向:emit JUMP_BACK + (base_off - target_off)(base_off = 读完 u16 操作数后的 ip);
        //   越界(反向/超 64KB)emit 占位 word 0 后返 false。
        bool emit_jump_back(u32 target_off, u32 line);
        // 局部槽 load/store:slot<=255 用短变体(LOAD/STORE_LOCAL + u8),否则长变体(_L + u16)。
        void emit_load_local(u16 slot, u32 line);
        void emit_store_local(u16 slot, u32 line);

        // 当前字节流长度(= 下一条 emit 的 offset)。
        [[nodiscard]]
        u32 size() const noexcept {
            return static_cast<u32>(code.size());
        }

        // ---- 常量池 ----
        // 暂不去重(clox 去重是优化项):先 append;ObjString 经 intern 同指针,
        // 未来加去重 hash set 不影响 API。索引 u16,超 65535 断言。
        u16 add_constant(Value value);

        // ---- 行号查询 ----
        // 查 `offset` 所属行号(RLE 二分:最大 entry.offset <= offset 的 line)。
        // 空表或 offset 在首条之前返回 0(未知行)。
        [[nodiscard]]
        u32 line_for_offset(usize offset) const noexcept;

        // ---- 异常记录表 ----
        // 按 ip 查最近覆盖的 try 记录(嵌套取最内层), 返回指向命中记录的指针(unwind 读 handle 与
        // stack_depth 两字段); 无覆盖返 nullopt。记录按 begin 单调; 二分 + 前溯, O(嵌套深度) 最坏。
        [[nodiscard]]
        Opt<const TryRecord*> find_try_handler(u32 ip) const noexcept;

        // ---- GC trace ----
        // 委托 constants.trace(gc)(code/lines/try_records 无 Value,不标)。由 ObjFunction::trace 调用。
        void trace(GC& gc) const noexcept { constants.trace(gc); }

        // ---- 反汇编 ----
        // 委托 Disassembler::disassembleCodeUnit(this, name) 输出整个 CodeUnit 的可读反汇编文本;
        // name 用作表头标识(函数名/模块名)。
        [[nodiscard]]
        String disassemble(StringView name) const;

    private:
        // 在当前 code.size() 偏移处按 line 记一条 RLE 行段;与末条同行则不追加。
        void record_line_(u32 line) noexcept;
    };

} // namespace aria

#endif // ARIA_CODEUNIT_HPP
