#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/code.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
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
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::OpCode;
using aria::Span;
using aria::u16;
using aria::u32;
using aria::u8;
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

    // IMPORT path:u16 alias:u16(均为常量池 ObjString 索引)。栈中性。
    void emit_import(CodeUnit& cu, u16 path_idx, u16 alias_idx, u32 line = 1) {
        cu.emit_op(OpCode::IMPORT, line);
        cu.emit_word(path_idx, line);
        cu.emit_word(alias_idx, line);
    }

    // 测试便利:M1 机制测试不关心模块归属,为每个函数造一个临时 "<script>" 模块
    // (满足「函数必属某模块」不变式)。需要真实模块归属的测试用 aria::new_function 显式传模块。
    // root_ 走 new_module 默认(当前工作目录,指针恒非空);run() 用其替换 source_roots_[0]。
    ObjModule* make_module(GC& gc) {
        return new_module(gc, new_string(gc, "<script>")); // root 缺省 -> cwd(失败时空串兜底)
    }

    // 3 参便利重载:造临时模块 + 委托 4 参 aria::new_function。屏蔽全局 aria::new_function。
    // 注:须先保 name 再 make_module -- make_module 内部分配在 stress GC 下会 collect,
    // 此时 name 仅为裸局部指针(无根)会被扫掉(原 4 参 aria::new_function 一进来就 guard name,
    // 此重载多了一步 make_module 故须提前保 name)。
    ObjFunction* new_function(GC& gc, ObjString* name, u8 arity) {
        auto  guard = gc.make_guard(name);
        auto* m     = make_module(gc);
        guard.push(m);
        return aria::new_function(gc, m, name, arity);
    }

    // ---- IMPORT 路径解析测试辅助(全量磁盘版:exists-check + 绝对规范键)----
    //
    // 设计(见 .claude/reference/runtime/import-path-resolution.md):IMPORT 把 specifier 经 resolve_module 解析为
    // 命中文件的绝对规范路径(weakly_canonical)作模块表键。ObjModule 持 (root_, name_):
    //   - root_ = 所属源根目录(如 base);name_ = 相对源根的路径(如 lib/utils)。
    //   - 模块绝对路径(= 模块表键)由 root_ + name_ 合成:root_ + "/" + name_ + ".aria"。
    //   - run() 把入口模块 root_ 播种为 source_roots_[0];相对导入基 = root_ + dirname(name_)。
    // 故测试需:
    //   1. 用真实临时文件让 resolve_module 的 exists-check 命中(testing::TempDir 下建空 .aria);
    //   2. 按解析出的绝对键预注册合成模块入 modules_;
    //   3. 给入口/目标模块设 root_(源根目录 base)+ name_(相对路径)。
    // macOS 下 testing::TempDir() 常落在 /var/... -> /private/var/... 符号链接后,而
    // resolve_module 经 weakly_canonical 解析符号链接,故基准须先规范化,保证预注册键与
    // resolve_module 输出逐字节一致。

    // 取本测试专用的规范绝对路径(解析符号链接):在 testing::TempDir() 下按「套件名_用例名」
    // 建独立子目录。gtest 的 TempDir() 是整个测试程序共享、跨运行不清理的同一目录,若各测试
    // 直接在其下建同路径文件(如 lib/math.aria),残留文件会污染依赖「该文件不存在」的用例
    // (典型:ImportBareSearchesSourceRoots 需入口根无 lib/math.aria,但其它用例会建同名文件)。
    // 按用例名隔离后,每测试独占一目录,互不污染;同测试重跑时自建文件幂等覆盖。
    std::string test_canon_dir() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto dir = std::filesystem::weakly_canonical(std::filesystem::path{testing::TempDir()} /
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

    // 造带源根的模块(name + root):name = 相对源根的路径,root = 所属源根目录(intern 的 ObjString*,非空)。
    // 妥善处理临时根:name 先 intern,guard 后再让 new_module 内部 guard 两者(分配 new_object 顶部
    // 的 maybe_collect 可能回收未被根持有的串)。调用方须先 guard 已创建的 root(本函数内
    // new_string(name) 分配时 root 须已入根)。new_module 本身对 nullptr root 会默认 cwd,但本
    // 辅助的用例都需精确控制源根,故一律显式传 root。
    ObjModule* new_disk_module(GC& gc, std::string_view name, ObjString* root) {
        auto* nm    = new_string(gc, name);
        auto  guard = gc.make_guard(nm);
        guard.push(root);
        return new_module(gc, nm, root);
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
        auto* nm    = new_string(gc, name);
        auto  guard = gc.make_guard(nm); // name 是 weak root,new_native_fn 顶 maybe_collect 前先保
        auto* nf    = new_native_fn(gc, nm, fn);
        return cu.add_constant(Value::from_obj(nf));
    }

} // namespace

