#include "compile/FunctionCtx.hpp"

#include <ranges>

#include "object/ObjFunction.hpp"

namespace aria {

    // ============================================================
    // 构造
    // ============================================================

    FunctionCtx::FunctionCtx(ObjFunction* fn) : enclosing_{nullptr}, fn_{fn}, scope_depth_{0} {
        auto this_ = Local{.name = String{}, .depth = 0, .is_captured = false, .is_initialized = true};
        locals_.push_back(std::move(this_)); // 哑元：slot 0 = callee
    }

    FunctionCtx::FunctionCtx(FunctionCtx& enclosing, ObjFunction* fn) :
        enclosing_{&enclosing}, fn_{fn}, scope_depth_{0} {
        auto this_ = Local{.name = String{}, .depth = 0, .is_captured = false, .is_initialized = true};
        locals_.push_back(std::move(this_)); // 哑元：slot 0 = callee
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
        // 同名局部必处不同作用域（is_defined_in_scope 禁同作用域重名），内层后声明故索引更高。
        // std::views::reverse 从末尾（内层）向前遍历；i 从 size 倒数，--i 后即当前元素的原始索引（slot）。
        // （std::views::enumerate 可一步配对索引但本环境 libc++ 尚未提供，故用 reverse + 倒数计数器。）
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
        // 退出当前作用域：--scope_depth_ 后弹出原 scope 的局部（depth > 新 scope_depth_；活局部按
        // depth 非递减序排列，故尾段即弹区；slot 0 哑元 depth=0 因 0 <= 任意 target_depth 恒在界外）。
        --scope_depth_;
        while (!locals_.empty() && locals_.back().depth > scope_depth_) {
            locals_.pop_back();
        }
    }

    // ============================================================
    // upvalue 登记
    // ============================================================

    Opt<u8> FunctionCtx::add_upvalue(const UpvalueDesc desc) {
        // 同 (is_local,index) 已登记 -> 复用其下标（同一局部被本函数多处引用只占一个 upvalue，
        // 「捕获即引用」的编译期对应：多引用点经同一 upvalue 索引读写同一槽）。
        for (usize i = 0; i < upvalues_.size(); ++i) {
            if (upvalues_[i] == desc) {
                return static_cast<u8>(i);
            }
        }
        // 容量检查（kMaxUpvalues = u8 索引域上限位置，见 FunctionCtx.hpp；越界判定与语义常量家族
        // 统一用 > 比较）：size > kMaxUpvalues 即 256 条已满（索引 0..255 全占用），新条目的索引
        // 将越出 u8 域 -- 返 nullopt 交 CodeGen fail(TooManyUpvalues) 翻译。
        if (upvalues_.size() > kMaxUpvalues) {
            return std::nullopt;
        }
        upvalues_.push_back(desc);
        return static_cast<u8>(upvalues_.size() - 1);
    }

} // namespace aria
