---
name: aria-util
description: aria 解释器 util 工具层模块参考：fs（路径/读盘）、utf8（码点编解码）、source_file（SourceFile/SourceLoc 位置类型）、io（print/println）、util（哈希/转义）、cli（参数解析器）。读写 src/util/** 或处理源文件加载、位置定位、命令行参数解析时使用。
paths:
  - "src/util/**"
---

# util 层模块参考

## `util/fs.hpp`

- **路径/读盘原语**：`read_file`/`current_dir`/`program_dir`/`absolute`/`resolve`（`weakly_canonical`）。
- **`module_name_and_dir(StringView path) -> Pair<String,String>{name,dir}`**：把文件路径拆为入口模块身份--`name` = basename 去最后一个扩展名（`.aria` 文件即得模块名，`path::stem()`）、`dir` = `dirname(absolute(path))`（`absolute` 失败退化为原路径）。纯路径工具，不读盘、不校验存在性；`name` 可能为空（目录/空/无文件名），调用方据空 `name` 判加载错误。
- **错误码**：`FsErrCode`；`detail::to_fserr(std::error_code)` 做 `std::error_code -> FsErrCode` 映射（经 `default_error_condition()`）；`detail::errno_to_fserr(int e)` 与无参重载（读当前线程 errno）做 errno -> `FsErrCode` 映射。
- `detail::executable_path()` 平台分流取当前可执行文件路径（`program_dir` 基础）。

## `util/utf8.hpp`

- `using codepoint = u32`；常量 `kReplacementChar`。
- `decode_one(str, offset=0)` -> `{codepoint, 字节数}`：非法序列返 `{kReplacementChar,1}`、`offset >= size()` 越界返 `{kReplacementChar,0}`。
- **ASCII 快路径就地内联、多字节交 `detail::decode_multibyte`（`ARIA_NOINLINE`）**：调用点绝大多数走 ASCII，多字节那套长度表/续接校验/组装/双重校验不随调用点展开（实测词法各形态快 3-12%，数字与探针见 `.claude/reference/compile/lexer-notes.md` §5）。
- `encode`/`is_valid`；`is_ascii(ch|u8)`（该字节是否 ASCII--源码字节经 char 或 u8 两种来路取，两个重载把窄化收进来，ASCII 边界只写一处）。
- `is_id_start`/`is_id_continue`/`is_whitespace`/`is_digit`/`is_alpha` 等 tokenizer 辅助（码点分类用区间近似，非完整 UCD）。

## `util/source_file.hpp`

- **`SourceFile`**：默认构造 + 3 参 `SourceFile(name, path, content)`；`from_path` 剥 BOM、CRLF->LF、UTF-8 校验，非法返 `FsErrCode::InvalidEncoding`；访问器 `name()`/`path()`/`content()` 返 `StringView`、`line_count()`。
- **位置类型 `LineCol`/`SourceLoc`（offsets-only）**：`SourceFile*` + 字节偏移；显式构造断言 src 非空，`source()`/`offset()` 取内部字段。行列是**派生量**：
  - `line()` 走 `SourceFile::line_at`（行表二分 + 单条行缓存），逐发射节点的热路径；
  - `line_col()`/`to_string()` 走 `locate`（列须数码点，O(行内码点数) 的**冷路径**，只应被错误渲染与测试调用，勿在逐 token 循环里读列）。
  - `to_string()` 渲染 `path:line:col`；空态（src=nullptr）返 0 偏移/0 行/空串（空态即「无位置」非「未知位置」，可与「空位置串 = 无前缀」的消费方直接组合、调用方无须先判 `source()`）。
- `line(n)`/`line_at(offset)`/`locate(offset)`：码点列；EOF（offset == 内容末尾）与越界钳制一律返「下一行第 1 列」（编辑器光标停末尾语义）。行表懒构建 + 行解析单条缓存（内容构造后不变 + 单线程编译，故缓存无需失效）。
- **生命期陷阱（跨模块，调用方必读）**：`SourceFile` 以引用/指针传入（`Lexer::tokenize(SourceFile&)`），`Token::loc_` 内的 `SourceLoc::src_`（经它查行表与缓存，故 `SourceFile` 亦须可就地读、地址稳定）与 `content()`/`name()`/`path()` 借出的 view 须在 `SourceFile` 存活且地址不变期间使用；`SourceFile` 含 `String content_`，**SSO 短串（短于阈值，如 `"_"`/`"f"`）move 后 data 地址会变**（SSO buffer 跟随对象，move 是逐字节拷贝）--实践：让 `SourceFile` 就位后再 tokenize，之后不再 move 该对象（放 `UPtr` 容器或长寿命成员）；多个 `SourceFile` 存容器并已取 view 后勿再增删致重分配。`Error` 不在此列（构造期把 `SourceLoc` 烘进自有 `message_` 串、不再持 `SourceFile*`，与 `SourceFile` 生命周期解耦）。

## `util/io.hpp`

`io::print`/`io::println`（`std::print`/`std::println` 的 using 别名）+ `readline(std::istream& = std::cin)`/`readchar(std::istream&)` 与 `readchar()`（读 `std::cin`）。

## `util/util.hpp`

`namespace aria::util`（与 fs/utf8 同）。

- **调试/信息**：`print_binary`（高位在前、按位分组打印对象表示）、`print_compiler_info`、`to_void_ptr`、`escape_string(StringView) -> String`（转义 `"`/`\`/`\n`/`\t`/`\r` 与控制字符为 `\x{HH}`，非 ASCII 透传）；`detail::emit_bytes` 为 `print_binary` 的打印辅助。
- **哈希**：`hash_num`/`hash_addr`（数值/地址哈希，经 `detail::splitmix64_mix32`（Vigna lowbias32，专为 32 位输出低偏置调优））、`hash_str`（字符串 FNV-1a 32-bit，经 `detail::fnv1a_32`）。
- **小端编解码**：`make_u16(u8,u8)`/`split_word(u16)`/`split_dword(u32)`。
- **栈/Opt 取值**：`pop_top(Stack<T>&) -> T`（取栈顶并弹出，`std::move` 取值避免重类型拷贝，调用方须保证栈非空，`[[nodiscard]]`）；`take(Opt<T>&) -> Opt<T>`（取走并置空，`std::exchange(opt, nullopt)` 收口，Rust `Option::take` 同名习语）。
- **拼接**：`join(Range, StringView delimiter, Fn&&)`（range 逐元素经 Fn 渲染后 delimiter 连接的共用底座，debug_repr 类拼接消费）。
- **下标换算**：`abs_diff(usize, usize)`（两下标距离：取大减小，无符号域不下溢；命名对齐 C++26 `std::abs_diff`）；`resolve_index(i64, usize)`（负下标从尾计数归一化 + 越界判定，`nullopt` 即越界；端点一律为实元素位置，无上界形态的末元素/空段折算在 `ObjRange::resolve_slice_bounds` 里做，不经此）；`resolve_position(i64, usize)`（插入位解析，`resolve_index` 的姊妹函数：同式从尾计数归一，唯上界放宽到 `== size` 即追加位（负数归一后至多 size-1，追加位只有正拼写），`list.insert` 消费）。
- **文本解析**：`parse_int_text(StringView) -> Opt<i64>` / `parse_float_text(StringView) -> Opt<f64>`--整串十进制**数据文本**解析：收前导 `[+-]`（`from_chars` 只认 `-`，`+` 由本函数剥）、整串须被消费完；不跳空白、不收下划线与进制前缀（那是源码字面量语法，见 `Lexer` 的 `parse_int` 一族，两套文法刻意不同）。`parse_int_text` 另闸语言 int 的 i48 域（`kIntMin`/`kIntMax` 来自 `aria.hpp`，越域返 `nullopt`--越域值经 `Value::from_int` 会被静默截尾）；`parse_float_text` 收 `inf`/`nan`、越 f64 域（如 `1e400`）返 `nullopt` 不饱和成 inf。失败一律 `nullopt`，兜底文案交调用方（string 的 as-int 方法处兜 nil）。

## `util/cli.hpp`

`namespace aria::util`，header-only 的命令行参数解析器 `Cli`。纯解析工具，不打印、不退出。

**定义/结果分离**

- `Cli` 仅持统一注册项（`parse` 为 `const`、可重复 parse），每次 `parse` 产出一个独立 `Cli::ParseResult`（public 嵌套类，`friend class Cli`；持单个结果数组 + 指回 `Cli` 的非拥有 `const Cli*`，借其注册项分派 `has`/`get`；调用方须保证 `Cli` 在 `ParseResult` 使用期存活）。
- **单一事实源**：注册查重与解析定位（`parse`/`has`/`get`/`help`）全部依赖 `defs_`，按 `long_name_`/`short_name_` 线性扫描（`register_name` 复用 `find_long`/`find_short` 查重，无平行索引结构）。
- **无 `clear()`**（`defs_` 不可变，builder 天然可复用）。

**统一枚举建模**

- public 嵌套 `enum class Slot : u8 { Empty, Flag, Option, Positional }` 贯穿定义/结果两侧。
- 私有嵌套 `Def`：`long_name_`（flag/option 长名 / positional 参数名）/`short_name_`（`'\0'` = 无，positional 不用）/`description_`/`default_value_`（option only，仅 help 展示）/`is_required_`（positional only）/`kind_`（`Slot`，构造 ASSERT 永非 `Empty`）。统一承载原 flag/option/positional 三类（无继承、无虚函数、按 `kind_` 区分哪些字段生效），经 `List<Def> defs_` 单表值存储--无 raw 指针，默认析构/拷贝/移动均正确（可安全拷贝/移动，`auto parser = Cli{...}` 及建造者链无约束）。
- 结果侧 `Cli::ParseResult` 持**单个** `List<SlotEntry> slots_`（私有嵌套 `SlotEntry{Slot state{Empty}; String value;}`，与 `defs_` 同序：第 n 位对应 `defs_[n]`；`state` = `Empty` 表未提供、种类值表已提供；`value` 存 option/positional 的值，flag 槽未用）+ `extra_args_`（单个结果数组同时承载 state 与 value，避免平行数组与 `vector<bool>` 特化坑）。

**positional 与名字空间**

- `add_positional` 调 `register_name`（长名入唯一长名空间、无短名），与 flag/option 共一个长名空间。
- 解析侧定位按 `kind_` 区分：`--name`/`-x` 经 `find_long_without_positional(StringView)`/`find_short(char)` 只匹配 flag/option（带 `kind_ != Positional` 过滤，未命中返 `nullopt`）；`has`/`get` 经 `find_long(StringView)` 查任意槽（长名全局唯一，单查即定位）。三者均返回 `Opt<usize>`。
- 位置参数填充靠扫描 `slots_` 找**首个「空且 `kind_ == Positional`」的槽**填入（以 `slots_` 填充状态为唯一事实源，无需计数器--Positional 按注册序填、填后不重置，「首个空 Positional 槽」恒等于「下一个待填槽」）；必填检查与 `help` 的 Arguments 分节/Usage 行均遍历 `defs_` 按 `kind_ == Positional` 过滤。
- **无 `positional_slots_` 派生成员**（positional 操作按 `slots_` 线性扫描 O(N)，启动一次可忽略）。

**链式构建器与查重**

- `add_flag(long_name, description, short='\0')`、`add_option(long_name, description, default="", short='\0')`（`get` 未命中返回 `nullopt`，调用方以 `value_or` 取 fallback）、`add_positional(name, description, is_required=true)`、`set_description`。
- 名字空间查重：跨/同 kind 重名或占用保留名 `--help`/`-h`（内置 help flag 构造时首个注册占用）注册期 ASSERT 拒绝（`register_name` 须在 `defs_` emplace 前调用；NDEBUG 下首个注册生效、后续重名照插、解析取首个匹配）；positional 无短名。

**`parse`：首错即止**

- 三重载（`argc`/`argv` 零拷贝跳 `argv[0]`（argc==0 防下溢）、`List<String>`、核心 `Span<const StringView>`，均 `const`）统一返回 `Result<ParseResult, String>`--有值 = 成功（含 `--help`/`-h` 命中内置 help flag 置位短路，经 `result.has("help")` 取），`unexpected` = 首个错误消息且解析即终止（定义/结果分离：错误不落 `Cli` 状态，重 parse 从新参数重新开始，各次 `ParseResult` 互不污染）。
- 核心 `parse` 仅做**按前缀分派**：私有 `enum class Step : u8 { Continue, ShortCircuit }` 表控制流，逐 token 交私有 handler--`parse_long(result, arg, args, i)`（`--name`/`--name=value`）、`parse_short(result, arg, args, i)`（`-abc` 簇 / `-oFILE` / `-o FILE`，遇取值选项即结束本组）、`parse_positional(result, arg)`（扫 `slots_` 填首个空 Positional 槽，单独 `-` 亦走此路）。前两者经 `find_long_without_positional`/`find_short` 定位槽，取值类可能前移 `i`（消费下一参数），命中 `--help`/`-h` 返回 `ShortCircuit`（主循环见之立即成功返回，跳过剩余参数与必填检查），遇错返 `unexpected`。
- 循环后 `check_required(result)` 查必填缺失（返 `Opt<String>` 错误消息）。
- **解析语义**：长选项 `--name value`/`--name=value`；短选项簇 `-abc`；取值 `-oFILE`/`-o FILE`（取值即结束本组扫描）；flag 不取值（`--flag=x` 的 x 忽略）；单独 `-` 按位置参数；位置参数按注册序；超额进 `result.extra_args()`；必填缺失报错（以 `slots_[k].state` 判定，区分「未提供」与「显式空串实参」）。

**访问与 help**

- `has`（是否在命令行提供/填充）/`get(name) -> Opt<String>`（按 `find_long` 单查定位后按 kind 分派：option/positional 已提供返值（含显式空串）、未提供或命中的是 flag（无值）返 `nullopt`；调用方以 `value_or` 取 fallback）/`extra_args()`。
- `help()`（在 `Cli` 上，渲染定义）：Usage 行（`[OPTIONS]` 恒展示，内置 help 必在）+ Arguments 分节（按注册序遍历 positional，遇首个才输出分节头，名字字段宽 16、描述对齐第 18 列、超长贴紧）+ Options 分节按**注册序**遍历 named 参数（flag/option，内置 help 首个注册、恒居首位）：先扫一遍算各前缀最大宽度（option 前缀含 `kOptValueSuffix = " <VALUE>"`），描述统一对齐到 `max_prefix+2` 列（所有描述同列、最长前缀得 2 空格间距），option 行尾再追加 `[default: ...]`；前缀经私有 static `render_prefix` 渲染（有短名 `  -x, --name` / 无短名 `    --name`）。不再 kind 分组、无前置 has_* 扫描遍。
