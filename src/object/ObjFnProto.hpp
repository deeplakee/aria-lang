#ifndef ARIA_OBJ_FN_PROTO_HPP
#define ARIA_OBJ_FN_PROTO_HPP

#include "bytecode/CodeUnit.hpp"
#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "object/Object.hpp"

namespace aria {

    class ObjString;
    class ObjModule;

    struct UpvalueDesc {
        bool is_local; // true:捕获直接外围帧的局部槽 index;false:穿透复用外围闭包的第 index 个 upvalue
        u16  index;    // 局部槽号或外围闭包 upvalue 下标(与 LOAD_LOCAL 的 slot:u16 同域)

        bool operator==(const UpvalueDesc&) const = default;
    };

    // 函数原型存于常量池，由 ObjFunction 包装为运行期函数值。
    // arity 不含 rest 槽；无 varargs 时实参数须落在 [min_arity, arity]。
    class ObjFnProto final : public Object {
    public:
        ObjFnProto(GC& gc, ObjModule* module, ObjString* name, u8 arity, u8 min_arity, bool is_varargs);
        ~ObjFnProto() override = default; // CodeUnit / upvalue_descs_ 级联自释放,无额外子内存

        [[nodiscard]]
        CodeUnit& unit() noexcept {
            return unit_;
        }

        [[nodiscard]]
        const CodeUnit& unit() const noexcept {
            return unit_;
        }

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        [[nodiscard]]
        u8 arity() const noexcept {
            return arity_;
        }

        [[nodiscard]]
        u8 min_arity() const noexcept {
            return min_arity_;
        }

        [[nodiscard]]
        bool is_varargs() const noexcept {
            return is_varargs_;
        }

        [[nodiscard]]
        ObjModule* module() const noexcept {
            return module_;
        }

        [[nodiscard]]
        Array<UpvalueDesc>& upvalue_descs() noexcept {
            return upvalue_descs_;
        }

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjFnProto);
        }

        [[nodiscard]]
        String repr() const override;

    private:
        CodeUnit           unit_;
        ObjModule*         module_; // 所属模块(恒非空)
        ObjString*         name_;
        u8                 arity_;
        u8                 min_arity_;
        bool               is_varargs_;    // varargs 函数(...rest;帧槽深 = arity + 1)
        Array<UpvalueDesc> upvalue_descs_; // 捕获描述表(编译期 flush,运行期只读)
    };

    // 调用方须根化 module 和 name；StringView 重载内部根化新建名称。
    [[nodiscard]]
    ObjFnProto* new_fn_proto(GC& gc, ObjModule* module, ObjString* name, u8 arity, u8 min_arity, bool is_varargs);

    [[nodiscard]]
    ObjFnProto* new_fn_proto(GC& gc, ObjModule* module, StringView name, u8 arity, u8 min_arity, bool is_varargs);

} // namespace aria

#endif // ARIA_OBJ_FN_PROTO_HPP
