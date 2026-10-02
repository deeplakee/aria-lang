#ifndef ARIA_ERROR_HPP
#define ARIA_ERROR_HPP

#include <format>

#include "common.hpp"
#include "error/ErrorCode.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"

namespace aria {

    // source_file 相关类型位于 aria::src 下，引入需逐条 using。
    using src::SourceFile;
    using src::SourceLoc;

    // 错误值对象:ErrorCode + 构造期一次性烘焙的完整消息,不存结构化位置、不持 SourceFile*、可任意拷移;
    // 编译期统一错误载体与运行期未捕获出口的物化形态(在途错误是 ObjException);语言 throw 走 VM 通道不经本类。
    class Error {
    public:
        // 构造工厂(唯一公开构造面;语义在工厂名上自文档化,构造点无法拼写错语义)

        // 码 + 细节、无位置;供内部/资源错误或不关心位置的场景。detail 为只读组件(取 StringView、烘进
        // message_ 不存储);**不设默认值** --为空须显式传 {},强制每个报错点说出发生了什么。
        [[nodiscard]]
        static Error from_detail(const ErrorCode code, const StringView detail) {
            return Error{code, make_message(code, detail)};
        }

        // 带位置版,供词法/语法/语义阶段:构造期就地烘位置前缀(SourceFile 此刻存活),此后不持位置;
        // 空态 loc 无须特判 --空位置串天然无前缀(如 CodeGen::fail 取 node->loc() 即可能为空态)。
        [[nodiscard]]
        static Error from_detail(const ErrorCode code, const SourceLoc loc, const StringView detail) {
            return Error{code, make_message(code, loc.to_string(), detail)};
        }

        // 成品语义:原样收已烘焙完整消息串,不经 make_message(否则双重前缀);禁传裸 detail(缺前缀)。
        [[nodiscard]]
        static Error from_baked(const ErrorCode code, const StringView message) {
            return Error{code, String{message}};
        }

        // 烘焙单点(一对共名重载,与 from_detail 同构):完整消息 = [location + ": "] + "Category: Name"
        //[ + " " + detail];detail 为原始细节串(不含 "Category:" 前缀,防双烘),位置串由调用方格式化好传入。

        static String make_message(const ErrorCode code, const StringView detail) {
            String s = std::format("{}: {}", to_string(category_of(code)), to_string(code));
            if (!detail.empty()) {
                s.append(" ").append(detail);
            }
            return s;
        }

        // 带位置版:非空位置串前缀 "location: ";空位置串退化为无位置版(空态 loc 自然合流)。
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

        // 完整可读消息(构造期烘焙成型,自存不依赖外部对象)。
        [[nodiscard]]
        const String& message() const noexcept {
            return message_;
        }

    private:
        // 原始构造:直收最终消息串不做加工;公开构造面一律走静态工厂(组件经 make_message、成品串经 from_baked)。
        Error(const ErrorCode code, String message) : code_{code}, message_{std::move(message)} {}

        ErrorCode code_;
        String    message_;
    };

    // 不可恢复错误：打印到 stderr 后以退出码 1 终止进程，不抛、不做栈展开。适用于 Internal 类
    // 不变式被破坏或 Resource 类资源耗尽 --这类「无法继续执行」的错误不靠返回值层层传播。
    [[noreturn]]
    inline void fatal_error(const Error& error) {
        io::println(stderr, "aria: fatal: {}", error.message());
        std::exit(1);
    }

    // 便捷重载：码 + 格式化细节（格式串须为编译期常量，经 std::format_string 静态校验）。不设成品串
    // 重载：动态串经 "{}" 实参传入，或先经 Error::from_detail 构造 Error 再走上一重载。
    template<typename... Args>
    [[noreturn]]
    void fatal_error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
        fatal_error(Error::from_detail(code, std::format(fmt, std::forward<Args>(args)...)));
    }

} // namespace aria

#endif // ARIA_ERROR_HPP
