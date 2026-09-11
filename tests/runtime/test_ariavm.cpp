#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/code.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"
#include "value/AriaHashTable.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::CodeUnit;
using aria::ErrorCode;
using aria::GC;
using aria::i8;
using aria::NativeFn;
using aria::new_module;
using aria::new_native_fn;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjClass;
using aria::ObjClosure;
using aria::ObjFunction;
using aria::ObjInstance;
using aria::ObjModule;
using aria::ObjString;
using aria::ObjUpvalue;
using aria::OpCode;
using aria::Span;
using aria::StringView;
using aria::TryRecord;
using aria::u16;
using aria::u32;
using aria::u8;
using aria::UpvalueDesc;
using aria::usize;
using aria::Value;

namespace {

    // LOAD_IMM 的 i8 立即数(经 emit_byte 写入)。
    void emit_imm(CodeUnit& cu, i8 v, u32 line = 1) {
        cu.emit_op(OpCode::LOAD_IMM, line);
        cu.emit_byte(static_cast<u8>(v), line);
    }

    // 局部槽的 u8 短操作数指令(LOAD_LOCAL/STORE_LOCAL)。
    void emit_local(CodeUnit& cu, OpCode op, u8 slot, u32 line = 1) {
        cu.emit_op(op, line);
        cu.emit_byte(slot, line);
    }

    // 回填一条 u16 跳转偏移(编译器 backpatch 的手写版;偏移以读完操作数后 ip 为基准)。
    void patch_word(CodeUnit& cu, usize operand_offset, u16 word) {
        cu.code[operand_offset]     = static_cast<u8>(word & 0xFF);
        cu.code[operand_offset + 1] = static_cast<u8>(word >> 8);
    }

    // u16 名字操作数指令(DEF/LOAD/STORE_GLOBAL):op + u16 常量池索引(名字 ObjString)。
    void emit_global(CodeUnit& cu, OpCode op, u16 name_idx, u32 line = 1) {
        cu.emit_op(op, line);
        cu.emit_word(name_idx, line);
    }

    // u8 upvalue 索引指令(LOAD_UPVALUE/STORE_UPVALUE)。
    void emit_upvalue(CodeUnit& cu, OpCode op, u8 idx, u32 line = 1) {
        cu.emit_op(op, line);
        cu.emit_byte(idx, line);
    }

    // CLOSURE fn:u16(常量池 ObjFunction 索引):VM 按 fn->upvalue_descs() 建捕获,压闭包值。
    void emit_closure(CodeUnit& cu, u16 fn_idx, u32 line = 1) {
        cu.emit_op(OpCode::CLOSURE, line);
        cu.emit_word(fn_idx, line);
    }

    // u16 名字操作数的类组/字段组指令(MAKE_CLASS/MAKE_METHOD/MAKE_STATIC/LOAD_FIELD/
    // STORE_FIELD/LOAD_THIS_FIELD/STORE_THIS_FIELD/LOAD_SUPER_FIELD):op + 常量池
    // ObjString 索引,与 emit_global 同形(DEF/LOAD/STORE_GLOBAL 用)。
    void emit_named(CodeUnit& cu, OpCode op, u16 name_idx, u32 line = 1) {
        cu.emit_op(op, line);
        cu.emit_word(name_idx, line);
    }

    // LOAD_CONST idx:u16(压常量池 idx 处的值)。
    void emit_const(CodeUnit& cu, u16 idx, u32 line = 1) {
        cu.emit_op(OpCode::LOAD_CONST, line);
        cu.emit_word(idx, line);
    }

    // 给 fn 追加一条捕获描述:is_local=true 捕直接外围帧局部槽 index(测试只用到此形态)。
    void capture_local(ObjFunction* fn, u16 index) { fn->upvalue_descs().push(UpvalueDesc{true, index}); }

    // IMPORT path:u16(常量池 ObjString 索引)。压模块值于栈顶（[...] -> [..., module]）；绑定由
    // 调用方按作用域经 DEF_GLOBAL / 值填槽自行完成。
    void emit_import(CodeUnit& cu, u16 path_idx, u32 line = 1) {
        cu.emit_op(OpCode::IMPORT, line);
        cu.emit_word(path_idx, line);
    }

    // 测试便利:intern + 守卫 name,再调 new_module(2-arg)。工厂不再替调用方守卫入参,故本助手显式
    // 守卫 name 跨 new_module 内部 new_string(cwd)/new_object。返回的 m 未根,调用方跨 GC 点持有
    // m 须自行再守卫。默认 "<script>"(M1 机制测试不关心模块归属,临时模块;dir_ 走 cwd,run() 替换
    // source_roots_[0])。需要真实模块归属用 aria::new_function 显式传模块。
    ObjModule* make_module(GC& gc, StringView name = "<script>") {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm);
        return new_module(gc, nm); // dir 缺省 -> cwd(失败时空串兜底)
    }

    // 显式目录版:intern + 守卫 name,先守 dir 再 new_string(name),调 new_module(3-arg)。
    ObjModule* make_module(GC& gc, StringView name, ObjString* dir) {
        auto guard = gc.make_guard(dir); // dir 先入根:下方 new_string(name) 可能 collect
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return new_module(gc, nm, dir);
    }

    // 指定共享模块的具名函数:M4 闭包测试用(DEF_GLOBAL/LOAD_GLOBAL 跨函数共享同模块 globals,
    // 各测试函数不能再走临时模块的 new_function 便利重载)。守 m 与 name 后调 4 参
    // aria::new_function;返回白色,调用方自守。
    ObjFunction* make_function(GC& gc, ObjModule* m, StringView name, u8 arity) {
        auto guard = gc.make_guard(m);
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return aria::new_function(gc, m, nm, arity);
    }

    // 3 参便利重载:造临时模块 + 委托 4 参 aria::new_function。屏蔽全局 aria::new_function。
    // 须先保 name 再 make_module -- make_module 内部分配在 stress GC 下会 collect,此时 name 仅
    // 为裸局部指针(无根)会被扫掉(aria::new_function 不再自守卫入参,故本重载全程自守 name+m)。
    // name=nullptr -> `<main>`(主入口单元统一名,ObjFunction ctor ASSERT name 非空)。
    ObjFunction* new_function(GC& gc, ObjString* name, u8 arity) {
        if (name == nullptr) {
            name = new_string(gc, "<main>");
        }
        auto guard = gc.make_guard(name);
        auto m     = make_module(gc);
        guard.push(m);
        return aria::new_function(gc, m, name, arity);
    }

    // 指定模块的匿名入口单元(`<main>` 名,arity 0)。工厂不再守卫入参,故先 guard m 再 new_string,
    // 再 push name -- 避免 new_string 与 new_function 内 new_object 回收未根持有的 m 与 name。
    ObjFunction* new_script(GC& gc, ObjModule* m) {
        auto guard = gc.make_guard(m);
        auto name  = new_string(gc, "<main>");
        guard.push(name);
        return aria::new_function(gc, m, name, 0);
    }

    // ---- IMPORT 路径解析测试辅助(全量磁盘版:exists-check + 绝对规范键)----
    //
    // 设计(见 .claude/reference/runtime/import-path-resolution.md):IMPORT 把 specifier 经 resolve_module 解析为
    // 命中文件的绝对规范路径(weakly_canonical)作模块表键。ObjModule 持 (dir_, name_):
    //   - dir_ = 模块文件所在目录(如 base);name_ = 文件名去 .aria 后缀(stem)。
    //   - 模块绝对路径(= 模块表键)由 dir_ + name_ 合成:dir_ + "/" + name_ + ".aria"。
    //   - run() 把入口模块 dir_ 播种为 source_roots_[0];相对导入基 = dirname(abs_path) = dir_。
    // 故测试需:
    //   1. 用真实临时文件让 resolve_module 的 exists-check 命中(testing::TempDir 下建空 .aria);
    //   2. 按解析出的绝对键预注册合成模块入 modules_;
    //   3. 给入口/目标模块设 dir_(所在目录 base)+ name_(文件名 stem)。
    // macOS 下 testing::TempDir() 常落在 /var/... -> /private/var/... 符号链接后,而
    // resolve_module 经 weakly_canonical 解析符号链接,故基准须先规范化,保证预注册键与
    // resolve_module 输出逐字节一致。

    // 取本测试专用的规范绝对路径(解析符号链接):在 testing::TempDir() 下按「套件名_用例名」
    // 建独立子目录。gtest 的 TempDir() 是整个测试程序共享、跨运行不清理的同一目录,若各测试
    // 直接在其下建同路径文件(如 lib/math.aria),残留文件会污染依赖「该文件不存在」的用例
    // (典型:ImportBareSearchesSourceRoots 需入口根无 lib/math.aria,但其它用例会建同名文件)。
    // 按用例名隔离后,每测试独占一目录,互不污染;同测试重跑时自建文件幂等覆盖。
    std::string test_canon_dir() {
        auto info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto dir  = std::filesystem::weakly_canonical(std::filesystem::path{testing::TempDir()} /
                                                      (std::string{info->test_suite_name()} + "_" + info->name()));
        std::filesystem::create_directories(dir);
        return dir.string();
    }

    // 在 base 下按相对路径 rel(含 .aria 后缀)创建空文件(含父目录),返回该文件的规范绝对路径
    // -- 即 IMPORT 解析应命中的模块表键(与 resolve_module 的 weakly_canonical 输出逐字节一致)。
    std::string touch_aria(const std::string& base, std::string_view rel) {
        auto file = std::filesystem::weakly_canonical(std::filesystem::path{base} / std::string{rel});
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out{file.string()}; // 创建空文件(out 析构时关闭)
        (void) out;
        return file.string();
    }

    // 在 base 下按相对路径 rel 写入源码内容 content(含父目录),返回该文件的规范绝对路径。
    // 供 IMPORT 加载层端到端测试:被导入模块有真实源码,触发 读盘 -> 编译 -> run-once 全链路。
    std::string write_aria(const std::string& base, std::string_view rel, std::string_view content) {
        auto file = std::filesystem::weakly_canonical(std::filesystem::path{base} / std::string{rel});
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out{file.string()};
        out << content;
        return file.string();
    }

    // 在 VM 模块表 modules 里按模块显示名(name_->view())查找模块对象;未命中返 nullptr。
    // 加载层测试经 interpret_from_path 跑完后,用此白盒检视被导入模块的 state / globals。
    ObjModule* find_module_by_name(aria::AriaHashTable& modules, StringView name) {
        ObjModule* found = nullptr;
        modules.for_each_occupied([&](const Value& /*key*/, const Value& val) {
            auto m = aria::Object::as<ObjModule>(val.as_obj());
            if (m->name() != nullptr && m->name()->view() == name) {
                found = m;
            }
        });
        return found;
    }

    // 造带目录的模块(name + dir):name = 文件名 stem,dir = 模块文件所在目录(intern 的 ObjString*,非空)。
    // 妥善处理临时根:工厂不再替调用方守卫入参,故 name 先 intern 再 guard,dir 亦 guard(分配 new_object
    // 顶部的 maybe_collect 可能回收未被根持有的串)。调用方须先 guard 已创建的 dir(本函数内
    // new_string(name) 分配时 dir 须已入根)。new_module 本身对 nullptr dir 会默认 cwd,但本
    // 辅助的用例都需精确控制目录,故一律显式传 dir。
    ObjModule* new_disk_module(GC& gc, std::string_view name, ObjString* dir) {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm);
        guard.push(dir);
        return new_module(gc, nm, dir);
    }

    // ---- 原生函数测试辅助 ----

    // double(x):把 x*2 写入槽 0(就地返回)。演示「读 slots[1..]、写 slots[0]」契约。
    // 元数自查(slots.size()-1 == argc);不符经 vm.fail 侧信道报错(一行 `return vm.fail(...)`)。
    bool double_native(AriaVM& vm, Span<Value> slots) {
        const auto argc = slots.size() - 1;
        if (argc != 1) {
            return vm.fail(ErrorCode::WrongArity, "double expects 1 arg, got {}", argc);
        }
        slots[0] = Value::from_int(slots[1].as_int() * 2);
        return true;
    }

    // fail_always():恒报错,验证侧信道错误从 run() 出栈为未捕获 Error。
    bool fail_always_native(AriaVM& vm, Span<Value> /*slots*/) {
        return vm.fail(ErrorCode::TypeMismatch, "fail_always always fails");
    }

    // 把 native 包成 Value 入常量池,返回常量池索引(emit LOAD_CONST 用)。
    u16 add_native_const(CodeUnit& cu, GC& gc, const char* name, NativeFn fn) {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm); // name 是 weak root,new_native_fn 顶 maybe_collect 前先保
        auto nf    = new_native_fn(gc, nm, fn);
        return cu.add_constant(Value::from_obj(nf));
    }

} // namespace

// stress GC 默认开：ariavm 集成测试的 VM 都开 stress，主动锻炼 run() 期值栈/帧根接线与
// IMPORT/全局/native 的分配安全，暴露「持裸指针跨分配」缺失根的 bug。需要关闭 GC 的用例
// （显式控制 GC 语义）另用裸 `AriaVM vm;` + 自行管理。fn/module 等跨分配持有的裸指针须 make_guard 根化。
class AriaVMStress : public ::testing::Test {
protected:
    AriaVM vm;

    void SetUp() override { vm.gc().set_stress(true); }
};

TEST_F(AriaVMStress, Arithmetic) {

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    emit_imm(cu, 1);                 // [1]
    emit_imm(cu, 2);                 // [1, 2]
    emit_imm(cu, 3);                 // [1, 2, 3]
    cu.emit_op(OpCode::MULTIPLY, 1); // [1, 6]
    cu.emit_op(OpCode::ADD, 1);      // [7]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out.value().is_int());
    EXPECT_EQ(out.value().as_int(), 7);
}

TEST_F(AriaVMStress, F64ConstantAndPromotion) {

    auto&      gc       = vm.gc();
    auto       fn       = new_function(gc, nullptr, 0);
    auto       fn_guard = gc.make_guard(fn);
    auto&      cu       = fn->unit();
    const auto c25      = cu.add_constant(Value::from_f64(2.5));
    const auto c05      = cu.add_constant(Value::from_f64(0.5));
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(c25, 1);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(c05, 1);
    cu.emit_op(OpCode::ADD, 1); // 3.0
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out.value().is_f64());
    EXPECT_DOUBLE_EQ(out.value().as_f64(), 3.0);
}

