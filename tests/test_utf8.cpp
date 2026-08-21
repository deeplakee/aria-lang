#include <gtest/gtest.h>

#include "util/utf8.hpp"

using namespace aria;
using utf8::codepoint;

namespace {
// 辅助：构造含指定字节的 StringView
StringView sv(std::initializer_list<u8> bytes) {
    static thread_local String buf;
    buf.clear();
    for (auto b : bytes) buf.push_back(static_cast<char>(b));
    return StringView{buf};
}
} // namespace

// ---------------------------------------------------------------------------
// decode_one
// ---------------------------------------------------------------------------

TEST(Utf8DecodeOne, Ascii) {
    auto [cp, n] = utf8::decode_one("A");
    EXPECT_EQ(cp, U'A');
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, AsciiBoundary0x7F) {
    auto [cp, n] = utf8::decode_one("\x7F");
    EXPECT_EQ(cp, 0x7Fu);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, TwoByteLatin) { // ¢ = U+00A2
    auto [cp, n] = utf8::decode_one("\xC2\xA2");
    EXPECT_EQ(cp, 0x00A2u);
    EXPECT_EQ(n, 2u);
}

TEST(Utf8DecodeOne, ThreeByteCjk) { // 中 = U+4E2D
    auto [cp, n] = utf8::decode_one("\xE4\xB8\xAD");
    EXPECT_EQ(cp, 0x4E2Du);
    EXPECT_EQ(n, 3u);
}

TEST(Utf8DecodeOne, FourByteEmoji) { // 😀 = U+1F600
    auto [cp, n] = utf8::decode_one("\xF0\x9F\x98\x80");
    EXPECT_EQ(cp, 0x1F600u);
    EXPECT_EQ(n, 4u);
}

TEST(Utf8DecodeOne, MaxCodepoint) { // U+10FFFF
    auto [cp, n] = utf8::decode_one("\xF4\x8F\xBF\xBF");
    EXPECT_EQ(cp, 0x10FFFFu);
    EXPECT_EQ(n, 4u);
}

TEST(Utf8DecodeOne, OffsetParameter) {
    // "a 中 b" 的字节：61 / E4 B8 AD / 62
    StringView s = "a\xE4\xB8\xAD""b";
    auto [c0, n0] = utf8::decode_one(s, 0);
    EXPECT_EQ(c0, U'a');
    EXPECT_EQ(n0, 1u);
    auto [c1, n1] = utf8::decode_one(s, 1);
    EXPECT_EQ(c1, 0x4E2Du);
    EXPECT_EQ(n1, 3u);
    auto [c4, n4] = utf8::decode_one(s, 4);
    EXPECT_EQ(c4, U'b');
    EXPECT_EQ(n4, 1u);
}

TEST(Utf8DecodeOne, DefaultOffsetIsZero) {
    auto [cp, n] = utf8::decode_one("Z");
    EXPECT_EQ(cp, U'Z');
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, OffsetBeyondSizeReturnsZero) {
    StringView s = "abc";
    auto [cp, n] = utf8::decode_one(s, s.size());
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 0u);
}

TEST(Utf8DecodeOne, OffsetWayBeyondSizeReturnsZero) {
    auto [cp, n] = utf8::decode_one("abc", 999);
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 0u);
}

// 非法输入 -----------------------------------------------------------------

