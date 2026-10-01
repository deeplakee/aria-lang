#ifndef ARIA_SOURCE_FILE_HPP
#define ARIA_SOURCE_FILE_HPP

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include "common.hpp"
#include "fs.hpp"
#include "utf8.hpp"

namespace aria::src {

    namespace stdfs = std::filesystem;

    // 行/列位置（1-based，符合大多数编辑器与编译器习惯）。
    struct LineCol {
        u32 line = 1; // 行号，从 1 开始
        u32 col  = 1; // 列号，从 1 开始；按码点计数，对中文源码友好
    };

    // 源文件信息：保存文件名、路径与内容。内容由 SourceFile 以 String 持有所有权，解析阶段可经
    // name()/path()/content() 取 StringView 直接引用，避免拷贝。生命期纪律：取出的 StringView 不得
    // 比所引用的 SourceFile 活得更久；构造完成后不要修改内容；多个 SourceFile 存入容器（如 List）
    // 且已取出 StringView 时，后续增删致重分配会移动内部 String --短串（SSO）会改变字符地址而使
    // StringView 悬空。from_path 加载处理：剥除前导 UTF-8 BOM（EF BB BF）；行尾归一化为 LF
    // （CRLF/CR -> LF）；校验内容为合法 UTF-8，非法则返回 InvalidEncoding。
    class SourceFile {
    public:
        SourceFile() = default;

        SourceFile(String name, String path, String content) noexcept :
            name_{std::move(name)}, path_{std::move(path)}, content_{std::move(content)} {}

        [[nodiscard]]
        StringView name() const noexcept {
            return name_;
        }

        [[nodiscard]]
        StringView path() const noexcept {
            return path_;
        }

        // 文件内容。底层 String 以 '\0' 结尾，便于需要哨兵的扫描逻辑。
        // 本类不做归一化：经 from_path 构造时已剥 BOM 并归一化 LF；直接三参构造时为调用方所给原文。
        [[nodiscard]]
        StringView content() const noexcept {
            return content_;
        }

        // 行数。与 wc -l 在"内容以 LF 结尾"时一致；最后一行即便没有结尾 LF 也算一行；
        // 末尾的 LF 不产生额外的空行。空内容返回 0。
        [[nodiscard]]
        u32 line_count() const {
            ensure_line_starts();
            return static_cast<u32>(line_starts_.size());
        }

        // 取第 line 行（1-based）的内容（不含行尾 LF）。越界返回空串。
        [[nodiscard]]
        StringView line(const u32 line) const {
            ensure_line_starts();
            if (line == 0 || line > line_starts_.size()) {
                return {};
            }
            const u32 begin = line_starts_[line - 1];
            // 到行尾 LF 前为止；不能直接用下一行起点 - 1，末行可能没有行尾 LF。
            u32 end = begin;
            while (end < content_.size() && content_[end] != '\n') {
                ++end;
            }
            return StringView{content_.data() + begin, end - begin};
        }

        // 将字节偏移解析为 1-based 行号（行表二分 + 单条行缓存 LineCache；offset 超出范围时钳制到内容末尾）。
        // offset == content.size()（EOF）返回 line_count() + 1（对齐 locate 的「EOF 落在下一行第 1 列」）。
        // AST 遍历按源序逐节点求行号、缓存命中率极高，故这条热路径只需一次区间比较。
        [[nodiscard]]
        u32 line_at(const u32 offset) const {
            if (offset >= content_.size()) {
                return line_count() + 1;
            }
            if (offset >= cache_.begin && offset < cache_.end) {
                return cache_.line;
            }
            ensure_line_starts();
            // upper_bound 给出第一个起始偏移 > offset 的行；其计数即 1-based 行号，下标 - 1 即行首。
            const u32 line = static_cast<u32>(std::ranges::upper_bound(line_starts_, offset) - line_starts_.begin());
            const u32 end  = line < line_starts_.size() ? line_starts_[line] : static_cast<u32>(content_.size());
            cache_         = LineCache{.begin = line_starts_[line - 1], .end = end, .line = line};
            return line;
        }

        // 将字节偏移解析为 1-based 的 (行, 列)。offset 超出范围时钳制到内容末尾；
        // offset == content.size()（EOF）返回下一行第 1 列（line_count()+1, 1）。
        // 列须数码点，故本方法是 O(行内码点数) 的冷路径（行号部分走 line_at 的缓存）--冷路径调用纪律见 SourceLoc 类注。
        [[nodiscard]]
        LineCol locate(u32 offset) const {
            if (offset > content_.size()) {
                offset = static_cast<u32>(content_.size());
            }
            if (offset == content_.size()) {
                return {line_count() + 1, 1};
            }
            const u32 line  = line_at(offset);
            const u32 begin = line_starts_[line - 1];
            return {line, count_codepoints(content_, begin, offset) + 1};
        }

        // 从磁盘读取并构造：fs::read_file 读原始字节，剥 BOM、CRLF/CR 归一化为 LF，校验 UTF-8
        // （非法返回 InvalidEncoding）。name 取路径 basename；读取失败原样返回 fs 错误码。
        [[nodiscard]]
        static Result<SourceFile, fs::FsErrCode> from_path(const StringView path) {
            auto content = fs::read_file(path);
            if (!content) {
                return std::unexpected(content.error());
            }
            String path_str{path};
            String name_str   = stdfs::path{path_str}.filename().string();
            auto   normalized = normalize(*content);
            if (!normalized) {
                return std::unexpected(fs::FsErrCode::InvalidEncoding);
            }
            return SourceFile{std::move(name_str), std::move(path_str), std::move(*normalized)};
        }

