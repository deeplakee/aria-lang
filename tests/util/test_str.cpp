#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <tuple>

#include "util/str.hpp"

using aria::String;
using aria::StringView;
using aria::usize;

namespace {

    // 按消费方形态走一遍:先按展开后长度定容,再解码直写,返回内容串。
    String decoded(const StringView raw, const usize decoded_len) {
        String out;
        out.resize(decoded_len);
        std::ignore = aria::str::decode_string_content(raw, out.data());
        return out;
    }

} // namespace

TEST(DecodeStringContent, PlainTextCopiesVerbatim) {
    EXPECT_EQ(decoded("hello", 5), "hello");
    EXPECT_EQ(decoded("", 0), "");
    // 多字节 UTF-8 透传不拆码点:中文四字 = 12 字节 + " ascii" 6 字节
    EXPECT_EQ(decoded("\xE4\xB8\xAD\xE6\x96\x87\xE6\xB7\xB7\xE6\x8E\x92 ascii", 18),
              "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB7\xB7\xE6\x8E\x92 ascii");
}

TEST(DecodeStringContent, SimpleEscapes) {
    // raw 19 字节 -> 12 字节:每个双字符转义展开成 1 字节,含 '\0'
    EXPECT_EQ(decoded(R"(a\nb\tc\r\\\"\'\0\$)", 12), (String{"a\nb\tc\r\\\"'\0$", 12}));
}

TEST(DecodeStringContent, EscapeAtRunBoundaries) {
    EXPECT_EQ(decoded(R"(\n)", 1), "\n");       // 首字符即转义(零普通前缀)
    EXPECT_EQ(decoded(R"(end\n)", 4), "end\n"); // 尾部转义
    EXPECT_EQ(decoded(R"(a\nb)", 3), "a\nb");   // 普通段夹转义
    EXPECT_EQ(decoded(R"(\t\t)", 2), "\t\t");   // 连续转义
}

TEST(DecodeStringContent, UnicodeEscapes) {
    EXPECT_EQ(decoded(R"(\u{41})", 1), "A");
    EXPECT_EQ(decoded(R"(\u{7f})", 1), (String{"\x7f", 1}));   // 上边界 ASCII
    EXPECT_EQ(decoded(R"(\u{4e2d})", 3), "\xE4\xB8\xAD");      // 3 字节码点
    EXPECT_EQ(decoded(R"(\u{1f600})", 4), "\xF0\x9F\x98\x80"); // 4 字节码点
    EXPECT_EQ(decoded(R"(a\u{41}b)", 3), "aAb");               // 夹在普通段中
}

TEST(DecodeStringContent, NulBytesSurvive) {
    // '\0' 转义产出真实 NUL 字节:内容按长度语义而非 C 串语义比较
    const String out = decoded(R"(\0\0)", 2);
    EXPECT_EQ(out.size(), 2);
    EXPECT_EQ(out, (String{"\0\0", 2}));
}

TEST(DecodeStringContent, ReturnsWrittenBytes) {
    char buffer[32]{};
    EXPECT_EQ(aria::str::decode_string_content("ab\\ncd", buffer), 5);
    EXPECT_EQ((StringView{buffer, 5}), "ab\ncd");
    EXPECT_EQ(aria::str::decode_string_content("", buffer), 0);
}

TEST(DecodeStringContent, ResidualTailUntouchedOnExactFit) {
    // out 容量按记账定容时写入不越界:超出记账长度的哨兵字节保持原样
    char buffer[8];
    std::memset(buffer, '#', sizeof(buffer));
    std::ignore = aria::str::decode_string_content("hi", buffer);
    EXPECT_EQ((StringView{buffer, 8}), (String{"hi######", 8}));
}
