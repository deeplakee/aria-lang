#ifndef ARIA_OBJ_MODULE_HPP
#define ARIA_OBJ_MODULE_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;
    class ObjFunction;

    // 模块对象:aria 的「模块 = 命名空间」(非类),一个源文件 = 一个模块。绝对路径(VM 模块表查重键)= dir_ + "/" + name_ +
    // ".aria",切分收口于 fs::module_name_and_dir。
    //   - name_:模块名(stem,intern),**指针恒非空**(内容可空 -- 合成顶层 <script> 等)。
    //   - dir_:目录(intern),相对导入基 + 播种 source_roots_[0];**指针恒非空**(缺省取 cwd,cwd 不可用以空串兜底 -- 空串
    //     让 resolve_module 跳空根,比拿 "." 碰运气诚实)。
    //   - entry_:模块体(run-once;保留不释放 -- 半初始化时常量池经 entry_ 仍可达,避免回收正在用的字面量)。可为 nullptr(
    //     目录包占位)。globals_:模块级绑定表(惰性分配)。
    //   - 加载事实源 = VM 模块表成员资格(对象无状态字段):编译成功才入表,循环导入命中表内体执行中的对象即复用。地址哈希
    //     型、final。trace 标 name_ + dir_ + entry_ + globals_。
    class ObjModule final : public Object {
    public:
        // name = 模块文件名去 .aria 后缀(stem);dir = 所在目录(指针恒非空,构造期 ASSERT)。
        ObjModule(GC& gc, ObjString* name, ObjString* dir);
        ~ObjModule() override = default; // globals_ 持 GC* 级联自释放;entry_/name_/dir_ 是 GC 对象,不归本类释放

        ObjModule(const ObjModule&)            = delete;
        ObjModule& operator=(const ObjModule&) = delete;
        ObjModule(ObjModule&&)                 = delete;
        ObjModule& operator=(ObjModule&&)      = delete;

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        // 模块文件所在目录(intern):合成绝对路径 + 相对导入基;恒非空、构造注入不可变(内容可空)。
        [[nodiscard]]
        ObjString* dir() const noexcept {
            return dir_;
        }

        // 绝对路径(= 模块表键形式)= dir_ + "/" + name_ + kAriaExtension,即时合成不驻留。
        // name_ 空串(合成顶层)仅返 dir_;dir_ 空串则返空串(供 resolve_module 判空拒绝)。
        [[nodiscard]]
        String abs_path() const;

        // 模块级绑定表:顶层 var/fun/def 的目标,LOAD/STORE/DEF_GLOBAL 操作此表(键 intern)。
        [[nodiscard]]
        AriaHashTable& globals() noexcept {
            return globals_;
        }

        [[nodiscard]]
        const AriaHashTable& globals() const noexcept {
            return globals_;
        }

        // 模块体(顶层语句编进的 ObjFunction,arity 0 匿名),导入时 run-once;nullptr 表无体。
        [[nodiscard]]
        ObjFunction* entry() const noexcept {
            return entry_;
        }

        void set_entry(ObjFunction* entry) noexcept { entry_ = entry; }

        // 命名成员读取协议 override:模块成员 = 模块全局绑定,查 globals_ 直读原值(纯查询,
        // GC-pure)。函数值为闭包、恒非方法,故不绑定 this(调用经 CALL_METHOD 时槽 0 留模块值,
        // 闭包不读之);nil 值绑定与「无此成员」由 find 的空态区分。miss 文案同基类默认形。
        [[nodiscard]]
        Opt<Value> load_field(AriaVM& vm, ObjString* name) override;

        // 命名成员写入:模块成员只读(定向文案)。越模块写会隐式创建未声明全局,违「赋值
        // 不隐式创建」;暴露可变状态走模块自己的函数。
        [[nodiscard]]
        bool store_field(AriaVM& vm, ObjString* name, Value value) override;

        // 标 name_ + dir_ + entry_ + globals_(key+value)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(globals_ 的 ctrl/entries 两块由 ~HashTable 自释放,不计入壳)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjModule);
        }

        // 调试渲染:`<module lib/utils>`;显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        // 模块文件名去 .aria 后缀(intern 驻留;指针恒非空,内容可空)
        ObjString* name_;
        // 模块文件所在目录(intern;指针恒非空,内容可空)
        ObjString*    dir_;
        ObjFunction*  entry_;   // 模块体(run-once;可为 nullptr)
        AriaHashTable globals_; // 模块级绑定表(惰性分配)
    };

    // 工厂:分配 ObjModule。守卫纪律见 Object.hpp,**调用方须自行根化 name 与 dir**;dir 指针须非空。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, ObjString* name, ObjString* dir);

    // 工厂重载(2 参):合成模块(<script> / REPL / 测试桩),dir 取 cwd(不可用空串兜底)。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, ObjString* name);

    // 工厂重载(StringView name):name 工厂内部驻留自守;dir 取 cwd。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, StringView name);

    // 工厂重载(StringView name, dir):两者均工厂内部驻留自守。dir 原样 intern(空串即空串锚点,
    // 不做 cwd 退化,需退化用 2 参 StringView 重载)。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, StringView name, StringView dir);

} // namespace aria

#endif // ARIA_OBJ_MODULE_HPP