    private:
        String name_;
        String path_;
        String content_;

        // 懒构建：line_starts_[i] 是第 i+1 行在 content_ 中的起始字节偏移。语义遵循主流惯例：
        // 一个"行"要么以 LF 结尾，要么是到 EOF 的一段内容（"a\nb" -> [0, 2]）；末尾的 LF 不产生
        // 额外的空行起点（"a\n" -> [0]），空内容则行表为空（"" -> []）。
        mutable List<u32> line_starts_;
        mutable bool      is_line_starts_built_ = false;

        // line_at 的单条行缓存：最近一次解析出的行区间 [begin, end) 与该行行号；三字段皆 0 即「无缓存」。
        // 内容构造后不变、编译单线程，故缓存无需失效。
        struct LineCache {
            u32 begin = 0;
            u32 end   = 0;
            u32 line  = 0;
        };
        mutable LineCache cache_;

        void ensure_line_starts() const {
            if (is_line_starts_built_) {
                return;
            }
            line_starts_.clear();
            if (content_.empty()) {
                is_line_starts_built_ = true;
                return;
            }
            line_starts_.push_back(0);
            for (u32 i = 0; i + 1 < content_.size(); ++i) {
                if (content_[i] == '\n') {
                    line_starts_.push_back(i + 1);
                }
            }
            is_line_starts_built_ = true;
        }

        // 统计 content 在 [begin, end) 内的码点数（用于把字节列换算成码点列）
        static u32 count_codepoints(const StringView content, const u32 begin, const u32 end) {
            u32 n = 0;
            u32 i = begin;
            while (i < end) {
                const auto [_, len] = utf8::decode_one(content, i);
                i += len;
                ++n;
            }
            return n;
        }

        // 剥除前导 BOM、CRLF/CR -> LF，并校验 UTF-8 合法性。
        // 非法 UTF-8 返回空（调用方据此返回 InvalidEncoding）。
        [[nodiscard]]
        static Opt<String> normalize(const StringView raw) {
            StringView s = raw;
            if (s.size() >= 3 && static_cast<u8>(s[0]) == 0xEF && static_cast<u8>(s[1]) == 0xBB &&
                static_cast<u8>(s[2]) == 0xBF) {
                s.remove_prefix(3);
            }
            if (!utf8::is_valid(s)) {
                return std::nullopt;
            }

            String out;
            out.reserve(s.size());
            for (usize i = 0; i < s.size(); ++i) {
                if (const char c = s[i]; c == '\r') {
                    out.push_back('\n');
                    if (i + 1 < s.size() && s[i + 1] == '\n') {
                        ++i; // 跳过 CRLF 中的 \n
                    }
                } else {
                    out.push_back(c);
                }
            }
            return out;
        }
    };

    // 源码位置：源文件指针 + 字节偏移。行列是**派生量**，位置状态只有偏移这一件事--扫描器热路径只推
    // 游标、不在推进时维护计数，回退/前瞻天然自由，也无「某条推进路径漏记账」的 bug 面。行列代价挪到
    // 消费点：line() 走行表二分 + 单条行缓存（逐节点调用的热路径）；line_col() / to_string() 列要在
    // 行内数码点，是 O(行内码点数) 的冷路径，只应被错误渲染与测试调用（不要在逐 token 循环里读列，
    // 那会把 O(n^2) 放回来）。src 为非拥有指针，不得比所引 SourceFile 活得更久、地址不得变动--本类
    // 经 src 查 SourceFile 里的惰性行表与行缓存。非空不变式由显式构造的 ASSERT 保证。默认构造为空态
    // （src=nullptr、offset=0），供容器占位--空态即「无位置」：line() 返 0、line_col() 返 {0,0}、
    // to_string() 返空串。
    class SourceLoc {
    public:
        // 空态：src=nullptr。供容器占位（如 List<Token> 预留槽位）。
        SourceLoc() noexcept : src_{nullptr}, offset_{0} {}

        // 真实位置构造：src 必须非空（断言保证），offset 为 token/节点起点的字节偏移。
        SourceLoc(SourceFile* src, const u32 offset) noexcept : src_{src}, offset_{offset} {
            ASSERT(src != nullptr, "src must not be null");
        }

        [[nodiscard]]
        SourceFile* source() const noexcept {
            return src_;
        }

        // 字节偏移（空态为 0）。
        [[nodiscard]]
        u32 offset() const noexcept {
            return offset_;
        }

        // 行号（1-based，派生自行表；空态为 0）。热路径（逐节点求行号）成本见类注。
        [[nodiscard]]
        u32 line() const noexcept {
            return src_ == nullptr ? 0 : src_->line_at(offset_);
        }

        // 行/列（1-based，列按码点计；空态为 {0,0}）。成本见类注：冷路径，O(行内码点数)。
        [[nodiscard]]
        LineCol line_col() const noexcept {
            return src_ == nullptr ? LineCol{0, 0} : src_->locate(offset_);
        }

        // 渲染 "path:line:col"（1-based）。空态（src 为空）返回空串--空态即「无位置」，空串可与
        // 消费方的「空位置串 = 无前缀」约定直接组合（如 Error::make_message）。成本同 line_col()。
        [[nodiscard]]
        String to_string() const {
            if (src_ == nullptr) {
                return {};
            }
            const auto [line, col] = src_->locate(offset_);
            return std::format("{}:{}:{}", src_->path(), line, col);
        }

    private:
        SourceFile* src_;
        u32         offset_;
    };
} // namespace aria::src

#endif // ARIA_SOURCE_FILE_HPP