TEST_F(AriaVMStress, WhileLoopWithJumps) {
    // slot1 = i(3 递减), slot2 = acc;while i > 0 { acc += i; i -= 1 } 返回 acc = 3+2+1 = 6
    // 序言的两次 LOAD_NIL 预留局部区(slots[1..3)):槽 0 是 callee,临时值在保留区之上压栈不覆写局部。

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    cu.emit_op(OpCode::LOAD_NIL, 1);
    cu.emit_op(OpCode::LOAD_NIL, 1);
    emit_imm(cu, 3);
    emit_local(cu, OpCode::STORE_LOCAL, 1);
    cu.emit_op(OpCode::POP, 1);
    emit_imm(cu, 0);
    emit_local(cu, OpCode::STORE_LOCAL, 2);
    cu.emit_op(OpCode::POP, 1);

    const usize loop_start = cu.code.size();
    emit_local(cu, OpCode::LOAD_LOCAL, 1);
    emit_imm(cu, 0);
    cu.emit_op(OpCode::GREATER, 1); // [i > 0]
    cu.emit_op(OpCode::JUMP_FALSE, 1);
    const usize jf_patch = cu.code.size();
    cu.emit_word(0, 1); // 占位,后回填 L_end

    emit_local(cu, OpCode::LOAD_LOCAL, 2);
    emit_local(cu, OpCode::LOAD_LOCAL, 1);
    cu.emit_op(OpCode::ADD, 1);
    emit_local(cu, OpCode::STORE_LOCAL, 2);
    cu.emit_op(OpCode::POP, 1);

    emit_local(cu, OpCode::LOAD_LOCAL, 1);
    emit_imm(cu, 1);
    cu.emit_op(OpCode::SUBTRACT, 1);
    emit_local(cu, OpCode::STORE_LOCAL, 1);
    cu.emit_op(OpCode::POP, 1);

    cu.emit_op(OpCode::JUMP_BACK, 1);
    const usize jb_patch = cu.code.size();
    cu.emit_word(0, 1); // 占位,后回填 loop_start

    const usize L_end = cu.code.size();    // 末段(L_end)起点,跳转回填基准
    emit_local(cu, OpCode::LOAD_LOCAL, 2); // L_end: [acc]
    cu.emit_op(OpCode::RETURN, 1);

    patch_word(cu, jf_patch, static_cast<u16>(L_end - (jf_patch + 2)));
    patch_word(cu, jb_patch, static_cast<u16>((jb_patch + 2) - loop_start));

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 6);
}

TEST_F(AriaVMStress, FunctionCall) {

    auto& gc        = vm.gc();
    auto  add       = new_function(gc, new_string(gc, "add"), 2);
    auto  add_guard = gc.make_guard(add); // add 裸持跨下方 main_fn 的 new_function(stress collect)
    auto& acu       = add->unit();
    emit_local(acu, OpCode::LOAD_LOCAL, 1); // 参数 1 在槽 1(槽 0 是 callee)
    emit_local(acu, OpCode::LOAD_LOCAL, 2); // 参数 2 在槽 2
    acu.emit_op(OpCode::ADD, 1);
    acu.emit_op(OpCode::RETURN, 1);

    auto       main_fn = new_function(gc, nullptr, 0);
    auto&      mcu     = main_fn->unit();
    const auto idx     = mcu.add_constant(Value::from_obj(add));
    emit_closure(mcu, idx); // [closure]
    emit_imm(mcu, 3);
    emit_imm(mcu, 4);
    mcu.emit_op(OpCode::CALL, 1);
    mcu.emit_byte(2, 1);
    mcu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(main_fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 7);
    EXPECT_EQ(vm.main_context().stack_size(), usize{0}); // callee 与帧已清干净
    EXPECT_TRUE(vm.main_context().frames().empty());
}

TEST_F(AriaVMStress, StackGrowsAndRebasesFrames) {
    // 压入超过初始容量(1024)的临时值触发值栈 2x 增长;增长后读取局部,验证帧的 slots 指针已重定位。

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    cu.emit_op(OpCode::LOAD_NIL, 1); // 预留局部槽 1(槽 0 是 callee)
    emit_imm(cu, 42);
    emit_local(cu, OpCode::STORE_LOCAL, 1);
    cu.emit_op(OpCode::POP, 1);

    for (usize i = 0; i < 2048; ++i) {
        cu.emit_op(OpCode::LOAD_NIL, 1); // 压 2048 个 nil,触发 2x 增长
    }
    for (usize i = 0; i < 2048; ++i) {
        cu.emit_op(OpCode::POP, 1);
    }

    emit_local(cu, OpCode::LOAD_LOCAL, 1); // 增长后读取局部,验证 slots 重定位
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out.value().is_int());
    EXPECT_EQ(out.value().as_int(), 42);
    EXPECT_GT(vm.main_context().stack_capacity(), usize{1024}); // 增长确已发生
}

TEST_F(AriaVMStress, TruthinessAndShortCircuit) {
    // false || true -> true(JUMP_TRUE_OR_POP 落空弹,压 true)
    {

        auto& gc       = vm.gc();
        auto  fn       = new_function(gc, nullptr, 0);
        auto  fn_guard = gc.make_guard(fn);
        auto& cu       = fn->unit();
        cu.emit_op(OpCode::LOAD_FALSE, 1);
        cu.emit_op(OpCode::JUMP_TRUE_OR_POP, 1);
        const usize patch = cu.code.size();
        cu.emit_word(0, 1);
        cu.emit_op(OpCode::LOAD_TRUE, 1);   // <b>(false || true 的右侧)
        const usize L_end = cu.code.size(); // 汇合点:跳/落空两路都在此收敛
        cu.emit_op(OpCode::RETURN, 1);
        patch_word(cu, patch, static_cast<u16>(L_end - (patch + 2)));

        const auto out = vm.run(fn);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(out.value().is_bool());
        EXPECT_TRUE(out.value().as_bool());
    }
    // nil && x -> nil(被测值 nil 为假,OR_POP 命中跳转留 nil)
    {

        auto& gc       = vm.gc();
        auto  fn       = new_function(gc, nullptr, 0);
        auto  fn_guard = gc.make_guard(fn);
        auto& cu       = fn->unit();
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::JUMP_FALSE_OR_POP, 1);
        const usize patch = cu.code.size();
        cu.emit_word(0, 1);
        cu.emit_op(OpCode::LOAD_TRUE, 1);   // <b>(nil && x 的右侧,不应执行)
        const usize L_end = cu.code.size(); // 汇合点:跳/落空两路都在此收敛
        cu.emit_op(OpCode::RETURN, 1);
        patch_word(cu, patch, static_cast<u16>(L_end - (patch + 2)));

        const auto out = vm.run(fn);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(out.value().is_nil());
    }
    // !nil -> true(0 为真:0 为真,NOT 后为 false)
    {

        auto& gc       = vm.gc();
        auto  fn       = new_function(gc, nullptr, 0);
        auto  fn_guard = gc.make_guard(fn);
        auto& cu       = fn->unit();
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::NOT, 1);
        cu.emit_op(OpCode::RETURN, 1);

        const auto out = vm.run(fn);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(out.value().as_bool());
    }
}

TEST_F(AriaVMStress, EqualitySemantics) {
    // 1 == 1.0 内容相等为 true;1 === 1.0 严格相等为 false

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    emit_imm(cu, 1);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(cu.add_constant(Value::from_f64(1.0)), 1);
    cu.emit_op(OpCode::EQUAL, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out.value().as_bool());
}

TEST_F(AriaVMStress, TypeMismatchIsUncaught) {

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    cu.emit_op(OpCode::LOAD_NIL, 3); // 行 3:验证直报站点位置前缀取故障指令行
    emit_imm(cu, 1, 3);
    cu.emit_op(OpCode::ADD, 3); // nil + 1 -> TypeMismatch
    cu.emit_op(OpCode::RETURN, 3);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
    // 直报站点统一切入寄存器(M3):消息在装箱点烘齐(码类前缀+细节,**不含位置前缀** --
    // 2026-09-10 起位置归跟踪行,对齐 clox/Python 惯例),未捕获物化时尾部附堆栈跟踪行
    // (单帧即 at <main>,行号 = ADD 指令所在行,pitfalls 坑 #16)。
    EXPECT_EQ(out.error().message(), "Runtime: TypeMismatch operator '+' requires numbers, got Nil and Int\n"
                                     "  at <main> (<script>:3)");
}

TEST_F(AriaVMStress, DivisionByZeroIsUncaught) {

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    emit_imm(cu, 1);
    emit_imm(cu, 0);
    cu.emit_op(OpCode::DIVIDE, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::DivisionByZero);
    // 同 TypeMismatchIsUncaught:位置由未捕获堆栈跟踪行携带(M3,消息无位置前缀)。
    EXPECT_EQ(out.error().message(), "Runtime: DivisionByZero integer division by zero\n"
                                     "  at <main> (<script>:1)");
}

TEST_F(AriaVMStress, WrongArityIsUncaught) {

    auto& gc        = vm.gc();
    auto  two       = new_function(gc, new_string(gc, "two"), 2);
    auto  two_guard = gc.make_guard(two); // two 裸持跨下方 fn 的 new_function(stress collect)
    auto& tcu       = two->unit();
    tcu.emit_op(OpCode::LOAD_NIL, 1);
    tcu.emit_op(OpCode::RETURN, 1);

    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    emit_closure(cu, cu.add_constant(Value::from_obj(two))); // [closure]
    emit_imm(cu, 1);                                         // 只给 1 个参数
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(1, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::WrongArity);
}

TEST_F(AriaVMStress, StackOverflowOnRunawayRecursion) {
    // fn 直接调用自己(arity 0),永不返回 -> 值栈/帧栈溢出

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    emit_closure(cu, cu.add_constant(Value::from_obj(fn))); // 常量池引用自己(每次调用现场包新闭包)
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::StackOverflow);
}

// 模块表是 GC 根:collect 经 VM 根 tracer -> modules_.trace 标全部模块,
// 模块进而 trace name_/entry_/globals_,整条链存活。
TEST_F(AriaVMStress, ModuleTableIsGcRoot) {

    auto& gc = vm.gc();

    auto path       = new_string(gc, "lib/utils");
    auto path_guard = gc.make_guard(path); // 工厂不再守卫入参:path 裸持跨 new_module 的 new_string(cwd)
    auto m          = new_module(gc, path);
    path_guard.push(m); // m 裸持跨下方 modules_.upsert 的 hash 分配
    // 入模块表(键=path,值=m)
    auto e   = vm.modules().upsert(Value::from_obj(path));
    e->value = Value::from_obj(m);

    // 给模块挂体 + 一条全局绑定,验证经模块表根 -> 模块 trace -> 子节点存活
    auto body = new_script(gc, m); // body 属于 m
    m->set_entry(body);
    auto g_key       = new_string(gc, "g");
    auto g_key_guard = gc.make_guard(g_key); // g_key 裸持跨下方 new_string(g_val)(stress collect)
    auto g_val       = new_string(gc, "a long global value string!!!");
    auto ge          = m->globals().upsert(Value::from_obj(g_key));
    ge->value        = Value::from_obj(g_val);

    const usize before = gc.bytes_allocated();
    gc.collect(); // 模块表=VM 根 -> 标 path+m -> m.trace 标 entry/globals -> 全存活
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(m->name()->view(), "lib/utils");
    EXPECT_EQ(m->entry(), body);
    EXPECT_EQ(g_val->view(), "a long global value string!!!");
}

// DEF_GLOBAL 是唯一创建模块全局的入口(顶层 var 声明):弹值,以常量池 name 为键 upsert 入
// 当前模块 globals。LOAD_GLOBAL 按名查表压入。
TEST_F(AriaVMStress, DefAndLoadGlobal) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_imm(cu, 42);                        // [42]
    emit_global(cu, OpCode::DEF_GLOBAL, x);  // [] 定义 x=42
    emit_global(cu, OpCode::LOAD_GLOBAL, x); // [42]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 42);
}

// STORE_GLOBAL 是赋值路径(peek-store,留值):命中已有条目则更新,不创建。
// DEF x=1; STORE x=2(留 2); POP; LOAD x -> 2。
TEST_F(AriaVMStress, StoreGlobalUpdatesExisting) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_imm(cu, 1);                          // [1]
    emit_global(cu, OpCode::DEF_GLOBAL, x);   // [] x=1
    emit_imm(cu, 2);                          // [2]
    emit_global(cu, OpCode::STORE_GLOBAL, x); // [2] peek-store x=2
    cu.emit_op(OpCode::POP, 1);               // []
    emit_global(cu, OpCode::LOAD_GLOBAL, x);  // [2]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 2);
}

// 赋值不隐式创建(grammar.txt §445):STORE_GLOBAL 未定义全局 -> UndefinedVariable。
TEST_F(AriaVMStress, StoreGlobalUndefinedErrors) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_imm(cu, 1);                          // [1]
    emit_global(cu, OpCode::STORE_GLOBAL, x); // 未定义 -> UndefinedVariable(peek 不弹)
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// LOAD_GLOBAL 未定义 -> UndefinedVariable 运行时错误。
TEST_F(AriaVMStress, LoadGlobalUndefinedErrors) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_global(cu, OpCode::LOAD_GLOBAL, x); // 未定义 -> UndefinedVariable
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// IMPORT 裸名解析命中(磁盘 exists-check + 模块表查重):入口源根(base)下建真实空文件
// lib/utils.aria,预注册其绝对键模块(模拟循环导入命中半初始化对象);
// import "lib/utils" as Utils -> resolve 沿 source_roots 命中入口根文件 -> 查表命中 ->
// 以 alias 绑入当前模块 globals。LOAD_GLOBAL 取回绑入的预注册模块对象。
TEST_F(AriaVMStress, ImportBindsPreRegisteredModule) {

    vm.set_source_roots({}); // 隔离:仅入口根,免默认 stdlib 干扰
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    // 源根 = base:入口与目标模块均在此目录(dir_ = base),name_ 为相对 base 的路径。
    auto dir_ptr   = new_string(gc, base);
    auto dir_guard = gc.make_guard(dir_ptr);
    // 目标文件 base/lib/utils.aria -> 其规范绝对路径即模块表键 K(resolve 命中此键)。
    const auto key_str = touch_aria(base, "lib/utils.aria");
    auto       key     = new_string(gc, key_str);
    dir_guard.push(key);
    auto m = make_module(gc, "lib/utils", dir_ptr);        // 目标:dir=base, name=lib/utils
    dir_guard.push(m);                                     // 保 m 过 modules_.upsert 的 hash 分配
    auto me   = vm.modules().upsert(Value::from_obj(key)); // 入表即「已加载」,无对象状态字段
    me->value = Value::from_obj(m);

    auto      mod       = new_disk_module(gc, "main", dir_ptr); // 入口:dir=base, name=main
    auto      fn        = new_script(gc, mod);
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/utils")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "Utils")));
    emit_import(cu, path_idx);                       // IMPORT "lib/utils" -> [module]
    emit_global(cu, OpCode::DEF_GLOBAL, alias_idx);  // []  绑全局 "Utils" = module
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), m); // 取回的是预注册模块对象
}

