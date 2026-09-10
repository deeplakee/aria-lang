#ifndef ARIA_ERRORCODE_HPP
#define ARIA_ERRORCODE_HPP

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

    // 解释器内部统一错误码：普通 enum class。
    // 枚举值转字符串用 to_string(ErrorCode)；所属大类用 category_of(ErrorCode)。
    //
    // 分层按「错误在哪一阶段被发现」，思路对标
    // CPython（编译期 SyntaxError vs 运行期异常）、Lua 状态码、crafting-interpreters：
    //   - Syntax   ：词法/语法阶段，源码结构本身不合法（lexer/parser 可直接判定）。
    //   - Semantic ：结构合法但违反类型/作用域/形态规则；文法明确推迟到语义/字节码阶段
    //                的检查（左值合法性、super 形态、break/continue/return 上下文、
    //                try 须有 handler、默认值不得引用同函数参数等）归此类。
    //   - Runtime  ：执行期间才暴露的语义错误（类型不符、越界、键缺失等）。
    //   - Internal ：解释器自身不变式被破坏，属实现 bug（不可达、栈失衡、坏字节码）。
    //   - Resource ：资源/环境受限（内存、文件、模块、容量上限）。
    enum class ErrorCode : u8 {
        Ok = 0,

        // ========== SYNTAX ERROR（词法 / 语法阶段）==========
        // --- 词法 ---
        UnterminatedString, // 字符串未闭合 / 跨行
        InvalidEscape,      // 未识别转义（无 \x；\u{} 非法）
        InvalidNumber,      // 数字字面量非法（前缀 / 下划线位置 / 空 mantissa 等）
        InvalidCharacter,   // 无法归入任何 token 的字符
        // --- 语法 ---
        UnexpectedToken,    // 非预期的 token
        UnexpectedEof,      // 源码提前结束
        ExpectedExpression, // 期待表达式却遇到他物
        ExpectedIdentifier, // 期待标识符
        ExpectedToken,      // 期待特定符号/关键字（具体对象由报错信息补充）
        InvalidPattern,     // 解构模式结构非法（rest 非末尾、..._ 等）
        VarargsNotLast,     // ... 必须位于参数列表末尾
        DefaultAfterPlain,  // 默认参数后不得再有无默认参数（文法强制顺序）

        // ========= SEMANTIC ERROR（语义分析阶段）=========
        // 下列多为文法明确「推迟到语义阶段」的检查。
        InvalidAssignmentTarget, // 赋值左值须为 identifier / obj.field / obj[index]
        InvalidSuperUse,         // super 须为 super.method(...) 形态
        SuperOutsideMethod,      // super 出现在非方法上下文
        ThisOutsideClass,        // this 出现在类外
        BreakOutsideLoop,        // break 须在循环内
        ContinueOutsideLoop,     // continue 须在循环内
        ReturnOutsideFunction,   // return 须在函数内
        TryWithoutHandler,       // try 须有 catch
        DefaultParamSelfRef,     // 默认值表达式引用了同函数的参数
        DuplicateParam,          // 同函数形参重名
        UndefinedType,           // 引用未定义的类名
        RedefinedVariable,       // 同作用域重复定义变量
        UninitializedVariable,   // 使用定义但未初始化的局部（如 var x = x + 1 自引用）
        RedefinedClass,          // 重复定义类
        RedefinedMember,         // 类内重复的方法/字段
        NumberOutOfRange,        // 整数字面量超出 i48 范围（溢出，文法约定语义阶段处理）
        NotImplemented,          // 功能尚未实现（CodeGen 占位 not_impl；经 Error 通道均编译期，故归语义阶段）

        // ========= RUNTIME ERROR（运行时阶段）=========
        TypeMismatch,      // 运算/操作的类型不符（如 number + 非数）
        InvalidOperand,    // 一元操作数非法（如对非数取负）
        NilDereference,    // 对 nil 取字段 / 下标 / 调用
        IndexOutOfBounds,  // list 下标越界（含解构元素不足）
        DivisionByZero,    // 除零
        ModuloByZero,      // 模零
        KeyError,          // map 键不存在
        UndefinedVariable, // 引用未定义的变量/函数（全局名 LOAD/STORE_GLOBAL 运行期查表 miss；
                           // 局部的未初始化读是编译期 UninitializedVariable，两者不同码）
        UndefinedProperty, // 对象无该字段/方法
        CallNonCallable,   // 调用非函数值
        WrongArity,        // 实参数量不符（含默认参数/varargs 填充后）
        NotIterable,       // forIn 目标不可遍历
        IteratorProtocol,  // has_next/next 缺失或返回类型错
        MatchNoArm,        // match 无匹配分支且无 "_" 兜底
        SuperNoBaseClass,  // super 无父类可访问
        UncaughtException, // 用户 throw 的值未被 catch 捕获
        StackOverflow,     // 递归过深
        CircularImport,    // import 形成循环

        // ======== INTERNAL ERROR（解释器不变式）========
        Unreachable,          // 逻辑上不可达的代码被执行
        AssertionFailed,      // 内部断言失败
        InvalidBytecode,      // codeunit 损坏 / 非法操作码
        OpcodeNotImplemented, // 合法 opcode 但运行期执行语义未实现（VM not_implemented 经 fatal_error）
        StackUnderflow,       // VM 栈失衡（弹出超过已压入）
        InvalidState,         // VM 处于非法内部状态

        // ========= RESOURCE ERROR（资源 / 环境）=========
        OutOfMemory,       // GC 分配失败
        FileReadFailed,    // 源码/模块文件读取失败（映射自 fs::FsErrCode）
        ModuleNotFound,    // import 的模块不存在
        CodeUnitTooLarge,  // 常量/指令数超限
        TooManyLocals,     // 局部变量数超限
        TooManyArguments,  // 单次调用实参数超限（CALL 操作数 u8 上限 255）
        TooManyParameters, // 函数形参数超限（arity u8 上限 255）
        TooManyUpvalues,   // upvalue 数超限
        SourceTooLarge,    // 单个源文件过大
    };

    using ErrCode = ErrorCode;

    // 错误大类可读名（如 "Syntax"）。
    [[nodiscard]]
    constexpr StringView to_string(const ErrorCategory c) noexcept {
        switch (c) {
            case ErrorCategory::Ok:
                return "Ok";
            case ErrorCategory::Syntax:
                return "Syntax";
            case ErrorCategory::Semantic:
                return "Semantic";
            case ErrorCategory::Runtime:
                return "Runtime";
            case ErrorCategory::Internal:
                return "Internal";
            case ErrorCategory::Resource:
                return "Resource";
            default:
                UNREACHABLE();
        }
    }

    // 错误码可读标识名（如 "UnterminatedString"），用于日志与调试。
    [[nodiscard]]
    constexpr StringView to_string(const ErrorCode c) noexcept {
        switch (c) {
            case ErrorCode::Ok:
                return "Ok";
            case ErrorCode::UnterminatedString:
                return "UnterminatedString";
            case ErrorCode::InvalidEscape:
                return "InvalidEscape";
            case ErrorCode::InvalidNumber:
                return "InvalidNumber";
            case ErrorCode::InvalidCharacter:
                return "InvalidCharacter";
            case ErrorCode::UnexpectedToken:
                return "UnexpectedToken";
            case ErrorCode::UnexpectedEof:
                return "UnexpectedEof";
            case ErrorCode::ExpectedExpression:
                return "ExpectedExpression";
            case ErrorCode::ExpectedIdentifier:
                return "ExpectedIdentifier";
            case ErrorCode::ExpectedToken:
                return "ExpectedToken";
            case ErrorCode::InvalidPattern:
                return "InvalidPattern";
            case ErrorCode::VarargsNotLast:
                return "VarargsNotLast";
            case ErrorCode::DefaultAfterPlain:
                return "DefaultAfterPlain";
            case ErrorCode::InvalidAssignmentTarget:
                return "InvalidAssignmentTarget";
            case ErrorCode::InvalidSuperUse:
                return "InvalidSuperUse";
            case ErrorCode::SuperOutsideMethod:
                return "SuperOutsideMethod";
            case ErrorCode::ThisOutsideClass:
                return "ThisOutsideClass";
            case ErrorCode::BreakOutsideLoop:
                return "BreakOutsideLoop";
            case ErrorCode::ContinueOutsideLoop:
                return "ContinueOutsideLoop";
            case ErrorCode::ReturnOutsideFunction:
                return "ReturnOutsideFunction";
            case ErrorCode::TryWithoutHandler:
                return "TryWithoutHandler";
            case ErrorCode::DefaultParamSelfRef:
                return "DefaultParamSelfRef";
            case ErrorCode::DuplicateParam:
                return "DuplicateParam";
            case ErrorCode::UndefinedVariable:
                return "UndefinedVariable";
            case ErrorCode::UndefinedType:
                return "UndefinedType";
            case ErrorCode::RedefinedVariable:
                return "RedefinedVariable";
            case ErrorCode::UninitializedVariable:
                return "UninitializedVariable";
            case ErrorCode::RedefinedClass:
                return "RedefinedClass";
            case ErrorCode::RedefinedMember:
                return "RedefinedMember";
            case ErrorCode::NumberOutOfRange:
                return "NumberOutOfRange";
            case ErrorCode::TypeMismatch:
                return "TypeMismatch";
            case ErrorCode::InvalidOperand:
                return "InvalidOperand";
            case ErrorCode::NilDereference:
                return "NilDereference";
            case ErrorCode::IndexOutOfBounds:
                return "IndexOutOfBounds";
            case ErrorCode::DivisionByZero:
                return "DivisionByZero";
            case ErrorCode::ModuloByZero:
                return "ModuloByZero";
            case ErrorCode::KeyError:
                return "KeyError";
            case ErrorCode::UndefinedProperty:
                return "UndefinedProperty";
            case ErrorCode::CallNonCallable:
                return "CallNonCallable";
            case ErrorCode::WrongArity:
                return "WrongArity";
            case ErrorCode::NotIterable:
                return "NotIterable";
            case ErrorCode::IteratorProtocol:
                return "IteratorProtocol";
            case ErrorCode::MatchNoArm:
                return "MatchNoArm";
            case ErrorCode::SuperNoBaseClass:
                return "SuperNoBaseClass";
            case ErrorCode::UncaughtException:
                return "UncaughtException";
            case ErrorCode::StackOverflow:
                return "StackOverflow";
            case ErrorCode::CircularImport:
                return "CircularImport";
            case ErrorCode::Unreachable:
                return "Unreachable";
            case ErrorCode::AssertionFailed:
                return "AssertionFailed";
            case ErrorCode::InvalidBytecode:
                return "InvalidBytecode";
            case ErrorCode::OpcodeNotImplemented:
                return "OpcodeNotImplemented";
            case ErrorCode::StackUnderflow:
                return "StackUnderflow";
            case ErrorCode::NotImplemented:
                return "NotImplemented";
            case ErrorCode::InvalidState:
                return "InvalidState";
            case ErrorCode::OutOfMemory:
                return "OutOfMemory";
            case ErrorCode::FileReadFailed:
                return "FileReadFailed";
            case ErrorCode::ModuleNotFound:
                return "ModuleNotFound";
            case ErrorCode::CodeUnitTooLarge:
                return "CodeUnitTooLarge";
            case ErrorCode::TooManyLocals:
                return "TooManyLocals";
            case ErrorCode::TooManyArguments:
                return "TooManyArguments";
            case ErrorCode::TooManyParameters:
                return "TooManyParameters";
            case ErrorCode::TooManyUpvalues:
                return "TooManyUpvalues";
            case ErrorCode::SourceTooLarge:
                return "SourceTooLarge";
            default:
                UNREACHABLE();
        }
    }

    // 错误码所属大类。
    [[nodiscard]]
    constexpr ErrorCategory category_of(ErrorCode c) noexcept {
        switch (c) {
            case ErrorCode::Ok:
                return ErrorCategory::Ok;
            // Syntax
            case ErrorCode::UnterminatedString:
            case ErrorCode::InvalidEscape:
            case ErrorCode::InvalidNumber:
            case ErrorCode::InvalidCharacter:
            case ErrorCode::UnexpectedToken:
            case ErrorCode::UnexpectedEof:
            case ErrorCode::ExpectedExpression:
            case ErrorCode::ExpectedIdentifier:
            case ErrorCode::ExpectedToken:
            case ErrorCode::InvalidPattern:
            case ErrorCode::VarargsNotLast:
            case ErrorCode::DefaultAfterPlain:
                return ErrorCategory::Syntax;
            // Semantic
            case ErrorCode::InvalidAssignmentTarget:
            case ErrorCode::InvalidSuperUse:
            case ErrorCode::SuperOutsideMethod:
            case ErrorCode::ThisOutsideClass:
            case ErrorCode::BreakOutsideLoop:
            case ErrorCode::ContinueOutsideLoop:
            case ErrorCode::ReturnOutsideFunction:
            case ErrorCode::TryWithoutHandler:
            case ErrorCode::DefaultParamSelfRef:
            case ErrorCode::DuplicateParam:
            case ErrorCode::UndefinedType:
            case ErrorCode::RedefinedVariable:
            case ErrorCode::RedefinedClass:
            case ErrorCode::RedefinedMember:
            case ErrorCode::NumberOutOfRange:
            case ErrorCode::UninitializedVariable:
            case ErrorCode::NotImplemented:
                return ErrorCategory::Semantic;
            // Runtime
            case ErrorCode::TypeMismatch:
            case ErrorCode::InvalidOperand:
            case ErrorCode::NilDereference:
            case ErrorCode::IndexOutOfBounds:
            case ErrorCode::DivisionByZero:
            case ErrorCode::ModuloByZero:
            case ErrorCode::KeyError:
            case ErrorCode::UndefinedVariable:
            case ErrorCode::UndefinedProperty:
            case ErrorCode::CallNonCallable:
            case ErrorCode::WrongArity:
            case ErrorCode::NotIterable:
            case ErrorCode::IteratorProtocol:
            case ErrorCode::MatchNoArm:
            case ErrorCode::SuperNoBaseClass:
            case ErrorCode::UncaughtException:
            case ErrorCode::StackOverflow:
            case ErrorCode::CircularImport:
                return ErrorCategory::Runtime;
            // Internal
            case ErrorCode::Unreachable:
            case ErrorCode::AssertionFailed:
            case ErrorCode::InvalidBytecode:
            case ErrorCode::OpcodeNotImplemented:
            case ErrorCode::StackUnderflow:
            case ErrorCode::InvalidState:
                return ErrorCategory::Internal;
            // Resource
            case ErrorCode::OutOfMemory:
            case ErrorCode::FileReadFailed:
            case ErrorCode::ModuleNotFound:
            case ErrorCode::CodeUnitTooLarge:
            case ErrorCode::TooManyLocals:
            case ErrorCode::TooManyArguments:
            case ErrorCode::TooManyParameters:
            case ErrorCode::TooManyUpvalues:
            case ErrorCode::SourceTooLarge:
                return ErrorCategory::Resource;
            default:
                UNREACHABLE();
        }
    }

} // namespace aria

#endif // ARIA_ERRORCODE_HPP
