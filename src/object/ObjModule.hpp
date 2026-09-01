#ifndef ARIA_OBJ_MODULE_HPP
#define ARIA_OBJ_MODULE_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;
    class ObjFunction;

    // 模块对象:aria 的「模块 = 命名空间」(非类)。一个源文件 = 一个模块。
    //
    //   - name_:相对源根的路径(intern 驻留,同指针),如 lib/utils。既作显示名(to_string /
    //     报错渲染),又与 root_ 一起合成模块的绝对路径(见下);不单独参与模块表查重。
    //   - root_:模块所属的源根目录(intern 驻留,同指针),如 /proj。模块的绝对规范路径由
    //     root_ + name_ 合成:文件路径(= VM 模块表查重键)= root_ + "/" + name_ + ".aria";
    //     相对导入基目录 = root_ + "/" + dirname(name_);run() 把入口模块的 root_ 播种为
    //     source_roots_[0]。加载层落地后由 loader 在 resolve 时定 root_(命中哪个源根即哪个);
    //     **指针恒非空**(构造期 ASSERT):new_module 未显式传 root 时取当前工作目录作默认。
    //     cwd 不可用时以**空串**兜底(不 fatal:罕见、绝对源根 [1..] 仍可用、嵌入不应因 cwd 异常
    //     拖死宿主);空串经下游空值守卫干净拒绝 cwd 锚定(resolve_module 跳空根、abs_path 返空
    //     -> 相对解析返 nullopt),比拿 "." 碰运气更诚实。
    //   - entry_:模块体(顶层语句编进的 ObjFunction,arity 0、匿名;主入口名 `<main>` / 导入名 `<module>`),
    //     导入时 run-once。
    //     保留不释放:trace 经它 reach 常量池;半初始化时 globals 未填满,常量池经 entry_ 仍可达,
    //     避免回收正在用的字面量。可为 nullptr(未来目录包占位;当前总有体)。
    //   - globals_:模块级绑定表(顶层 var/fun/def 的目标)。键为 ObjString*(经 intern,内容语义
    //     靠 === 同指针),值为绑定 Value。LOAD/STORE/DEF_GLOBAL 操作此表。惰性分配(首次 upsert 才建表)。
    //   - state_:导入状态机(Loading/Loaded),循环导入检测:命中 Loading 返回半初始化对象。
    //     未入 VM 模块表 = 未加载(隐含第三态,不入枚举)。
    //
    //   地址哈希型可变对象(走 Object{ObjType::MODULE} ctor);equals 保持默认地址相等--
    //     模块按身份判等(模块表保证同路径同对象,无内容相等语义)。final,不再派生。
    //   trace():标 name_ + root_ + entry_ + 委托 globals_.trace(gc)(遍历占用槽 mark_value key+value)。
    class ObjModule final : public Object {
    public:
        // 导入状态机。模块插入 VM 模块表时即置 Loading;模块体跑完置 Loaded。
        // 未入表 = 未加载(隐含,不入此枚举,避免与表查重逻辑耦合)。
        enum class ModuleState : u8 {
            Loading, // 模块体正在执行(初始化中);循环导入命中此态返回半初始化对象
            Loaded,  // 模块体执行完毕,globals_ 完整
        };

        // name = 相对源根的路径(显示名 + 合成绝对路径用);root = 所属源根目录(恒非空,见 new_module,构造期 ASSERT)。
        ObjModule(GC& gc, ObjString* name, ObjString* root);
        ~ObjModule() override = default; // globals_ 持 GC* 级联自释放;entry_/name_/root_ 是 GC 对象,不归本类释放

        ObjModule(const ObjModule&)            = delete;
        ObjModule& operator=(const ObjModule&) = delete;
        ObjModule(ObjModule&&)                 = delete;
        ObjModule& operator=(ObjModule&&)      = delete;

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        // 所属源根目录(intern):与 name_ 合成模块绝对路径(模块表键 + 相对导入基);指针恒非空
        // (构造期 ASSERT),内容可空(cwd 不可用时 new_module 以空串兜底,见类注释)。构造注入、
        // 不可变:加载器在 resolve 时定 root_(命中哪个源根即哪个),再 new_module(name, root) 建模块。故无 setter。
        [[nodiscard]]
        ObjString* root() const noexcept {
            return root_;
        }

        // 模块的绝对文件路径(= VM 模块表查重键形式)= root_ + "/" + name_ + ".aria"。
        // name_ 为 nullptr 或空串(合成顶层)则仅返 root_(无文件名);root_ 内容**空串**(cwd 不可用时
        // new_module 2 参重载兜底)则返空串 -- 供 resolve_module 相对分支判空直接返 nullopt。root_ 指针
        // 恒非空(构造期 ASSERT,无合法指针空态),故不判 nullptr。供 IMPORT 取当前模块目录
        // (dirname(abs_path) = 当前模块所在目录)、加载层入表键等用。返回 String(即时合成,不驻留)。
        [[nodiscard]]
        String abs_path() const;

        // 模块级绑定表:顶层 var/fun/def 的目标。LOAD/STORE/DEF_GLOBAL 操作此表。
        // 键为 ObjString*(intern,内容语义经 === 同指针),值为绑定 Value。
        [[nodiscard]]
        AriaHashTable& globals() noexcept {
            return globals_;
        }

        [[nodiscard]]
        const AriaHashTable& globals() const noexcept {
            return globals_;
        }

        // 模块体(顶层语句编进的 ObjFunction,arity 0、匿名)。导入时 run-once。
        // 编译完成后由 VM 经 set_entry 挂入;nullptr 表无体(未来目录包占位)。
        [[nodiscard]]
        ObjFunction* entry() const noexcept {
            return entry_;
        }

        void set_entry(ObjFunction* entry) noexcept { entry_ = entry; }

        [[nodiscard]]
        ModuleState state() const noexcept {
            return state_;
        }

        void set_state(ModuleState s) noexcept { state_ = s; }

        // 标 name_ + root_ + entry_ + globals_(key+value)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(globals_ 的 ctrl/entries 两块由 ~HashTable 自释放,不计入壳)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjModule);
        }

        // 可读描述:`<module lib/utils>`;name_ 为 nullptr 防御性渲染 `<module>`。
        // override Object::to_string 默认的 `<Module at 0x...>`。
        [[nodiscard]]
        String to_string() const override;

    private:
        ObjString*    name_;    // 相对源根的路径(intern 驻留;显示名 + 合成绝对路径用)
        ObjString*    root_;    // 所属源根目录(intern;合成绝对路径 + 相对导入基 + run() 播种源根;指针恒非空,内容可空)
        ObjFunction*  entry_;   // 模块体(run-once;可为 nullptr)
        AriaHashTable globals_; // 模块级绑定表(惰性分配)
        ModuleState   state_;   // Loading / Loaded
    };

    // 工厂:分配 ObjModule 并置 Loading 态。root 为显式源根目录(指针须非空,构造期 ASSERT;内容
    //        可空 -- 调用方若需「无源根 -> cwd 退化」语义用 2 参重载)。工厂不替调用方守卫入参--本重载
    //        只做一次 new_object、无内部新建对象,故**调用方须在调用前自行根化 name 与 root**(跨
    //        new_object 顶 maybe_collect;intern 驻留池是 weak root,不保命),与 new_function 同理。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, ObjString* name, ObjString* root);

    // 工厂重载(2 参):无显式源根的合成模块(<script> / REPL / 测试桩)。root 取当前工作目录 -- cwd 不可用
    //        时以空串兜底(不 fatal:罕见、绝对源根 [1..] 仍可用、嵌入不应因 cwd 异常拖死宿主);空串经下游
    //        空值守卫干净拒绝 cwd 锚定(resolve_module 跳空根、abs_path 返空 -> 相对解析返 nullopt),比拿
    //        "." 碰运气更诚实。cwd 串经 new_string intern 驻留。委托 3 参重载建对象。
    //        根化契约:name 是调用方入参,**调用方须根化**(下方 new_string(cwd) 与 new_object 均 GC);
    //        root_str 是本函数内部新建,工厂自行守卫跨 new_object(「每方守自己创建的」)。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, ObjString* name);

    // 工厂重载(2 参, StringView name):name 以 StringView 传入,工厂内部 new_string 驻留并自行守卫
    //        (工厂守「自己创建的」),调用方无需手动建串根化。root 取 cwd(不可用空串兜底),委托
    //        new_module(GC&, ObjString*) 重载。调用方无需根化任何入参,传 StringView 即可。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, StringView name);

    // 工厂重载(3 参, StringView name + StringView root):name 与 root 均以 StringView 传入,工厂内部
    //        new_string 驻留并自行守卫(工厂守「自己创建的」),委托 new_module(GC&, ObjString*, ObjString*)
    //        重载建对象。root 串内容可空(调用方需「无源根 -> cwd 退化」语义用上面 2 参 StringView 重载,
    //        本重载原样 intern 调用方给的 root 串,空串即空串锚点)。调用方无需根化任何入参,传 StringView 即可。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, StringView name, StringView root);

} // namespace aria

#endif // ARIA_OBJ_MODULE_HPP
