#ifndef ARIA_UTIL_CLI_HPP
#define ARIA_UTIL_CLI_HPP

// 命令行参数解析器 Cli：链式注册 flag / option / positional 后 parse(argv)，返回 ParseResult
// （has / get / extra_args）；help() 渲染帮助文本。
//
// 定义与结果分离：Cli 仅持注册项（const 可重复 parse、互不污染），每次 parse 产出 ParseResult。
// 单一事实源：注册查重与解析定位全走 defs_ 线性扫描（register_name 复用 find_long/find_short），
// 无平行索引。统一枚举 Slot 贯穿定义/结果两侧：定义侧 Def.kind_ 永非 Empty（ASSERT 把关），结果
// 侧 SlotEntry.state 用 Empty 表未提供、种类值表已提供。flag/option/positional 共唯一长名空间；
// --name/-x 只匹配 flag/option（按 kind_ 过滤），has/get 查任意槽。内置 help flag（--help / -h）
// 构造期首个注册，parse 命中即短路成功（仍置位，经 result.has("help") 取）。首错即止。值语义，
// 纯解析工具，不打印、不退出。

#include <algorithm>
#include <format>
#include "common.hpp"

namespace aria::util {

    class Cli {
    public:
        enum class Slot : u8 {
            Empty, // 仅结果侧出现：该槽未提供/未填充
            Flag,
            Option,
            Positional,
        };

        // 解析结果：定义/结果分离后的结果侧。slots_ 与 defs_ 同序（第 n 位 SlotEntry 对应第 n 位
        // 定义）；持非拥有 const Cli*，调用方须保证 Cli 在结果用完前存活。
        class ParseResult {
        public:
            [[nodiscard]]
            bool has(const StringView name) const {
                const auto idx = cli_->find_long(name);
                if (!idx) {
                    return false; // 未注册名
                }
                return slots_[*idx].state != Slot::Empty;
            }

