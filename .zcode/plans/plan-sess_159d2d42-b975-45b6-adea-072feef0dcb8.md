## 结论

`CallFrame::last_off`（u32 偏移）改为存 `const u8* last_ip`（指令起始指针）**可行**：执行期 `code` 缓冲恒定（非移动 GC + CodeUnit 是 ObjFunction 值成员 + 执行期零 emit + IMPORT 编译新建 unit），指针不悬空；消费端（报错行号、M3 unwind 查表）全在冷路径，查表时一次减法反推 offset；CallFrame 尺寸不变（48B），不破坏 FrameStack 的 trivially-copyable 约束。`TryRecord`/行号表保持 offset 键不改。

## 改动清单

**代码（4 处）**
1. `src/runtime/Movement.hpp:24`：`u32 last_off;` → `const u8* last_ip;`，注释改为「本帧最近取指指令的起始指针（主循环取指前写；报错定位行号 / M3 unwind 查表锚点，查表时按 last_ip - unit->code.data() 反推 offset；前提：执行期 code 缓冲恒定——非移动 GC + 执行期零 emit）」。不加 NSDMI。
2. `src/runtime/Movement.cpp:25`：`f.last_off = 0;` → `f.last_ip = f.ip;`（f.ip 上一行已赋值 = code 起始），注释说明不用 nullptr（与 data() 相减是 UB）。
3. `src/runtime/AriaVM.cpp:688-694`：`frame.last_off = static_cast<u32>(...)` → `frame.last_ip = frame.ip;`，注释重写（去掉「u32 截断安全 4GB」段，改为「offset 由查表冷路径按 last_ip 与 unit->code.data() 相减反推」）。
4. `src/runtime/AriaVM.cpp:163`：`line_for_offset(frame.last_off)` → `line_for_offset(static_cast<usize>(frame.last_ip - frame.unit->code.data()))`；同步 :153、:157、:626 注释里的 last_off 措辞。

**注释同步（2 文件 3 处）**
5. `src/runtime/AriaVM.hpp:120`、`:207`；`src/object/ObjNativeFn.hpp:39`——last_off → last_ip 措辞。

**文档同步（3 文件，项目规则强制）**
6. `.claude/rules/runtime.md:13`：CallFrame 字段描述。
7. `.claude/reference/runtime/exception-implementation-pitfalls.md`：机械改名（:13/:23-32/:42-46/:53-69/:75-88/:146/:336-338/:360-374/:390/:407-408/:430），更新代码片段与 struct 草图，并在坑 #1/#2 或 B2 处补一句表示决策（存指针、查表时反推 offset、依赖「执行期 code 恒定」不变式）。
8. `.claude/reference/runtime/vm-design.md:92/:130/:153/:157`。

## 验证

- `clang++ -std=c++23 -I src -fsyntax-only` 快检改动头文件；
- `cmake --build build --target aria_tests -j` + `ctest --test-dir build --output-on-failure` 全绿（tests 无直接引用 last_off；test_ariavm.cpp 的 `<script>:N:` 行号断言验证语义等价）。