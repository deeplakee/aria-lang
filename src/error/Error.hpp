#ifndef ARIA_ERROR_HPP
#define ARIA_ERROR_HPP

#include <format>

#include "common.hpp"
#include "error/ErrorCode.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"

namespace aria {

    // 将 source_file 相关类型引入 aria 命名空间（见 CLAUDE.md：这些类型位于
    // aria::src 下，引用需分别 using）。
    using src::SourceFile;
    using src::SourceLoc;
    using src::SourceSpan;

    // 错误值对象：聚合 ErrorCode + 完整可读消息，作为编译期各阶段的统一错误载体与
    // 运行期未捕获出口的边界物化形态--运行期在途错误实体是 ObjException（存 current_ctx
    // 挂起错误寄存器，见 .claude/rules/runtime.md「VM 异常通道落地状态」），Error 仅在
    // dispatch_loop 返回时经反提拆件（AriaVM uncaught_error_parts）+ from_baked 物化构造，
    // 不参与 dispatch_loop 内部传播。
    //
    // 设计要点：
    //   - 值类型（可拷贝/移动），供 Result<T, Error> 携带，符合项目「错误处理倾向
    //     Result 返回而非抛 C++ 异常」的约束。
    //   - 只两个字段：code_（机器标识，供分类/名称/相等判定经 code() 再取）+ message_
    //     （完整人类可读串）。message_ 在构造期一次性烘焙成型--把位置前缀（编译期取
    //     SourceLoc 的 "path:line:col"，运行期取调用方格式化的 "path:line"）、分类名、
    //     码名、细节拼成最终串存下，构造完成后 Error 完全自有、不持任何 SourceFile*
    //     裸指针，可任意拷贝/移动/跨流程传递，无悬空风险。
    //   - 不保留结构化位置（LineCol/SourceLoc）字段：解释器无需多错误按位置排序/去重等
    //     能力，结构化位置只会徒增复杂度与生命期约束。需要位置时直接读 message_ 串即可。
    //
    // 生命周期：位置在构造期一次性格式化（此时 SourceFile 必然存活），构造完成后 Error 与
    // SourceFile 完全解耦。这与 Token::lexeme_ 仍借 StringView 指向 SourceFile::content_
    // （需 SourceFile 存活）的约束不同：Error 不再有此类约束。
    //
    // 注意：本类只承载「解释器报告的错误」。aria 语言自身的 throw/catch 抛的是 Value，
    // 由 VM 用 THROW 操作码 + CodeUnit 内异常记录表实现（不引入 SETUP_EXCEPT，见
    // CLAUDE.md「错误处理」第 2 条），与 C++ 异常无关，不经过本类。
    class Error {
    public:
        // ---- 构造工厂(唯一公开构造面;语义在工厂名上自文档化,构造点无法拼写错语义)----

        // 细节语义(无位置):码 + 细节,供内部/资源错误或不关心位置的场景使用。
        // detail 是「只读组件」(被 make_message 烘进 message_,本身不存储),故取 const String&
        // 而非 by-value -- by-value 是 sink 惯用法,用在此处只会让 lvalue 调用点多付一次参数拷贝
        // (烘进 message_ 的那份拷贝省不掉)。
        // detail **不设默认值**:无 detail 的错误缺上下文(detail 是运行期错误的唯一上下文载体),
        // 有意为空须显式传 {} / "",强制每个报错点说出发生了什么。
        // message_ 烘为 "Category: Name[ detail]"。
        [[nodiscard]]
        static Error from_detail(const ErrorCode code, const String& detail) {
            return Error{code, make_message(code, detail)};
        }

        // 细节语义(带位置):码 + SourceLoc + 细节,供词法/语法/语义阶段使用。
        // 与上一重载同属「组件构造」,以 SourceLoc 参数区分、共名 from_detail -- 位置只是
        // 细节语义的一个可选变体,不给它单独立名。构造期就地用 loc 烘位置前缀(SourceFile
        // 此刻存活,安全),此后不再持有 SourceLoc/SourceFile*。空态 loc 无须特判:
        // to_string 对空态渲染空串、make_message 对空位置串天然无前缀,空态 loc 与
        // 无 loc 在此自然合流(空态合法存在:默认构造供容器占位,测试/嵌入方无源文件
        // 构 AST 亦用,CodeGen::fail 取 node->loc() 即可能为空态)。
        // detail 同上一重载(const String&,无默认值)。
        // message_ 烘为 "path:line:col: Category: Name[ detail]"(空态 loc 无位置段)。
        [[nodiscard]]
        static Error from_detail(const ErrorCode code, const SourceLoc& loc, const String& detail) {
            return Error{code, make_message(code, loc.to_string(), detail)};
        }