            // 取 option / positional 的值；未提供或名属 flag（无值）时返回 nullopt
            // （显式置空 --output= / 空串实参返回 Some("")；调用方以 value_or 提供 fallback）
            [[nodiscard]]
            Opt<String> get(const StringView name) const {
                const auto idx = cli_->find_long(name);
                if (!idx || cli_->defs_[*idx].kind_ == Slot::Flag) {
                    return std::nullopt; // 未注册，或命中的是 flag（无值）
                }
                if (const usize index = *idx; slots_[index].state != Slot::Empty) {
                    return slots_[index].value;
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

            // 结果侧每槽记录：state + value（flag 槽 value 未用）。
            struct SlotEntry {
                Slot   state{Slot::Empty};
                String value;
            };

            explicit ParseResult(const Cli& cli) : cli_{&cli} { slots_.resize(cli.defs_.size()); }

            const Cli*      cli_;
            List<SlotEntry> slots_;
            List<String>    extra_args_;
        };

        explicit Cli(const StringView program_name = "") : program_name_{program_name}, description_{}, defs_{} {
            register_builtin_help();
        }

        // 注册布尔开关：--verbose / -v（short_name 传 '\0' 表示无短名）
        Cli& add_flag(const StringView long_name, const StringView description, const char short_name = '\0') {
            // 查重须先于 emplace（扫 defs_，后插自撞）；OOM 由值类型随 defs_ 一起回滚
            register_name(long_name, short_name);
            defs_.emplace_back(long_name, short_name, description, "", false, Slot::Flag);
            return *this;
        }

        // default_value 仅用于 help() 展示，不参与 get() 的取值。
        Cli& add_option(const StringView long_name, const StringView description, const StringView default_value = "",
                        const char short_name = '\0') {
            register_name(long_name, short_name);
            defs_.emplace_back(long_name, short_name, description, default_value, false, Slot::Option);
            return *this;
        }

        // 注册位置参数（按出现顺序填充；is_required=false 时可缺省）
        Cli& add_positional(const StringView name, const StringView description, const bool is_required = true) {
            // positional 长名入唯一名字空间（查重）；--name/-x 解析不匹配 positional（见
            // find_long_without_positional）
            register_name(name, '\0');
            defs_.emplace_back(name, '\0', description, "", is_required, Slot::Positional);
            return *this;
        }

        // 解析 main 的 argc/argv（零拷贝：argv 各元素以 StringView 借用，跳过 argv[0]）
        [[nodiscard]]
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

        [[nodiscard]]
        Result<ParseResult, String> parse(const List<String>& args) const {
            List<StringView> views;
            views.reserve(args.size());
            for (const auto& s: args) {
                views.emplace_back(s);
            }
            return parse(views);
        }

        // 核心解析。首错即返回 unexpected(消息)（错误不落 Cli 状态，重 parse 从新参数重新开始）。
        [[nodiscard]]
        Result<ParseResult, String> parse(const Span<const StringView> args) const {
            ParseResult result{*this};

            for (usize i = 0; i < args.size(); ++i) {
                const auto arg  = args[i];
                auto       step = Step::Continue;

                if (arg.starts_with("--")) {
                    auto s = parse_long(result, arg, args, i);
                    if (!s)
                        return std::unexpected(std::move(s).error());
                    step = *s;
                } else if (arg.starts_with('-') && arg.size() > 1) {
                    auto s = parse_short(result, arg, args, i);
                    if (!s)
                        return std::unexpected(std::move(s).error());
                    step = *s;
                } else {
                    parse_positional(result, arg);
                }

                if (step == Step::ShortCircuit) {
                    return result;
                }
            }

            // 缺失的必填位置参数 -> 错误（用 state 而非 value 判定，理由见 check_required）
            if (const auto err = check_required(result)) {
                return std::unexpected(*err);
            }
            return result;
        }

        [[nodiscard]]
        String help() const {
            String result;

            if (program_name_.empty()) {
                result += "Usage: program";
            } else {
                result += std::format("Usage: {}", program_name_);
            }

            // [OPTIONS] 恒展示：内置 help flag 构造期首个注册，named 参数恒存在
            result += " [OPTIONS]";

            // 必填 <name>，可选 [name]：位置参数按注册序（defs_ 中 Positional 出现序）
            for (const auto& p: defs_) {
                if (p.kind_ == Slot::Positional) {
                    result += p.is_required_ ? std::format(" <{}>", p.long_name_) : std::format(" [{}]", p.long_name_);
                }
            }
            result += '\n';

            if (!description_.empty()) {
                result += std::format("\n{}\n", description_);
            }

            // Arguments 分节：按注册序遍历 positional，遇首个时才输出分节头
            bool any_positional = false;
            for (const auto& p: defs_) {
                if (p.kind_ != Slot::Positional) {
                    continue;
                }
                if (!any_positional) {
                    result += "\nArguments:\n";
                    any_positional = true;
                }
                // 名字字段宽 16、描述对齐第 18 列（2 空格缩进 + 16；超长则贴紧，不留空格）
                const usize pad = 16 - std::min<usize>(16, p.long_name_.size());
                result += std::format("  {}{}{}\n", p.long_name_, String(pad, ' '), p.description_);
            }

            // Options 分节：named 参数（flag/option）按注册序渲染
            // 先算各前缀最大宽度（option 含 " <VALUE>"），描述统一对齐到 max+2 列，所有描述同列
            usize max_prefix = 0;
            for (const auto& d: defs_) {
                if (d.kind_ == Slot::Positional) {
                    continue;
                }
                const usize width = render_prefix(d.long_name_, d.short_name_).size() +
                                    (d.kind_ == Slot::Option ? kOptValueSuffix.size() : 0);
                max_prefix        = std::max(max_prefix, width);
            }
            const usize desc_col = max_prefix + 2;
            result += "\nOptions:\n";
            for (const auto& d: defs_) {
                if (d.kind_ == Slot::Positional) {
                    continue;
                }
                String prefix = render_prefix(d.long_name_, d.short_name_);
                if (d.kind_ == Slot::Option) {
                    prefix += kOptValueSuffix;
                }
                const usize pad  = desc_col - prefix.size(); // prefix <= max_prefix -> pad >= 2
                auto        line = std::format("{}{}{}", prefix, String(pad, ' '), d.description_);
                if (d.kind_ == Slot::Option && !d.default_value_.empty()) {
                    line += std::format(" [default: {}]", d.default_value_);
                }
                line += '\n';
                result += line;
            }

            return result;
        }

        // 补充程序描述（渲染在 Usage 行之后）
        Cli& set_description(const StringView desc) {
            description_ = String{desc};
            return *this;
        }

    private:
        // 统一注册项（定义侧）：flag/option/positional 共一表，按 kind_ 区分哪些字段生效（kind_ 永非 Empty）。
        struct Def {
            Def(const StringView long_name, const char short_name, const StringView description,
                const StringView default_value, const bool is_required, const Slot kind) :
                long_name_{long_name}, short_name_{short_name}, description_{description},
                default_value_{default_value}, is_required_{is_required}, kind_{kind} {
                ASSERT(kind_ != Slot::Empty, "kind must not be Empty");
            }

            String long_name_;  // flag/option 长名；positional 参数名
            char   short_name_; // '\0' = 无短名（positional 不用）
            String description_;
            String default_value_; // option only（仅 help 展示）
            bool   is_required_;   // positional only
            Slot   kind_;          // Flag | Option | Positional（定义侧永非 Empty）
        };

        // 公共渲染前缀："  -x, --name"（无短名则 "    --name"）；flag/option 渲染共用
        [[nodiscard]] static String render_prefix(const StringView long_name, const char short_name) {
            return short_name != '\0' ? std::format("  -{}, --{}", short_name, long_name)
                                      : std::format("    --{}", long_name);
        }

        [[nodiscard]] Opt<usize> find_long(const StringView name) const {
            for (usize k = 0; k < defs_.size(); ++k) {
                if (defs_[k].long_name_ == name) {
                    return k;
                }
            }
            return std::nullopt;
        }

        // 在 defs_ 中按长名找 flag/option 槽（供 --name 解析，排除 positional）。未找到返回 nullopt。
        [[nodiscard]] Opt<usize> find_long_without_positional(const StringView long_name) const {
            for (usize k = 0; k < defs_.size(); ++k) {
                if (defs_[k].kind_ != Slot::Positional && defs_[k].long_name_ == long_name) {
                    return k;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] Opt<usize> find_short(const char short_name) const {
            for (usize k = 0; k < defs_.size(); ++k) {
                if (defs_[k].short_name_ == short_name) {
                    return k;
                }
            }
            return std::nullopt;
        }

        enum class Step : u8 {
            Continue,
            ShortCircuit, // 命中内置 help flag，置位后立即成功返回（跳过剩余参数与必填检查）
        };

        // 长选项 --name / --name=value：flag 不取值（--flag=x 的 x 忽略），option 取内联值或下一参数。
        // positional 不参与（find_long_without_positional 过滤）。
        [[nodiscard]] Result<Step, String> parse_long(ParseResult& result, const StringView arg,
                                                      const Span<const StringView> args, usize& i) const {
            const auto opt      = arg.substr(2);
            const auto eq_pos   = opt.find('=');
            const auto opt_name = opt.substr(0, eq_pos);

            const auto found = find_long_without_positional(opt_name);
            if (!found) {
                return std::unexpected(std::format("unknown option: --{}", opt_name));
            }
            const usize idx = *found;

            if (defs_[idx].kind_ == Slot::Flag) {
                result.slots_[idx].state = Slot::Flag;
                // 按名短路 help 的安全性依赖保留名纪律：--help/-h 被内置占用，用户同名注册被 ASSERT 拒绝。
                return opt_name == kHelpLongName ? Step::ShortCircuit : Step::Continue;
            }

            if (eq_pos == StringView::npos) {
                if (i + 1 >= args.size()) {
                    return std::unexpected(std::format("option --{} requires a value", opt_name));
                }
                result.slots_[idx].value.assign(args[++i]);
            } else {
                result.slots_[idx].value.assign(opt.substr(eq_pos + 1));
            }
            result.slots_[idx].state = Slot::Option; // 已提供（含 --name= 显式置空）
            return Step::Continue;
        }

        // 短选项组 -abc（flag 簇）；遇取值选项 -oFILE / -o FILE 后结束本组。
        [[nodiscard]] Result<Step, String> parse_short(ParseResult& result, const StringView arg,
                                                       const Span<const StringView> args, usize& i) const {
            for (usize j = 1; j < arg.size(); ++j) {
                const char c = arg[j];

                const auto found = find_short(c);
                if (!found) {
                    return std::unexpected(std::format("unknown option: -{}", c));
                }
                const usize idx = *found;

                if (defs_[idx].kind_ == Slot::Flag) {
                    result.slots_[idx].state = Slot::Flag;
                    if (c == kHelpShortName) {
                        return Step::ShortCircuit; // 保留名纪律同 parse_long
                    }
                    continue;
                }

                if (j + 1 < arg.size()) {
                    result.slots_[idx].value.assign(arg.substr(j + 1));
                } else if (i + 1 < args.size()) {
                    result.slots_[idx].value.assign(args[++i]);
                } else {
                    return std::unexpected(std::format("option -{} requires a value", c));
                }
                result.slots_[idx].state = Slot::Option; // 已提供（含 -o 后空串）
                return Step::Continue;
            }
            return Step::Continue;
        }

        // 位置参数：填入 defs_ 中首个「空且种类为 Positional」的槽（Positional 按注册序填、填后不重置，
        // 故「首个空槽」即「下一个待填槽」，无需另维护填充计数器）；单独 "-" 也走此路；无空槽则收进 extra。
        void parse_positional(ParseResult& result, const StringView arg) const {
            for (usize k = 0; k < defs_.size(); ++k) {
                if (defs_[k].kind_ == Slot::Positional && result.slots_[k].state == Slot::Empty) {
                    result.slots_[k].value.assign(arg);
                    result.slots_[k].state = Slot::Positional;
                    return;
                }
            }
            // 所有 Positional 槽均已填 -> 超额，收进 extra（如转发给脚本的剩余参数）
            result.extra_args_.emplace_back(arg);
        }

        // 必填位置参数缺失检查。返回首个缺失项的错误消息，无缺失返回 nullopt。
        // 用 state 而非 value 判定：区分'未提供'与'显式空串实参'。
        [[nodiscard]] Opt<String> check_required(const ParseResult& result) const {
            for (usize k = 0; k < defs_.size(); ++k) {
                if (defs_[k].kind_ == Slot::Positional && defs_[k].is_required_ &&
                    result.slots_[k].state == Slot::Empty) {
                    return std::format("missing required argument: {}", defs_[k].long_name_);
                }
            }
            return std::nullopt;
        }

        // 注册名查重（长名 + 短名，共用一个长名空间；须在 defs_ emplace 之前调用）。
        // 重名（含占用内置 --help/-h）为调用方编程错误，注册期 ASSERT 拒绝；NDEBUG 下首个注册生效。
        void register_name(const StringView long_name, const char short_name) const {
            ASSERT(!find_long(long_name), std::format("long name {} registered twice", long_name).c_str());

            if (short_name == '\0') {
                return;
            }

            ASSERT(!find_short(short_name), std::format("short name -{} registered twice", short_name).c_str());
        }

        // 注册内置 help flag 为 defs_[0]（恒居 help() 渲染首位）；后续用户注册 help/-h 撞重名 ASSERT。
        void register_builtin_help() {
            register_name(kHelpLongName, kHelpShortName);
            defs_.emplace_back(kHelpLongName, kHelpShortName, kHelpDescription, "", false, Slot::Flag);
        }

        static constexpr StringView kHelpLongName    = "help";
        static constexpr char       kHelpShortName   = 'h';
        static constexpr StringView kHelpDescription = "Show this help message";
        static constexpr StringView kOptValueSuffix  = " <VALUE>"; // option 行前缀尾部（Options 对齐宽度计入）

        String program_name_;
        String description_;

        // 统一注册项（flag/option/positional 共表，按 kind_ 分派）；注册查重与解析定位唯一事实源
        List<Def> defs_;

        // 不持解析结果：slots_/extra_args_ 随 ParseResult 走（定义/结果分离）
    };

} // namespace aria::util

#endif // ARIA_UTIL_CLI_HPP