// IMPORT 解析失败(无源根命中 nope/missing.aria)且无嵌入层加载 -> ModuleNotFound。
// 入口根(base)与 stdlib(已置空)均无该文件,resolve_module 返回 nullopt。
TEST_F(AriaVMStress, ImportNotFoundErrors) {

    vm.set_source_roots({}); // 隔离:仅入口根
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto      dir_ptr   = new_string(gc, base); // 入口源根 = base(run 播种 source_roots[0] = base)
    auto      dir_guard = gc.make_guard(dir_ptr);
    auto      fn        = new_script(gc, new_disk_module(gc, "main", dir_ptr));
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "nope/missing")));
    emit_import(cu, path_idx);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::ModuleNotFound);
}

// IMPORT 规范化:预注册 base/lib/utils.aria,用 "lib/./utils" 导入 -- resolve_module 经
// weakly_canonical 折叠 ".",归一为 base/lib/utils.aria,命中同一绝对键,取回预注册模块对象。
TEST_F(AriaVMStress, ImportNormalizesAbsolutePath) {

    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto       dir_ptr   = new_string(gc, base);
    auto       dir_guard = gc.make_guard(dir_ptr);
    const auto key_str   = touch_aria(base, "lib/utils.aria");
    auto       key       = new_string(gc, key_str);
    dir_guard.push(key);
    auto m = make_module(gc, "lib/utils", dir_ptr);
    dir_guard.push(m);
    auto me   = vm.modules().upsert(Value::from_obj(key));
    me->value = Value::from_obj(m);

    auto      fn        = new_script(gc, new_disk_module(gc, "main", dir_ptr));
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/./utils")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "Utils")));
    emit_import(cu, path_idx);                       // IMPORT "lib/./utils" -> [module]
    emit_global(cu, OpCode::DEF_GLOBAL, alias_idx);  // []  绑全局 "Utils"
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), m); // 折 "." 后命中同一模块
}

// IMPORT 相对路径解析:导入函数所属模块 dir_ = base、name_ = lib/main,"./helper" 相对当前
// 模块目录(dir_ + dirname(name_) = base/lib)解析 -> base/lib/helper.aria,命中预注册的该绝对键模块。
// 相对导入基 = dirname(abs_path) = dir_ + dirname(name_),caller-local,不碰 source_roots。
TEST_F(AriaVMStress, ImportNormalizesRelativePath) {

    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    // 源根 = base:导入方/入口模块 dir_ = base、name_ = lib/main(相对基 = base/lib)。
    auto dir_ptr   = new_string(gc, base);
    auto dir_guard = gc.make_guard(dir_ptr);
    // 目标 base/lib/helper.aria -> 绝对键 K。
    const auto key_str = touch_aria(base, "lib/helper.aria");
    auto       key     = new_string(gc, key_str);
    dir_guard.push(key);
    auto helper = make_module(gc, "lib/helper", dir_ptr);
    dir_guard.push(helper);
    auto he   = vm.modules().upsert(Value::from_obj(key));
    he->value = Value::from_obj(helper);

    auto      mod       = new_disk_module(gc, "lib/main", dir_ptr);
    auto      fn        = new_script(gc, mod);
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "./helper")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "H")));
    emit_import(cu, path_idx);                       // IMPORT "./helper" -> [module]
    emit_global(cu, OpCode::DEF_GLOBAL, alias_idx);  // []  绑全局 "H"
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), helper); // 相对解析后命中 base/lib/helper
}

// IMPORT 裸名沿 source_roots 逐根搜索(对齐 Python sys.path 顺序搜索):入口根(base)无
// lib/math.aria -> 落到 stdlib 根(base/stdlib)命中。证明裸名用源根列表,而非「当前模块目录」--
// 此处导入方即入口,当前目录 = 入口目录 = base,而文件在 stdlib,故裸名越过了当前目录命中 stdlib。
// (M1 单模块运行:导入方恒为入口;裸名 vs 相对的区别体现在裸名查 source_roots 列表而非单个
// caller 目录,本例靠 stdlib 第二根体现该区别。)
TEST_F(AriaVMStress, ImportBareSearchesSourceRoots) {

    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto dir_ptr   = new_string(gc, base); // 入口源根 = base
    auto dir_guard = gc.make_guard(dir_ptr);
    // stdlib 第二根 = base/stdlib(建目录);文件只放 stdlib,base 下不放 -> 强制 fall-through。
    const auto stdlib_dir = std::filesystem::weakly_canonical(std::filesystem::path{base} / "stdlib").string();
    std::filesystem::create_directories(stdlib_dir);
    auto stdlib_ptr = new_string(gc, stdlib_dir); // 目标模块所属源根 = stdlib
    dir_guard.push(stdlib_ptr);
    const auto key_str = touch_aria(stdlib_dir, "lib/math.aria"); // = base/stdlib/lib/math.aria
    vm.set_source_roots({stdlib_dir});

    auto key = new_string(gc, key_str);
    dir_guard.push(key);
    auto target = make_module(gc, "lib/math", stdlib_ptr); // 目标:dir=stdlib, name=lib/math
    dir_guard.push(target);
    auto te   = vm.modules().upsert(Value::from_obj(key));
    te->value = Value::from_obj(target);

    auto      fn        = new_script(gc, new_disk_module(gc, "main", dir_ptr)); // 入口:dir=base
    auto      fn_guard  = gc.make_guard(fn); // fn(+所属 module)裸持跨下方 new_string(path/alias)(stress collect)
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/math"))); // 裸路径
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx);                       // IMPORT "lib/math" -> [module]
    emit_global(cu, OpCode::DEF_GLOBAL, alias_idx);  // []  绑全局 "M"
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), target); // 入口根未命中,落 stdlib 命中
}

// IMPORT 末尾 ".aria" 后缀可选:"lib/math" 与 "lib/math.aria" 归一为同一文件。
// 预注册 base/lib/math.aria(绝对键),用 "lib/math.aria" 导入 -> resolve 剥后缀再补回,
// 解析到同一 base/lib/math.aria,命中预注册模块对象。
TEST_F(AriaVMStress, ImportStripsAriaSuffix) {

    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto       dir_ptr   = new_string(gc, base);
    auto       dir_guard = gc.make_guard(dir_ptr);
    const auto key_str   = touch_aria(base, "lib/math.aria");
    auto       key       = new_string(gc, key_str);
    dir_guard.push(key);
    auto m = make_module(gc, "lib/math", dir_ptr);
    dir_guard.push(m);
    auto me   = vm.modules().upsert(Value::from_obj(key));
    me->value = Value::from_obj(m);

    auto      fn        = new_script(gc, new_disk_module(gc, "main", dir_ptr));
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/math.aria")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx);                       // IMPORT "lib/math.aria" -> [module]
    emit_global(cu, OpCode::DEF_GLOBAL, alias_idx);  // []  绑全局 "M"
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), m); // 剥 .aria 后命中同一文件键
}

// IMPORT 相对路径 + .aria 后缀组合:导入方 dir_ = base、name_ = lib/main,"./math.aria"
// 相对当前模块目录(dir_ + dirname(name_) = base/lib)解析 -> 剥 .aria 再补回 ->
// base/lib/math.aria,命中预注册目标。与 ImportStripsAriaSuffix(裸名)互补:验证 .aria 剥离在
// 相对路径上同样生效。
TEST_F(AriaVMStress, ImportStripsAriaSuffixOnRelative) {

    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto       dir_ptr   = new_string(gc, base);
    auto       dir_guard = gc.make_guard(dir_ptr);
    const auto key_str   = touch_aria(base, "lib/math.aria");
    auto       key       = new_string(gc, key_str);
    dir_guard.push(key);
    auto target = make_module(gc, "lib/math", dir_ptr);
    dir_guard.push(target);
    auto te   = vm.modules().upsert(Value::from_obj(key));
    te->value = Value::from_obj(target);

    auto      mod       = new_disk_module(gc, "lib/main", dir_ptr);
    auto      fn        = new_script(gc, mod);
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "./math.aria")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx);                       // IMPORT "./math.aria" -> [module]
    emit_global(cu, OpCode::DEF_GLOBAL, alias_idx);  // []  绑全局 "M"
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), target); // 相对 + 剥 .aria -> base/lib/math
}

