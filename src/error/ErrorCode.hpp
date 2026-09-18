#ifndef ARIA_ERRORCODE_HPP
#define ARIA_ERRORCODE_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 错误大类：普通 enum class。
    // 转字符串用 to_string(ErrorCategory)；由 ErrorCode 经 category_of() 映射而来。
    enum class ErrorCategory : u8 {
        Ok,
        Syntax,
        Semantic,
        Runtime,
        Internal,
        Resource,
    };

    // 大类可读名表（下标即 std::to_underlying(c)，行序须与枚举声明一致；哨兵断言拦截不同步）。
    inline constexpr StringView kCategoryNames[] = {
            "Ok", "Syntax", "Semantic", "Runtime", "Internal", "Resource",
    };

    // 「名字/大类」两个查表函数共用此表；表体与枚举同源生成（见 ARIA_ERROR_LIST 注），
    // 无不同步风险。非法值（u8 强转越界）由查表函数的 ASSERT 拦截。
    struct CodeInfo {
        StringView    name; // 可读标识名（如 "UnterminatedString"）
        ErrorCategory cat;  // 所属大类
    };

// 解释器内部统一错误码。分层按「错误在哪一阶段被发现」（对标 CPython 编译期/运行期二分、
// Lua 状态码、crafting-interpreters）：
//   - Syntax   ：词法/语法阶段，源码结构本身不合法（lexer/parser 可直接判定）。
//   - Semantic ：结构合法但违反类型/作用域/形态规则（文法明确推迟到语义/字节码阶段的检查归此类）。
//   - Runtime  ：执行期间才暴露的语义错误（类型不符、越界、键缺失等）。
//   - Internal ：解释器自身不变式被破坏，属实现 bug（不可达、栈失衡、坏字节码）。
//   - Resource ：资源/环境受限（内存、文件、模块、容量上限）。
//
// 错误码全量注册表（单一事实源，同 code.hpp 的 ARIA_OPCODE_LIST / TokenType.hpp 的
// ARIA_TOKEN_LIST 风格）：枚举声明与信息表 kCodeTable 都由 ARIA_ERROR_LIST(X) 展开，
// 新增错误码加一行 X(名字, 大类) 即收口，名字串经 # 派生，无第二处手写。隐式连续编号，
// 下标即 std::to_underlying(code)。逐值注释用块注释（行注释会吞续行符）。
#define ARIA_ERROR_LIST(X)                                                                                       \
    X(Ok, Ok)                                                                                                    \
    /* ========== SYNTAX ERROR（词法 / 语法阶段）========== */                                                   \
    /* --- 词法 --- */                                                                                           \
    X(UnterminatedString, Syntax) /* 字符串未闭合 / 跨行 */                                                      \
    X(InvalidEscape, Syntax)      /* 未识别转义（无 \x；\u{} 非法） */                                           \
    X(InvalidNumber, Syntax)      /* 数字字面量非法（前缀 / 下划线位置 / 空 mantissa 等） */                     \
    X(InvalidCharacter, Syntax)   /* 无法归入任何 token 的字符 */                                                \
    /* --- 语法 --- */                                                                                           \
    X(UnexpectedToken, Syntax)    /* 非预期的 token */                                                           \
    X(UnexpectedEof, Syntax)      /* 源码提前结束 */                                                             \
    X(ExpectedExpression, Syntax) /* 期待表达式却遇到他物 */                                                     \
    X(ExpectedIdentifier, Syntax) /* 期待标识符 */                                                               \
    X(ExpectedToken, Syntax)      /* 期待特定符号/关键字（具体对象由报错信息补充） */                            \
    X(InvalidPattern, Syntax)     /* 解构模式结构非法（rest 非末尾、..._ 等） */                                 \
    X(VarargsNotLast, Syntax)     /* ... 必须位于参数列表末尾 */                                                 \
    X(DefaultAfterPlain, Syntax)  /* 默认参数后不得再有无默认参数（文法强制顺序） */                             \
    /* ========== SEMANTIC ERROR（语义分析阶段，多为文法明确推迟到语义阶段的检查）========== */                  \
    X(InvalidAssignmentTarget, Semantic) /* 赋值左值须为 identifier / obj.field / obj[index] */                  \
    X(SuperOutsideMethod, Semantic)      /* super 出现在非方法上下文 */                                          \
    X(ThisOutsideClass, Semantic)        /* this 出现在类外 */                                                   \
    X(BreakOutsideLoop, Semantic)        /* break 须在循环内 */                                                  \
    X(ContinueOutsideLoop, Semantic)     /* continue 须在循环内 */                                               \
    X(ReturnOutsideFunction, Semantic)   /* return 须在函数内 */                                                 \
    X(TryWithoutHandler, Semantic)       /* try 须有 catch */                                                    \
    X(UnreachableArm, Semantic)          /* match 通配臂后仍有臂（死臂；_ 恒末臂、至多一条） */                  \
    X(DuplicateParam, Semantic)          /* 同函数形参重名 */                                                    \
    X(UndefinedType, Semantic)           /* 引用未定义的类名 */                                                  \
    X(RedefinedVariable, Semantic)       /* 同作用域重复定义变量 */                                              \
    X(RedefinedClass, Semantic)          /* 重复定义类 */                                                        \
    X(NumberOutOfRange, Semantic)        /* 整数字面量超出 i48 范围（溢出，文法约定语义阶段处理） */             \
    X(NotImplemented, Semantic)          /* 功能尚未实现（CodeGen 占位；经 Error 通道均编译期，故归语义阶段） */ \
    /* ========== RUNTIME ERROR（运行时阶段）========== */                                                       \
    X(TypeMismatch, Runtime)       /* 运算/操作的类型不符（如 number + 非数） */                                 \
    X(InvalidOperand, Runtime)     /* 一元操作数非法（如对非数取负） */                                          \
    X(IndexOutOfBounds, Runtime)   /* list 下标越界（含解构元素不足） */                                         \
    X(DivisionByZero, Runtime)     /* 除零 */                                                                    \
    X(ModuloByZero, Runtime)       /* 模零 */                                                                    \
    X(KeyError, Runtime)           /* map 键不存在 */                                                            \
    X(UndefinedVariable, Runtime)  /* 引用未定义的变量/函数（运行期查表 miss） */                                \
    X(UndefinedProperty, Runtime)  /* 对象无该字段/方法 */                                                       \
    X(CallNonCallable, Runtime)    /* 调用非函数值 */                                                            \
    X(WrongArity, Runtime)         /* 实参数量不符（含默认参数/varargs 填充后） */                               \
    X(NotIterable, Runtime)        /* forIn 目标不可遍历 */                                                      \
    X(IteratorProtocol, Runtime)   /* has_next/next 缺失或返回类型错 */                                          \
    X(IterationExhausted, Runtime) /* 迭代器耗尽后调用 next */                                                   \
    X(MatchNoArm, Runtime)         /* match 无匹配分支且无 "_" 兜底 */                                           \
    X(SuperNoBaseClass, Runtime)   /* super 无父类可访问 */                                                      \
    X(UncaughtException, Runtime)  /* 用户 throw 的值未被 catch 捕获 */                                          \
    X(StackOverflow, Runtime)      /* 递归过深 */                                                                \
    X(CircularImport, Runtime)     /* import 形成循环 */                                                         \
    /* ========== INTERNAL ERROR（解释器不变式）========== */                                                    \
    X(Unreachable, Internal)          /* 逻辑上不可达的代码被执行 */                                             \
    X(AssertionFailed, Internal)      /* 内部断言失败 */                                                         \
    X(InvalidBytecode, Internal)      /* codeunit 损坏 / 非法操作码 */                                           \
    X(OpcodeNotImplemented, Internal) /* 合法 opcode 但运行期执行语义未实现 */                                   \
    X(StackUnderflow, Internal)       /* VM 栈失衡（弹出超过已压入） */                                          \
    X(InvalidState, Internal)         /* VM 处于非法内部状态 */                                                  \
    /* ========== RESOURCE ERROR（资源 / 环境）========== */                                                     \
    X(OutOfMemory, Resource)       /* GC 分配失败 */                                                             \
    X(FileReadFailed, Resource)    /* 源码/模块文件读取失败（映射自 fs::FsErrCode） */                           \
    X(ModuleNotFound, Resource)    /* import 的模块不存在 */                                                     \
    X(CodeUnitTooLarge, Resource)  /* 常量/指令数超限 */                                                         \
    X(TooManyLocals, Resource)     /* 局部变量数超限 */                                                          \
    X(TooManyArguments, Resource)  /* 单次调用实参数超限（CALL 操作数 u8 上限 255） */                           \
    X(TooManyParameters, Resource) /* 函数形参数超限（arity u8 上限 255） */                                     \
    X(TooManyUpvalues, Resource)   /* upvalue 数超限 */                                                          \
    X(TooManyElements, Resource)   /* 集合字面量元素数超限（MAKE_LIST/MAKE_MAP 操作数 u16 上限） */              \
    X(SourceTooLarge, Resource)    /* 单个源文件过大 */