TEST(Utf8DecodeOne, LoneContinuationByte) {
    auto [cp, n] = utf8::decode_one("\x80");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, TruncatedSequence) { // 需 3 字节但只有 1 个续接
    auto [cp, n] = utf8::decode_one("\xE4\xB8");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, OverlongSlash) { // '/' 用 2 字节表示 -> 超长
    auto [cp, n] = utf8::decode_one("\xC0\xAF");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, OverlongNul) { // U+0000 用 2 字节
    auto [cp, n] = utf8::decode_one("\xC0\x80");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, SurrogateHalf) { // U+D800
    auto [cp, n] = utf8::decode_one("\xED\xA0\x80");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, OutOfRangeLead) { // 0xF8 起始非法
    auto [cp, n] = utf8::decode_one("\xF8\x80\x80\x80\x80");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, BadContByteMidSequence) { // E4 B8 'z'（z 非续接）
    auto [cp, n] = utf8::decode_one("\xE4\xB8z");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

TEST(Utf8DecodeOne, CodepointAboveMax) { // U+110000 = F4 90 80 80
    auto [cp, n] = utf8::decode_one("\xF4\x90\x80\x80");
    EXPECT_EQ(cp, utf8::kReplacementChar);
    EXPECT_EQ(n, 1u);
}

// ---------------------------------------------------------------------------
// decode（整串）
// ---------------------------------------------------------------------------

TEST(Utf8Decode, Mixed) {
    auto cps = utf8::decode("a\xE4\xB8\xAD""b");
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[0], U'a');
    EXPECT_EQ(cps[1], 0x4E2Du);
    EXPECT_EQ(cps[2], U'b');
}

TEST(Utf8Decode, ReplacesInvalid) {
    auto cps = utf8::decode("a\xFF""b");
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[1], utf8::kReplacementChar);
}

TEST(Utf8Decode, Empty) {
    auto cps = utf8::decode("");
    EXPECT_TRUE(cps.empty());
}

// ---------------------------------------------------------------------------
// is_valid
// ---------------------------------------------------------------------------

TEST(Utf8IsValid, ValidAscii) {
    EXPECT_TRUE(utf8::is_valid("hello world"));
}

TEST(Utf8IsValid, ValidMixed) {
    EXPECT_TRUE(utf8::is_valid("hello \xE4\xB8\xAD \xF0\x9F\x98\x80"));
}

TEST(Utf8IsValid, Empty) { EXPECT_TRUE(utf8::is_valid("")); }

TEST(Utf8IsValid, InvalidBadByte) {
    EXPECT_FALSE(utf8::is_valid("bad \xFF byte"));
}

TEST(Utf8IsValid, InvalidOverlong) {
    EXPECT_FALSE(utf8::is_valid("\xC0\xAF"));
}

TEST(Utf8IsValid, InvalidSurrogate) {
    EXPECT_FALSE(utf8::is_valid("\xED\xA0\x80"));
}

TEST(Utf8IsValid, InvalidTruncated) {
    EXPECT_FALSE(utf8::is_valid("\xE4\xB8"));
}

// ---------------------------------------------------------------------------
// count
// ---------------------------------------------------------------------------

TEST(Utf8Count, Ascii) { EXPECT_EQ(utf8::count("abc"), 3u); }

TEST(Utf8Count, Cjk) { EXPECT_EQ(utf8::count("\xE4\xB8\xAD\xE6\x96\x87"), 2u); }

TEST(Utf8Count, Empty) { EXPECT_EQ(utf8::count(""), 0u); }

TEST(Utf8Count, InvalidBytesCountAsOne) {
    // \xFF 各占 1 码点（count 不严格校验）
    EXPECT_EQ(utf8::count("a\xFF""b"), 3u);
}

// ---------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------

TEST(Utf8Encode, Ascii) { EXPECT_EQ(utf8::encode(U'A'), "A"); }

TEST(Utf8Encode, TwoByte) { EXPECT_EQ(utf8::encode(0x00A2u), "\xC2\xA2"); }

TEST(Utf8Encode, ThreeByte) { EXPECT_EQ(utf8::encode(0x4E2Du), "\xE4\xB8\xAD"); }

TEST(Utf8Encode, FourByte) { EXPECT_EQ(utf8::encode(0x1F600u), "\xF0\x9F\x98\x80"); }

TEST(Utf8Encode, RoundTripAllLengths) {
    const codepoint cps[] = {U'A', 0x00A2u, 0x4E2Du, 0x1F600u, 0x10FFFFu};
    for (auto cp : cps) {
        auto bytes = utf8::encode(cp);
        auto [dec, n] = utf8::decode_one(bytes);
        EXPECT_EQ(dec, cp);
        EXPECT_EQ(n, bytes.size());
    }
}

