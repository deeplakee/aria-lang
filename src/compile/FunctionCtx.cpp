#include "compile/FunctionCtx.hpp"

#include <ranges>

#include "object/ObjFunction.hpp"

namespace aria {

    // ============================================================
    // 构造
    // ============================================================

    FunctionCtx::FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, const FnKind kind) :
        enclosing_{enclosing}, fn_{fn}, kind_{kind}, scope_depth_{0} {
        // 槽 0：实例方法族 = 具名局部 this（caller 压 receiver 占此槽，恒已初始化）；其余 = 哑元
        // callee（空名，词法不可达，用户代码不可引用）。this 为关键字不会与用户标识符撞名
        // （kThisName 注），可安全参与局部查名与捕获。
        auto slot0 = Local{.name           = is_method(kind) ? String{kThisName} : String{},
                           .depth          = 0,
                           .is_captured    = false,
                           .is_initialized = true};
        locals_.push_back(std::move(slot0));
    }

    // ============================================================
    // 局部 / 作用域
    // ============================================================

    u16 FunctionCtx::add_local(const StringView name) {
        auto local = Local{.name = String{name}, .depth = scope_depth_, .is_captured = false, .is_initialized = false};
        locals_.push_back(std::move(local));
        return static_cast<u16>(locals_.size() - 1);
    }

    bool FunctionCtx::is_defined_in_scope(const StringView name) const {
        for (const auto& local: std::views::reverse(locals_)) {
            if (local.depth < scope_depth_) {
                return false; // 外层同名允许 shadow
            }
            if (local.name == name) {
                return true;
            }
        }
        return false;
    }

    void FunctionCtx::mark_initialized(const u16 slot) { locals_[slot].is_initialized = true; }

    bool FunctionCtx::is_initialized(const u16 slot) const { return locals_[slot].is_initialized; }

    Opt<u16> FunctionCtx::find_local(const StringView name) const {
        // 从内向外查找（高索引 = 更内层作用域），首个命中即最内层同名局部，早退。
        // std::views::reverse 从末尾向前遍历；i 从 size 倒数，--i 后即当前元素原始索引（slot）。
        // （libc++ 尚未提供 std::views::enumerate，故用 reverse + 倒数计数器。）
        usize i = locals_.size();
        for (const auto& local: std::views::reverse(locals_)) {
            --i;
            if (local.name == name) {
                return static_cast<u16>(i);
            }
        }
        return std::nullopt;
    }

    void FunctionCtx::begin_scope() { ++scope_depth_; }

    void FunctionCtx::end_scope() {
        // 弹出 depth > 新 scope_depth_ 的局部（不变式与 break/continue 例外见头文件 end_scope 注）。
        --scope_depth_;
        while (!locals_.empty() && locals_.back().depth > scope_depth_) {
            locals_.pop_back();
        }
    }

    // ============================================================
    // upvalue 登记
    // ============================================================

    Opt<u8> FunctionCtx::add_upvalue(const UpvalueDesc desc) {
        // 同 (is_local,index) 已登记 -> 复用其下标（同一局部被多处引用只占一个 upvalue，
        // 多引用点经同一 upvalue 索引读写同一槽）。
        for (usize i = 0; i < upvalues_.size(); ++i) {
            if (upvalues_[i] == desc) {
                return static_cast<u8>(i);
            }
        }
        // 容量判定 > kMaxUpvalues 的语义见 FunctionCtx.hpp kMaxUpvalues 注。
        if (upvalues_.size() > kMaxUpvalues) {
            return std::nullopt;
        }
        upvalues_.push_back(desc);
        return static_cast<u8>(upvalues_.size() - 1);
    }

} // namespace aria