#define ARIA_ERROR_ENUM(name, category) name,
    enum class ErrorCode : u8 { ARIA_ERROR_LIST(ARIA_ERROR_ENUM) };
#undef ARIA_ERROR_ENUM

#define ARIA_ERROR_INFO(name, category) {#name, ErrorCategory::category},
    inline constexpr CodeInfo kCodeTable[] = {ARIA_ERROR_LIST(ARIA_ERROR_INFO)};
#undef ARIA_ERROR_INFO

#undef ARIA_ERROR_LIST

    static_assert(std::to_underlying(ErrorCategory::Resource) + 1 == std::size(kCategoryNames),
                  "kCategoryNames 与 ErrorCategory 枚举不同步");

    // 错误大类可读名（如 "Syntax"）。
    [[nodiscard]]
    constexpr StringView to_string(const ErrorCategory c) noexcept {
        const auto index = std::to_underlying(c);
        ASSERT(index < std::size(kCategoryNames), "ErrorCategory out of range");
        return kCategoryNames[index];
    }

    // 错误码可读标识名（如 "UnterminatedString"），用于日志与调试。
    [[nodiscard]]
    constexpr StringView to_string(const ErrorCode c) noexcept {
        const auto index = std::to_underlying(c);
        ASSERT(index < std::size(kCodeTable), "ErrorCode out of range");
        return kCodeTable[index].name;
    }

    // 错误码所属大类。
    [[nodiscard]]
    constexpr ErrorCategory category_of(const ErrorCode code) noexcept {
        const auto index = std::to_underlying(code);
        ASSERT(index < std::size(kCodeTable), "ErrorCode out of range");
        return kCodeTable[index].cat;
    }

} // namespace aria

#endif // ARIA_ERRORCODE_HPP
