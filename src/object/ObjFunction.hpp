#ifndef ARIA_OBJ_FUNCTION_HPP
#define ARIA_OBJ_FUNCTION_HPP

#include "bytecode/CodeUnit.hpp"
#include "common.hpp"
#include "memory/Array.hpp"
// Array<UpvalueDesc> 成员以 GC 为分配器,实例化点须 GC 完整(仓库约定:具体类自 include,不经容器传递)
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

    // 函数对象:字节码(CodeUnit)+ 所属模块 + 名 + 元数;地址哈希型。
    // arity_ 不含 varargs 槽(帧槽深 = arity + 1);调用时实参数必须落在 [min_arity, arity] 区间。
    class ObjFunction final : public Object {
    public:
        ObjFunction(GC& gc, ObjModule* module, ObjString* name, u8 arity, u8 min_arity, bool is_varargs);
        ~ObjFunction() override = default; // CodeUnit / upvalue_descs_ 级联自释放,无额外子内存

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

        // 必传参数数(<= arity,差额为带默认值参数);call_closure 区间检查下界。
        [[nodiscard]]
        u8 min_arity() const noexcept {
            return min_arity_;
        }

        // varargs 函数标志(末位参数为 ...rest;帧槽深 = arity + 1)。
        [[nodiscard]]
        bool is_varargs() const noexcept {
            return is_varargs_;
        }

        // 所属模块(不可变非空),供 VM 定位模块 globals。
        [[nodiscard]]
        ObjModule* module() const noexcept {
            return module_;
        }

        // 捕获描述表(编译期 CodeGen 一次性 flush;VM 执行 CLOSURE 时读)。
        [[nodiscard]]
        Array<UpvalueDesc>& upvalue_descs() noexcept {
            return upvalue_descs_;
        }

        // 标 name_ + module_ + 常量池(code/lines 无 Value 子节点);module_ 非空,mark_object 容 nullptr 仅防御。
        void trace(GC& gc) const noexcept override;

        // 壳定长(CodeUnit / upvalue_descs_ 内部 Array 自管理,级联释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjFunction);
        }

        // 调试渲染:`<fn add>`(入口 `<fn <main>>`、lambda `<fn <anonymous>>`);显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        CodeUnit           unit_;
        ObjModule*         module_; // 所属模块(恒非空)
        ObjString*         name_;
        u8                 arity_;
        u8                 min_arity_;
        bool               is_varargs_;    // varargs 函数(...rest;帧槽深 = arity + 1)
        Array<UpvalueDesc> upvalue_descs_; // 捕获描述表(编译期 flush,运行期只读)
    };

    // 工厂:分配 ObjFunction。module 与 name 皆 weak root,调用方须自行根化;StringView 重载内部
    // intern name 并自守,调用方只需根化 module。
    [[nodiscard]]
    ObjFunction* new_function(GC& gc, ObjModule* module, ObjString* name, u8 arity, u8 min_arity, bool is_varargs);

    [[nodiscard]]
    ObjFunction* new_function(GC& gc, ObjModule* module, StringView name, u8 arity, u8 min_arity, bool is_varargs);

} // namespace aria

#endif // ARIA_OBJ_FUNCTION_HPP
