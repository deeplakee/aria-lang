#ifndef ARIA_UTIL_CLI_HPP
#define ARIA_UTIL_CLI_HPP

// 命令行参数解析器 Cli：链式注册 flag / option / positional 后 parse(argv)，
// 返回独立 ParseResult（has / get / extra_args）；help() 渲染帮助文本。
//
// 定义与结果分离：Cli 仅持注册项（const 可重复 parse、互不污染），每次 parse 产出一个
// ParseResult（持结果数组 + 指回 Cli 的非拥有 const Cli*，调用方须保证 Cli 在其使用期间存活）。
//
// 统一枚举 Slot 贯穿定义/结果两侧：定义侧 Def.kind_ 永非 Empty（ASSERT 把关）；
// 结果侧 SlotEntry.state 用 Empty 表未提供、种类值表已提供（与 defs_ 同序、单数组）。
//
// 单一事实源：parse/has/get 仅依赖 defs_（线性扫描定位槽）；long_index_/short_index_ 仅注册期查重用。
// flag/option/positional 共唯一长名空间；--name/-x 解析只匹配 flag/option（按 kind_ 过滤），has/get 查任意槽。
//
// 内置 help flag（--help / -h）首个注册：命中即置位短路、视为成功（经 result.has("help") 取）。
//
// parse 首错即终止：返回 unexpected(首个错误消息)。值语义，纯解析工具，不打印、不退出。

#include <algorithm>
#include <format>
#include "common.hpp"

namespace aria::util {

    class Cli {
    public:
        // ============================================================
        // 统一枚举：定义侧（Def.kind_，永非 Empty）与结果侧（SlotEntry.state，Empty 表未提供）共用
        // ============================================================

        enum class Slot : u8 {
            Empty,      // 仅结果侧出现：该槽未提供/未填充
            Flag,       // 布尔开关（不取值）
            Option,     // 带值选项
            Positional, // 位置参数
        };

        // ============================================================
        // 解析结果：定义/结果分离后的结果侧（每次 parse 产出一份）
        // ============================================================

        // 持结果数组 + 指回 Cli 的非拥有指针；第 n 位 SlotEntry 对应 defs_ 第 n 位定义
        // （语义见文件头），调用方须保证 Cli 在 ParseResult 使用期间存活。
        class ParseResult {
        public:
            // 名字是否被解析到（flag/option/positional：看是否在命令行提供/填充）
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
                // option / positional：已提供（含显式空串）才返回值
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

            // 结果侧每槽记录（语义见文件头）：state + value（flag 槽 value 未用）。
            struct SlotEntry {
                Slot   state{Slot::Empty};
                String value;
            };

            explicit ParseResult(const Cli& cli) : cli_{&cli} {
                // slots_ 与 defs_ 同序、按其尺寸预置（构造体为 complete-class context，可见后置成员）
                slots_.resize(cli.defs_.size());
            }

            const Cli*      cli_;
            List<SlotEntry> slots_; // 与 defs_ 同序：每槽 state + value
            List<String>    extra_args_;
        };

        // ============================================================
        // 构建器：注册 flag / option / positional
        // ============================================================

        explicit Cli(const StringView program_name = "") :
            program_name_{program_name}, description_{}, defs_{}, long_index_{}, short_index_{} {
            register_builtin_help();
        }

        // 注册布尔开关：--verbose / -v（short_name 传 '\0' 表示无短名）
        Cli& add_flag(const StringView long_name, const StringView description, const char short_name = '\0') {
            const usize idx = defs_.size();
            // emplace_back 抛出（OOM）时该条目随 defs_ 一起回滚（值类型，无指针泄漏）：aria 视 OOM 为 fatal 量级
            defs_.emplace_back(long_name, short_name, description, "", false, Slot::Flag);
            register_name(long_name, idx, short_name);
            return *this;
        }

        // 注册带值选项：--output FILE / -o FILE。
        // default_value 仅用于 help() 展示，get() 未提供时返回 nullopt（调用方以 value_or 提供 fallback）。
        Cli& add_option(const StringView long_name, const StringView description, const StringView default_value = "",
                        const char short_name = '\0') {
            const usize idx = defs_.size();
            defs_.emplace_back(long_name, short_name, description, default_value, false, Slot::Option);
            register_name(long_name, idx, short_name);
            return *this;
        }

        // 注册位置参数（按出现顺序填充；is_required=false 时可缺省）
        Cli& add_positional(const StringView name, const StringView description, const bool is_required = true) {
            const usize idx = defs_.size();
            defs_.emplace_back(name, '\0', description, "", is_required, Slot::Positional);
            // positional 长名入唯一名字空间；--name/-x 解析不匹配 positional（见 find_long_without_positional）
            register_name(name, idx, '\0');
            return *this;
        }

        // ============================================================
        // 解析。成功返回 ParseResult，失败返回 unexpected(首个错误消息)（解析即终止）
        // ============================================================

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

        // 解析字符串列表（便捷重载）
        [[nodiscard]]
        Result<ParseResult, String> parse(const List<String>& args) const {
            List<StringView> views;
            views.reserve(args.size());
            for (const auto& s: args) {
                views.emplace_back(s);
            }
            return parse(views);
        }

