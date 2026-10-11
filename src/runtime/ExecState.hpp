#ifndef ARIA_EXEC_STATE_HPP
#define ARIA_EXEC_STATE_HPP

#include "common.hpp"

namespace aria {

    // 执行状态(协程生命周期五态)。
    enum class ExecState : u8 {
        Suspended, // 已创建未首启,或 yield 挂起(可 resume)
        Normal,    // 自己 resume 了别人,自身在链中间挂起(Lua 的 normal)
        Running,   // 正在执行(即 VM 的当前上下文)
        Done,      // 协程体正常返回(死态)
        Failed,    // 未捕获错误死亡(死态)
    };

    // 状态拼写(使用小写)。
    [[nodiscard]]
    constexpr StringView to_string(const ExecState state) noexcept {
        switch (state) {
            case ExecState::Suspended:
                return "suspended";
            case ExecState::Normal:
                return "normal";
            case ExecState::Running:
                return "running";
            case ExecState::Done:
                return "done";
            case ExecState::Failed:
                return "failed";
            default:
                UNREACHABLE();
        }
    }

} // namespace aria

#endif // ARIA_EXEC_STATE_HPP
