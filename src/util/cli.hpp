#ifndef ARIA_UTIL_CLI_HPP
#define ARIA_UTIL_CLI_HPP

// 命令行参数解析器 Cli：链式注册 flag / option / positional 后 parse(argv)，
// 返回独立 ParseResult（has / get / extra_args）；help() 渲染帮助文本。
// 定义（模板）与解析结果分离：Cli 仅持注册项与命名索引（const 可重复 parse、互不污染），
// 每次解析产出一个 ParseResult（持结果数组 + 指回 Cli 的非拥有 const Cli*，借其命名索引分派）。
// 内置 help flag（--help / -h）首个注册：命中即置位短路、视为成功（经 result.has("help") 取）。
// parse 首错即终止：返回 unexpected(首个错误消息)；成功返回 ParseResult。
// Flag / Option 平铺为独立 struct、经 List<Flag>/List<Option> 值存储（无继承多态、无 raw 指针），
// 按 {Kind, idx} 索引分派；值语义，默认可拷贝/移动。纯解析工具，不打印、不退出（策略交调用方）。

#include <algorithm>
#include <format>
#include "common.hpp"

namespace aria::util {

    class Cli {
    public:
        // ============================================================
        // 解析结果：定义/结果分离后的结果侧（每次 parse 产出一份）
        // ============================================================

        // ParseResult 持结果数组 + 指回 Cli 的非拥有指针（借 Cli 的命名索引分派 has/get）。
        // 调用方须保证 Cli 在 ParseResult 使用期间存活（同 SourceLoc::src_ 持 SourceFile* 的非拥有约定）。
        class ParseResult {
        public:
            // 名字是否被解析到（flag/option 看是否在命令行提供；不含 positional）
            [[nodiscard]]
            bool has(const StringView name) const {
                const auto name_str = String{name};
                if (!cli_->long_index_.contains(name_str)) {
                    return false;
                }
                const auto& [kind, idx] = cli_->long_index_.at(name_str);
                return kind == NameRef::Kind::Flag ? flag_set_[idx] : option_set_[idx];
            }

            // 取 option / positional 的值；未提供时返回 nullopt（显式置空 --output= 返回 Some("")）
            // （flag 恒无值，落到 positional 扫描；调用方以 value_or 提供 fallback）
            [[nodiscard]]
            Opt<String> get(const StringView name) const {
                const auto name_str = String{name};
                if (cli_->long_index_.contains(name_str)) {
                    const auto& [kind, idx] = cli_->long_index_.at(name_str);
                    if (kind == NameRef::Kind::Option && option_set_[idx]) {
                        return option_value_[idx]; // 已提供（含显式空串）
                    }
                    // flag 命中或 option 未提供：落 positional 扫描
                }
                for (usize i = 0; i < cli_->positionals_.size(); ++i) {
                    if (cli_->positionals_[i].name_ == name_str && positional_set_[i]) {
                        return positional_value_[i]; // 已填充（含显式空串实参）
                    }
                }
                return std::nullopt;
            }

            // 超出注册数的位置参数（如转发给脚本的剩余参数）
            [[nodiscard]]
            const List<String>& extra_args() const {
                return extra_args_;
            }

        private:
            friend class Cli;

            explicit ParseResult(const Cli& cli) : cli_{&cli} {
                // 各结果数组与定义 List 同序、按其尺寸预置（构造体为 complete-class context，可见后置成员）
                flag_set_.assign(cli.flags_.size(), false);
                option_set_.assign(cli.options_.size(), false);
                option_value_.resize(cli.options_.size());
                positional_set_.assign(cli.positionals_.size(), false);
                positional_value_.resize(cli.positionals_.size());
            }

            const Cli*   cli_;
            List<bool>   flag_set_;         // 与 flags_ 同序：是否在命令行提供
            List<bool>   option_set_;       // 与 options_ 同序：是否在命令行提供（含显式置空）
            List<String> option_value_;     // 与 options_ 同序：解析所得值
            List<bool>   positional_set_;   // 与 positionals_ 同序：是否被填充
            List<String> positional_value_; // 与 positionals_ 同序：填充值
            List<String> extra_args_;
        };

        // ============================================================
        // 构建器：注册 flag / option / positional
        // ============================================================