TEST(Utf8Encode, SurrogateReturnsEmpty) { EXPECT_TRUE(utf8::encode(0xD800u).empty()); }

TEST(Utf8Encode, AboveMaxReturnsEmpty) { EXPECT_TRUE(utf8::encode(0x110000u).empty()); }

// ---------------------------------------------------------------------------
// 字符分类
// ---------------------------------------------------------------------------

TEST(Utf8Classify, IsAlnum) {
    EXPECT_TRUE(utf8::is_alnum(U'a'));
    EXPECT_TRUE(utf8::is_alnum(U'Z'));
    EXPECT_TRUE(utf8::is_alnum(U'0'));
    EXPECT_FALSE(utf8::is_alnum(U'_'));
    EXPECT_FALSE(utf8::is_alnum(U'-'));
}

TEST(Utf8Classify, IsAlphaAscii) {
    EXPECT_TRUE(utf8::is_alpha(U'a'));
    EXPECT_TRUE(utf8::is_alpha(U'Z'));
    EXPECT_FALSE(utf8::is_alpha(U'0'));
}

TEST(Utf8Classify, IsAlphaCjk) {
    EXPECT_TRUE(utf8::is_alpha(0x4E2Du)); // 中
}

TEST(Utf8Classify, IsDigitAscii) {
    EXPECT_TRUE(utf8::is_digit(U'5'));
    EXPECT_FALSE(utf8::is_digit(U'a'));
}

TEST(Utf8Classify, IsIdStart) {
    EXPECT_TRUE(utf8::is_id_start(U'_'));
    EXPECT_TRUE(utf8::is_id_start(U'x'));
    EXPECT_TRUE(utf8::is_id_start(0x4E2Du)); // 中
    EXPECT_FALSE(utf8::is_id_start(U'1'));
    EXPECT_FALSE(utf8::is_id_start(U'-'));
}

TEST(Utf8Classify, IsIdContinue) {
    EXPECT_TRUE(utf8::is_id_continue(U'_'));
    EXPECT_TRUE(utf8::is_id_continue(U'1'));
    EXPECT_TRUE(utf8::is_id_continue(0x4E2Du));
    EXPECT_FALSE(utf8::is_id_continue(U'-'));
    EXPECT_FALSE(utf8::is_id_continue(U' '));
}

TEST(Utf8Classify, IsWhitespace) {
    EXPECT_TRUE(utf8::is_whitespace(U' '));
    EXPECT_TRUE(utf8::is_whitespace(U'\t'));
    EXPECT_TRUE(utf8::is_whitespace(U'\n'));
    EXPECT_TRUE(utf8::is_whitespace(U'\r'));
    EXPECT_TRUE(utf8::is_whitespace(0x3000u)); // 全角空格
    EXPECT_FALSE(utf8::is_whitespace(U'a'));
}

// ---------------------------------------------------------------------------
// 迭代器 / view
// ---------------------------------------------------------------------------

TEST(Utf8View, IteratesCodepoints) {
    StringView s = "a\xE4\xB8\xAD""b";
    List<codepoint> got;
    for (auto cp : utf8::view(s)) {
        got.push_back(cp);
    }
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], U'a');
    EXPECT_EQ(got[1], 0x4E2Du);
    EXPECT_EQ(got[2], U'b');
}

TEST(Utf8View, EmptyRange) {
    int n = 0;
    for (auto cp : utf8::view("")) {
        (void) cp;
        ++n;
    }
    EXPECT_EQ(n, 0);
}

TEST(Utf8View, ByteOffsetTracking) {
    StringView s = "a\xE4\xB8\xAD""b";
    auto it = utf8::view(s).begin();
    EXPECT_EQ(it.byte_offset(), 0u);
    ++it;
    EXPECT_EQ(it.byte_offset(), 1u);
    ++it;
    EXPECT_EQ(it.byte_offset(), 4u);
}

TEST(Utf8View, InvalidByteIteratesAsReplacement) {
    auto it = utf8::view("a\xFF").begin();
    EXPECT_EQ(*it, U'a');
    ++it;
    EXPECT_EQ(*it, utf8::kReplacementChar);
}
