#include <gtest/gtest.h>

#include <format>
#include <type_traits>

#include "util/cli.hpp"

using namespace aria;
using util::Cli;

// 值类型（Flag/Option 平铺值存储，无 raw 指针；ParseResult 持非拥有 const Cli* + 值数组）：默认可拷贝/可移动
static_assert(std::is_copy_constructible_v<Cli>);
static_assert(std::is_move_constructible_v<Cli>);
static_assert(std::is_copy_constructible_v<Cli::ParseResult>);
static_assert(std::is_move_constructible_v<Cli::ParseResult>);

namespace {
    // 便捷入口：从初始化列表建 StringView 列表再走核心 Span 重载 parse。
    auto parse_args(const Cli& parser, std::initializer_list<StringView> args) {
        const List<StringView> views{args};
        return parser.parse(views);
    }
} // namespace

// ---------------------------------------------------------------------------
// flag：长名 / 短名 / 簇 / 内联值忽略
// ---------------------------------------------------------------------------

TEST(CliFlag, LongName) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出");
    const auto r = parse_args(parser, {"--verbose"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
}

TEST(CliFlag, ShortName) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v');
    const auto r = parse_args(parser, {"-v"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
}

TEST(CliFlag, ShortClusterSetsAll) {
    auto parser = Cli{};
    parser.add_flag("all", "全部", 'a').add_flag("brief", "简短", 'b').add_flag("count", "计数", 'c');
    const auto r = parse_args(parser, {"-abc"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("all"));
    EXPECT_TRUE(r->has("brief"));
    EXPECT_TRUE(r->has("count"));
}

TEST(CliFlag, InlineValueIgnored) {
    // flag 不取值：--flag=x 的 x 忽略，视为置位
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出");
    const auto r = parse_args(parser, {"--verbose=x"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
}

TEST(CliFlag, UnsetFlagIsFalse) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v');
    const auto r = parse_args(parser, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has("verbose"));
    EXPECT_FALSE(r->has("unregistered"));
}

// ---------------------------------------------------------------------------
// option：--name value / --name=value / -o value / -oVALUE
// ---------------------------------------------------------------------------

TEST(CliOption, LongNextArg) {
    auto parser = Cli{};
    parser.add_option("output", "输出文件", "", 'o');
    const auto r = parse_args(parser, {"--output", "out.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("output"), "out.aria");
}

TEST(CliOption, LongInlineValue) {
    auto parser = Cli{};
    parser.add_option("output", "输出文件", "", 'o');
    const auto r = parse_args(parser, {"--output=out.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("output"), "out.aria");
}

TEST(CliOption, ShortAttachedValue) {
    // -oFILE：值为当前参数余下部分
    auto parser = Cli{};
    parser.add_option("output", "输出文件", "", 'o');
    const auto r = parse_args(parser, {"-oout.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("output"), "out.aria");
}

TEST(CliOption, ShortNextArg) {
    // -o FILE：值为下一参数
    auto parser = Cli{};
    parser.add_option("output", "输出文件", "", 'o');
    const auto r = parse_args(parser, {"-o", "out.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("output"), "out.aria");
}

TEST(CliOption, FlagThenOptionInCluster) {
    // -vo FILE：v 是 flag 置位，o 是取值选项取下一参数
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v').add_option("output", "输出文件", "", 'o');
    const auto r = parse_args(parser, {"-vo", "out.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_EQ(r->get("output"), "out.aria");
}

TEST(CliOption, GetFallbackWhenMissing) {
    // default_value 仅用于 help 展示；get 未命中返回 nullopt，调用方以 value_or 取 fallback
    auto parser = Cli{};
    parser.add_option("output", "输出文件", "out.aria");
    const auto r = parse_args(parser, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->get("output").has_value());
    EXPECT_EQ(r->get("output").value_or("fallback.aria"), "fallback.aria");
    EXPECT_FALSE(r->has("output"));
}

TEST(CliOption, EmptyInlineValue) {
    // --output= 显式置空：get 返回 Some("")、has 为 true（区分'显式置空'与'未提供'）
    auto parser = Cli{};
    parser.add_option("output", "输出文件");
    const auto r = parse_args(parser, {"--output="});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("output"), "");
    EXPECT_TRUE(r->has("output"));
}

// ---------------------------------------------------------------------------
// positional：必填 / 可选 / 超额收集
// ---------------------------------------------------------------------------

TEST(CliPositional, RequiredFilledInOrder) {
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件").add_positional("arg", "脚本参数", false);
    const auto r = parse_args(parser, {"main.aria", "script-arg"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("script"), "main.aria");
    EXPECT_EQ(r->get("arg"), "script-arg");
}

TEST(CliPositional, OptionalMissingOk) {
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件").add_positional("arg", "脚本参数", false);
    const auto r = parse_args(parser, {"main.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("script"), "main.aria");
    EXPECT_EQ(r->get("arg").value_or("fb"), "fb");
}

TEST(CliPositional, MissingRequiredErrors) {
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件");
    const auto r = parse_args(parser, {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "missing required argument: script");
}

TEST(CliPositional, ExtraCollected) {
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件");
    const auto r = parse_args(parser, {"main.aria", "a", "b"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("script"), "main.aria");
    ASSERT_EQ(r->extra_args().size(), 2u);
    EXPECT_EQ(r->extra_args()[0], "a");
    EXPECT_EQ(r->extra_args()[1], "b");
}

TEST(CliPositional, DashOnlyArgIsPositional) {
    // 单独的 "-"（size==1）不进短选项分支，按位置参数处理
    auto parser = Cli{};
    parser.add_positional("stdin-marker", "短横线占位");
    const auto r = parse_args(parser, {"-"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get("stdin-marker"), "-");
}

TEST(CliPositional, EmptyStringSatisfiesRequired) {
    // 显式空串实参应满足必填（用 set_ 而非 value_ 判定：区分'未提供'与'显式空串'）
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件");
    const auto r = parse_args(parser, {""});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->get("script").has_value());
    EXPECT_EQ(*r->get("script"), "");
}

TEST(CliPositional, HasReportsPositional) {
    // has 覆盖 positional：已填充 -> true，未填充的可选 -> false
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件").add_positional("arg", "脚本参数", false);
    const auto r = parse_args(parser, {"main.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("script"));
    EXPECT_FALSE(r->has("arg"));
    EXPECT_FALSE(r->has("unregistered"));
}

TEST(CliPositional, HasReportsEmptyStringPositional) {
    // 显式空串实参：has 应为 true（区分'已提供空串'与'未提供'，同 get 语义）
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件");
    const auto r = parse_args(parser, {""});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("script"));
}

// ---------------------------------------------------------------------------
// 混合注册：flag / option / positional 同一 parser（合并索引 + 多态分派路径）
// ---------------------------------------------------------------------------

TEST(CliMixed, FlagOptionPositionalTogether) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v')
            .add_option("output", "输出文件", "", 'o')
            .add_positional("script", "脚本文件");
    const auto r = parse_args(parser, {"-v", "--output", "out.aria", "main.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_EQ(r->get("output"), "out.aria");
    EXPECT_EQ(r->get("script"), "main.aria");
}

TEST(CliMixed, DistinctFlagAndOptionNames) {
    // flag 与 option 共用一个名字空间但各自独立可查（名字不同即可共存）
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出").add_option("verbose-level", "详细级别");
    const auto r = parse_args(parser, {"--verbose", "--verbose-level", "3"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_TRUE(r->has("verbose-level"));
    EXPECT_EQ(r->get("verbose-level"), "3");
}

// ---------------------------------------------------------------------------
// --help / -h
// ---------------------------------------------------------------------------

TEST(CliHelpRequest, LongHelp) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出");
    const auto r = parse_args(parser, {"--verbose", "--help"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("help"));
}

TEST(CliHelpRequest, ShortHelp) {
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件");
    // -h 优先于必填位置参数缺失检查：请求帮助即成功返回
    const auto r = parse_args(parser, {"-h"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("help"));
}

TEST(CliHelpRequest, NotRequestedByDefault) {
    auto       parser = Cli{};
    const auto r      = parse_args(parser, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has("help"));
}

TEST(CliHelpRequest, ShortClusterHelpShortCircuits) {
    // 簇中遇 -h：内置 help 命中即短路，其前 flag 已置位，其后参数不再解析
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v').add_positional("script", "脚本文件");
    const auto r = parse_args(parser, {"-vh", "ignored.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_TRUE(r->has("help"));
    // 短路：必填位置参数未被填充（短路先于必填检查返回）
    EXPECT_FALSE(r->get("script").has_value());
}

// ---------------------------------------------------------------------------
// 解析错误：未知选项 / 缺值（首错即终止，消息经 unexpected 带出）
// ---------------------------------------------------------------------------

TEST(CliErrors, UnknownLongOption) {
    auto       parser = Cli{};
    const auto r      = parse_args(parser, {"--nope"});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "unknown option: --nope");
}

TEST(CliErrors, UnknownShortOption) {
    auto       parser = Cli{};
    const auto r      = parse_args(parser, {"-z"});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "unknown option: -z");
}

TEST(CliErrors, LongOptionMissingValue) {
    auto parser = Cli{};
    parser.add_option("output", "输出文件");
    const auto r = parse_args(parser, {"--output"});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "option --output requires a value");
}

TEST(CliErrors, ShortOptionMissingValue) {
    auto parser = Cli{};
    parser.add_option("output", "输出文件", "", 'o');
    const auto r = parse_args(parser, {"-o"});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "option -o requires a value");
}

TEST(CliErrors, ParseAgainAfterErrorWorks) {
    // 错误只经返回值走、不落 Cli 状态：再次 parse 从新参数重新开始
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v');
    EXPECT_FALSE(parse_args(parser, {"--nope"}).has_value());
    const auto r = parse_args(parser, {"--verbose"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
}

TEST(CliErrors, ReuseSameTemplateParsesCleanly) {
    // 定义/结果分离：同一 Cli 可重复 parse，各次 ParseResult 互不污染
    auto parser = Cli{};
    parser.add_flag("verbose", "详细", 'v').add_option("output", "输出", "", 'o').add_positional("script", "脚本");
    // 首次：全提供
    const auto r1 = parse_args(parser, {"-v", "--output", "o.aria", "main.aria"});
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->has("verbose"));
    EXPECT_EQ(r1->get("output"), "o.aria");
    EXPECT_EQ(r1->get("script"), "main.aria");
    // 二次：空参 -> 不残留首次结果，必填缺失报错
    const auto r2 = parse_args(parser, {});
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), "missing required argument: script");
    // 首次结果不受二次解析影响（独立 ParseResult）
    EXPECT_TRUE(r1->has("verbose"));
    EXPECT_EQ(r1->get("script"), "main.aria");
}

// ---------------------------------------------------------------------------
// 三个 parse 重载入口
// ---------------------------------------------------------------------------

TEST(CliOverloads, ArgvSkipsProgramName) {
    char  arg0[] = "aria";
    char  arg1[] = "--verbose";
    char  arg2[] = "main.aria";
    char* argv[] = {arg0, arg1, arg2};

    auto parser = Cli{"aria"}; // 保证省略，无拷贝/移动
    parser.add_flag("verbose", "详细输出").add_positional("script", "脚本文件");
    const auto r = parser.parse(3, argv);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_EQ(r->get("script"), "main.aria");
}

TEST(CliOverloads, ArgvZeroArgcSafe) {
    // argc==0 时不得下溢出巨值 reserve
    auto parser = Cli{};
    EXPECT_TRUE(parser.parse(0, nullptr).has_value());
}

TEST(CliOverloads, ListOfString) {
    const List<String> args = {"--verbose", "main.aria"};

    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出").add_positional("script", "脚本文件");
    const auto r = parser.parse(args);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_EQ(r->get("script"), "main.aria");
}

TEST(CliOverloads, ConstParserParses) {
    // parse 为 const：定义/结果分离后 const Cli 可直接解析（builder 不再被解析改写）
    auto builder = Cli{};
    builder.add_flag("verbose", "详细输出", 'v').add_positional("script", "脚本文件");
    const auto parser = builder; // 拷一份定型的定义
    const auto r      = parser.parse(List<String>{"-v", "main.aria"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has("verbose"));
    EXPECT_EQ(r->get("script"), "main.aria");
}

// ---------------------------------------------------------------------------
// help() 文本渲染
// ---------------------------------------------------------------------------

TEST(CliHelpText, UsageLineAndPositionals) {
    // [OPTIONS] 恒展示（内置 help 始终在）；必填 <name>，可选 [name]
    auto parser = Cli{"aria"};
    parser.add_positional("script", "脚本文件").add_positional("arg", "脚本参数", false);
    const auto text = parser.help();
    EXPECT_TRUE(text.starts_with("Usage: aria [OPTIONS] <script> [arg]\n"));
}

TEST(CliHelpText, UsageShowsOptionsPlaceholder) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出");
    const auto text = parser.help();
    EXPECT_TRUE(text.starts_with("Usage: program [OPTIONS]\n"));
}

TEST(CliHelpText, UsageFallsBackToProgram) {
    auto       parser = Cli{};
    const auto text   = parser.help();
    EXPECT_TRUE(text.starts_with("Usage: program [OPTIONS]\n"));
}

TEST(CliHelpText, DescriptionRendered) {
    auto parser = Cli{};
    parser.set_description("aria 解释器");
    const auto text = parser.help();
    EXPECT_NE(text.find("\naria 解释器\n"), String::npos);
}

TEST(CliHelpText, ArgumentsSection) {
    auto parser = Cli{};
    parser.add_positional("script", "脚本文件");
    const auto text = parser.help();
    EXPECT_NE(text.find("\nArguments:\n"), String::npos);
    // 名字后补空格使描述起于第 18 列（2 + 名长 6 + 补 10）
    EXPECT_NE(text.find(std::format("  script{}脚本文件\n", String(10, ' '))), String::npos);
}

TEST(CliHelpText, OptionsSectionFlagsAndOptions) {
    auto parser = Cli{};
    parser.add_flag("verbose", "详细输出", 'v').add_option("output", "输出文件", "out.aria", 'o');
    const auto text = parser.help();

    // 描述统一对齐到 max_prefix+2 列：最长前缀 "  -o, --output <VALUE>"(22) -> 描述起于第 24 列
    // help 恒在首位（"  -h, --help" 12 字符 + 12 空格）
    EXPECT_NE(text.find(std::format("  -h, --help{}Show this help message\n", String(12, ' '))), String::npos);
    // flag 行："  -v, --verbose" 15 字符 + 9 空格
    EXPECT_NE(text.find(std::format("  -v, --verbose{}详细输出\n", String(9, ' '))), String::npos);
    // option 行："  -o, --output <VALUE>" 22 字符 + 2 空格，default_value 展示在描述后
    EXPECT_NE(text.find(std::format("  -o, --output <VALUE>{}输出文件 [default: out.aria]\n", String(2, ' '))),
              String::npos);
}

TEST(CliHelpText, LongFlagNameAligned) {
    // 长名按 max_prefix+2 对齐：自身是最长前缀，描述跟 2 空格（不再贴紧 0 空格）
    auto parser = Cli{};
    parser.add_flag("a-very-long-flag-name", "长名开关");
    const auto text = parser.help();
    // "    --a-very-long-flag-name"(27) 为最长前缀 -> 描述起于第 29 列（2 空格间距）
    EXPECT_NE(text.find("    --a-very-long-flag-name  长名开关\n"), String::npos);
}

TEST(CliHelpText, OptionsInRegistrationOrder) {
    // Options 分节按 defs_ 注册序渲染（内置 help 首个注册、恒居首位）
    auto parser = Cli{};
    parser.add_option("output", "输出文件").add_flag("verbose", "详细输出");
    const auto text = parser.help();
    ASSERT_NE(text.find("--output"), String::npos);
    ASSERT_NE(text.find("--verbose"), String::npos);
    // 注册序：output 先于 verbose
    EXPECT_LT(text.find("--output"), text.find("--verbose"));
    // 内置 help 恒居 Options 分节首位
    EXPECT_LT(text.find("--help"), text.find("--output"));
}