        explicit Cli(const StringView program_name = "") :
            program_name_{program_name}, description_{}, flags_{}, options_{}, positionals_{},
            long_index_{}, short_index_{} {
            register_builtin_help();
        }
        // 值类型：flags_/options_ 等皆值容器、无 raw 指针 -> 默认析构/拷贝/移动均正确（可安全拷贝/移动）。
        // auto parser = Cli{...} 及建造者链无约束；不再需要继承多态时代的 =delete。

        // 注册布尔开关：--verbose / -v（short_name 传 '\0' 表示无短名）
        Cli& add_flag(const StringView long_name, const StringView description, const char short_name = '\0') {
            const usize idx = flags_.size();
            // emplace_back 抛出（OOM）时该条目随 flags_ 一起回滚（值类型，无指针泄漏）：aria 视 OOM 为 fatal 量级
            flags_.emplace_back(long_name, short_name, description);
            register_name(long_name, NameRef::Kind::Flag, idx, short_name);
            return *this;
        }

        // 注册带值选项：--output FILE / -o FILE。
        // default_value 仅用于 help() 展示，get() 未提供时返回 nullopt（调用方以 value_or 提供 fallback）。
        Cli& add_option(const StringView long_name, const StringView description, const StringView default_value = "",
                        const char short_name = '\0') {
            const usize idx = options_.size();
            options_.emplace_back(long_name, short_name, description, default_value);
            register_name(long_name, NameRef::Kind::Option, idx, short_name);
            return *this;
        }

        // 注册位置参数（按出现顺序填充；is_required=false 时可缺省）
        Cli& add_positional(const StringView name, const StringView description, const bool is_required = true) {
            positionals_.emplace_back(name, description, is_required);
            return *this;
        }

        // ============================================================
        // 解析。成功返回 ParseResult，失败返回 unexpected(首个错误消息)（解析即终止）
        // ============================================================

        // 解析 main 的 argc/argv（零拷贝：argv 各元素以 StringView 借用，跳过 argv[0]）
        Result<ParseResult, String> parse(const i32 argc, char* argv[]) const {
            List<StringView> args;
            // argc==0 时防 argc-1 下溢成巨值
            const auto count = static_cast<usize>(argc > 1 ? argc - 1 : 0);
            args.reserve(count);
            for (i32 i = 1; i < argc; ++i) {
                args.emplace_back(argv[i]);
            }
            return parse(args);
        }

        // 解析字符串列表（便捷重载）
        Result<ParseResult, String> parse(const List<String>& args) const {
            List<StringView> views;
            views.reserve(args.size());
            for (const auto& s: args) {
                views.emplace_back(s);
            }
            return parse(views);
        }