TEST(AriaVM, Arithmetic) {
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
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

TEST(AriaVM, F64ConstantAndPromotion) {
    AriaVM     vm;
    auto&      gc  = vm.gc();
    auto*      fn  = new_function(gc, nullptr, 0);
    auto&      cu  = fn->unit();
    const auto c25 = cu.add_constant(Value::from_f64(2.5));
    const auto c05 = cu.add_constant(Value::from_f64(0.5));
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

TEST(AriaVM, WhileLoopWithJumps) {
    // slot1 = i(3 递减), slot2 = acc;while i > 0 { acc += i; i -= 1 } 返回 acc = 3+2+1 = 6
    // 序言的两次 LOAD_NIL 预留局部区(slots[1..3)):槽 0 是 callee,临时值在保留区之上压栈不覆写局部。
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
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

TEST(AriaVM, FunctionCall) {
    AriaVM vm;
    auto&  gc  = vm.gc();
    auto*  add = new_function(gc, new_string(gc, "add"), 2);
    auto&  acu = add->unit();
    emit_local(acu, OpCode::LOAD_LOCAL, 1); // 参数 1 在槽 1(槽 0 是 callee)
    emit_local(acu, OpCode::LOAD_LOCAL, 2); // 参数 2 在槽 2
    acu.emit_op(OpCode::ADD, 1);
    acu.emit_op(OpCode::RETURN, 1);

    auto*      main_fn = new_function(gc, nullptr, 0);
    auto&      mcu     = main_fn->unit();
    const auto idx     = mcu.add_constant(Value::from_obj(add));
    mcu.emit_op(OpCode::LOAD_CONST, 1);
    mcu.emit_word(idx, 1);
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

TEST(AriaVM, StackGrowsAndRebasesFrames) {
    // 压入超过初始容量(1024)的临时值触发值栈 2x 增长;增长后读取局部,验证帧的 slots 指针已重定位。
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
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

TEST(AriaVM, TruthinessAndShortCircuit) {
    // false || true -> true(JUMP_TRUE_OR_POP 落空弹,压 true)
    {
        AriaVM vm;
        auto&  gc = vm.gc();
        auto*  fn = new_function(gc, nullptr, 0);
        auto&  cu = fn->unit();
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
        AriaVM vm;
        auto&  gc = vm.gc();
        auto*  fn = new_function(gc, nullptr, 0);
        auto&  cu = fn->unit();
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
        AriaVM vm;
        auto&  gc = vm.gc();
        auto*  fn = new_function(gc, nullptr, 0);
        auto&  cu = fn->unit();
        cu.emit_op(OpCode::LOAD_NIL, 1);
        cu.emit_op(OpCode::NOT, 1);
        cu.emit_op(OpCode::RETURN, 1);

        const auto out = vm.run(fn);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(out.value().as_bool());
    }
}

TEST(AriaVM, EqualitySemantics) {
    // 1 == 1.0 内容相等为 true;1 === 1.0 严格相等为 false
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
    emit_imm(cu, 1);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(cu.add_constant(Value::from_f64(1.0)), 1);
    cu.emit_op(OpCode::EQUAL, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out.value().as_bool());
}

TEST(AriaVM, TypeMismatchIsUncaught) {
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
    cu.emit_op(OpCode::LOAD_NIL, 1);
    emit_imm(cu, 1);
    cu.emit_op(OpCode::ADD, 1); // nil + 1 -> TypeMismatch
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
}

TEST(AriaVM, DivisionByZeroIsUncaught) {
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
    emit_imm(cu, 1);
    emit_imm(cu, 0);
    cu.emit_op(OpCode::DIVIDE, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::DivisionByZero);
}

TEST(AriaVM, WrongArityIsUncaught) {
    AriaVM vm;
    auto&  gc  = vm.gc();
    auto*  two = new_function(gc, new_string(gc, "two"), 2);
    auto&  tcu = two->unit();
    tcu.emit_op(OpCode::LOAD_NIL, 1);
    tcu.emit_op(OpCode::RETURN, 1);

    auto* fn = new_function(gc, nullptr, 0);
    auto& cu = fn->unit();
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(cu.add_constant(Value::from_obj(two)), 1);
    emit_imm(cu, 1); // 只给 1 个参数
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(1, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::WrongArity);
}

TEST(AriaVM, StackOverflowOnRunawayRecursion) {
    // fn 直接调用自己(arity 0),永不返回 -> 值栈/帧栈溢出
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(cu.add_constant(Value::from_obj(fn)), 1); // 常量池引用自己
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::StackOverflow);
}

// 模块表是 GC 根:collect 经 VM 根 tracer -> modules_.trace 标全部模块,
// 模块进而 trace name_/entry_/globals_,整条链存活。
TEST(AriaVM, ModuleTableIsGcRoot) {
    AriaVM vm;
    auto&  gc = vm.gc();

    auto* path = new_string(gc, "lib/utils");
    auto* m    = new_module(gc, path);
    // 入模块表(键=path,值=m)
    auto* e  = vm.modules().upsert(Value::from_obj(path));
    e->value = Value::from_obj(m);

    // 给模块挂体 + 一条全局绑定,验证经模块表根 -> 模块 trace -> 子节点存活
    auto* body = aria::new_function(gc, m, nullptr, 0); // body 属于 m
    m->set_entry(body);
    auto* g_key = new_string(gc, "g");
    auto* g_val = new_string(gc, "a long global value string!!!");
    auto* ge    = m->globals().upsert(Value::from_obj(g_key));
    ge->value   = Value::from_obj(g_val);

    const usize before = gc.bytes_allocated();
    gc.collect(); // 模块表=VM 根 -> 标 path+m -> m.trace 标 entry/globals -> 全存活
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(m->name()->view(), "lib/utils");
    EXPECT_EQ(m->entry(), body);
    EXPECT_EQ(g_val->view(), "a long global value string!!!");
}

// DEF_GLOBAL 是唯一创建模块全局的入口(顶层 var 声明):弹值,以常量池 name 为键 upsert 入
// 当前模块 globals。LOAD_GLOBAL 按名查表压入。
TEST(AriaVM, DefAndLoadGlobal) {
    AriaVM    vm;
    auto&     gc = vm.gc();
    auto*     fn = new_function(gc, nullptr, 0);
    auto&     cu = fn->unit();
    const u16 x  = cu.add_constant(Value::from_obj(new_string(gc, "x")));
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
TEST(AriaVM, StoreGlobalUpdatesExisting) {
    AriaVM    vm;
    auto&     gc = vm.gc();
    auto*     fn = new_function(gc, nullptr, 0);
    auto&     cu = fn->unit();
    const u16 x  = cu.add_constant(Value::from_obj(new_string(gc, "x")));
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
TEST(AriaVM, StoreGlobalUndefinedErrors) {
    AriaVM    vm;
    auto&     gc = vm.gc();
    auto*     fn = new_function(gc, nullptr, 0);
    auto&     cu = fn->unit();
    const u16 x  = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_imm(cu, 1);                          // [1]
    emit_global(cu, OpCode::STORE_GLOBAL, x); // 未定义 -> UndefinedVariable(peek 不弹)
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// LOAD_GLOBAL 未定义 -> UndefinedVariable 运行时错误。
TEST(AriaVM, LoadGlobalUndefinedErrors) {
    AriaVM    vm;
    auto&     gc = vm.gc();
    auto*     fn = new_function(gc, nullptr, 0);
    auto&     cu = fn->unit();
    const u16 x  = cu.add_constant(Value::from_obj(new_string(gc, "x")));
    emit_global(cu, OpCode::LOAD_GLOBAL, x); // 未定义 -> UndefinedVariable
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// IMPORT 裸名解析命中(磁盘 exists-check + 模块表查重):入口源根(base)下建真实空文件
// lib/utils.aria,预注册其绝对键模块(Loading 态模拟循环导入命中半初始化对象);
// import "lib/utils" as Utils -> resolve 沿 source_roots 命中入口根文件 -> 查表命中 ->
// 以 alias 绑入当前模块 globals。LOAD_GLOBAL 取回绑入的预注册模块对象。
TEST(AriaVM, ImportBindsPreRegisteredModule) {
    AriaVM vm;
    vm.set_source_roots({}); // 隔离:仅入口根,免默认 stdlib 干扰
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    // 源根 = base:入口与目标模块均属此源根(root_ = base),name_ 为相对 base 的路径。
    auto* root_ptr   = new_string(gc, base);
    auto  root_guard = gc.make_guard(root_ptr);
    // 目标文件 base/lib/utils.aria -> 其规范绝对路径即模块表键 K(resolve 命中此键)。
    const auto key_str = touch_aria(base, "lib/utils.aria");
    auto*      key     = new_string(gc, key_str);
    root_guard.push(key);
    auto* m = new_module(gc, new_string(gc, "lib/utils"), root_ptr); // 目标:root=base, name=lib/utils
    root_guard.push(m);                                              // 保 m 过 modules_.upsert 的 hash 分配
    m->set_state(ObjModule::ModuleState::Loading);
    auto* me  = vm.modules().upsert(Value::from_obj(key));
    me->value = Value::from_obj(m);

    auto*     mod       = new_disk_module(gc, "main", root_ptr); // 入口:root=base, name=main
    auto*     fn        = aria::new_function(gc, mod, nullptr, 0);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/utils")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "Utils")));
    emit_import(cu, path_idx, alias_idx);            // IMPORT "lib/utils" as Utils;栈中性
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), m); // 取回的是预注册模块对象
}

// IMPORT 解析失败(无源根命中 nope/missing.aria)且无嵌入层加载 -> ModuleNotFound。
// 入口根(base)与 stdlib(已置空)均无该文件,resolve_module 返回 nullopt。
TEST(AriaVM, ImportNotFoundErrors) {
    AriaVM vm;
    vm.set_source_roots({}); // 隔离:仅入口根
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto*     root_ptr   = new_string(gc, base); // 入口源根 = base(run 播种 source_roots[0] = base)
    auto      root_guard = gc.make_guard(root_ptr);
    auto*     fn         = aria::new_function(gc, new_disk_module(gc, "main", root_ptr), nullptr, 0);
    auto&     cu         = fn->unit();
    const u16 path_idx   = cu.add_constant(Value::from_obj(new_string(gc, "nope/missing")));
    const u16 alias_idx  = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx, alias_idx);
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::ModuleNotFound);
}

// IMPORT 规范化:预注册 base/lib/utils.aria,用 "lib/./utils" 导入 -- resolve_module 经
// weakly_canonical 折叠 ".",归一为 base/lib/utils.aria,命中同一绝对键,取回预注册模块对象。
TEST(AriaVM, ImportNormalizesAbsolutePath) {
    AriaVM vm;
    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto*      root_ptr   = new_string(gc, base);
    auto       root_guard = gc.make_guard(root_ptr);
    const auto key_str    = touch_aria(base, "lib/utils.aria");
    auto*      key        = new_string(gc, key_str);
    root_guard.push(key);
    auto* m = new_module(gc, new_string(gc, "lib/utils"), root_ptr);
    root_guard.push(m);
    auto* me  = vm.modules().upsert(Value::from_obj(key));
    me->value = Value::from_obj(m);

    auto*     fn        = aria::new_function(gc, new_disk_module(gc, "main", root_ptr), nullptr, 0);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/./utils")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "Utils")));
    emit_import(cu, path_idx, alias_idx);            // IMPORT "lib/./utils" as Utils
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), m); // 折 "." 后命中同一模块
}

// IMPORT 相对路径解析:导入函数所属模块 root_ = base、name_ = lib/main,"./helper" 相对当前
// 模块目录(root_ + dirname(name_) = base/lib)解析 -> base/lib/helper.aria,命中预注册的该绝对键模块。
// 相对导入基 = root_ + dirname(name_),caller-local,不碰 source_roots。
TEST(AriaVM, ImportNormalizesRelativePath) {
    AriaVM vm;
    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    // 源根 = base:导入方/入口模块 root_ = base、name_ = lib/main(相对基 = base/lib)。
    auto* root_ptr   = new_string(gc, base);
    auto  root_guard = gc.make_guard(root_ptr);
    // 目标 base/lib/helper.aria -> 绝对键 K。
    const auto key_str = touch_aria(base, "lib/helper.aria");
    auto*      key     = new_string(gc, key_str);
    root_guard.push(key);
    auto* helper = new_module(gc, new_string(gc, "lib/helper"), root_ptr);
    root_guard.push(helper);
    auto* he  = vm.modules().upsert(Value::from_obj(key));
    he->value = Value::from_obj(helper);

    auto*     mod       = new_disk_module(gc, "lib/main", root_ptr);
    auto*     fn        = aria::new_function(gc, mod, nullptr, 0);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "./helper")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "H")));
    emit_import(cu, path_idx, alias_idx);            // IMPORT "./helper" as H
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
TEST(AriaVM, ImportBareSearchesSourceRoots) {
    AriaVM     vm;
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto* root_ptr   = new_string(gc, base); // 入口源根 = base
    auto  root_guard = gc.make_guard(root_ptr);
    // stdlib 第二根 = base/stdlib(建目录);文件只放 stdlib,base 下不放 -> 强制 fall-through。
    const auto stdlib_dir = std::filesystem::weakly_canonical(std::filesystem::path{base} / "stdlib").string();
    std::filesystem::create_directories(stdlib_dir);
    auto* stdlib_ptr = new_string(gc, stdlib_dir); // 目标模块所属源根 = stdlib
    root_guard.push(stdlib_ptr);
    const auto key_str = touch_aria(stdlib_dir, "lib/math.aria"); // = base/stdlib/lib/math.aria
    vm.set_source_roots({stdlib_dir});

    auto* key = new_string(gc, key_str);
    root_guard.push(key);
    auto* target = new_module(gc, new_string(gc, "lib/math"), stdlib_ptr); // 目标:root=stdlib, name=lib/math
    root_guard.push(target);
    auto* te  = vm.modules().upsert(Value::from_obj(key));
    te->value = Value::from_obj(target);

    auto*     fn        = aria::new_function(gc, new_disk_module(gc, "main", root_ptr), nullptr, 0); // 入口:root=base
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/math"))); // 裸路径
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx, alias_idx);            // IMPORT "lib/math" as M
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
TEST(AriaVM, ImportStripsAriaSuffix) {
    AriaVM vm;
    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto*      root_ptr   = new_string(gc, base);
    auto       root_guard = gc.make_guard(root_ptr);
    const auto key_str    = touch_aria(base, "lib/math.aria");
    auto*      key        = new_string(gc, key_str);
    root_guard.push(key);
    auto* m = new_module(gc, new_string(gc, "lib/math"), root_ptr);
    root_guard.push(m);
    auto* me  = vm.modules().upsert(Value::from_obj(key));
    me->value = Value::from_obj(m);

    auto*     fn        = aria::new_function(gc, new_disk_module(gc, "main", root_ptr), nullptr, 0);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "lib/math.aria")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx, alias_idx);            // IMPORT "lib/math.aria" as M
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), m); // 剥 .aria 后命中同一文件键
}

