---
name: aria-add-builtin
description: 给 aria 加内建方法、静态原语或新内建类时使用。列出完整同步面：方法体与方法表、make_class 注册口（实例方法 vs 不绑定的静态原语）、receiver<T> 与 obj_tag 守卫、arity_error 元数口、返回值写槽与 GC 根、语料与文档（含 docs/guide/17-builtin-reference.md）。
---

# 加内建方法 / 静态原语 / 内建类

> 场景：给某个内建类型加方法，或加一个新的 bootstrap 类。机制背景见 `.agents/reference/object.md`「内置类型的成员面」
> 与 `.agents/reference/runtime.md`「类与对象 bootstrap」。

## 1. 先判定是哪种成员

| 形态 | 注册口 | 绑 this？ |
| :--- | :--- | :--- |
| 实例方法（多数） | `Builtin::register_class_methods` | 是（戳 `defining_class_`） |
| 静态原语（如 `Coroutine.create`） | `Builtin::register_class_functions` | 否（不戳定义类） |
| 全局函数（`type`/`str`/…） | `Builtin::register_builtins` 填 `builtins_` | 无接收者 |

## 2. 六步（缺一项就是半成品）

1. **方法体**：住 `src/runtime/builtins/<XxxClass>.cpp` 的**匿名命名空间**，签名 `bool fn(const NativeCallInfo& info)`，函数体首行 `auto& ctx = info.ctx();`。它是原生叶子：**不得操作值栈簿记**，实参经 `info[i]` 只读，返回值经 `info.set_return_val(...)` 就地写槽 0。
2. **方法表**：同一匿名命名空间的 `constexpr BuiltinFnEntry kXxxBuiltins[] = {{"name", &fn}, ...}`。新类的方法表可以住宿主类头的 `static constexpr`（`CoroutineClass` 先例）。
3. **注册**：在 `XxxClass::make_class` 里调对应注册口。**新类**还要三处：`runtime/value_register.hpp` 类段加一行（格位/名表/访问器同源生成）、`AriaVM::bootstrap_registers` 里建类填格、`builtins_` 裸名曝光（键 = 类名的 PascalCase）。
4. **接收者守卫**：要取 `this` 就写 `info.receiver<ObjXxx>()`（按 `obj_tag` 收窄，miss 报 `TypeMismatch` 转 nullptr）。**新增会被 `receiver<T>()` 收窄的类型必须同批扩 `Object.hpp` 的 `obj_tag<T>()`**，否则命中 `STATIC_UNREACHABLE`--那是编译期炸，不是运行期错。
5. **元数与报错**：`ctx.arity_error(argc, n)` / `arity_error_range` / `arity_error_at_least` 三形态，**只给数量、不手写格式串**；文案走句式家族（技能 `aria-error-messages`）。
6. **GC**：分配后立刻 `set_return_val` 发布（栈即根）；`set_return_val` 之前跨过分配点持有的中间对象要 `make_guard`。真 GC 点只有 `new_object` 顶部的 `maybe_collect`。

## 3. 验收面

- 正向语料：`tests/language/positive/<层>/<主题>.aria`，`assert` 收口；**只要方法打印了东西就得配 `.out`**（逐字节，含末尾换行）。
- 负向语料：元数错 / 类型错写 `negative/runtime_errors/*.aria` + `.err`（只取稳定子串，禁绝对路径与行号）。
- 白盒：跨模块行为（GC 根、绑定形态、`receiver` 守卫）补 `tests/runtime/` 用例。

## 4. 文档

- `.agents/reference/object.md`「内置类型的成员面」：改类面清单（新方法/新类都要）。
- 新类：`reference/runtime.md`「类与对象 bootstrap」的类清单与格数一并改。
- **`docs/guide/17-builtin-reference.md`**：人类文档逐条列内建方法，必须同步（它是面向读者的验收面）。
- `README.md` 的语言特性表按「特性」粒度写，单个方法一般不动。
