---
name: aria-util
description: aria 解释器 util 工具层模块参考：fs（路径/读盘）、utf8（码点编解码）、source_file（SourceFile/SourceLoc 位置类型）、io（print/println）、util（哈希/转义）、cli（参数解析器）。读写 src/util/** 或处理源文件加载、位置定位、命令行参数解析时使用。
paths:
  - "src/util/**"
---

# util 层模块参考

## `util/fs.hpp`

- **路径/读盘原语**：`read_file`/`current_dir`/`program_dir`/`absolute`/`resolve`（`weakly_canonical`）。
- **`module_name_and_dir(StringView path) -> Pair<String,String>{name,dir}`**：把文件路径拆为入口模块身份--`name` = basename 去最后一个扩展名（`path::stem()`）、`dir` = `dirname(absolute(path))`（`absolute` 失败退化为原路径）。纯路径工具，不读盘、不校验存在性；`name` 可能为空，调用方据空 `name` 判加载错误。
- **错误码**：`FsErrCode`；`detail::to_fserr(std::error_code)` 做映射（经 `default_error_condition()`）；`detail::errno_to_fserr(int e)` 与无参重载（读当前线程 errno）；`detail::executable_path()` 四平台分流取当前可执行文件路径（`program_dir` 基础）。

## `util/utf8.hpp`

- `using codepoint = u32`；常量 `kReplacementChar`。
- `decode_one(str, offset=0)` -> `{codepoint, 字节数}`：合法序列返真实码点与字节数；非法序列返 `{kReplacementChar, 1}`（只吞一个坏字节）；`offset >= size()` 越界返 `{kReplacementChar, 0}` 不读取。
- **ASCII 快路径就地内联、多字节交 `detail::decode_multibyte`（`ARIA_NOINLINE`）**：调用点绝大多数走 ASCII，多字节那套长度表/续接校验/组装/双重校验不随调用点展开（实测词法各形态快 3-12%，数字与探针见 `.claude/reference/compile/lexer-notes.md` §5）。
- `encode`/`is_valid`；`is_ascii(ch|u8)`（该字节是否 ASCII--源码字节经 char 或 u8 两种来路取，两个重载把窄化收进来，ASCII 边界只写一处）。
- `is_id_start`/`is_id_continue`/`is_whitespace`/`is_digit`/`is_alpha` 等 tokenizer 辅助（码点分类用区间近似，非完整 UCD）。

## `util/source_file.hpp`

- **`SourceFile`**：默认构造 + 3 参 ctor；`from_path` 剥 BOM、CRLF->LF、UTF-8 校验，非法返 `FsErrCode::InvalidEncoding`；访问器 `name()`/`path()`/`content()` 返 `StringView`、`line_count()`。
- **位置类型 `LineCol`/`SourceLoc`（offsets-only）**：`SourceFile*` + 字节偏移；行列是**派生量**。`line()` 走 `SourceFile::line_at`（行表二分 + 单条行缓存），逐发射节点的热路径；`line_col()`/`to_string()` 走 `locate`（列须数码点，O(行内码点数) 的**冷路径**，只应被错误渲染与测试调用，勿在逐 token 循环里读列）。
- `to_string()` 渲染 `path:line:col`；空态（src=nullptr）返 0 偏移/0 行/空串（空态即「无位置」非「未知位置」，可与「空位置串 = 无前缀」的消费方直接组合）。
- `line(n)`/`line_at(offset)`/`locate(offset)`：EOF（offset == 内容末尾）与越界钳制一律返「下一行第 1 列」（编辑器光标停末尾语义）。行表懒构建 + 行解析单条缓存（内容构造后不变 + 单线程编译，故缓存无需失效）。
- **生命期陷阱（跨模块，调用方必读）**：`SourceFile` 以引用/指针传入（`Lexer::tokenize(SourceFile&)`），`Token::loc_` 内的 `SourceLoc::src_`（经它查行表与缓存，故 `SourceFile` 亦须可就地读、地址稳定）与 `content()`/`name()`/`path()` 借出的 view 须在 `SourceFile` 存活且地址不变期间使用；`SourceFile` 含 `String content_`，**SSO 短串 move 后 data 地址会变**（SSO buffer 跟随对象）--实践：让 `SourceFile` 就位后再 tokenize，之后不再 move 该对象；多个 `SourceFile` 存容器并已取 view 后勿再增删致重分配。`Error` 不在此列（构造期把 `SourceLoc` 烘进自有 `message_` 串，与 `SourceFile` 生命周期解耦）。

## `util/io.hpp`

`io::print`/`io::println`（`std::print`/`std::println` 的 using 别名，全库输出统一入口）+ `readline(std::istream& = std::cin)`/`readchar(std::istream&)` 与 `readchar()`（读 `std::cin`；当前全库零消费者，REPL 行读取走 isocline，见 `main.cpp`）。

## `util/util.hpp`

`namespace aria::util`（与 fs/utf8 同）。

- **调试/信息**：`print_binary`（高位在前、按位分组打印表示；当前零消费者）、`print_compiler_info`（当前零消费者）、`to_void_ptr`、`escape_string(StringView) -> String`（转义 `"`/`\`/`\n`/`\t`/`\r` 与控制字符为 `\x{HH}`，非 ASCII 透传；供 `ObjString` debug/repr 渲染）。
- **哈希**：`hash_num`/`hash_addr`（数值/地址哈希，经 `detail::splitmix64_mix32`（Vigna lowbias32，专为 32 位输出低偏置调优））、`hash_str`（字符串 FNV-1a 32-bit，经 `detail::fnv1a_32`；二参重载自前缀终态续算，`hash(a)` 续算 b 即 `hash(a+b)`）。
- **小端编解码**：`make_u16(u8,u8)`/`split_word(u16)`/`split_dword(u32)`（`split_dword` 当前零消费者）。
- **栈/Opt 取值**：`pop_top(Stack<T>&) -> T`（取栈顶并弹出，调用方须保证栈非空，`[[nodiscard]]`）；`take` 重载族（取走并置空，`std::exchange` 习语，`[[nodiscard]]` 返被取值）：`take(Opt<T>&) -> Opt<T>`（置 `nullopt`）、`take(T*&) -> T*`（置 `nullptr`）、`take(UnsignedInteger T&) -> T`（归零；concept `UnsignedInteger = is_unsigned_v && !is_same_v<bool>`，bool 特意排除）、`take(bool&) -> bool`（置 `false`，具名重载两态语义）。
- **拼接**：`join(Range, StringView delimiter, Fn&&)`（range 逐元素经 Fn 渲染后 delimiter 连接的共用底座，debug_repr 类拼接消费）；`concat_string(StringView, StringView)`（两段拼接成新串，一次定容两段 append；`new_string` 两段重载未命中臂与模块加载文件名拼接消费）。
- **下标换算**：`abs_diff(usize, usize)`（两下标距离，无符号域不下溢；命名对齐 C++26 `std::abs_diff`）；`resolve_index(i64, usize)`（负下标从尾计数归一化 + 越界判定，`nullopt` 即越界；无上界形态的末元素/空段折算在 `ObjRange::resolve_slice_bounds` 里做）；`resolve_position(i64, usize)`（插入位解析，`resolve_index` 的姊妹函数：同式归一，唯上界放宽到 `== size` 即追加位，`list.insert` 消费）。
- **文本解析**：`parse_int_text(StringView) -> Opt<i64>` / `parse_float_text(StringView) -> Opt<f64>`--整串十进制**数据文本**解析：收前导 `[+-]`、整串须被消费完；不跳空白、不收下划线与进制前缀（那是源码字面量语法，见 `Lexer` 的 `parse_int` 一族，两套文法刻意不同）。`parse_int_text` 另闸语言 int 的 i48 域（越域返 `nullopt`--越域值经 `Value::from_int` 会被静默截尾）；`parse_float_text` 收 `inf`/`nan`、越 f64 域返 `nullopt` 不饱和成 inf。失败一律 `nullopt`，兜底文案交调用方。

## `util/cli.hpp`

`namespace aria::util`，header-only 的命令行参数解析器 `Cli`。纯解析工具，不打印、不退出。

**定义/结果分离**

- `Cli` 仅持统一注册项（`parse` 为 `const`、可重复 parse），每次 `parse` 产出一个独立 `Cli::ParseResult`（public 嵌套类，`friend class Cli`；持结果数组 + 非拥有 `const Cli*`，故调用方须保证 `Cli` 存活到结果用完）。
- **单一事实源**：注册查重与解析定位（`parse`/`has`/`get`/`help`）全部依赖 `defs_` 线性扫描，无平行索引结构；`defs_` 注册后不可变（无 `clear()`），builder 天然可复用。

**统一枚举建模**

- public 嵌套 `enum class Slot : u8 { Empty, Flag, Option, Positional }` 贯穿定义/结果两侧。
- 私有嵌套 `Def` 统一承载 flag/option/positional 三类（无继承、无虚函数，按 `kind_` 区分哪些字段生效；`kind_` 构造 ASSERT 永非 `Empty`），经 `List<Def> defs_` 单表值存储--无 raw 指针，默认析构/拷贝/移动均正确。
- 结果侧 `ParseResult` 持单个 `List<SlotEntry>`（私有 `SlotEntry{Slot state; String value;}`，与 `defs_` 同序；`state` = `Empty` 表未提供）+ 独立的 `extra_args_`（`List<String>`，超额位置参数）--state 与 value 同装单数组，避免平行数组与 `vector<bool>` 特化坑。

**positional 与名字空间**

- `add_positional` 的长名与 flag/option 共一个长名空间（无短名）。解析侧定位按 `kind_` 区分：`--name`/`-x` 经 `find_long_without_positional`/`find_short` 只匹配 flag/option；`has`/`get` 经 `find_long` 查任意槽。
- 位置参数填充靠扫描 `slots_` 找**首个「空且 `kind_ == Positional`」的槽**（以 `slots_` 填充状态为唯一事实源，无需计数器）；必填检查与 `help` 的 Arguments 分节按 `kind_ == Positional` 过滤。

**链式构建器与查重**

- `add_flag(long_name, description, short='\0')`、`add_option(long_name, description, default="", short='\0')`、`add_positional(name, description, is_required=true)`、`set_description`；`get` 未命中返回 `nullopt`，调用方以 `value_or` 取 fallback。
- 名字空间查重：跨/同 kind 重名或占用保留名 `--help`/`-h`（内置 help flag 构造时首个注册占用）注册期 ASSERT 拒绝（`register_name` 须在 `defs_` emplace 前调用；NDEBUG 下首个注册生效）。

**`parse`：首错即止**

- 三重载（`argc`/`argv` 零拷贝跳 `argv[0]` 且 argc==0 防下溢、`List<String>`、核心 `Span<const StringView>`，均 `const`）统一返回 `Result<ParseResult, String>`--有值 = 成功（含 `--help`/`-h` 命中内置 help flag 置位短路，经 `result.has("help")` 取），`unexpected` = 首个错误消息且解析即终止（错误不落 `Cli` 状态，重 parse 从新参数重新开始）。
- 核心 `parse` 仅做**按前缀分派**（私有 `enum class Step : u8 { Continue, ShortCircuit }`），逐 token 交私有 handler `parse_long`/`parse_short`/`parse_positional`；命中 `--help`/`-h` 返回 `ShortCircuit`（跳过剩余参数与必填检查），循环后 `check_required` 查必填缺失。
- **解析语义**：长选项 `--name value`/`--name=value`；短选项簇 `-abc`；取值 `-oFILE`/`-o FILE`（取值即结束本组）；flag 不取值；单独 `-` 按位置参数；位置参数按注册序；超额进 `extra_args()`；必填缺失报错（以 `slots_[k].state` 判定，区分「未提供」与「显式空串实参」）。

**访问与 help**

- `has`/`get(name) -> Opt<String>`（option/positional 已提供返值（含显式空串）、未提供或命中的是 flag 返 `nullopt`）/`extra_args()`。
- `help()`（在 `Cli` 上）：Usage 行（`[OPTIONS]` 恒展示）+ Arguments 分节（按注册序遍历 positional）+ Options 分节按**注册序**遍历 named 参数（内置 help 恒居首位），描述统一对齐到各前缀最大宽度 + 2 列，option 行尾追加 `[default: ...]`。
