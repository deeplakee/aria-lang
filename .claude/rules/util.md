---
paths:
  - "src/util/**"
---

# util 层模块参考

- `util/fs.hpp`：`read_file`/`current_dir`/`program_dir`/`absolute`/`resolve`（`weakly_canonical`）。错误码 `FsErrCode`；`detail::to_fserr(std::error_code)` 做 `std::error_code`->`FsErrCode` 映射（经 `default_error_condition()`），`detail::errno_to_fserr(int e=errno)` 做 errno->`FsErrCode` 映射（无参重载读当前线程 errno）；`detail::executable_path()` 平台分流取当前可执行文件路径（`program_dir` 基础）。
- `util/utf8.hpp`：`using codepoint = u32`；常量 `kMaxSeqLen = 4`（UTF-8 最大序列字节）、`kReplacementChar`。`decode_one(str, offset=0)`->`{codepoint, 字节数}`（非法序列返 `{kReplacementChar,1}`、`offset >= size()` 越界返 `{kReplacementChar,0}`）、`decode(StringView)`->`List<codepoint>`（整串解码，非法序列替换为 `kReplacementChar`）、`encode`/`is_valid`/`count`/`view(str)` 迭代器（`iterator` 为前向迭代器，`byte_offset()` 取当前字节偏移）、`is_id_start`/`is_id_continue`/`is_whitespace`/`is_digit`/`is_alpha`/`is_alnum` 等 tokenizer 辅助（码点分类用区间近似，非完整 UCD）。
- `util/source_file.hpp`：`SourceFile`（默认构造 + 3 参 `SourceFile(name, path, content)`；`from_path` 剥 BOM、CRLF->LF、UTF-8 校验，非法返 `FsErrCode::InvalidEncoding`；访问器 `name()`/`path()`/`content()` 返 `StringView`、`line_count()`）；位置类型 `SourceSpan`/`LineCol`/`SpanLines`/`SourceLoc`（`SourceFile*`+`LineCol`，显式构造断言 src 非空，`to_string()` 渲染 `path:line:col`，空态/无效行列渲染为 `?`；`SourceLoc::source()`/`line_col()`/`line()` 取内部字段，`line()` 直返 `line_col_.line`）；`line(n)`/`locate(offset)`（码点列）/`locate_span()`，行表懒构建。
- `util/io.hpp`：`io::print`/`io::println`（`std::print`/`std::println` 的 using 别名）+ `readline(std::istream& = std::cin)`/`readchar(std::istream&)` 与 `readchar()`（读 `std::cin`）。
- `util/util.hpp`（`namespace aria::util`，同 fs/utf8）：`print_binary`（高位在前、按位分组打印对象表示）、`print_compiler_info`、`to_void_ptr`、`escape_string(StringView)`->`String`（转义 `"`/`\`/`\n`/`\t`/`\r` 与控制字符为 `\x{HH}`，非 ASCII 透传）、`hash_num`/`hash_addr`（数值/地址哈希，经 `detail::splitmix64_mix32`（Vigna lowbias32，专为 32 位输出低偏置调优））、`hash_str`（字符串 FNV-1a 32-bit，经 `detail::fnv1a_32`）；小端编解码 `make_u16(u8,u8)`/`split_word(u16)`/`split_dword(u32)`；`pop_top(Stack<T>&)->T`（取栈顶并弹出，`std::move` 取值避免重类型拷贝，调用方须保证栈非空，`[[nodiscard]]`）；`detail::emit_bytes` 为 `print_binary` 的打印辅助。