        // 核心解析。--help / -h 命中内置 help flag 置位后短路、视为成功（经 result.has("help") 取）；
        // 首错即返回 unexpected(消息)（定义/结果分离：错误不落 Cli 状态，重 parse 从新参数重新开始）
        Result<ParseResult, String> parse(const Span<const StringView> args) const {
            ParseResult result{*this};
            usize       positional_idx = 0;

            for (usize i = 0; i < args.size(); ++i) {
                const auto arg = args[i];

                if (arg.starts_with("--")) {
                    // 长选项：--name 或 --name=value；eq_pos==npos 时 substr(0, npos) 即整名
                    const auto opt          = arg.substr(2);
                    const auto eq_pos       = opt.find('=');
                    const auto opt_name     = opt.substr(0, eq_pos);
                    const auto opt_name_str = String{opt_name};

                    if (!long_index_.contains(opt_name_str)) {
                        return std::unexpected(std::format("unknown option: --{}", opt_name));
                    }
                    const auto& [kind, idx] = long_index_.at(opt_name_str);

                    if (kind == NameRef::Kind::Flag) {
                        result.flag_set_[idx] = true; // flag：置位即完（不取值，--flag=x 的 x 忽略）
                        if (opt_name == kHelpLongName) {
                            return result; // 内置 help：置位后短路，不再解析其余参数
                        }
                        continue;
                    }

                    // option：内联值（--name=value）或下一参数（--name value）
                    if (eq_pos == StringView::npos) {
                        if (i + 1 >= args.size()) {
                            return std::unexpected(std::format("option --{} requires a value", opt_name));
                        }
                        result.option_value_[idx].assign(args[++i]);
                    } else {
                        result.option_value_[idx].assign(opt.substr(eq_pos + 1));
                    }
                    result.option_set_[idx] = true; // 已提供（含 --name= 显式置空）
                    continue;
                }

                if (arg.starts_with('-') && arg.size() > 1) {
                    // 短选项组：-abc（flag 簇）；遇取值选项 -oFILE / -o FILE 后结束本组
                    for (usize j = 1; j < arg.size(); ++j) {
                        const char c = arg[j];

                        const auto it = short_index_.find(c);
                        if (it == short_index_.end()) {
                            return std::unexpected(std::format("unknown option: -{}", c));
                        }
                        const auto& [kind, idx] = it->second;

                        if (kind == NameRef::Kind::Flag) {
                            result.flag_set_[idx] = true; // flag：置位后继续扫描下一字符
                            if (c == kHelpShortName) {
                                return result; // 内置 help：置位后短路
                            }
                            continue;
                        }

                        // option：值为当前参数余下部分（-oFILE）或下一参数（-o FILE）
                        if (j + 1 < arg.size()) {
                            result.option_value_[idx].assign(arg.substr(j + 1));
                        } else if (i + 1 < args.size()) {
                            result.option_value_[idx].assign(args[++i]);
                        } else {
                            return std::unexpected(std::format("option -{} requires a value", c));
                        }
                        result.option_set_[idx] = true; // 已提供（含 -o 后空串）
                        break;                          // 取值后结束本组（余下字符已作值）
                    }
                    continue;
                }

                // 位置参数：按注册顺序填充，超出注册数的收集进 extra
                if (positional_idx < positionals_.size()) {
                    result.positional_value_[positional_idx].assign(arg);
                    result.positional_set_[positional_idx] = true;
                    ++positional_idx;
                } else {
                    result.extra_args_.emplace_back(arg);
                }
            }

            // 缺失的必填位置参数 -> 错误（用 set_ 而非 value_ 判定：区分'未提供'与'显式空串实参'）
            for (usize i = 0; i < positionals_.size(); ++i) {
                if (positionals_[i].is_required_ && !result.positional_set_[i]) {
                    return std::unexpected(std::format("missing required argument: {}", positionals_[i].name_));
                }
            }

            return result;
        }

        // ============================================================
        // 帮助文本
        // ============================================================

        [[nodiscard]]
        String help() const {
            String result;

            if (program_name_.empty()) {
                result += "Usage: program";
            } else {
                result += std::format("Usage: {}", program_name_);
            }

            // [OPTIONS] 恒展示（内置 help flag 始终在 flags_ 中）
            if (!flags_.empty() || !options_.empty()) {
                result += " [OPTIONS]";
            }

            // 必填 <name>，可选 [name]
            for (const auto& p: positionals_) {
                result += p.is_required_ ? std::format(" <{}>", p.name_) : std::format(" [{}]", p.name_);
            }

            result += "\n";

            if (!description_.empty()) {
                result += std::format("\n{}\n", description_);
            }

            if (!positionals_.empty()) {
                result += "\nArguments:\n";
                for (const auto& p: positionals_) {
                    // 描述对齐第 16 列（名字超长则贴紧，不留空格）
                    const usize pad = 16 - std::min<usize>(16, p.name_.size());
                    result += std::format("  {}{}{}\n", p.name_, String(pad, ' '), p.description_);
                }
            }

            if (!flags_.empty() || !options_.empty()) {
                result += "\nOptions:\n";
                // 按 kind 分组渲染：flags 在前（内置 help 首个注册、恒居首位），options 在后
                for (const auto& f: flags_) {
                    // 描述对齐第 22 列（按 "-x, --name" 计，名字超长则贴紧）
                    const usize pad = 22 - std::min<usize>(22, f.long_name_.size() + 4);
                    result += std::format("{}{}{}\n", render_prefix(f.long_name_, f.short_name_), String(pad, ' '),
                                          f.description_);
                }
                for (const auto& o: options_) {
                    // 描述对齐第 22 列（按 "-x, --name <VALUE>" 计，名字超长则贴紧）
                    const usize pad  = 22 - std::min<usize>(22, o.long_name_.size() + 11);
                    auto        line = std::format("{} <VALUE>{}{}", render_prefix(o.long_name_, o.short_name_),
                                                   String(pad, ' '), o.description_);
                    if (!o.default_value_.empty()) {
                        line += std::format(" [default: {}]", o.default_value_);
                    }
                    line += "\n";
                    result += line;
                }
            }

            return result;
        }

