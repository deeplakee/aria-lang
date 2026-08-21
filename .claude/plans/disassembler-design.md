# 反汇编器 (Disassembler) 实现计划

## 目标
为 `Disassembler` 类实现静态反汇编方法。顶层 `static String disassembleCodeUnit(const CodeUnit* codeunit, StringView name)`:接受一个 `CodeUnit`,解码其字节流,返回反汇编文本。格式基于 `docs/bytecode-instruction-set.md §8`(标注为「建议」),采用「解析信息注释」详细度(用户已选)。

## 文件改动
1. **重命名** `src/bytecode/Disassembler.h` -> `src/bytecode/Disassembler.hpp`,守卫 `ARIA_DISASSEMBLER_H` -> `ARIA_DISASSEMBLER_HPP`(对齐项目 `.hpp` 强制约定;用户已选)。
2. **`src/bytecode/Disassembler.cpp`**:实现全部逻辑,include 改 `"Disassembler.hpp"`。
3. **`CMakeLists.txt:59`**:`src/bytecode/Disassembler.h` -> `src/bytecode/Disassembler.hpp`(`.cpp` 已登记于 :58,无需新增)。

不动 `OpCode` 枚举、`CodeUnit`、其它模块。

## 类设计(仅静态方法,无状态)
```cpp
// Disassembler.hpp
#include "common.hpp"   // String/StringView 等别名

namespace aria {
    class CodeUnit;     // 前向声明,头不 include CodeUnit.hpp(保持轻)

    class Disassembler {
    public:
        [[nodiscard]] static String disassembleCodeUnit(const CodeUnit* codeunit, StringView name);

    private:
        Disassembler() = delete;   // 仅静态方法,禁实例化
        static String        disassembleInstruction(const CodeUnit* cu, usize& offset);
        static StringView    opcode_name(OpCode op);      // opcode -> 名字
        static Opd           opd_format(OpCode op);       // opcode -> 操作数格式(共享解码表)
        static String        format_constant(Value v);    // 值可读化
        static bool          read_u8 (const CodeUnit* cu, usize& offset, u8&  out); // 越界返 false
        static bool          read_u16(const CodeUnit* cu, usize& offset, u16& out); // 小端,越界返 false
    };
}
```
- `Opd`(操作数格式枚举)与字符串转义辅助 `escape_string` 放 `.cpp` 匿名命名空间。
- `.cpp` include:`Disassembler.hpp`、`bytecode/CodeUnit.hpp`、`value/Value.hpp`、`object/ObjString.hpp`、`object/Object.hpp`、`<format>`。
- 构造用大括号、变量声明按 CLAUDE.md「变量声明风格」;输出字符串用 `std::format` 拼接(不裸调 `std::print`,本工具只返回 String 不打印)。

## 输出格式(用户选定:解析信息注释)
```
== <name> ==

constants:
  0000: "hello"
  0001: 42
  0002: nil

0000  LOAD_CONST      0000    ; "hello"
0003  LOAD_LOCAL      00
0005  ADD
0006  PRINT
0007  LOAD_IMM        FF      ; -1
0009  JUMP_FALSE      0009 -> 0020
0012  CLOSURE         0001    ; <obj:Function>
0014  MAKE_LIST       0003
0016  IMPORT          0002 0004    ; "lib/utils" as "Utils"
```
- 列:`偏移(4 hex 大写)` `  ` `opcode 名(左对齐 pad 17)` `  ` `操作数(hex)` `  ` `; 注释`。
- **注释仅在「能解析出操作数不可见信息」时给出**:LOAD_CONST 的常量值、name 类指令的名字串、LOAD_IMM 的有符号十进制、跳转目标、CLOSURE 的函数 repr、IMPORT 的 path/alias 名。`ADD`/`POP`/`LOAD_LOCAL` 等不加注释(信息已在操作数)。
- **跳转**:同时给原始偏移与目标(`0009 -> 0020` 前向 / `<- 0020` 后向)。目标 = 读操作数后的 ip ± off(§4.12):前向 `target = start + 3 + off`,后向 `target = start + 3 - off`(`start` 为本指令起始 offset,3 = 1 opcode + 2 字节 u16)。
- **不含**静态栈效应串(§8 标「建议」;栈效应在文档表,避免双处漂移)。**不含**逐指令行号(§8 未列;`CodeUnit::line_for_offset` 可后加 `; line N`)。
- **常量池小节**:非空时在表头后列 `idx: repr`,便于解读常量索引操作数。
- **鲁棒性**:操作数读取越界时该行注 `<truncated>`,`offset` 推到末尾,不崩溃(调试工具应容忍残缺字节码)。