// 源根列表在 run() 时按 [入口模块 dir_, stdlib 目录] 播种:入口模块 dir_ = base、name_ = main
// -> 入口源根 = base(绝对路径);stdlib 置空隔离 -> source_roots = [base]。
// 对齐 Python sys.path[0] = 入口脚本所在目录(此处入口源根即 dir_,键 = 绝对规范路径)。
TEST_F(AriaVMStress, SourceRootSeededFromEntryModuleDir) {

    vm.set_source_roots({}); // 隔离:仅入口源根
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto  dir_ptr   = new_string(gc, base);
    auto  dir_guard = gc.make_guard(dir_ptr);
    auto  mod       = new_disk_module(gc, "main", dir_ptr); // 入口:dir=base, name=main
    auto  fn        = new_script(gc, mod);
    auto  fn_guard  = gc.make_guard(fn);
    auto& cu        = fn->unit();
    cu.emit_op(OpCode::HALT, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(vm.source_roots().size(), 1u);
    EXPECT_EQ(vm.source_roots()[0], base); // 入口源根 = base(绝对)
}

// 源根列表:默认 <script> 合成模块经 make_module 持 dir_ = 当前工作目录(保证非空),
// run() 用其原地替换入口槽 [0](构造时占位的 cwd);stdlib 置空隔离 -> source_roots = [cwd]。
// 对齐 Python sys.path[0] = 入口脚本所在目录 -- 合成入口无明确目录时退化为 cwd 占位。
TEST_F(AriaVMStress, SourceRootSeededWithCwdForScriptEntry) {

    vm.set_source_roots({}); // 隔离:仅入口槽,免默认 stdlib
    auto& gc = vm.gc();

    auto  fn = new_function(gc, nullptr, 0); // make_module -> <script>, dir_ = cwd
    auto& cu = fn->unit();
    cu.emit_op(OpCode::HALT, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(vm.source_roots().size(), 1u);
    EXPECT_EQ(vm.source_roots()[0], std::filesystem::current_path().string()); // 入口槽 = cwd
}

// ---- IMPORT 加载层(磁盘读 + 编译 + run-once + 入表)端到端 ----
//
// 经 interpret_from_path 跑真实 .aria 文件(testing::TempDir 下),触发 IMPORT 未命中分支的完整加载链路。
// set_source_roots({}) 隔离 stdlib,使裸名/相对导入仅落入口源根(= 入口文件目录,run() 播种 source_roots[0])。
// 跑完后白盒检视 vm.modules()(IMPORT 入表的被导入模块,入口模块不入表)。

// 加载层基本链路:helper 定义 var x = 42;main 导入它 -> 读盘 -> 编译 -> run-once(helper globals 填 x=42)
// -> 入表 Loaded -> 压栈绑定。检视 modules 里 helper 模块 Loaded 且 globals.x == 42。
TEST_F(AriaVMStress, ImportLoadsDiskModuleRunsBodyAndPopulatesGlobals) {

    vm.set_source_roots({}); // 隔离:仅入口源根
    const auto base = test_canon_dir();
    write_aria(base, "helper.aria", "var x = 42;");
    const auto main_path = write_aria(base, "main.aria", "import \"./helper\" as H;");

    const auto result = vm.interpret_from_path(main_path);
    ASSERT_EQ(result, aria::InterpretResult::Ok);

    auto helper = find_module_by_name(vm.modules(), "helper");
    ASSERT_NE(helper, nullptr);
    auto x_entry = helper->globals().find(Value::from_obj(new_string(vm.gc(), "x")));
    ASSERT_NE(x_entry, nullptr);
    EXPECT_TRUE(x_entry->value.is_int());
    EXPECT_EQ(x_entry->value.as_int(), 42);
}

// 被导入模块编译期错误经 Error 原样透传(含其文件位置):helper 有语法错 -> interpret 返 RuntimeError。
// 分类按失败阶段而非错误码大类:错误在主模块执行期的 IMPORT 站点浮现(经异常通道传播、可被 try/catch
// 捕获),主入口自身编译已成功 -- "可 catch 的错误"不构成 CompileError(2026-09 决策,见 runtime.md)。
TEST_F(AriaVMStress, ImportModuleCompileErrorPropagates) {

    vm.set_source_roots({});
    const auto base = test_canon_dir();
    write_aria(base, "helper.aria", "var = 5;"); // 语法错:var 后期望标识符
    const auto main_path = write_aria(base, "main.aria", "import \"./helper\" as H;");

    const auto result = vm.interpret_from_path(main_path);
    EXPECT_EQ(result, aria::InterpretResult::RuntimeError);
}

// 被导入模块运行期错误(模块体 run-once 期间)经 Error 原样透传:helper `var x = 1/0;`(整除零)
// -> interpret 返 RuntimeError。
TEST_F(AriaVMStress, ImportModuleRuntimeErrorPropagates) {

    vm.set_source_roots({});
    const auto base = test_canon_dir();
    write_aria(base, "helper.aria", "var x = 1/0;");
    const auto main_path = write_aria(base, "main.aria", "import \"./helper\" as H;");

    const auto result = vm.interpret_from_path(main_path);
    EXPECT_EQ(result, aria::InterpretResult::RuntimeError);
}

// M3:模块体 run-once 期间 throw -- unwind 弹 <module> 帧后,导入方 IMPORT 站点所在 try 捕获
// (外层帧 last_ip = IMPORT 指令,pitfalls 坑 #2),异常值落 catch 参数槽;boomer 半初始化仍在
// 表中(加载事实源 = 表成员资格)。经 run(SourceFile&, ObjModule&) 取返回值断言(interpret 只回类别)。
TEST_F(AriaVMStress, ImportModuleThrowCaughtByImporter) {

    vm.set_source_roots({});
    const auto base = test_canon_dir();
    write_aria(base, "boomer.aria", "var x = 1; throw \"boom\";");
    const auto main_path =
            write_aria(base, "main.aria",
                       "try {\n    import \"./boomer\" as B;\n    return 0;\n} catch (e) {\n    return e;\n}\n");

    auto loaded = aria::SourceFile::from_path(main_path);
    ASSERT_TRUE(loaded.has_value());
    aria::SourceFile source = std::move(loaded.value());
    auto             dir    = new_string(vm.gc(), base);
    auto             module = make_module(vm.gc(), "main", dir); // dir 先入根,make_module 内部自守
    auto             guard  = vm.gc().make_guard(module);
    const auto       out    = vm.run(source, *module);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    ASSERT_TRUE(out.value().is_obj());
    const auto thrown = aria::Object::as<ObjString>(out.value().as_obj());
    ASSERT_NE(thrown, nullptr);
    EXPECT_EQ(thrown->view(), "boom");

    EXPECT_NE(find_module_by_name(vm.modules(), "boomer"), nullptr);
}

// 循环导入:a 导入 b、b 导入 a(均经 IMPORT 入表 -> 命中 Loading 半初始化对象)。两者各 run-once 一次,
// 均 Loaded,globals 填充,且交叉绑定(b 的 A == a、a 的 B == b)。main(入口)不入表。
TEST_F(AriaVMStress, CircularImportCompletesBothLoaded) {

    vm.set_source_roots({});
    const auto base = test_canon_dir();
    write_aria(base, "a.aria", "import \"./b\" as B; var x = 1;");
    write_aria(base, "b.aria", "import \"./a\" as A; var y = 2;");
    const auto main_path = write_aria(base, "main.aria", "import \"./a\" as A;");

    const auto result = vm.interpret_from_path(main_path);
    ASSERT_EQ(result, aria::InterpretResult::Ok);

    auto a = find_module_by_name(vm.modules(), "a");
    auto b = find_module_by_name(vm.modules(), "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // a.globals: x=1, B=b(循环导入命中 a 的半初始化对象时,b 的 A 绑定它;后 a 的体跑完 globals 完整)
    auto ax = a->globals().find(Value::from_obj(new_string(vm.gc(), "x")));
    ASSERT_NE(ax, nullptr);
    EXPECT_EQ(ax->value.as_int(), 1);
    auto a_b = a->globals().find(Value::from_obj(new_string(vm.gc(), "B")));
    ASSERT_NE(a_b, nullptr);
    EXPECT_EQ(aria::Object::as<ObjModule>(a_b->value.as_obj()), b);

    // b.globals: y=2, A=a
    auto by = b->globals().find(Value::from_obj(new_string(vm.gc(), "y")));
    ASSERT_NE(by, nullptr);
    EXPECT_EQ(by->value.as_int(), 2);
    auto b_a = b->globals().find(Value::from_obj(new_string(vm.gc(), "A")));
    ASSERT_NE(b_a, nullptr);
    EXPECT_EQ(aria::Object::as<ObjModule>(b_a->value.as_obj()), a);
}

// 重复导入同一模块:第二次 IMPORT 命中 Loaded 模块(表查重复用),不再 run-once。检视 modules 中
// helper 仅一个、Loaded、globals.x==42。
TEST_F(AriaVMStress, ReimportReusesLoadedModule) {

    vm.set_source_roots({});
    const auto base = test_canon_dir();
    write_aria(base, "helper.aria", "var x = 42;");
    const auto main_path = write_aria(base, "main.aria", "import \"./helper\" as H1; import \"./helper\" as H2;");

    const auto result = vm.interpret_from_path(main_path);
    ASSERT_EQ(result, aria::InterpretResult::Ok);

    auto helper = find_module_by_name(vm.modules(), "helper");
    ASSERT_NE(helper, nullptr);
    auto x_entry = helper->globals().find(Value::from_obj(new_string(vm.gc(), "x")));
    ASSERT_NE(x_entry, nullptr);
    EXPECT_EQ(x_entry->value.as_int(), 42);
    EXPECT_EQ(vm.modules().size(), 1u); // 仅 helper 一个被导入模块
}

// ---- 原生函数:CALL 命中 ObjNativeFn,同步调用,返回值写槽 0 ----

// double(21) == 42:验证原生函数读 args(slots[1])、写返回槽(slots[0]),VM drop(argc) 后栈顶即返回值。
TEST_F(AriaVMStress, NativeFnSlot0Return) {

    auto&      gc       = vm.gc();
    auto       fn       = new_function(gc, nullptr, 0);
    auto       fn_guard = gc.make_guard(fn);
    auto&      cu       = fn->unit();
    const auto nf_idx   = add_native_const(cu, gc, "double", double_native);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf_idx, 1); // [nf]
    emit_imm(cu, 21);        // [nf, 21]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(1, 1); // [42]  (double 写槽 0,VM drop 1)
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << "expected success, got error";
    ASSERT_TRUE(out.value().is_int());
    EXPECT_EQ(out.value().as_int(), 42);
}

// 无参原生:CALL 0,slots 仅含槽 0,写返回值后 drop(0)。
TEST_F(AriaVMStress, NativeFnZeroArity) {

    auto& gc       = vm.gc();
    auto  fn       = new_function(gc, nullptr, 0);
    auto  fn_guard = gc.make_guard(fn);
    auto& cu       = fn->unit();
    // 复用 double_native 但传 0 参:它会因 argc != 1 报错 -- 故另造一个无参内建。
    static auto answer_native = +[](AriaVM& /*vm*/, Span<Value> slots) -> bool {
        slots[0] = Value::from_int(42); // 无参,直接写槽 0
        return true;
    };
    const auto nf_idx = add_native_const(cu, gc, "answer", answer_native);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf_idx, 1); // [nf]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // [42]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_int());
    EXPECT_EQ(out.value().as_int(), 42);
}

// 原生函数侧信道报错:vm.fail 写寄存器,run() 取出作未捕获 Error 返回。
TEST_F(AriaVMStress, NativeFnSideChannelError) {

    auto&      gc       = vm.gc();
    auto       fn       = new_function(gc, nullptr, 0);
    auto       fn_guard = gc.make_guard(fn);
    auto&      cu       = fn->unit();
    const auto nf_idx   = add_native_const(cu, gc, "fail_always", fail_always_native);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf_idx, 1); // [nf]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 调用 -> raise
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value()) << "expected error, got value";
    const auto& err = out.error();
    EXPECT_EQ(err.code(), ErrorCode::TypeMismatch);
    // vm.fail 装箱路径(消息无位置前缀,2026-09-10 起):原生不进帧,顶帧即 caller,
    // 跟踪行位置 = CALL 站点行(本例行 1),合成模块退化 "<script>:line";CALL 失败同走
    // unwind(M3),未捕获尾部附 at <main> 跟踪行。
    EXPECT_EQ(err.message(), "Runtime: TypeMismatch fail_always always fails\n"
                             "  at <main> (<script>:1)");
}

// 原生函数元数自查:double 收 0 参时经 vm.fail 报 WrongArity。
TEST_F(AriaVMStress, NativeFnAritySelfCheck) {

    auto&      gc       = vm.gc();
    auto       fn       = new_function(gc, nullptr, 0);
    auto       fn_guard = gc.make_guard(fn);
    auto&      cu       = fn->unit();
    const auto nf_idx   = add_native_const(cu, gc, "double", double_native);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf_idx, 1); // [nf]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 调 double() 传 0 参 -> WrongArity
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::WrongArity);
}

// ============================================================
// M4 闭包机制(手写 emit;全部跑在 AriaVMStress 下,每个 new_object 触发 collect,
// 顺带锻炼开链 tracer 标根 / CLOSURE 根安全 / 关闭挂点的分配安全)
// ============================================================

// incr 闭包体(arity 0,捕获 uv0 = 外层局部 n):
//   n += 1 经 LOAD_UPVALUE/STORE_UPVALUE(peek-store 写穿),返回新值。
void emit_incr_body(CodeUnit& cu) {
    emit_upvalue(cu, OpCode::LOAD_UPVALUE, 0);  // [n]
    emit_imm(cu, 1);                            // [n, 1]
    cu.emit_op(OpCode::ADD, 1);                 // [n+1]
    emit_upvalue(cu, OpCode::STORE_UPVALUE, 0); // [n+1](写穿到 uv0)
    cu.emit_op(OpCode::POP, 1);                 // []
    emit_upvalue(cu, OpCode::LOAD_UPVALUE, 0);  // [n]
    cu.emit_op(OpCode::RETURN, 1);
}

// 计数器闭包:make_counter 返回捕获 n 的 incr 闭包;主程序连调 4 次
// (1 次建 + 3 次自增),结果 1+2+3=6 -- 证明 STORE_UPVALUE 写穿共享、
// RETURN 关闭挂点把 n 迁入 upvalue 自持(外层帧已销毁,闭包仍活)。
TEST_F(AriaVMStress, ClosureCounterSharedState) {

    auto& gc    = vm.gc();
    auto  incr  = new_function(gc, new_string(gc, "incr"), 0);
    auto  guard = gc.make_guard(incr);
    emit_incr_body(incr->unit());
    capture_local(incr, 1); // 捕 make_counter 的槽 1(n)

    auto outer = new_function(gc, new_string(gc, "make_counter"), 0);
    guard.push(outer);
    {
        auto&      ocu      = outer->unit();
        const auto incr_idx = ocu.add_constant(Value::from_obj(incr));
        ocu.emit_op(OpCode::LOAD_NIL, 1);        // [nil@slot1]  n 的槽(值填槽)
        emit_imm(ocu, 0);                        // [nil, 0]
        emit_local(ocu, OpCode::STORE_LOCAL, 1); // slot1 = 0
        ocu.emit_op(OpCode::POP, 1);             // top=slot1 之上,n=0
        emit_closure(ocu, incr_idx);             // [c](捕获 slot1)
        ocu.emit_op(OpCode::RETURN, 1);          // 返回 c;n 的 upvalue 随 RETURN 关闭迁移
    }

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&      cu        = fn->unit();
        const auto outer_idx = cu.add_constant(Value::from_obj(outer));
        cu.emit_op(OpCode::LOAD_NIL, 1); // slot1 = counter 存储槽
        emit_closure(cu, outer_idx);     // [nil, closure]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                     // [nil, counter](CALL 消费 callee 槽,闭包须存槽复用)
        emit_local(cu, OpCode::STORE_LOCAL, 1); // counter 入 slot1
        cu.emit_op(OpCode::POP, 1);             // [counter@slot1]
        for (int i = 0; i < 3; ++i) {
            emit_local(cu, OpCode::LOAD_LOCAL, 1); // 重取闭包再调
            cu.emit_op(OpCode::CALL, 1);
            cu.emit_byte(0, 1); // [1] / [1, 2] / [1, 2, 3]
        }
        cu.emit_op(OpCode::ADD, 1); // [1, 5]
        cu.emit_op(OpCode::ADD, 1); // [6]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 6); // 1+2+3:三次自增写同一 upvalue、读回递增
}

