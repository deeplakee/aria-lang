#ifndef ARIA_OBJ_FUNCTION_HPP
#define ARIA_OBJ_FUNCTION_HPP

#include "bytecode/CodeUnit.hpp"
#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp" // Array<UpvalueDesc> 成员以 GC 为分配器,实例化点须 GC 完整(仓库约定:具体类自 include,不经容器传递)
#include "object/Object.hpp"

namespace aria {

    class ObjString;
    class ObjModule;

    // 捕获描述(编译期填好,存 ObjFunction 元数据、不进字节码流;指令集 §4.13):
    // VM 执行 CLOSURE 指令时遍历本表逐个建/复用 ObjUpvalue 填 ObjClosure::upvalues_。
    struct UpvalueDesc {
        bool is_local; // true:捕获直接外围帧的局部槽 index;false:穿透复用外围闭包的第 index 个 upvalue
        u16  index;    // 局部槽号或外围闭包 upvalue 下标(与 LOAD_LOCAL_L 的 slot:u16 同域)
    };

    // 函数对象:持一个 CodeUnit(字节码容器,值成员)+ 所属模块 + 函数名 + 参数个数(arity)。
    //
    //   - unit_:CodeUnit 值成员(非 Object,见 CodeUnit 注释)。编译期由字节码编译器
    //     经 unit() 直接操作裸字段 emit;运行期 VM 读 code/constants/try_records。
    //     其内部 Array 持 GC* 自释放,~ObjFunction -> ~CodeUnit 级联释放。
    //   - module_:所属模块(词法归属,构造时传入、不可变、非空)。函数必然定义在某个模块内
    //     (模块体与其内嵌套函数同属一模块;入口脚本本身也是一个模块)。VM 据此定位「当前模块
    //     globals」(LOAD/STORE/DEF_GLOBAL 查 frame.module->globals(),module_ 经 CallFrame.module
    //     缓存)。ctor 断言非空,杜绝「无模块函数」。
    //   - name_:ObjString*(经 intern 驻留,同名同指针;恒非空)。模块入口函数名 `<main>`(主入口)/`<module>`(导入)、
    //     lambda 名 `<anonymous>`(`<>` 是正常标识符中不可用的符号,具独特辨识度);具名函数为
    //     其声明名。统一模型:每个函数都有名字,ctor ASSERT 非空。to_string 渲染 `<fn name>`。
    //   - arity_:参数个数(u8,上限 255;编译期编译器保证不越界)。
    //   - upvalue_descs_:捕获描述表(M4 闭包,编译期一次性 flush,运行期只读)。每条 UpvalueDesc
    //     {is_local, index} 描述本函数的一个捕获:is_local=true 捕直接外围帧局部槽 index,
    //     false 穿透复用外围闭包的第 index 个 upvalue。存元数据、不进字节码流,CLOSURE 保持
    //     定长 3B(ConstU16);与 ObjClosure::upvalues_ 按下标一一对应。描述项纯标量,trace 不标。
    //
    //   地址哈希型可变对象(走 Object{Kind} ctor);equals 保持默认地址相等--
    //     函数无"内容相等"语义(同名函数体可不同)。
    //   trace():标 name_ + module_ + 委托 unit_.trace(常量池中的 Value,code/lines 无子节点)。
    //     module_ 回指形成 module <-> entry 环,mark-sweep 三色标记天然破环,无 double-free
    //     (两者皆 GC 对象,各自由 sweep 整体回收,~ObjFunction 不释放 module_)。
    class ObjFunction final : public Object {
    public:
        ObjFunction(GC& gc, ObjModule* module, ObjString* name, u8 arity);
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

        // 所属模块(词法归属,构造时确定、不可变、非空),供 VM 定位模块 globals。
        [[nodiscard]]
        ObjModule* module() const noexcept {
            return module_;
        }

        // 捕获描述表(非 const:编译期 CodeGen 一次性 flush;const:VM 执行 CLOSURE 时读)。
        [[nodiscard]]
        Array<UpvalueDesc>& upvalue_descs() noexcept {
            return upvalue_descs_;
        }

        [[nodiscard]]
        const Array<UpvalueDesc>& upvalue_descs() const noexcept {
            return upvalue_descs_;
        }

        // 标 name_ + module_ + 常量池(code/lines 无 Value 子节点;upvalue_descs_ 纯标量不标)。
        // module_ 非空;mark_object 容 nullptr 仅防御。
        void trace(GC& gc) const noexcept override;

        // 壳定长(CodeUnit / upvalue_descs_ 内部 Array 自管理,级联释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjFunction);
        }

        // 可读描述:`<fn add>`(clox 风格);name_ 恒非空,统一 `<fn name>`(入口渲染 `<fn <main>>`/`<fn <module>>`、
        // lambda 渲染 `<fn <anonymous>>`)。override Object::to_string 默认的 `<Function at 0x...>`。
        [[nodiscard]]
        String to_string() const override;

    private:
        CodeUnit           unit_;
        ObjModule*         module_; // 所属模块(非空,构造时传入)
        ObjString*         name_;
        u8                 arity_;
        Array<UpvalueDesc> upvalue_descs_; // 捕获描述表(编译期 flush,运行期只读)
    };

    // 工厂:分配 ObjFunction 并初始化空 CodeUnit。工厂不替调用方守卫入参--module 与 name 经
    //        intern/模块表皆是 weak root,new_object 顶部 maybe_collect 可能回收未被根持有的两者,
    //        但工厂只做一次 new_object、无内部新建对象,故**调用方须在调用前自行根化 module 与 name**
    //        (跨 new_object)。即「每方只守卫自己创建的对象」:工厂不创建入参,不守卫。
    [[nodiscard]]
    ObjFunction* new_function(GC& gc, ObjModule* module, ObjString* name, u8 arity);

} // namespace aria

#endif // ARIA_OBJ_FUNCTION_HPP
