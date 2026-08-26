#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "compile/Token.hpp"
#include "error/Error.hpp"
#include "error/ErrorCode.hpp"
#include "object/Object.hpp"
#include "util/fs.hpp"
#include "util/io.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

#include "isocline.h"

using namespace aria;

void repl() {
    ic_set_history(nullptr, 500);
    char* input;
    while ((input = ic_readline("")) != nullptr) {
        String line = input;
        free(input);
        io::println("{}", line);
        if (line == "exit") {
            break;
        }
    }
}

// test
int main() {
    io::println("Hello,world!");
    io::println("{}", sizeof(Value));
    auto val1 = Value::from_i32(123);
    auto val2 = val1;
    io::println("{}", val2.type_name());
    auto a = std::make_unique<int>(123);
    util::print_binary(a.get());
    auto dir_wrap = fs::current_dir();
    if (!dir_wrap) {
        io::println(stderr, "dir1 error");
        return 1;
    }
    io::println("current dir: {}", dir_wrap.value());
    dir_wrap = fs::resolve(dir_wrap.value(), "../../aria/src/runtime/../a.hpp");
    if (!dir_wrap) {
        io::println(stderr, "dir2 error");
        return 1;
    }
    io::println("{}", dir_wrap.value());
    dir_wrap = fs::program_dir();
    if (!dir_wrap) {
        io::println(stderr, "dir3 error");
        return 1;
    }
    io::println("program dir: {}", dir_wrap.value());

    String str        = "你好，世界！";
    auto   codepoints = utf8::decode(str);
    for (const auto& codepoint: codepoints) {
        io::println("0X{:04X}", codepoint);
    }
    io::println();

    for (const auto& codepoint: utf8::view(str)) {
        io::println("0X{:04X}", codepoint);
    }

    List<int> list = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    io::println("list size: {}", list.size());


    io::println("size of Error: {}", sizeof(Error));
    io::println("size of ErrorCode: {}", sizeof(ErrorCode));
    io::println("size of SourceFile*: {}", sizeof(SourceFile*));
    io::println("size of SourceLoc: {}", sizeof(SourceLoc));
    io::println("size of String: {}", sizeof(String));
    io::println("size of Object: {}", sizeof(Object));

    char msg[] = "123456";
    io::println("msg: {}", strlen(msg));

    String code = R"(var a = "hello, world!";
var b = 1.23e5;
var c = -100;
var d = a + b * c - 0.6;
var e = 0x123;
var f = "\u{1F600}";
var g = f + "\t", h = g, i;
)";


    auto s       = SourceFile{"code", "", code};
    auto lexer   = Lexer{};
    auto tks_res = lexer.tokenize(&s);
    if (!tks_res) {
        io::println(stderr, "tks_result error");
        return 1;
    }
    io::println("==== tokens =====");
    const auto& tks = tks_res.value();
    for (usize i = 0; i < tks.size(); ++i) {
        io::println("{:<3}:{}", i, tks[i].to_string());
    }

    // token 的 lexeme 与 AST 的 SourceLoc 均指向 s（SourceFile），s 存活至函数返回，安全。
    io::println("==== ast =====");
    auto parser  = Parser{};
    auto ast_res = parser.parse(tks_res.value());
    if (!ast_res) {
        io::println(stderr, "ast parse error:");
        for (const auto& e: ast_res.error()) {
            io::println(stderr, "  {}", e.format());
        }
        return 1;
        std::unreachable();
    }
    ast_res.value()->display();

    constexpr auto Range = std::views::iota;
    for (auto i: Range(0, 10)) {
        io::println("{}", i);
    }

    // auto array = List<int>{1,2,3,4,5,6,7,8,9,10};

    // repl();
    return 0;
}