// 同槽捕获复用:一次 make_counter 建两个闭包(c1/c2 入模块 globals),
// 行为上共享同一 n(c1 自增两次后 c2 读到 3,各一份则为 1);
// 结构上 run 后白盒断言两闭包 upvalues()[0] 为同一 ObjUpvalue 指针。
TEST_F(AriaVMStress, SameSlotCaptureSharesOneUpvalue) {

    auto& gc    = vm.gc();
    auto  incr  = new_function(gc, new_string(gc, "incr"), 0);
    auto  guard = gc.make_guard(incr);
    emit_incr_body(incr->unit());
    capture_local(incr, 1);

    // outer 与 main 共享模块:DEF_GLOBAL(c1/c2)写 outer 的模块 globals,main LOAD_GLOBAL 须同源。
    auto m       = make_module(gc);
    auto m_guard = gc.make_guard(m);
    auto outer   = make_function(gc, m, "make_two", 0);
    guard.push(outer);
    {
        auto&      ocu      = outer->unit();
        const auto incr_idx = ocu.add_constant(Value::from_obj(incr));
        const auto c1_name  = ocu.add_constant(Value::from_obj(new_string(gc, "c1")));
        const auto c2_name  = ocu.add_constant(Value::from_obj(new_string(gc, "c2")));
        ocu.emit_op(OpCode::LOAD_NIL, 1); // slot1 = n
        emit_imm(ocu, 0);
        emit_local(ocu, OpCode::STORE_LOCAL, 1);
        ocu.emit_op(OpCode::POP, 1);
        emit_closure(ocu, incr_idx);                   // [c1]
        emit_global(ocu, OpCode::DEF_GLOBAL, c1_name); // []
        emit_closure(ocu, incr_idx);                   // [c2](同槽,find 复用同一 uv)
        emit_global(ocu, OpCode::DEF_GLOBAL, c2_name); // []
        ocu.emit_op(OpCode::LOAD_NIL, 1);              // [nil]
        ocu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&      cu        = fn->unit();
        const auto outer_idx = cu.add_constant(Value::from_obj(outer));
        const auto c1_name   = cu.add_constant(Value::from_obj(new_string(gc, "c1")));
        const auto c2_name   = cu.add_constant(Value::from_obj(new_string(gc, "c2")));
        emit_closure(cu, outer_idx); // [closure]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [nil]
        cu.emit_op(OpCode::POP, 1);
        emit_global(cu, OpCode::LOAD_GLOBAL, c1_name); // [c1]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [1]
        cu.emit_op(OpCode::POP, 1);
        emit_global(cu, OpCode::LOAD_GLOBAL, c1_name); // [c1]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [2]
        cu.emit_op(OpCode::POP, 1);
        emit_global(cu, OpCode::LOAD_GLOBAL, c2_name); // [c2]
        cu.emit_op(OpCode::CALL, 1);                   // c2 读同一 n(已被 c1 加到 2)-> 3
        cu.emit_byte(0, 1);                            // [3]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 3); // 行为:共享(c1 两次自增,c2 读到 3)

    // 结构:两闭包的 upvalues()[0] 同一 ObjUpvalue(经共享模块 m 的 globals 取回;
    // 存活链:m_guard -> module -> globals -> 闭包)。
    auto c1_entry = m->globals().find(Value::from_obj(new_string(gc, "c1")));
    auto c2_entry = m->globals().find(Value::from_obj(new_string(gc, "c2")));
    ASSERT_NE(c1_entry, nullptr);
    ASSERT_NE(c2_entry, nullptr);
    auto c1 = aria::Object::as<ObjClosure>(c1_entry->value.as_obj());
    auto c2 = aria::Object::as<ObjClosure>(c2_entry->value.as_obj());
    ASSERT_EQ(c1->upvalue_count(), usize{1});
    ASSERT_EQ(c2->upvalue_count(), usize{1});
    EXPECT_EQ(c1->upvalues()[0], c2->upvalues()[0]);
}

// CLOSE_UPVALUE 指令:批量关闭所有槽址 >= 当前栈顶的开 upvalue(无弹栈,弹栈由前置 POP 承担,
// 对齐 Lua OP_CLOSE);关闭后读/写走已迁移的 closed_(若未迁移,弹掉的槽会被后续压栈覆写,
// 再经 uv 读到覆写值 -> 报错)。
TEST_F(AriaVMStress, CloseUpvalueReadsMigratedValue) {

    auto& gc    = vm.gc();
    auto  incr  = new_function(gc, new_string(gc, "incr"), 0);
    auto  guard = gc.make_guard(incr);
    emit_incr_body(incr->unit());
    capture_local(incr, 1);

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&      cu       = fn->unit();
        const auto incr_idx = cu.add_constant(Value::from_obj(incr));
        const auto c_name   = cu.add_constant(Value::from_obj(new_string(gc, "c")));
        cu.emit_op(OpCode::LOAD_NIL, 1); // slot1 = n(被捕获局部)
        emit_imm(cu, 7);
        emit_local(cu, OpCode::STORE_LOCAL, 1);      // n=7(经槽上方临时 peek-store)
        cu.emit_op(OpCode::POP, 1);                  // top=slot2,slot1=7
        emit_closure(cu, incr_idx);                  // [c](uv 指向 slot1,开链)
        emit_global(cu, OpCode::DEF_GLOBAL, c_name); // [] c 入 globals
        cu.emit_op(OpCode::POP, 1);                  // 弹 slot1 的槽(编译器 emit_pop_locals_to 镜像:count==1 降级 POP)
        cu.emit_op(OpCode::CLOSE_UPVALUE, 1);        // 批量关槽址 >= 新栈顶的开 uv(7 迁入 closed_),无弹栈
        emit_global(cu, OpCode::LOAD_GLOBAL, c_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [8](读 closed_ 7 -> +1)
        emit_global(cu, OpCode::LOAD_GLOBAL, c_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [8, 9](再自增:写 closed_)
        cu.emit_op(OpCode::ADD, 1);
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 17); // 8+9:CLOSE 后读写均在 closed_ 上持续
}

// open upvalue 指着的栈被压 2048 个临时值触发两轮 2x 增长(1024->2048->4096):
// grow_stack_ 第三类重绑后 LOAD_UPVALUE 仍读对(未重绑即读已释放旧块)。
TEST_F(AriaVMStress, StackGrowsRebasesOpenUpvalues) {

    auto& gc     = vm.gc();
    auto  reader = new_function(gc, new_string(gc, "reader"), 0);
    auto  guard  = gc.make_guard(reader);
    {
        auto& rcu = reader->unit();
        emit_upvalue(rcu, OpCode::LOAD_UPVALUE, 0);
        rcu.emit_op(OpCode::RETURN, 1);
    }
    capture_local(reader, 1);

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&      cu         = fn->unit();
        const auto reader_idx = cu.add_constant(Value::from_obj(reader));
        cu.emit_op(OpCode::LOAD_NIL, 1); // slots 1(n), 2(c)
        cu.emit_op(OpCode::LOAD_NIL, 1);
        emit_imm(cu, 42);
        emit_local(cu, OpCode::STORE_LOCAL, 1); // n=42
        cu.emit_op(OpCode::POP, 1);
        emit_closure(cu, reader_idx);           // [c] uv -> slot1(open)
        emit_local(cu, OpCode::STORE_LOCAL, 2); // c 存槽 2,uv 仍开
        cu.emit_op(OpCode::POP, 1);
        for (usize i = 0; i < 2048; ++i) {
            cu.emit_op(OpCode::LOAD_NIL, 1); // 压 2048 个临时:1024 满 -> 增长;2048 满 -> 再增长
        }
        for (usize i = 0; i < 2048; ++i) {
            cu.emit_op(OpCode::POP, 1);
        }
        emit_local(cu, OpCode::LOAD_LOCAL, 2); // [c](增长后重取)
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [42]:uv 重绑后仍读 slot1
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 42);
    EXPECT_GT(vm.main_context().stack_capacity(), usize{2048}); // 增长确已发生(两轮)
}

// unwind 跨帧关闭(未命中路径):被调函数捕获 n=42 后 throw,其帧无 handler 被弹 --
// 未命中分支的 close_upvalues(frame.slots) 把 n 迁入 upvalue 自持;主帧 catch 覆写
// 该栈区(3 个 nil 压过陈旧槽)后调用幸存闭包 -> 43。若未关闭,读到被覆写的 nil ->
// nil+1 报 TypeMismatch(测试的牙齿)。
TEST_F(AriaVMStress, UnwindClosesCapturedUpvalue) {

    auto& gc      = vm.gc();
    auto  m       = make_module(gc); // thrower DEF_GLOBAL c 与 main LOAD_GLOBAL c 须同模块
    auto  m_guard = gc.make_guard(m);
    auto  incr    = make_function(gc, m, "incr", 0);
    auto  guard   = gc.make_guard(incr);
    emit_incr_body(incr->unit());
    capture_local(incr, 1);

    auto thrower = make_function(gc, m, "thrower", 0);
    guard.push(thrower);
    {
        auto&      tcu      = thrower->unit();
        const auto incr_idx = tcu.add_constant(Value::from_obj(incr));
        const auto c_name   = tcu.add_constant(Value::from_obj(new_string(gc, "c")));
        tcu.emit_op(OpCode::LOAD_NIL, 1); // slot1 = n
        emit_imm(tcu, 42);
        emit_local(tcu, OpCode::STORE_LOCAL, 1);
        tcu.emit_op(OpCode::POP, 1);
        emit_closure(tcu, incr_idx);                  // [c]
        emit_global(tcu, OpCode::DEF_GLOBAL, c_name); // c 入共享模块 globals(幸存载体)
        const auto boom_idx = tcu.add_constant(Value::from_obj(new_string(gc, "boom")));
        emit_const(tcu, boom_idx);      // [str]
        tcu.emit_op(OpCode::THROW, 1);  // -> unwind:thrower 帧无 handler,弹帧关链
        tcu.emit_op(OpCode::RETURN, 1); // 不可达
    }

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&      cu          = fn->unit();
        const auto thrower_idx = cu.add_constant(Value::from_obj(thrower));
        const auto c_name      = cu.add_constant(Value::from_obj(new_string(gc, "c")));
        cu.emit_op(OpCode::LOAD_NIL, 1); // slot1 = catch 参数槽预占(stack_depth=1)
        const usize begin = cu.code.size();
        emit_closure(cu, thrower_idx); // [closure]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);               // thrower throw -> unwind 到本帧
        const usize end = cu.code.size(); // try 区间终点(CALL 之后)
        cu.emit_op(OpCode::JUMP, 1);      // 正常路径跳过 catch
        const usize j_patch = cu.code.size();
        cu.emit_word(0, 1);
        const usize handle = cu.code.size(); // L_catch:异常值已落 slot1
        cu.emit_op(OpCode::POP, 1);          // 弹异常串
        cu.emit_op(OpCode::LOAD_NIL, 1);     // 覆写 slots+1..+3(含 n 的陈旧槽)
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::POP_N, 1);
        cu.emit_byte(3, 1);
        emit_global(cu, OpCode::LOAD_GLOBAL, c_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [43]:n 已随帧关闭迁入 upvalue
        cu.emit_op(OpCode::RETURN, 1);
        const usize l_end = cu.code.size();
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::RETURN, 1);
        patch_word(cu, j_patch, static_cast<u16>(l_end - (j_patch + 2)));
        cu.try_records.push(TryRecord{static_cast<u32>(begin), static_cast<u32>(end), static_cast<u32>(handle), 1});
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 43); // 42+1:未命中路径关闭已生效
}

// unwind 命中路径关闭:try 体局部(slot2)被捕获、闭包存 globals 后 throw --
// handler 命中分支 close_upvalues(slots+stack_depth) 先于截栈关闭被丢弃区间的开指;
// catch 覆写该区间后调用闭包 -> 5。若未关闭,读到被覆写的 nil。
TEST_F(AriaVMStress, UnwindHitClosesTryBodyUpvalue) {

    auto& gc     = vm.gc();
    auto  reader = new_function(gc, new_string(gc, "reader"), 0);
    auto  guard  = gc.make_guard(reader);
    {
        auto& rcu = reader->unit();
        emit_upvalue(rcu, OpCode::LOAD_UPVALUE, 0);
        rcu.emit_op(OpCode::RETURN, 1);
    }
    capture_local(reader, 2); // 捕 main 的 slot2(try 体局部 n;非 slot1 -- 那是 catch 参数槽)

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&      cu         = fn->unit();
        const auto reader_idx = cu.add_constant(Value::from_obj(reader));
        const auto r_name     = cu.add_constant(Value::from_obj(new_string(gc, "r")));
        cu.emit_op(OpCode::LOAD_NIL, 1); // slots 1(catch 参数),2(try 体局部 n),3(c 临时)
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::LOAD_NIL, 1);
        const usize begin = cu.code.size();
        emit_imm(cu, 5);
        emit_local(cu, OpCode::STORE_LOCAL, 2); // n=5 @slot2
        cu.emit_op(OpCode::POP, 1);
        emit_closure(cu, reader_idx);                // [c] uv -> slot2(捕获 index 2)
        emit_global(cu, OpCode::DEF_GLOBAL, r_name); // c 入 globals
        const auto x_idx = cu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_const(cu, x_idx);            // [str]
        cu.emit_op(OpCode::THROW, 1);     // -> 命中本帧 handler
        const usize end = cu.code.size(); // try 区间含 THROW(throw 时刻 last_ip < end)
        cu.emit_op(OpCode::JUMP, 1);
        const usize j_patch = cu.code.size();
        cu.emit_word(0, 1);
        const usize handle = cu.code.size(); // L_catch
        cu.emit_op(OpCode::POP, 1);          // 弹异常串
        cu.emit_op(OpCode::LOAD_NIL, 1);     // 覆写 slots+1..+3(含 n 的陈旧槽)
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::POP_N, 1);
        cu.emit_byte(3, 1);
        emit_global(cu, OpCode::LOAD_GLOBAL, r_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [5]:n 已在截栈前迁入 upvalue
        cu.emit_op(OpCode::RETURN, 1);
        const usize l_end = cu.code.size();
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::RETURN, 1);
        patch_word(cu, j_patch, static_cast<u16>(l_end - (j_patch + 2)));
        cu.try_records.push(TryRecord{static_cast<u32>(begin), static_cast<u32>(end), static_cast<u32>(handle), 1});
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 5); // 命中路径关闭已生效(否则读到被覆写的 nil)
}

// 开链存活(clox 已知坑的防线):第一个闭包建完立即丢弃(无根),其 upvalue 仍开着挂在链上;
// 第二个 CLOSURE 顶 maybe_collect(stress 必 collect)时该节点仅被链引用 -- vm_roots tracer
// 若不标链,节点被回收,capture_upvalue 走悬垂链 / 复用已回收节点。行为断言:复用节点
// 读到 42(被扫节点复用会读到垃圾)。
TEST_F(AriaVMStress, OpenUpvalueChainSurvivesGcWithDeadClosure) {

    auto& gc     = vm.gc();
    auto  reader = new_function(gc, new_string(gc, "reader"), 0);
    auto  guard  = gc.make_guard(reader);
    {
        auto& rcu = reader->unit();
        emit_upvalue(rcu, OpCode::LOAD_UPVALUE, 0);
        rcu.emit_op(OpCode::RETURN, 1);
    }
    capture_local(reader, 1);

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&      cu         = fn->unit();
        const auto reader_idx = cu.add_constant(Value::from_obj(reader));
        const auto r_name     = cu.add_constant(Value::from_obj(new_string(gc, "r")));
        cu.emit_op(OpCode::LOAD_NIL, 1); // slots 1(n), 2(c1)
        cu.emit_op(OpCode::LOAD_NIL, 1);
        emit_imm(cu, 42);
        emit_local(cu, OpCode::STORE_LOCAL, 1);
        cu.emit_op(OpCode::POP, 1);
        emit_closure(cu, reader_idx);                // [c1] uv 开、链上
        cu.emit_op(OpCode::POP, 1);                  // c1 死(无根),uv 仍开
        emit_closure(cu, reader_idx);                // GC 窗口:uv 仅链引用 -> tracer 必须标链
        emit_global(cu, OpCode::DEF_GLOBAL, r_name); // c2(与死 c1 共享同一 uv)入 globals
        emit_global(cu, OpCode::LOAD_GLOBAL, r_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [42]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value().as_int(), 42); // 复用的链节点存活且指槽正确
}

// ============================================================
// M5 类机制(阶段 2:VM 机制,手写 emit;编译器发射阶段 3 翻转)。全部跑在
// AriaVMStress 下,每个 new_object 触发 collect,顺带锻炼 bootstrap 根 /
// 实例化快慢路径 / LOAD_FIELD 绑定 + 缓存回填 / 超类链的分配安全。
// ============================================================

// 无自定义 init 的类(无成员):LOAD_OBJECT + MAKE_CLASS 后 Foo() -- ctor 自 super 派生继承
// Object 的原生 no-op init,call_class 槽 0 原位换实例后 call_value 走 call_native 同步
// 调用(no-op 不写 slots[0],this 原样即返回值),不进帧、留空 ObjInstance。
TEST_F(AriaVMStress, InstantiateNoInitUsesSeededNativeInit) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 foo      = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    cu.emit_op(OpCode::LOAD_OBJECT, 1);        // [Object]
    emit_named(cu, OpCode::MAKE_CLASS, foo);   // [Foo]
    emit_global(cu, OpCode::DEF_GLOBAL, foo);  // []
    emit_global(cu, OpCode::LOAD_GLOBAL, foo); // [Foo]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // [instance](原生 no-op init 原样返回 this)
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    ASSERT_TRUE(out.value().is_obj());
    auto inst = aria::Object::as<ObjInstance>(out.value().as_obj());
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst->cls()->name()->view(), "Foo");
    EXPECT_EQ(inst->cls()->superclass(), vm.object_class()); // 无显式父类 -> Object 根
    // seed = Object 的原生 no-op init(MAKE_CLASS 继承,Value 经 === 判同):
    EXPECT_TRUE(aria::value_identical(inst->cls()->init(), vm.object_class()->init()));
    // no-op init 留空实例(无字段要初始化):任一名字 load_field 全链 miss(整表访问器
    // fields() 已删,行为级钉法;miss 的 fail 装箱是分配点,实例先入根)。
    auto ig   = gc.make_guard(inst);
    auto nope = new_string(gc, "nope");
    auto ng   = gc.make_guard(nope);
    EXPECT_FALSE(inst->load_field(vm, nope).has_value());
}

