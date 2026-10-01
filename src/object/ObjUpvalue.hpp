#ifndef ARIA_OBJ_UPVALUE_HPP
#define ARIA_OBJ_UPVALUE_HPP

#include "object/Object.hpp"
#include "value/Value.hpp" // Value(成员 closed_ 与 location_ 指向的栈槽元素)

namespace aria {

    class GC;

    // Upvalue 对象:闭包对外层局部的「捕获即引用」载体(ObjType::UPVALUE)。open 态持指向值栈某槽的指针(外层后续修改对内
    // 层可见);帧退出时 close() 把值迁进 closed_ 自持。
    //   - location_:open 指入值栈(grow_stack_ 搬迁后经 set_location 重绑),closed 恒指 &closed_;两态统一经 value_slot()
    //     取读写槽,LOAD/STORE_UPVALUE 不分支。
    //   - next_open_:open upvalue 按槽址降序的侵入式开链(VM 持链头 open_upvalues_);同槽捕获经链查复用同一对象,close 摘
    //     链。地址哈希型、final、非拷贝/非移动(按身份共享,浅拷贝
    //     破坏开链不变式)。trace 标 *value_slot()(open 标栈槽内值 / closed 标 closed_;槽内 Value 可装箱任意对象)。
    class ObjUpvalue final : public Object {
    public:
        // slot = 被捕获的值栈槽地址(open 起点;恒非空,栈槽必存在)。
        explicit ObjUpvalue(Value* slot) noexcept :
            Object{ObjType::UPVALUE}, location_{slot}, closed_{Value::nil_val()}, next_open_{nullptr} {
            ASSERT(slot != nullptr, "upvalue slot must not be null");
        }
        ~ObjUpvalue() override = default; // 壳定长,无外挂子内存

        ObjUpvalue(const ObjUpvalue&)            = delete;
        ObjUpvalue& operator=(const ObjUpvalue&) = delete;
        ObjUpvalue(ObjUpvalue&&)                 = delete;
        ObjUpvalue& operator=(ObjUpvalue&&)      = delete;

        // open 态:location_ 尚指外部栈槽(close 后恒指 &closed_)。
        [[nodiscard]]
        bool is_open() const noexcept {
            return location_ != &closed_;
        }

        // 统一读写槽:open -> location_(栈槽),closed -> &closed_。LOAD/STORE_UPVALUE 走此取址。
        [[nodiscard]]
        Value* value_slot() noexcept {
            return location_;
        }

        [[nodiscard]]
        const Value* value_slot() const noexcept {
            return location_;
        }

        // 关闭:把栈槽值迁进 closed_,location_ 转指自身--此后原槽销毁/复用均不影响本 upvalue。
        void close() noexcept {
            closed_   = *location_;
            location_ = &closed_;
        }

        // 重绑栈槽(grow_stack_ 搬迁值栈后由 VM 调):仅对 open 态有意义,closed 态不持栈槽。
        void set_location(Value* slot) noexcept {
            ASSERT(is_open(), "upvalue is closed");
            ASSERT(slot != nullptr, "slot must not be null");
            location_ = slot;
        }

        // 开链访问器(链头在执行上下文,按槽址降序;遍历/插链/摘链由 ObjMovement 管)。
        [[nodiscard]]
        ObjUpvalue* next_open() const noexcept {
            return next_open_;
        }

        void set_next_open(ObjUpvalue* next) noexcept { next_open_ = next; }

        // 标 *value_slot()(open 标栈槽内值,closed 标 closed_--两态同一地址语义)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(无外挂子内存)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjUpvalue);
        }

        // 调试渲染:`<upvalue>` 稳定短文案;显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        Value*      location_;  // open:指入值栈的槽;closed:恒指 &closed_
        Value       closed_;    // close() 迁入的自持值(ctor 播 nil_val)
        ObjUpvalue* next_open_; // 开链下一节点(按槽址降序;链尾 nullptr;勿用 next_--撞基类 Object::next_)
    };

    // 工厂:分配 ObjUpvalue 并置 open 指向 slot。入参是裸栈槽地址,无对象可守;返回对象白色无根,
    // 调用方须立即链入 VM 开链(或入闭包 upvalues_,经 VM 根 tracer 标根)。
    [[nodiscard]]
    ObjUpvalue* new_upvalue(GC& gc, Value* slot);

} // namespace aria

#endif // ARIA_OBJ_UPVALUE_HPP