// IMPORT 相对路径 + .aria 后缀组合:导入方 root_ = base、name_ = lib/main,"./math.aria"
// 相对当前模块目录(root_ + dirname(name_) = base/lib)解析 -> 剥 .aria 再补回 ->
// base/lib/math.aria,命中预注册目标。与 ImportStripsAriaSuffix(裸名)互补:验证 .aria 剥离在
// 相对路径上同样生效。
TEST(AriaVM, ImportStripsAriaSuffixOnRelative) {
    AriaVM vm;
    vm.set_source_roots({});
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto*      root_ptr   = new_string(gc, base);
    auto       root_guard = gc.make_guard(root_ptr);
    const auto key_str    = touch_aria(base, "lib/math.aria");
    auto*      key        = new_string(gc, key_str);
    root_guard.push(key);
    auto* target = new_module(gc, new_string(gc, "lib/math"), root_ptr);
    root_guard.push(target);
    auto* te  = vm.modules().upsert(Value::from_obj(key));
    te->value = Value::from_obj(target);

    auto*     mod       = new_disk_module(gc, "lib/main", root_ptr);
    auto*     fn        = aria::new_function(gc, mod, nullptr, 0);
    auto&     cu        = fn->unit();
    const u16 path_idx  = cu.add_constant(Value::from_obj(new_string(gc, "./math.aria")));
    const u16 alias_idx = cu.add_constant(Value::from_obj(new_string(gc, "M")));
    emit_import(cu, path_idx, alias_idx);            // IMPORT "./math.aria" as M
    emit_global(cu, OpCode::LOAD_GLOBAL, alias_idx); // [module]
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_obj());
    EXPECT_EQ(aria::Object::as<ObjModule>(out.value().as_obj()), target); // 相对 + 剥 .aria -> base/lib/math
}