// 自定义 init 带参:Foo(7) -- 槽 0 原位换实例后 call_closure 进方法帧 [this, a]
// (this 替代 callee 占槽 0,实参槽位不动);init 内 this.tag = a(STORE_THIS_FIELD)
// 落 fields 表、返回 this(LOAD_LOCAL 0 + RETURN);调用点取回实例后 LOAD_FIELD 读回
// 字段(fields 命中)。
TEST_F(AriaVMStress, InstantiateInitSetsThisField) {

    auto& gc    = vm.gc();
    auto  init  = new_function(gc, new_string(gc, "init"), 1);
    auto  guard = gc.make_guard(init);
    {
        auto&     icu = init->unit();
        const u16 tag = icu.add_constant(Value::from_obj(new_string(gc, "tag")));
        emit_local(icu, OpCode::LOAD_LOCAL, 1);         // [a](槽 1 = 实参;槽 0 = this)
        emit_named(icu, OpCode::STORE_THIS_FIELD, tag); // [a](peek-store this.tag = a,值留栈)
        icu.emit_op(OpCode::POP, 1);                    // []
        emit_local(icu, OpCode::LOAD_LOCAL, 0);         // [this]
        icu.emit_op(OpCode::RETURN, 1);                 // 返回 this -> 实例
    }

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
        const u16 init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
        const u16 tag       = cu.add_constant(Value::from_obj(new_string(gc, "tag")));
        cu.emit_op(OpCode::LOAD_OBJECT, 1);      // [Object]
        emit_named(cu, OpCode::MAKE_CLASS, foo); // [Foo]
        emit_closure(cu, cu.add_constant(Value::from_obj(init)));
        emit_named(cu, OpCode::MAKE_METHOD, init_name); // [Foo](注册 init,覆盖 seed)
        emit_global(cu, OpCode::DEF_GLOBAL, foo);       // []
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);      // [Foo]
        emit_imm(cu, 7);                                // [Foo, 7]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(1, 1);                      // [instance](init 帧返回 this)
        emit_named(cu, OpCode::LOAD_FIELD, tag); // [7]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_TRUE(out.value().is_int());
    EXPECT_EQ(out.value().as_int(), 7);
}

// 方法调用两步走:LOAD_FIELD 命中类表方法 -> 绑定 ObjBoundMethod(this=obj)并回填
// fields 缓存 -> CALL 走 BOUND_METHOD 分支进方法帧([closure, this])。方法内
// LOAD/STORE_THIS_FIELD 读写 this 字段;调用点用 DUP 保住实例副本跨 CALL,
// 验证 this 突变对外可见。
TEST_F(AriaVMStress, MethodCallMutatesThisField) {

    auto& gc    = vm.gc();
    auto  init  = new_function(gc, new_string(gc, "init"), 0);
    auto  guard = gc.make_guard(init);
    {
        auto&     icu    = init->unit();
        const u16 x_name = icu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_imm(icu, 10);                                 // [10]
        emit_named(icu, OpCode::STORE_THIS_FIELD, x_name); // [10](this.x = 10)
        icu.emit_op(OpCode::POP, 1);                       // []
        emit_local(icu, OpCode::LOAD_LOCAL, 0);            // [this]
        icu.emit_op(OpCode::RETURN, 1);                    // 返回 this
    }
    auto m = new_function(gc, new_string(gc, "m"), 0);
    guard.push(m);
    {
        auto&     mcu    = m->unit();
        const u16 x_name = mcu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_named(mcu, OpCode::LOAD_THIS_FIELD, x_name);  // [x]
        emit_imm(mcu, 1);                                  // [x, 1]
        mcu.emit_op(OpCode::ADD, 1);                       // [x+1]
        emit_named(mcu, OpCode::STORE_THIS_FIELD, x_name); // [x+1](写回,值留栈)
        mcu.emit_op(OpCode::POP, 1);                       // []
        mcu.emit_op(OpCode::LOAD_NIL, 1);                  // [nil]
        mcu.emit_op(OpCode::RETURN, 1);                    // 方法返回 nil
    }

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
        const u16 init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
        const u16 m_name    = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        const u16 x_name    = cu.add_constant(Value::from_obj(new_string(gc, "x")));
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, foo);
        emit_closure(cu, cu.add_constant(Value::from_obj(init)));
        emit_named(cu, OpCode::MAKE_METHOD, init_name); // init
        emit_closure(cu, cu.add_constant(Value::from_obj(m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name); // m
        emit_global(cu, OpCode::DEF_GLOBAL, foo);    // []
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);   // [Foo]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                         // [i](init:x=10)
        cu.emit_op(OpCode::DUP, 1);                 // [i, i]
        emit_named(cu, OpCode::LOAD_FIELD, m_name); // [i, bound]
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                         // [i, nil](m 调毕,this.x=11)
        cu.emit_op(OpCode::POP, 1);                 // [i]
        emit_named(cu, OpCode::LOAD_FIELD, x_name); // [11](读回突变后的字段)
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 11);
}

// 继承覆写 + super:Sub.m 经 LOAD_SUPER_FIELD 从 defining class 的父链(不含自身)
// 取 Base 的被覆写实现、绑 this=帧槽 0 后 CALL;子调父结果 + 常量验证父实现真被调到。
TEST_F(AriaVMStress, InheritanceOverrideAndSuperCall) {

    auto& gc     = vm.gc();
    auto  base_m = new_function(gc, new_string(gc, "m"), 0);
    auto  guard  = gc.make_guard(base_m);
    {
        auto& bmu = base_m->unit();
        emit_imm(bmu, 1);
        bmu.emit_op(OpCode::RETURN, 1); // Base.m 返回 1
    }
    auto sub_m = new_function(gc, new_string(gc, "m"), 0);
    guard.push(sub_m);
    {
        auto&     smu    = sub_m->unit();
        const u16 m_name = smu.add_constant(Value::from_obj(new_string(gc, "m")));
        emit_named(smu, OpCode::LOAD_SUPER_FIELD, m_name); // [super-bound]
        smu.emit_op(OpCode::CALL, 1);
        smu.emit_byte(0, 1);         // [1](Base.m 返回值)
        emit_imm(smu, 10);           // [1, 10]
        smu.emit_op(OpCode::ADD, 1); // [11]
        smu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
        const u16 sub_name  = cu.add_constant(Value::from_obj(new_string(gc, "Sub")));
        const u16 m_name    = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        // Base:m
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, base_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(base_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, base_name);
        // Sub : Base(父类经 LOAD_GLOBAL 运行期解析),覆写 m
        emit_global(cu, OpCode::LOAD_GLOBAL, base_name); // [Base]
        emit_named(cu, OpCode::MAKE_CLASS, sub_name);    // [Sub](seed = Base 的 init)
        emit_closure(cu, cu.add_constant(Value::from_obj(sub_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, sub_name);
        // Sub().m()
        emit_global(cu, OpCode::LOAD_GLOBAL, sub_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                         // [i]
        emit_named(cu, OpCode::LOAD_FIELD, m_name); // [bound](绑 Sub.m)
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [11]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 11);

    // 结构:Sub 的父链确指 Base(经共享 globals 取回类值)。
    auto base_cls = aria::Object::as<ObjClass>(
            fn->module()->globals().find(Value::from_obj(new_string(gc, "Base")))->value.as_obj());
    auto sub_cls = aria::Object::as<ObjClass>(
            fn->module()->globals().find(Value::from_obj(new_string(gc, "Sub")))->value.as_obj());
    EXPECT_EQ(sub_cls->superclass(), base_cls);
}

// 类上赋值写遮蔽(读穿透、写遮蔽):Sub.x = v 落接收类(Sub)自身表 -- 继承名新建遮蔽键、
// 父表不动;Sub.x 取新值、Base.x 不变;实例读取(沿链)亦见遮蔽后的新值。
TEST_F(AriaVMStress, ClassWriteShadowsInheritedMember) {

    auto&     gc        = vm.gc();
    auto      m         = make_module(gc);
    auto      m_guard   = gc.make_guard(m);
    auto      fn        = make_function(gc, m, "<main>", 0);
    auto      guard     = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
    const u16 sub_name  = cu.add_constant(Value::from_obj(new_string(gc, "Sub")));
    const u16 x_name    = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    const u16 r1_name   = cu.add_constant(Value::from_obj(new_string(gc, "r1")));
    const u16 r2_name   = cu.add_constant(Value::from_obj(new_string(gc, "r2")));
    const u16 r3_name   = cu.add_constant(Value::from_obj(new_string(gc, "r3")));
    // Base:x = 1(静态)
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, base_name);
    emit_imm(cu, 1);
    emit_named(cu, OpCode::MAKE_STATIC, x_name);
    emit_global(cu, OpCode::DEF_GLOBAL, base_name);
    // Sub : Base(无成员)
    emit_global(cu, OpCode::LOAD_GLOBAL, base_name);
    emit_named(cu, OpCode::MAKE_CLASS, sub_name);
    emit_global(cu, OpCode::DEF_GLOBAL, sub_name);
    // Sub.x = 42:继承名新建遮蔽键
    emit_global(cu, OpCode::LOAD_GLOBAL, sub_name); // [Sub]
    emit_imm(cu, 42);                               // [Sub, 42]
    emit_named(cu, OpCode::STORE_FIELD, x_name);    // [42](单槽下移)
    cu.emit_op(OpCode::POP, 1);                     // []
    emit_global(cu, OpCode::LOAD_GLOBAL, sub_name);
    emit_named(cu, OpCode::LOAD_FIELD, x_name);   // [42]
    emit_global(cu, OpCode::DEF_GLOBAL, r1_name); // r1 = Sub.x(遮蔽键命中)
    emit_global(cu, OpCode::LOAD_GLOBAL, base_name);
    emit_named(cu, OpCode::LOAD_FIELD, x_name);   // [1]
    emit_global(cu, OpCode::DEF_GLOBAL, r2_name); // r2 = Base.x(父表未被波及)
    emit_global(cu, OpCode::LOAD_GLOBAL, sub_name);
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1);                           // [i]
    emit_named(cu, OpCode::LOAD_FIELD, x_name);   // [42]
    emit_global(cu, OpCode::DEF_GLOBAL, r3_name); // r3 = (Sub()).x(实例沿链读见遮蔽)
    cu.emit_op(OpCode::LOAD_NIL, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();

    auto fetch = [&](const char* n) { return m->globals().find(Value::from_obj(new_string(gc, n)))->value.as_int(); };
    EXPECT_EQ(fetch("r1"), 42); // Sub.x 取遮蔽值
    EXPECT_EQ(fetch("r2"), 1);  // Base.x 不变
    EXPECT_EQ(fetch("r3"), 42); // 实例沿链读见遮蔽
}

// 类上赋值动态新增(2026-09-11 改定,原 ClassWriteRejectsNewMember 的「无 monkey-patch
// 拒新增」废止):沿链全 miss 的新名字经 STORE_FIELD 落接收类自身表,类上读回即新值
//(父类不可见由 test_objclass 层的 base miss 断言覆盖)。
TEST_F(AriaVMStress, ClassWriteCreatesNewMember) {

    auto&     gc        = vm.gc();
    auto      fn        = new_function(gc, nullptr, 0);
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
    const u16 sub_name  = cu.add_constant(Value::from_obj(new_string(gc, "Sub")));
    const u16 x_name    = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    const u16 brand     = cu.add_constant(Value::from_obj(new_string(gc, "brand")));
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, base_name);
    emit_imm(cu, 1);
    emit_named(cu, OpCode::MAKE_STATIC, x_name);
    emit_global(cu, OpCode::DEF_GLOBAL, base_name);
    emit_global(cu, OpCode::LOAD_GLOBAL, base_name);
    emit_named(cu, OpCode::MAKE_CLASS, sub_name);
    emit_global(cu, OpCode::DEF_GLOBAL, sub_name);
    emit_global(cu, OpCode::LOAD_GLOBAL, sub_name); // [Sub]
    emit_imm(cu, 5);                                // [Sub, 5]
    emit_named(cu, OpCode::STORE_FIELD, brand);     // [5]:新名落 Sub 自身表(恒成功)
    cu.emit_op(OpCode::POP, 1);                     // []
    emit_global(cu, OpCode::LOAD_GLOBAL, sub_name); // [Sub]
    emit_named(cu, OpCode::LOAD_FIELD, brand);      // [5]:读回新值
    cu.emit_op(OpCode::RETURN, 1);                  // 返回 5

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 5);
}

// 方法经类上赋值改写(STORE_FIELD 类路径 = 静态写):旧解析沿用首解析绑定快照(bound);
// 改写后新解析读回**原值**不绑定(赋值闭包无 defining class 戳 ⟹ 非方法槽,2026-09-11
// 改定方法性看戳不看值类型),自由调用新值仍可执行。方法性随值携带:经类路径拷贝
// MAKE_METHOD 出品的戳定闭包(如 Foo.m = Base.m)保方法性,赋 lambda/裸函数降为静态。
TEST_F(AriaVMStress, MethodRewriteViaClassAssignmentSnapshot) {

    auto& gc      = vm.gc();
    auto  m       = make_module(gc);
    auto  m_guard = gc.make_guard(m);
    auto  old_m   = make_function(gc, m, "old_m", 0);
    auto  guard   = gc.make_guard(old_m);
    {
        old_m->unit().emit_op(OpCode::LOAD_IMM, 1);
        old_m->unit().emit_byte(1, 1);
        old_m->unit().emit_op(OpCode::RETURN, 1);
    }
    auto new_m = make_function(gc, m, "new_m", 0);
    guard.push(new_m);
    {
        emit_imm(new_m->unit(), 2);
        new_m->unit().emit_op(OpCode::RETURN, 1);
    } // new_m 返回 2

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&     cu      = fn->unit();
        const u16 foo     = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
        const u16 m_name  = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        const u16 b1_name = cu.add_constant(Value::from_obj(new_string(gc, "b1")));
        const u16 b2_name = cu.add_constant(Value::from_obj(new_string(gc, "b2")));
        const u16 r1_name = cu.add_constant(Value::from_obj(new_string(gc, "r1")));
        const u16 r2_name = cu.add_constant(Value::from_obj(new_string(gc, "r2")));
        // Foo:m = old_m
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, foo);
        emit_closure(cu, cu.add_constant(Value::from_obj(old_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, foo);
        // i1 = Foo();b1 = i1.m(首解析:绑 old 闭包 + 回填 fields 缓存)
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);         // [i1]
        cu.emit_op(OpCode::DUP, 1); // [i1, i1]
        emit_named(cu, OpCode::LOAD_FIELD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, b1_name); // [i1]
        // Foo.m = new_m 闭包(类上赋值改写方法槽)
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        emit_closure(cu, cu.add_constant(Value::from_obj(new_m)));
        emit_named(cu, OpCode::STORE_FIELD, m_name); // [new_m 值留栈]
        cu.emit_op(OpCode::POP, 1);                  // [i1]
        // i2 = Foo();b2 = i2.m(改写后的新解析:绑 new 闭包)
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [i1, i2]
        cu.emit_op(OpCode::DUP, 1);
        emit_named(cu, OpCode::LOAD_FIELD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, b2_name); // [i1]
        // r1 = b1()(旧快照 -> 1);r2 = b2()(新解析 -> 2)
        emit_global(cu, OpCode::LOAD_GLOBAL, b1_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);
        emit_global(cu, OpCode::DEF_GLOBAL, r1_name); // [i1]
        emit_global(cu, OpCode::LOAD_GLOBAL, b2_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);
        emit_global(cu, OpCode::DEF_GLOBAL, r2_name); // []
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();

    auto fetch = [&](const char* n) { return m->globals().find(Value::from_obj(new_string(gc, n)))->value; };
    EXPECT_EQ(fetch("r1").as_int(), 1); // 已解析实例沿用旧绑定(首解析快照)
    const auto b2_v = fetch("b2");
    EXPECT_EQ(b2_v.as_obj()->type(), aria::ObjType::CLOSURE);                  // 新解析:赋值闭包未戳 ⟹ 读回原值不绑定
    EXPECT_EQ(aria::Object::as<ObjClosure>(b2_v.as_obj())->function(), new_m); // === new_m 闭包本体
    EXPECT_EQ(fetch("r2").as_int(), 2);                                        // b2 自由调用仍执行新闭包(返回 2)
}

