#ifndef ARIA_ERRORCODE_HPP
#define ARIA_ERRORCODE_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 错误大类（enum class）；转字符串用 to_string(ErrorCategory)，由 ErrorCode 经 category_of() 映射而来。
    enum class ErrorCategory : u8 {
        Ok,
        Syntax,
        Semantic,
        Runtime,
        Internal,
        Resource,
    };

    // 大类可读名表（下标即 std::to_underlying(c)，行序须与枚举一致；哨兵断言只拦表长漂移，行序错位靠增删自查）。
    inline constexpr StringView kCategoryNames[] = {
            "Ok", "Syntax", "Semantic", "Runtime", "Internal", "Resource",
    };

    // 「名字/大类」两个查表函数共用此表；表体与枚举同源展开、无不同步风险，非法值由查表函数的 ASSERT 拦截。
    struct CodeInfo {
        StringView    name; // 可读标识名（如 "UnterminatedString"）
        ErrorCategory cat;  // 所属大类
    };

// 解释器内部统一错误码，按「错误在哪一阶段被发现」分五大类（各类界定见宏内分节横幅）。枚举与
// 信息表 kCodeTable 经 X-Macro 同源展开，名字串经 # 派生；隐式连续编号、下标即 std::to_underlying(code)。
// 逐值注释用块注释（行注释会吞续行符）。
#define ARIA_ERROR_LIST(X)                                                                                          \
    X(Ok, Ok)                                                                                                       \
    /* ========== SYNTAX ERROR (lexer / parser stage) ========== */                                                 \
    /* --- lexer --- */                                                                                             \
    X(UnterminatedString, Syntax) /* string not closed / crosses a line break */                                    \
    X(InvalidEscape, Syntax)      /* unknown escape (no \x; malformed \u{}) */                                      \
    X(InvalidNumber, Syntax)      /* malformed number literal (base prefix, underscores, mantissa) */               \
    X(InvalidCharacter, Syntax)   /* character that starts no token */                                              \
    /* --- parser --- */                                                                                            \
    X(UnexpectedEof, Syntax)      /* source ends early */                                                           \
    X(ExpectedExpression, Syntax) /* expression expected, something else found */                                   \
    X(ExpectedIdentifier, Syntax) /* identifier expected */                                                         \
    X(ExpectedToken, Syntax)      /* expected symbol or keyword (named by the message) */                           \
    X(InvalidPattern, Syntax)     /* malformed destructuring pattern (rest not last, ..._) */                       \
    X(VarargsNotLast, Syntax)     /* '...' must be the last parameter */                                            \
    X(DefaultAfterPlain, Syntax)  /* no plain parameter after a defaulted one */                                    \
    /* ========== SEMANTIC ERROR (checks the grammar defers) ========== */                                          \
    X(InvalidAssignmentTarget, Semantic) /* lvalue must be identifier / obj.field / obj[index] */                   \
    X(SuperOutsideMethod, Semantic)      /* super outside a method */                                               \
    X(ThisOutsideClass, Semantic)        /* this outside a class */                                                 \
    X(BreakOutsideLoop, Semantic)        /* break outside a loop */                                                 \
    X(ContinueOutsideLoop, Semantic)     /* continue outside a loop */                                              \
    X(ReturnValueAtTopLevel, Semantic)   /* return with a value at top level */                                     \
    X(TryWithoutHandler, Semantic)       /* try without a catch clause */                                           \
    X(UnreachableArm, Semantic)          /* arm after the '_' arm (dead arm; '_' comes last, at most once) */       \
    X(DuplicateParam, Semantic)          /* parameter name used twice in one function */                            \
    X(UndefinedType, Semantic)           /* class name that is not defined */                                       \
    X(RedefinedVariable, Semantic)       /* variable defined twice in one scope */                                  \
    X(RedefinedClass, Semantic)          /* class defined twice */                                                  \
    X(NumberOutOfRange, Semantic)        /* integer literal beyond i48 (the grammar defers overflow here) */        \
    /* ========== RUNTIME ERROR (execution stage) ========== */                                                     \
    X(TypeMismatch, Runtime)                /* operand type mismatch (number + non-number) */                       \
    X(InvalidOperand, Runtime)              /* bad unary operand (negating a non-number) */                         \
    X(IndexOutOfBounds, Runtime)            /* index or slice out of bounds (list, string, substring, empty pop) */ \
    X(DivisionByZero, Runtime)              /* division by zero */                                                  \
    X(ModuloByZero, Runtime)                /* modulo by zero */                                                    \
    X(KeyError, Runtime)                    /* map key missing */                                                   \
    X(EmptyPattern, Runtime)                /* empty pattern (split separator or replace needle is empty) */        \
    X(UndefinedVariable, Runtime)           /* name not found at runtime (variable, function or global) */          \
    X(UndefinedProperty, Runtime)           /* receiver has no such field or method */                              \
    X(CallNonCallable, Runtime)             /* calling a non-callable value */                                      \
    X(WrongArity, Runtime)                  /* argument count wrong (after defaults and varargs fill) */            \
    X(NotIterable, Runtime)                 /* for-in target is not iterable */                                     \
    X(IteratorProtocol, Runtime)            /* has_next/next missing or wrongly typed */                            \
    X(IterationExhausted, Runtime)          /* next called after the iterator was exhausted */                      \
    X(MatchNoArm, Runtime)                  /* no arm matched and no '_' fallback */                                \
    X(SuperNoBaseClass, Runtime)            /* super with no base class */                                          \
    X(UncaughtException, Runtime)           /* thrown value never caught */                                         \
    X(Error, Runtime)                       /* user-raised exception object (the Error builtin) */                  \
    X(ResumeDeadCoroutine, Runtime)         /* resume on a Done/Failed coroutine */                                 \
    X(ResumeNonSuspendedCoroutine, Runtime) /* resume on a coroutine still on the resume chain */                   \
    X(YieldOutsideCoroutine, Runtime)       /* yield in the main context */                                         \
    X(StackOverflow, Runtime)               /* recursion too deep */                                                \
    X(CircularImport, Runtime)              /* import cycle */                                                      \
    /* ========== INTERNAL ERROR (interpreter invariants) ========== */                                             \
    X(Unreachable, Internal)     /* code that should be unreachable ran */                                          \
    X(AssertionFailed, Internal) /* internal assertion failed */                                                    \
    X(InvalidBytecode, Internal) /* corrupt code unit or invalid opcode */                                          \
    X(StackUnderflow, Internal)  /* VM stack imbalance (popped more than pushed) */                                 \
    X(InvalidState, Internal)    /* VM in an invalid internal state */                                              \
    /* ========== RESOURCE ERROR (resources / environment) ========== */                                            \
    X(OutOfMemory, Resource)       /* allocation failed */                                                          \
    X(FileReadFailed, Resource)    /* entry source file unreadable; import reads report ModuleNotFound */           \
    X(ModuleNotFound, Resource)    /* imported module not found */                                                  \
    X(CodeUnitTooLarge, Resource)  /* too many constants or instructions */                                         \
    X(TooManyLocals, Resource)     /* too many locals */                                                            \
    X(TooManyArguments, Resource)  /* too many arguments (CALL operand is u8) */                                    \
    X(TooManyParameters, Resource) /* too many parameters (arity is u8) */                                          \
    X(TooManyUpvalues, Resource)   /* too many upvalues */                                                          \
    X(TooManyElements, Resource)   /* too many elements (MAKE_LIST/MAKE_MAP u16, BUILD_STRING u8) */                \
    X(SourceTooLarge, Resource)    /* single source file too large */

#define ARIA_ERROR_ENUM(name, category) name,
    enum class ErrorCode : u8 { ARIA_ERROR_LIST(ARIA_ERROR_ENUM) };
#undef ARIA_ERROR_ENUM

#define ARIA_ERROR_INFO(name, category) {#name, ErrorCategory::category},
    inline constexpr CodeInfo kCodeTable[] = {ARIA_ERROR_LIST(ARIA_ERROR_INFO)};
#undef ARIA_ERROR_INFO

#undef ARIA_ERROR_LIST

    static_assert(std::to_underlying(ErrorCategory::Resource) + 1 == std::size(kCategoryNames),
                  "kCategoryNames and ErrorCategory enum are out of sync");

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

    [[nodiscard]]
    constexpr ErrorCategory category_of(const ErrorCode code) noexcept {
        const auto index = std::to_underlying(code);
        ASSERT(index < std::size(kCodeTable), "ErrorCode out of range");
        return kCodeTable[index].cat;
    }

} // namespace aria

#endif // ARIA_ERRORCODE_HPP
