#ifndef ARIA_OBJ_MOVEMENT_HPP
#define ARIA_OBJ_MOVEMENT_HPP

#include <format>

#include "common.hpp"
#include "object/Object.hpp"
#include "runtime/AriaExeContext.hpp"

namespace aria {

    class AriaVM;

    // GC 对象壳(Object 与执行上下文本体 AriaExeContext 多继承,唯一派生):给执行上下文以
    // GC 一等值身份,主上下文与协程统一本型;执行状态与机器全住基类,本类只有 Object 协议
    // override 与构造接线,trace 沿 previous_ 级联(链上挂起成员的唯一可达边)。
    class ObjMovement final : public Object, public AriaExeContext {
    public:
        explicit ObjMovement(GC* gc, AriaVM* host) noexcept : Object{ObjType::MOVEMENT}, AriaExeContext{gc, host} {}

        ~ObjMovement() override = default;

        ObjMovement(const ObjMovement&)            = delete;
        ObjMovement& operator=(const ObjMovement&) = delete;
        ObjMovement(ObjMovement&&)                 = delete;
        ObjMovement& operator=(ObjMovement&&)      = delete;

        // Object 协议(声明序随 Object.hpp)。

        // GC 标记:值栈已用区间/活动帧/开指链/挂起错误/resume 链(基类状态)级联;开指链是
        // 「闭包已死而 upvalue 仍在链」的悬垂防线。定义在 .cpp(基类转换需完整类型)。
        void trace(GC& gc) const noexcept override;

        // 壳字节数(不含值栈/帧数组等子内存:值栈由 ~AriaExeContext 经自持 GC* 释放,帧数组由
        // UPtr 自释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjMovement);
        }

        // 调试渲染:<coroutine 状态拼写>;aria:: 限定取自由函数重载(基类成员 to_string 会遮蔽)。
        [[nodiscard]]
        String debug_repr() const override {
            return std::format("<coroutine {}>", aria::to_string(state_));
        }
    };

    // 工厂:分配执行上下文并绑定宿主回指针。返回对象白色无根,调用方须自行根化(主上下文经
    // current_、协程经 create 原语发布)。构造临界区内(宿主 bootstrap 期)宿主服务面尚未就位,
    // 期间只建对象、不走 ctx 服务面。
    [[nodiscard]]
    ObjMovement* new_movement(GC& gc, AriaVM& host);

} // namespace aria

#endif // ARIA_OBJ_MOVEMENT_HPP