        // 成品语义:以**已烘焙完整消息串**原样构造,不经 make_message(否则把 "Category: Name"
        // 前缀再烘一遍成双重前缀)。两类合法调用方(均在 VM 未捕获出口侧):ObjException::to_error()
        // (其 message_ 与 Error::message() 同形,寄存器载荷反提)与 AriaVM::unwind 物化未捕获
        // Error 时烘焙堆栈跟踪(反提消息 + 逐帧 at 行拼接后经本工厂重建,M3)。
        // 禁止传组件串(裸 detail)-- 会得到缺前缀的消息,渲染不一致。设计见
        // .claude/reference/runtime/exception-implementation-pitfalls.md 坑 #7。
        [[nodiscard]]
        static Error from_baked(const ErrorCode code, const StringView message) {
            return Error{code, String{message}};
        }

        // 报错点的格式化细节在调用处自行 std::format 后走 from_detail(仅编译期收口,如
        // CodeGen::fail / Lexer / Parser;运行期一律 make_message 烘齐 -- 见 AriaVM.hpp 的
        // raise 模板装箱与 uncaught_error_parts 兜底)。

        // 烘焙单点(公开,一对共名重载,以位置参数区分,与 from_detail 两重载同构镜像):
        // 完整消息 = [location + ": "] + "Category: Name"[ + " " + detail]。
        // detail 为**原始细节串**(不含 "Category:" 前缀 -- 防双烘),位置串由调用方格式化好传入
        // (编译期 SourceLoc::to_string 的 "path:line:col" / 运行期 "path:line" / "<name>:line")。
        // from_detail 经此合成;装箱点 AriaVM::raise 亦直接使用(烘齐后 new_exception 装箱,
        // 不经 Error 对象中转)。编译/运行期消息形态同源于此。

        // 无位置版:消息 = "Category: Name"[ + " " + detail]。供无位置语义的报错点直接使用
        // (from_detail 无 loc 重载 / uncaught_error_parts 非 ObjException 兜底,与 from_detail
        // 同源同串),无须显式传空位置占位。
        static String make_message(const ErrorCode code, const StringView detail) {
            String s = std::format("{}: {}", to_string(category_of(code)), to_string(code));
            if (!detail.empty()) {
                s.append(" ").append(detail);
            }
            return s;
        }

        // 带位置版:非空位置串前缀 "location: ";空位置串退化为无位置版 -- 空态 loc
        // (SourceLoc::to_string 空态渲染空串)在此自然合流,调用方无须先判空规避。
        // 当前消费方为编译期路径(from_detail 带 loc 重载);运行期装箱自 2026-09-10 起
        // 不烘位置(AriaVM::raise 走无位置版,位置由 unwind 跟踪行给出)。
        static String make_message(const ErrorCode code, const StringView location, const StringView detail) {
            if (location.empty()) {
                return make_message(code, detail);
            }
            return std::format("{}: {}", location, make_message(code, detail));
        }

        // 所属错误码(分类/名称/相等判定经此再取)。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return code_;
        }

        // 完整可读消息(构造期烘焙成型,含位置前缀 + 分类名 + 码名 + 细节)。
        // 形如 "main.aria:3:5: Syntax: UnterminatedString 字符串未闭合"(带位置)或
        // "Syntax: UnterminatedString 字符串未闭合"(无位置)。自存、不依赖任何外部对象。
        [[nodiscard]]
        const String& message() const noexcept {
            return message_;
        }

    private:
        // 原始构造(唯一默认形态):直接收 code + **最终消息串**,不经 make_message、不做任何加工。
        // 烘焙单点收于公开 make_message 重载(from_detail 两重载与 VM 的 raise / runtime_err 共用);
        // from_baked 装载已烘串。公开构造面一律走上方静态工厂。
        Error(ErrorCode code, String message) : code_{code}, message_{std::move(message)} {}

        ErrorCode code_;
        String    message_;
    };

    // 不可恢复错误：打印错误到 stderr 后以退出码 1 终止进程。
    //
    // 适用场景：解释器自身不变式被破坏（Internal 类，如 Unreachable /
    // InvalidBytecode）或资源耗尽（Resource 类，如 OutOfMemory）--这类错误
    // 不应靠返回值层层传播，直接终止更安全。函数标记 [[noreturn]]。
    //
    // 与抛异常的区别：fatal_error 不抛、不做栈展开，打印后立即 std::exit，
    // 适用于「无法继续执行」的致命错误；可恢复或可跨栈传播的错误用
    // Result<T, Error> / AriaException。
    [[noreturn]]
    inline void fatal_error(const Error& error) {
        io::println(stderr, "aria: fatal: {}", error.message());
        std::exit(1);
    }

    // 便捷重载：码 + 格式化细节（格式串须为编译期常量，经 std::format_string 静态校验）。
    // 不设成品串重载：动态串经 "{}" 实参传入 -- fatal_error(code, "{}", str)，
    // 或先经 Error::from_detail 构造 Error 再走上一重载。
    template<typename... Args>
    [[noreturn]]
    void fatal_error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
        fatal_error(Error::from_detail(code, std::format(fmt, std::forward<Args>(args)...)));
    }

} // namespace aria

#endif // ARIA_ERROR_HPP