        // 补充程序描述（渲染在 Usage 行之后）
        Cli& set_description(const StringView desc) {
            description_ = String{desc};
            return *this;
        }

    private:
        // 长名/短名 -> {种类, 在 flags_ 或 options_ 中的下标}
        struct NameRef {
            enum class Kind : u8 { Flag, Option };
            Kind  kind;
            usize idx;
        };

        // 布尔开关（--verbose / -v）：不取值（定义侧；是否提供见 ParseResult::flag_set_）
        struct Flag {
            Flag(const StringView long_name, const char short_name, const StringView description) :
                long_name_{long_name}, short_name_{short_name}, description_{description} {}

            String long_name_;
            char   short_name_; // '\0' = 无短名
            String description_;
        };

        // 带值选项（--output FILE / -o FILE）（定义侧；值/是否提供见 ParseResult::option_*）
        struct Option {
            Option(const StringView long_name, const char short_name, const StringView description,
                   const StringView default_value) :
                long_name_{long_name}, short_name_{short_name}, description_{description},
                default_value_{default_value} {}

            String long_name_;
            char   short_name_;      // '\0' = 无短名
            String description_;
            String default_value_;   // 仅 help 展示
        };

        // 位置参数（不参与命名索引，按出现顺序填充）（定义侧；值/是否填充见 ParseResult::positional_*）
        struct Positional {
            Positional(const StringView name, const StringView description, const bool is_required) :
                name_{name}, description_{description}, is_required_{is_required} {}

            String name_;
            String description_;
            bool   is_required_;
        };

        // 公共渲染前缀："  -x, --name"（无短名则 "    --name"）；flags_/options_ 渲染共用
        [[nodiscard]] static String render_prefix(const StringView long_name, const char short_name) {
            return short_name != '\0' ? std::format("  -{}, --{}", short_name, long_name)
                                      : std::format("    --{}", long_name);
        }

        // 注册名入索引（长名 + 短名）。flag / option 共用一个名字空间：
        // 跨/同 kind 重名、占用内置帮助保留名（--help / -h）均为调用方编程错误，
        // 注册期 ASSERT 拒绝；NDEBUG 下首个注册生效（后续重名经 emplace 忽略）。
        // ASSERT 的 message 经宏文本替换进失败分支，std::format 仅失败时构造。
        void register_name(const StringView long_name, const NameRef::Kind kind, const usize idx,
                           const char short_name) {

            const auto ref = NameRef{.kind = kind, .idx = idx};

            [[maybe_unused]] const bool long_inserted = long_index_.emplace(String{long_name}, ref).second;
            ASSERT(long_inserted, std::format("arg 名 --{} 重复注册", long_name).c_str());

            if (short_name == '\0') {
                return;
            }

            [[maybe_unused]] const bool short_inserted = short_index_.emplace(short_name, ref).second;
            ASSERT(short_inserted, std::format("arg 短名 -{} 重复注册", short_name).c_str());
        }

        // 注册内置 help flag 为 flags_[0]（构造共用：恒居 help() 渲染首位，parse 命中即短路）。
        // 不经 register_name（其保留名 ASSERT 专挡用户占 --help/-h），直连索引。
        void register_builtin_help() {
            flags_.emplace_back(kHelpLongName, kHelpShortName, kHelpDescription);
            long_index_.emplace(String{kHelpLongName}, NameRef{.kind = NameRef::Kind::Flag, .idx = 0});
            short_index_.emplace(kHelpShortName, NameRef{.kind = NameRef::Kind::Flag, .idx = 0});
        }

        // 内置 help flag 的固定身份（构造时首个注册，parse 命中即置位短路）
        static constexpr StringView kHelpLongName    = "help";
        static constexpr char       kHelpShortName   = 'h';
        static constexpr StringView kHelpDescription = "Show this help message";

        String                   program_name_;
        String                   description_;
        List<Flag>               flags_;
        List<Option>             options_;
        List<Positional>         positionals_;
        HashMap<String, NameRef> long_index_;  // 长名（flag/option 共用）-> {种类, flags_/options_ 下标}
        HashMap<char, NameRef>   short_index_; // 短名（flag/option 共用）-> {种类, flags_/options_ 下标}
        // 不持解析结果：flag_set_/option_value_/extra_args_ 等随 ParseResult 走（定义/结果分离）
    };

} // namespace aria::util

#endif // ARIA_UTIL_CLI_HPP