// 源根列表在 run() 时按 [入口模块 root_, stdlib 目录] 播种:入口模块 root_ = base、name_ = main
// -> 入口源根 = base(绝对路径);stdlib 置空隔离 -> source_roots = [base]。
// 对齐 Python sys.path[0] = 入口脚本所在目录(此处入口源根即 root_,键 = 绝对规范路径)。
TEST(AriaVM, SourceRootSeededFromEntryModuleDir) {
    AriaVM vm;
    vm.set_source_roots({}); // 隔离:仅入口源根
    auto&      gc   = vm.gc();
    const auto base = test_canon_dir();

    auto* root_ptr   = new_string(gc, base);
    auto  root_guard = gc.make_guard(root_ptr);
    auto* mod        = new_disk_module(gc, "main", root_ptr); // 入口:root=base, name=main
    auto* fn         = aria::new_function(gc, mod, nullptr, 0);
    auto& cu         = fn->unit();
    cu.emit_op(OpCode::HALT, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(vm.source_roots().size(), 1u);
    EXPECT_EQ(vm.source_roots()[0], base); // 入口源根 = base(绝对)
}

// 源根列表:默认 <script> 合成模块经 make_module 持 root_ = 当前工作目录(保证非空),
// run() 用其原地替换入口槽 [0](构造时占位的 cwd);stdlib 置空隔离 -> source_roots = [cwd]。
// 对齐 Python sys.path[0] = 入口脚本所在目录 -- 合成入口无明确目录时退化为 cwd 占位。
TEST(AriaVM, SourceRootSeededWithCwdForScriptEntry) {
    AriaVM vm;
    vm.set_source_roots({}); // 隔离:仅入口槽,免默认 stdlib
    auto& gc = vm.gc();

    auto* fn = new_function(gc, nullptr, 0); // make_module -> <script>, root_ = cwd
    auto& cu = fn->unit();
    cu.emit_op(OpCode::HALT, 1);

    const auto out = vm.run(fn);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(vm.source_roots().size(), 1u);
    EXPECT_EQ(vm.source_roots()[0], std::filesystem::current_path().string()); // 入口槽 = cwd
}

// ---- 原生函数:CALL 命中 ObjNativeFn,同步调用,返回值写槽 0 ----

// double(21) == 42:验证原生函数读 args(slots[1])、写返回槽(slots[0]),VM drop(argc) 后栈顶即返回值。
TEST(AriaVM, NativeFnSlot0Return) {
    AriaVM     vm;
    auto&      gc     = vm.gc();
    auto*      fn     = new_function(gc, nullptr, 0);
    auto&      cu     = fn->unit();
    const auto nf_idx = add_native_const(cu, gc, "double", double_native);
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
TEST(AriaVM, NativeFnZeroArity) {
    AriaVM vm;
    auto&  gc = vm.gc();
    auto*  fn = new_function(gc, nullptr, 0);
    auto&  cu = fn->unit();
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
TEST(AriaVM, NativeFnSideChannelError) {
    AriaVM     vm;
    auto&      gc     = vm.gc();
    auto*      fn     = new_function(gc, nullptr, 0);
    auto&      cu     = fn->unit();
    const auto nf_idx = add_native_const(cu, gc, "fail_always", fail_always_native);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf_idx, 1); // [nf]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 调用 -> raise
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value()) << "expected error, got value";
    const auto& err = out.error();
    EXPECT_EQ(err.code(), ErrorCode::TypeMismatch);
    EXPECT_NE(err.message().find("fail_always always fails"), std::string::npos);
}

// 原生函数元数自查:double 收 0 参时经 vm.fail 报 WrongArity。
TEST(AriaVM, NativeFnAritySelfCheck) {
    AriaVM     vm;
    auto&      gc     = vm.gc();
    auto*      fn     = new_function(gc, nullptr, 0);
    auto&      cu     = fn->unit();
    const auto nf_idx = add_native_const(cu, gc, "double", double_native);
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(nf_idx, 1); // [nf]
    cu.emit_op(OpCode::CALL, 1);
    cu.emit_byte(0, 1); // 调 double() 传 0 参 -> WrongArity
    cu.emit_op(OpCode::RETURN, 1);

    const auto out = vm.run(fn);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::WrongArity);
}
