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

    // 行/列位置(1-based)。
    struct LineCol {
        u32 line = 1;
        u32 col  = 1; // 列按码点计数
    };

    // 源文件载体:name/path/content 各以 String 持有。生命期纪律:借出的 StringView 不得活得比对象久;
    // 对象入容器增删致 String 重分配时,SSO 短串 move 会改字符地址使既借 StringView 悬空;构造后勿改内容。
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

        // 底层 String 以 '\0' 结尾,哨兵扫描可用;BOM 剥除/行尾归 LF 仅 from_path 做,三参构造存原文。
        [[nodiscard]]
        StringView content() const noexcept {
            return content_;
        }

        // 与 wc -l 同语义:无尾 LF 的末行也算一行,尾 LF 不额外计空行;空内容为 0。
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

        // 字节偏移 -> 1-based 行号(行表二分 + 单条行缓存,逐节点求行号的热路径)。
        // offset 超界或 EOF(== content.size())返回 line_count() + 1,即「EOF 落在下一行第 1 列」。
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

        // 字节偏移 -> 1-based (行, 列);EOF(== content.size())返回下一行第 1 列(line_count()+1, 1)。
        // 列须数码点,是 O(行内码点数) 的冷路径(行号部分走 line_at 的缓存)。
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

        // 磁盘读取并构造:剥 BOM、行尾归 LF、校验 UTF-8(非法返 InvalidEncoding);name 取 basename,读失败原样返回错误码。
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

        // 懒构建行表:line_starts_[i] 为第 i+1 行在 content_ 中的起始字节偏移;「行」以 LF 结尾或到 EOF,
        // 尾 LF 不产生额外空行起点,空内容行表为空。
        mutable List<u32> line_starts_;
        mutable bool      is_line_starts_built_ = false;

        // line_at 的单条行缓存:最近解析的行区间 [begin, end) 与行号,全 0 即「无缓存」;内容构造后不变+单线程,无需失效。
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

        // 剥前导 BOM、CRLF/CR -> LF,校验 UTF-8;非法返回空(调用方据此返回 InvalidEncoding)。
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

    // 源码位置:SourceFile* + 字节偏移,行列是派生量。src 非拥有,不得比所引 SourceFile 活得久、地址不得变。
    // line_col()/to_string() 是 O(行内码点数) 的冷路径,勿在逐 token 循环里读列;空态(src=nullptr)即「无位置」。
    class SourceLoc {
    public:
        // 空态:src=nullptr。
        SourceLoc() noexcept : src_{nullptr}, offset_{0} {}

        // 真实位置构造:src 必须非空(断言保证)。
        SourceLoc(SourceFile* src, const u32 offset) noexcept : src_{src}, offset_{offset} {
            ASSERT(src != nullptr, "src must not be null");
        }

        [[nodiscard]]
        SourceFile* source() const noexcept {
            return src_;
        }

        [[nodiscard]]
        u32 offset() const noexcept {
            return offset_;
        }

        // 行号(1-based;空态为 0)。
        [[nodiscard]]
        u32 line() const noexcept {
            return src_ == nullptr ? 0 : src_->line_at(offset_);
        }

        // 行/列(1-based,列按码点计;空态为 {0,0})。
        [[nodiscard]]
        LineCol line_col() const noexcept {
            return src_ == nullptr ? LineCol{0, 0} : src_->locate(offset_);
        }

        // 渲染 "path:line:col";空态返回空串,可与「空位置串 = 无前缀」直接组合;成本同 line_col()。
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