// super 不污染动态派发缓存(铁则 2):Sub.m 内 super.m() 取被覆写前的父实现、绑 this
// 后 CALL -- 若 super 命中被写进实例 fields,第二次 i.m() 会被缓存劫持到 Base.m
// (结果 1 而非 51)。两次调用均为 51 即未污染。
TEST_F(AriaVMStress, SuperCallDoesNotPolluteCache) {

    auto& gc      = vm.gc();
    auto  m       = make_module(gc);
    auto  m_guard = gc.make_guard(m);
    auto  base_m  = make_function(gc, m, "m", 0);
    auto  guard   = gc.make_guard(base_m);
    {
        base_m->unit().emit_op(OpCode::LOAD_IMM, 1);
        base_m->unit().emit_byte(1, 1);
        base_m->unit().emit_op(OpCode::RETURN, 1);
    }
    auto sub_m = make_function(gc, m, "m", 0);
    guard.push(sub_m);
    {
        auto&     smu    = sub_m->unit();
        const u16 m_name = smu.add_constant(Value::from_obj(new_string(gc, "m")));
        emit_named(smu, OpCode::LOAD_SUPER_FIELD, m_name);
        smu.emit_op(OpCode::CALL, 1);
        smu.emit_byte(0, 1); // [1](super.m() 的返回值)
        emit_imm(smu, 50);
        smu.emit_op(OpCode::ADD, 1); // [51]
        smu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
        const u16 sub_name  = cu.add_constant(Value::from_obj(new_string(gc, "Sub")));
        const u16 m_name    = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, base_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(base_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, base_name);
        emit_global(cu, OpCode::LOAD_GLOBAL, base_name);
        emit_named(cu, OpCode::MAKE_CLASS, sub_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(sub_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, sub_name);
        emit_global(cu, OpCode::LOAD_GLOBAL, sub_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);         // [i]
        cu.emit_op(OpCode::DUP, 1); // [i, i]
        emit_named(cu, OpCode::LOAD_FIELD, m_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);         // [i, 51](第一次:super 命中,若被污染缓存则此调用已劫持)
        cu.emit_op(OpCode::POP, 1); // [i]
        emit_named(cu, OpCode::LOAD_FIELD, m_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [51](第二次:若缓存被污染成 Base.m 则得 1)
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 51); // 两次均走 Sub.m(含 super 调父),缓存未被污染
}

// 深栈多临时值下的 STORE_FIELD 单槽下移:赋值两侧压多层临时值,若单槽下移
// (peek(1)=peek(0)+drop)错位(如弹双槽/少弹)后续 ADD 链即错乱。
TEST_F(AriaVMStress, StoreFieldDeepStackShift) {

    auto&     gc        = vm.gc();
    auto      m         = make_module(gc);
    auto      m_guard   = gc.make_guard(m);
    auto      fn        = make_function(gc, m, "<main>", 0);
    auto      guard     = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
    const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    const u16 x_name    = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    // 前言:Base:x = 0(静态,供深栈赋值改写)+ Foo(空类)
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, base_name);
    emit_imm(cu, 0);
    emit_named(cu, OpCode::MAKE_STATIC, x_name);
    emit_global(cu, OpCode::DEF_GLOBAL, base_name);
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, foo);
    emit_global(cu, OpCode::DEF_GLOBAL, foo);
    // 深栈:两层临时值(40/50)之下,先后做实例路径与类路径的 STORE_FIELD,
    // 单槽下移(peek(1)=peek(0)+drop)错位即 ADD 链/读回错乱;验证值经 globals 读回。
    const u16 r1_name = cu.add_constant(Value::from_obj(new_string(gc, "r1")));
    const u16 r2_name = cu.add_constant(Value::from_obj(new_string(gc, "r2")));
    emit_imm(cu, 40);                          // [40]
    emit_imm(cu, 50);                          // [40, 50]
    emit_global(cu, OpCode::LOAD_GLOBAL, foo); // 实例路径:i.x = 5(动态字段,即创建)
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1);         // [40, 50, i]
    cu.emit_op(OpCode::DUP, 1); // [40, 50, i, i](留副本跨写)
    emit_imm(cu, 5);
    emit_named(cu, OpCode::STORE_FIELD, x_name);     // [40, 50, i, 5](弹 obj 副本,值留栈)
    cu.emit_op(OpCode::POP, 1);                      // [40, 50, i](弹验证值)
    emit_named(cu, OpCode::LOAD_FIELD, x_name);      // obj=peek(0)=i ✓ -> [40, 50, 5](fields 命中)
    emit_global(cu, OpCode::DEF_GLOBAL, r1_name);    // r1 = i.x = 5
    emit_global(cu, OpCode::LOAD_GLOBAL, base_name); // 类路径:Base.x = 7(深栈下移)
    emit_imm(cu, 7);
    emit_named(cu, OpCode::STORE_FIELD, x_name);     // [40, 50, 7]
    cu.emit_op(OpCode::POP, 1);                      // [40, 50](弹类写验证值)
    cu.emit_op(OpCode::POP, 1);                      // [40]
    cu.emit_op(OpCode::POP, 1);                      // []
    emit_global(cu, OpCode::LOAD_GLOBAL, base_name); // 类写读回
    emit_named(cu, OpCode::LOAD_FIELD, x_name);      // [7]
    emit_global(cu, OpCode::DEF_GLOBAL, r2_name);    // []
    cu.emit_op(OpCode::LOAD_NIL, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();

    auto fetch = [&](const char* n) { return m->globals().find(Value::from_obj(new_string(gc, n)))->value.as_int(); };
    EXPECT_EQ(fetch("r1"), 5); // 实例写生效(fields 命中;未写则链 miss 报 UndefinedProperty)
    EXPECT_EQ(fetch("r2"), 7); // 类写生效(读穿透,接收类自身槽更新)
}

// 实例方法内 LOAD/STORE_THIS_FIELD 深栈正确性:this 不经栈(取帧槽 1),值 peek-store
// 留栈顶;方法帧中压多层临时值后读写 this 字段,栈高不错位。
TEST_F(AriaVMStress, ThisFieldDeepStackInMethod) {

    auto& gc    = vm.gc();
    auto  init  = new_function(gc, new_string(gc, "init"), 0);
    auto  guard = gc.make_guard(init);
    {
        auto&     icu    = init->unit();
        const u16 x_name = icu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_imm(icu, 5);
        emit_named(icu, OpCode::STORE_THIS_FIELD, x_name); // this.x = 5
        icu.emit_op(OpCode::POP, 1);
        emit_local(icu, OpCode::LOAD_LOCAL, 0);
        icu.emit_op(OpCode::RETURN, 1);
    }
    auto m = new_function(gc, new_string(gc, "m"), 0);
    guard.push(m);
    {
        auto&     mcu    = m->unit();
        const u16 x_name = mcu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_imm(mcu, 60);                                 // [60]
        emit_imm(mcu, 70);                                 // [60, 70]
        emit_named(mcu, OpCode::LOAD_THIS_FIELD, x_name);  // [60, 70, 5]
        emit_imm(mcu, 1);                                  // [60, 70, 5, 1]
        mcu.emit_op(OpCode::ADD, 1);                       // [60, 70, 6]
        emit_named(mcu, OpCode::STORE_THIS_FIELD, x_name); // [60, 70, 6](值留栈)
        mcu.emit_op(OpCode::ADD, 1);                       // [60, 76]
        mcu.emit_op(OpCode::ADD, 1);                       // [136]
        mcu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
        const u16 init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
        const u16 m_name    = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, foo);
        emit_closure(cu, cu.add_constant(Value::from_obj(init)));
        emit_named(cu, OpCode::MAKE_METHOD, init_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, foo);
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [i](init:x=5)
        emit_named(cu, OpCode::LOAD_FIELD, m_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [136](深栈下 this.x+1)
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 136);
}

// 实例字段遮蔽同名静态成员(铁则 3:fields 命中优先):init 内 this.x=9 落 fields 表,
// 实例读 fields 命中得 9;类路径读静态表原槽仍得 1。
TEST_F(AriaVMStress, InstanceFieldShadowsStatic) {

    auto& gc    = vm.gc();
    auto  init  = new_function(gc, new_string(gc, "init"), 0);
    auto  guard = gc.make_guard(init);
    {
        auto&     icu    = init->unit();
        const u16 x_name = icu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_imm(icu, 9);
        emit_named(icu, OpCode::STORE_THIS_FIELD, x_name);
        icu.emit_op(OpCode::POP, 1);
        emit_local(icu, OpCode::LOAD_LOCAL, 0);
        icu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = new_function(gc, nullptr, 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
        const u16 init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
        const u16 x_name    = cu.add_constant(Value::from_obj(new_string(gc, "x")));
        // Foo:x = 1(静态)+ init(this.x = 9)
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, foo);
        emit_imm(cu, 1);
        emit_named(cu, OpCode::MAKE_STATIC, x_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(init)));
        emit_named(cu, OpCode::MAKE_METHOD, init_name);
        emit_global(cu, OpCode::DEF_GLOBAL, foo);
        // (Foo()).x - Foo.x == 8:fields 命中优先,静态回退不被实例写波及
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                         // [i]
        emit_named(cu, OpCode::LOAD_FIELD, x_name); // [9](fields 命中)
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        emit_named(cu, OpCode::LOAD_FIELD, x_name); // [9, 1](类路径静态直读)
        cu.emit_op(OpCode::SUBTRACT, 1);            // [8]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 8);
}

// ---- M5 报错矩阵( LOAD_FIELD/STORE_FIELD/MAKE_CLASS/LOAD_SUPER_FIELD 运行期防线)----

// 对 nil 取字段 -> UndefinedProperty(nil 并入非对象统一文案「type Nil does not support
// field access」,2026-09-10 化简:field 族不再特判 NilDereference,类型名 Nil 已可辨识)。
TEST_F(AriaVMStress, LoadFieldOnNilErrors) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    cu.emit_op(OpCode::LOAD_NIL, 1); // [nil]
    emit_named(cu, OpCode::LOAD_FIELD, x);
    cu.emit_op(OpCode::RETURN, 1); // 不可达

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
}

// 对非对象非 nil 原语取字段 -> UndefinedProperty(文案:该类型不支持字段访问)。
TEST_F(AriaVMStress, LoadFieldOnNonObjectErrors) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_imm(cu, 1); // [1]
    emit_named(cu, OpCode::LOAD_FIELD, x);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
}

// 对 nil 写字段 -> UndefinedProperty(nil 并入非对象统一文案,同 LoadFieldOnNilErrors;
// obj peek(1) 不弹,先于取值失败)。
TEST_F(AriaVMStress, StoreFieldOnNilErrors) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 x        = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    cu.emit_op(OpCode::LOAD_NIL, 1); // [nil]
    emit_imm(cu, 1);                 // [nil, 1]
    emit_named(cu, OpCode::STORE_FIELD, x);
    cu.emit_op(OpCode::RETURN, 1); // 不可达

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
}

// MAKE_CLASS 栈顶 super 非类值 -> TypeMismatch(superclass 须为类)。
TEST_F(AriaVMStress, MakeClassNonClassSuperErrors) {

    auto&     gc       = vm.gc();
    auto      fn       = new_function(gc, nullptr, 0);
    auto      fn_guard = gc.make_guard(fn);
    auto&     cu       = fn->unit();
    const u16 foo      = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    emit_imm(cu, 1); // [1](非类值充当 super)
    emit_named(cu, OpCode::MAKE_CLASS, foo);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
}

// 类上 init 赋值为非可调用值:init Value 化(2026-09-10 整改)后赋值**放行**
// (store_field 同步 init_,表槽/init_ 一致),实例化时 call_value 分发报
// CallNonCallable 兜底 -- 原「init 仅认闭包」的赋值点 TypeMismatch 特例已退役。
TEST_F(AriaVMStress, ClassInitAssignNonCallableErrorsOnInstantiate) {

    auto&     gc        = vm.gc();
    auto      fn        = new_function(gc, nullptr, 0);
    auto      fn_guard  = gc.make_guard(fn);
    auto&     cu        = fn->unit();
    const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    const u16 init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, foo);        // [Foo]
    emit_global(cu, OpCode::DEF_GLOBAL, foo);       // []
    emit_global(cu, OpCode::LOAD_GLOBAL, foo);      // [Foo]
    emit_imm(cu, 5);                                // [Foo, 5](非可调用值)
    emit_named(cu, OpCode::STORE_FIELD, init_name); // [5](赋值放行:init_ 同步为 5)
    cu.emit_op(OpCode::POP, 1);                     // []
    emit_global(cu, OpCode::LOAD_GLOBAL, foo);      // [Foo]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 实例化:call_value(init_=5) -> CallNonCallable
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::CallNonCallable);
}

// 原 SuperOutsideMethodClosureFails / SuperInObjectMethodFails 两用例随 2026-09-10
// 整改退役:LOAD_SUPER_FIELD 的 defining==nullptr / superclass==nullptr 是编译器
// 保证的不变式(指令只在方法闭包体内发射;Object 的方法内用 super 由编译期
// SuperNoBaseClass 挡),运行期防线由可 catch 的 raise 降为 ASSERT -- 语言写不出
// 的形态直接 abort 暴露,不再构成运行期错误,无法以 interpret 结果断言。