        // 核心解析：按前缀分派逐 token 交给 handler；--help / -h 命中内置 help 置位后短路、视为成功。
        // 首错即返回 unexpected(消息)（错误不落 Cli 状态，重 parse 从新参数重新开始）。
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
                    // 单独 "-"（size<=1）或普通实参 -> 位置参数
                    parse_positional(result, arg);
                }

                if (step == Step::ShortCircuit) {
                    return result; // 内置 help：置位后短路，不再解析其余参数、亦跳过必填检查
                }
            }

            // 缺失的必填位置参数 -> 错误（用 state 而非 value 判定，理由见 check_required）
            if (const auto err = check_required(result)) {
                return std::unexpected(*err);
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

            // Options 分节：named 参数（flag/option）按注册序渲染（内置 help 首个注册、恒居首位）
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
            const usize desc_col = max_prefix + 2; // 最长前缀 + 2 空格间距
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
                ASSERT(kind_ != Slot::Empty, "Def kind 不能为 Empty");
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

        // 在 defs_ 中按长名找任意槽（供 has/get；长名全局唯一，单查即定位）。未找到返回 nullopt。
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

        // 在 defs_ 中按短名找 flag/option 槽（供 -x 解析）。未找到返回 nullopt。
        [[nodiscard]] Opt<usize> find_short(const char short_name) const {
            for (usize k = 0; k < defs_.size(); ++k) {
                if (defs_[k].short_name_ == short_name) {
                    return k;
                }
            }
            return std::nullopt;
        }

        // ============================================================
        // parse 的逐 token handler：控制流经 Step 枚举回传主循环；取值类 handler 可能前移 i
        // （消费下一参数为值）；遇错返回 unexpected(消息)。
        // ============================================================

        enum class Step : u8 {
            Continue,     // 该 token 处理完毕，继续解析下一参数
            ShortCircuit, // 命中内置 help flag，置位后立即成功返回（跳过剩余参数与必填检查）
        };

        // 长选项 --name / --name=value。positional 不参与（find_long_without_positional 过滤）。
        // flag 不取值（--flag=x 的 x 忽略）；option 取内联值或下一参数。命中 --help 返回 ShortCircuit。
        [[nodiscard]] Result<Step, String> parse_long(ParseResult& result, const StringView arg,
                                                      const Span<const StringView> args, usize& i) const {
            const auto opt      = arg.substr(2); // eq_pos==npos 时 substr(0, npos) 即整名
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
        // flag 置位后继续扫描下一字符；option 取余下部分或下一参数。命中 -h 返回 ShortCircuit。
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
                // 取值后结束本组（余下字符已作值）
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

        // 注册名入查重索引（长名 + 短名，共用一个长名空间）。跨/同 kind 重名、占用 --help/-h 保留名
        // 均为调用方编程错误，注册期 ASSERT 拒绝；NDEBUG 下首个注册生效。索引仅注册期查重用，解析侧唯一定位源是 defs_。
        // ASSERT 的 message 经宏文本替换进失败分支，std::format 仅失败时构造。
        void register_name(const StringView long_name, const usize idx, const char short_name) {
            [[maybe_unused]] const bool long_inserted = long_index_.emplace(String{long_name}, idx).second;
            ASSERT(long_inserted, std::format("arg 名 {} 重复注册", long_name).c_str());

            if (short_name == '\0') {
                return;
            }

            [[maybe_unused]] const bool short_inserted = short_index_.emplace(short_name, idx).second;
            ASSERT(short_inserted, std::format("arg 短名 -{} 重复注册", short_name).c_str());
        }

        // 注册内置 help flag 为 defs_[0]（构造共用：恒居 help() 渲染首位，parse 命中即短路）。
        // 不经 register_name（其保留名 ASSERT 专挡用户占 --help/-h），直连索引。
        void register_builtin_help() {
            defs_.emplace_back(kHelpLongName, kHelpShortName, kHelpDescription, "", false, Slot::Flag);
            long_index_.emplace(String{kHelpLongName}, 0);
            short_index_.emplace(kHelpShortName, 0);
        }

        // 内置 help flag 的固定身份（构造时首个注册，parse 命中即置位短路）
        static constexpr StringView kHelpLongName    = "help";
        static constexpr char       kHelpShortName   = 'h';
        static constexpr StringView kHelpDescription = "Show this help message";
        static constexpr StringView kOptValueSuffix  = " <VALUE>"; // option 行前缀尾部（Options 对齐宽度计入）

        String program_name_;
        String description_;

        // 统一注册项（flag/option/positional 共表，按 kind_ 分派）；解析侧唯一事实源
        List<Def> defs_;

        // 长名/短名查重索引（仅注册期用，解析侧不读）
        HashMap<String, usize> long_index_;
        HashMap<char, usize>   short_index_;

        // 不持解析结果：slots_/extra_args_ 随 ParseResult 走（定义/结果分离）
    };

} // namespace aria::util

#endif // ARIA_UTIL_CLI_HPP