## 操作数格式映射(opd_format,覆盖全部 OpCode,无 default 靠 -Wswitch 查漏)
| Opd | 操作数 | OpCode |
| :--- | :--- | :--- |
| None | 无 | HALT NOP LOAD_NIL LOAD_TRUE LOAD_FALSE LOAD_INDEX STORE_INDEX EQUAL NOT_EQUAL STRICT_EQUAL STRICT_NOT_EQUAL GREATER GREATER_EQUAL LESS LESS_EQUAL ADD SUBTRACT MULTIPLY DIVIDE MOD NOT NEGATE POP DUP DUP2 PRINT LOAD_OBJECT CLOSE_UPVALUE THROW RETURN |
| ImmI8 | i8 | LOAD_IMM |
| U8 | u8 | LOAD_LOCAL STORE_LOCAL LOAD_UPVALUE STORE_UPVALUE CALL POP_N |
| RangeFlags | u8 | MAKE_RANGE |
| ConstIdx | u16(值显示) | LOAD_CONST |
| NameIdx | u16(名显示) | DEF_GLOBAL LOAD_GLOBAL STORE_GLOBAL LOAD_FIELD STORE_FIELD LOAD_THIS_FIELD STORE_THIS_FIELD MAKE_CLASS MAKE_METHOD MAKE_STATIC LOAD_SUPER_METHOD |
| ClosureFn | u16(函数 repr) | CLOSURE |
| SlotU16 | u16 | LOAD_LOCAL_L STORE_LOCAL_L |
| CountU16 | u16 | MAKE_LIST MAKE_MAP |
| JumpFwd | u16 -> target | JUMP JUMP_TRUE JUMP_TRUE_NOPOP JUMP_FALSE JUMP_FALSE_NOPOP |
| JumpBack | u16 <- target | JUMP_BACK |
| Import | u16 path + u16 alias | IMPORT |
| Invoke | u16 name + u8 argc | INVOKE_METHOD(预备指令,仍按此解码显示) |

`disassembleInstruction` 按 `Opd` 分支读操作数 + 渲染;`opcode_name`/`opd_format` 各一个 switch 列举全部枚举值(无 default),末尾 `UNREACHABLE()`(同 `Op::to_string` 风格)。

## format_constant(Value)
- Nil -> `nil`;Bool -> `true`/`false`;Int -> 十进制(`42`、`-1`)。
- F64 -> 保证含 `.`/`e`/`E`:整值补 `.0`(`1.0`),与 Int 区分;NaN/inf 直出。
- ObjString -> `"..."`:`escape_string` 转义 `"` `\` `\n` `\t` `\r`,控制字符(`<0x20`)-> `\x{HH}`,非 ASCII UTF-8 透传。
- 其它 Obj -> `<obj:Type>`(经 `ObjType::to_string(obj->type())`);`ObjFunction` 未落地时亦安全(走此分支)。
- NameIdx 操作数期望常量是 ObjString:若是则显示 `"name"`,否则退化为 `format_constant`(容错)。

## 验证
- 语法检查:`clang++ -std=c++23 -I src -fsyntax-only src/bytecode/Disassembler.cpp`。
- 不引入实例方法/状态(符合「仅静态方法」)。

## 不做 / 后续
- 不实现 VM/编译器;不改 `OpCode`/`CodeUnit`。
- 可选后续:`tests/test_disassembler.cpp`(构造 `GC`+`CodeUnit` emit 几条指令+常量,断言输出含预期行;需 `GC` 实例,稍重,本次先不做,待用户需要再加)。
- 可选后续:逐指令行号注释;`opcode_name`/`opd_format` 提为 public 供 VM 共享解码表(VM 落地后)。