// 类成员图跨 GC 存活(白盒):类/方法闭包/绑定/实例经共享模块 globals 根可达,
// run() 后显式 collect 字节数不减、对象图仍可遍历。stress 下每 new_object 已触发
// collect,此处再钉一层「run 外显式 collect 不误伤已发布进模块 globals 的类图」。
TEST_F(AriaVMStress, ClassGraphSurvivesExplicitCollect) {

    auto& gc      = vm.gc();
    auto  m       = make_module(gc);
    auto  m_guard = gc.make_guard(m);
    auto  init    = make_function(gc, m, "init", 0);
    auto  guard   = gc.make_guard(init);
    {
        auto&     icu = init->unit();
        const u16 tag = icu.add_constant(Value::from_obj(new_string(gc, "tag")));
        emit_imm(icu, 3);
        emit_named(icu, OpCode::STORE_THIS_FIELD, tag);
        icu.emit_op(OpCode::POP, 1);
        emit_local(icu, OpCode::LOAD_LOCAL, 0);
        icu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
        const u16 init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
        const u16 tag       = cu.add_constant(Value::from_obj(new_string(gc, "tag")));
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, foo);
        emit_closure(cu, cu.add_constant(Value::from_obj(init)));
        emit_named(cu, OpCode::MAKE_METHOD, init_name);
        emit_global(cu, OpCode::DEF_GLOBAL, foo);
        emit_global(cu, OpCode::LOAD_GLOBAL, foo);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);         // [i](init:tag=3)
        cu.emit_op(OpCode::DUP, 1); // [i, i]
        emit_named(cu, OpCode::LOAD_FIELD, init_name);
        emit_global(cu, OpCode::DEF_GLOBAL, init_name); // [i](b = i.init 绑定,入 globals)
        cu.emit_op(OpCode::POP, 1);                     // []
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();

    // run() 清场后经共享模块 globals 白盒取回绑定,验证类图根链完整。
    auto b_entry = m->globals().find(Value::from_obj(new_string(gc, "init")));
    ASSERT_NE(b_entry, nullptr);
    auto b = aria::Object::as<ObjBoundMethod>(b_entry->value.as_obj());
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->method_name(), "init"); // 非虚取名:闭包方法取 fn 名

    // run() 收尾 reset() 只清栈不回收:主入口闭包等栈级临时物已不可达但仍在堆上,
    // 先 collect 清掉 run 期遗留,再钉住类图基线 -- 第二次 collect 字节数须不变。
    gc.collect();
    const usize before = gc.bytes_allocated();
    gc.collect(); // 类值经 m_guard -> module -> globals -> 类表 -> init 闭包全链标根
    EXPECT_EQ(gc.bytes_allocated(), before);

    // collect 后对象图仍完好:绑定经 fields 缓存/表可达,字段值经协议读回一致
    //(整表访问器 fields() 已删,改走 load_field;命中路径纯查询无分配,读安全)。
    auto inst = aria::Object::as<ObjInstance>(b->receiver().as_obj());
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst->cls()->name()->view(), "Foo");
    auto tag_v = inst->load_field(vm, new_string(gc, "tag"));
    ASSERT_TRUE(tag_v.has_value());
    EXPECT_EQ(tag_v->as_int(), 3);
}

// ============================================================
// M5 泛化:native 落表(原生值经 MAKE_STATIC/类上赋值落类表 = 静态槽语义,读恒原值
// --方法性 = defining class 戳,2026-09-11 改定,原生方法绑定随 uniform 绑定废止;
// 绑定形态 ObjBoundMethod 的原生分支仍由对象层直接构造覆盖,见 test_objboundmethod)。
// ============================================================

// echo_this():验证原生的调用约定 -- 绑定调用(call_bound_method)时 VM 把调用区槽 0
// (bound 对象)覆写为 receiver 后调用:原生读 slots[0] 即 this(校验为 ObjInstance)、
// 返回值写 slots[0] 原地,实参槽位与自由调用一致(slots[1..argc],本例 0 参)。
bool echo_this_native(AriaVM& vm, Span<Value> slots) {
    if (slots.size() - 1 != 0) {
        return vm.fail(ErrorCode::WrongArity, "echo_this expects 0 args, got {}", slots.size() - 1);
    }
    if (!slots[0].is_obj() || slots[0].as_obj()->type() != aria::ObjType::INSTANCE) {
        return vm.fail(ErrorCode::TypeMismatch, "echo_this receiver is not an instance");
    }
    slots[0] = Value::from_int(1); // 返回 1:覆写 this 槽(返回槽即原 this 位)
    return true;
}

// 静态槽持可调用值(原生经 MAKE_STATIC,var 声明/类上赋值同形)经实例读取**原值直读**
//(方法性 = defining class 戳,2026-09-11 改定,不再按值类型绑定):读回值与常量池原生
// === 同一对象,无 ObjBoundMethod 包装。原「静态槽持可调用值同样绑定」(uniform 绑定,
// Python 函数语义)随方法性翻注册期戳废止。
TEST_F(AriaVMStress, StaticCallableReadsRawOnInstance) {

    auto&      gc       = vm.gc();
    auto       fn       = new_function(gc, nullptr, 0);
    auto       fn_guard = gc.make_guard(fn);
    auto&      cu       = fn->unit();
    const u16  foo      = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    const u16  f_name   = cu.add_constant(Value::from_obj(new_string(gc, "f")));
    const auto nf       = add_native_const(cu, gc, "echo_this", echo_this_native);
    cu.emit_op(OpCode::LOAD_OBJECT, 1);      // [Object]
    emit_named(cu, OpCode::MAKE_CLASS, foo); // [Foo]
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf, 1);                         // [Foo, native]
    emit_named(cu, OpCode::MAKE_STATIC, f_name); // 静态槽存原生(var 声明 lowering 同形)
    emit_global(cu, OpCode::DEF_GLOBAL, foo);    // []
    emit_global(cu, OpCode::LOAD_GLOBAL, foo);   // [Foo]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1);                         // [i](快路径:无 init)
    emit_named(cu, OpCode::LOAD_FIELD, f_name); // [raw-native](实例读取:非方法槽 ⟹ 原值)
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf, 1);                 // [raw, native](常量池原值)
    cu.emit_op(OpCode::STRICT_EQUAL, 1); // [true](同一对象,无绑定包装)
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_TRUE(out.value().as_bool());
}

// 类路径读取(静态访问)取出裸原生值,不绑定 -- 直调时 slots[0] = callee(原生自身),
// 非实例接收者,原生体内自查报 TypeMismatch。与实例路径(StaticCallableReadsRawOnInstance)
// 互补,钉死「类表可调用值不因值类型被绑定」的边界。
TEST_F(AriaVMStress, NativeSlotClassAccessIsFreeCall) {

    auto&      gc       = vm.gc();
    auto       fn       = new_function(gc, nullptr, 0);
    auto       fn_guard = gc.make_guard(fn);
    auto&      cu       = fn->unit();
    const u16  foo      = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    const u16  m_name   = cu.add_constant(Value::from_obj(new_string(gc, "m")));
    const auto nf       = add_native_const(cu, gc, "echo_this", echo_this_native);
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, foo);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf, 1);
    emit_named(cu, OpCode::MAKE_STATIC, m_name);
    emit_global(cu, OpCode::DEF_GLOBAL, foo);
    emit_global(cu, OpCode::LOAD_GLOBAL, foo);  // [Foo]
    emit_named(cu, OpCode::LOAD_FIELD, m_name); // [native](类路径:裸值,无绑定无缓存)
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 自由调用:slots[0] = callee(非实例) -> TypeMismatch
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
}

// super 读静态方法(2026-09-11 改定:fun 静态经 MAKE_STATIC 注册,不戳 defining class):
// LOAD_SUPER_FIELD 沿父链命中未戳闭包 -> 原值直读,与类路径裸读 === 同一闭包(方法性看
// 戳不看值类型;原「原生方法经 super 绑定」随方法性翻注册期戳退役)。
TEST_F(AriaVMStress, SuperReadsStaticMethodRaw) {

    auto& gc      = vm.gc();
    auto  m       = make_module(gc);
    auto  m_guard = gc.make_guard(m);
    auto  base_f  = make_function(gc, m, "f", 0);
    auto  guard   = gc.make_guard(base_f);
    {
        emit_imm(base_f->unit(), 7);
        base_f->unit().emit_op(OpCode::RETURN, 1); // Base.f 返回 7(未被本用例消费,钉可调用性)
    }
    auto sub_m = make_function(gc, m, "m", 0);
    guard.push(sub_m);
    {
        auto&     smu    = sub_m->unit();
        const u16 f_name = smu.add_constant(Value::from_obj(new_string(gc, "f")));
        const u16 b_name = smu.add_constant(Value::from_obj(new_string(gc, "Base")));
        emit_named(smu, OpCode::LOAD_SUPER_FIELD, f_name); // [raw](未戳闭包:原值直读)
        emit_global(smu, OpCode::LOAD_GLOBAL, b_name);     // [raw, Base]
        emit_named(smu, OpCode::LOAD_FIELD, f_name);       // [raw, raw2](类路径裸读)
        smu.emit_op(OpCode::STRICT_EQUAL, 1);              // [true](=== 同一闭包,无绑定包装)
        smu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
        const u16 sub_name  = cu.add_constant(Value::from_obj(new_string(gc, "Sub")));
        const u16 m_name    = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        const u16 f_name    = cu.add_constant(Value::from_obj(new_string(gc, "f")));
        // Base:fun f(MAKE_STATIC 静态方法,不戳 defining class)
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, base_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(base_f)));
        emit_named(cu, OpCode::MAKE_STATIC, f_name);
        emit_global(cu, OpCode::DEF_GLOBAL, base_name);
        // Sub : Base,覆写 m(实例方法,MAKE_METHOD 戳)
        emit_global(cu, OpCode::LOAD_GLOBAL, base_name);
        emit_named(cu, OpCode::MAKE_CLASS, sub_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(sub_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, sub_name);
        // (Sub()).m()
        emit_global(cu, OpCode::LOAD_GLOBAL, sub_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                         // [i]
        emit_named(cu, OpCode::LOAD_FIELD, m_name); // [bound](绑 Sub.m)
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [true]
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_TRUE(out.value().as_bool());
}

// super 读静态成员(2026-09-11 改定:LOAD_SUPER_FIELD 语义 = 沿父链读成员,方法性看
// defining class 戳):Base 静态 var x=1,Sub 覆写 m 内 super.x 命中静态值原值直读压栈
//(不绑定、不写 fields 缓存,铁则 2);返回值即父类静态,验证父链真被读到。
TEST_F(AriaVMStress, SuperReadsStaticMember) {

    auto& gc      = vm.gc();
    auto  m       = make_module(gc);
    auto  m_guard = gc.make_guard(m);
    auto  sub_m   = make_function(gc, m, "m", 0);
    auto  guard   = gc.make_guard(sub_m);
    {
        auto&     smu    = sub_m->unit();
        const u16 x_name = smu.add_constant(Value::from_obj(new_string(gc, "x")));
        emit_named(smu, OpCode::LOAD_SUPER_FIELD, x_name); // [1](静态值直读)
        smu.emit_op(OpCode::RETURN, 1);
    }

    auto fn = make_function(gc, m, "<main>", 0);
    guard.push(fn);
    {
        auto&     cu        = fn->unit();
        const u16 base_name = cu.add_constant(Value::from_obj(new_string(gc, "Base")));
        const u16 sub_name  = cu.add_constant(Value::from_obj(new_string(gc, "Sub")));
        const u16 m_name    = cu.add_constant(Value::from_obj(new_string(gc, "m")));
        const u16 x_name    = cu.add_constant(Value::from_obj(new_string(gc, "x")));
        // Base:x = 1(静态)
        cu.emit_op(OpCode::LOAD_OBJECT, 1);
        emit_named(cu, OpCode::MAKE_CLASS, base_name);
        emit_imm(cu, 1);
        emit_named(cu, OpCode::MAKE_STATIC, x_name);
        emit_global(cu, OpCode::DEF_GLOBAL, base_name);
        // Sub : Base(无成员),覆写 m:super.x 直读
        emit_global(cu, OpCode::LOAD_GLOBAL, base_name);
        emit_named(cu, OpCode::MAKE_CLASS, sub_name);
        emit_closure(cu, cu.add_constant(Value::from_obj(sub_m)));
        emit_named(cu, OpCode::MAKE_METHOD, m_name);
        emit_global(cu, OpCode::DEF_GLOBAL, sub_name);
        // (Sub()).m()
        emit_global(cu, OpCode::LOAD_GLOBAL, sub_name);
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1);                         // [i]
        emit_named(cu, OpCode::LOAD_FIELD, m_name); // [bound](绑 Sub.m)
        cu.emit_op(OpCode::CALL, 1);
        cu.emit_byte(0, 1); // [1](super.x 直读)
        cu.emit_op(OpCode::RETURN, 1);
    }

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 1);
}

// 原生 init 注册走类上赋值路径(MAKE_METHOD 2026-09-11 起仅收闭包,原生落表经
// STORE_FIELD):init Value 化(2026-09-10 整改)后**合法** -- set_field 命中 "init" 同步
// init_,实例化经 call_value 走 call_native:slots[0] = this(echo 校验接收者是实例后覆写
// 返回 1),原生 init 的返回值即实例化结果(嵌入 API 灵活性:不写槽即返回 this,写槽可
// 返回任意值)。
TEST_F(AriaVMStress, NativeInitInstantiates) {

    auto&      gc        = vm.gc();
    auto       fn        = new_function(gc, nullptr, 0);
    auto       fn_guard  = gc.make_guard(fn);
    auto&      cu        = fn->unit();
    const u16  foo       = cu.add_constant(Value::from_obj(new_string(gc, "Foo")));
    const u16  init_name = cu.add_constant(Value::from_obj(new_string(gc, "init")));
    const auto nf        = add_native_const(cu, gc, "echo_this", echo_this_native);
    cu.emit_op(OpCode::LOAD_OBJECT, 1);
    emit_named(cu, OpCode::MAKE_CLASS, foo);   // [Foo]
    emit_global(cu, OpCode::DEF_GLOBAL, foo);  // []
    emit_global(cu, OpCode::LOAD_GLOBAL, foo); // [Foo]
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf, 1);                            // [Foo, native]
    emit_named(cu, OpCode::STORE_FIELD, init_name); // [native](赋值放行:"init" 同步 init_)
    cu.emit_op(OpCode::POP, 1);                     // []
    emit_global(cu, OpCode::LOAD_GLOBAL, foo);      // [Foo]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 实例化:原生 init 收 slots[0]=this(echo 校验后返回 1)
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 1); // 原生 init 的返回值即实例化结果
}
