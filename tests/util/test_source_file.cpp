#include <gtest/gtest.h>

#include <fstream>

#include "util/source_file.hpp"

using namespace aria;
using namespace aria::src;

namespace {
    // 写一个临时文件，返回其路径。文件在测试结束时由 gtest 的环境清理，这里用唯一名避免冲突。
    String write_temp_file(const String& name, const std::vector<u8>& bytes) {
        String        path = testing::TempDir() + "/" + name;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamoff>(bytes.size()));
        f.close();
        return path;
    }

    // 便利：从字符串字面量写文件（按 UTF-8 字节）
    String write_temp_file(const String& name, StringView content) {
        std::vector<u8> bytes(content.begin(), content.end());
        return write_temp_file(name, bytes);
    }
} // namespace

// ---------------------------------------------------------------------------
// 构造与访问器
// ---------------------------------------------------------------------------

TEST(SourceFile, ConstructorsAndAccessors) {
    SourceFile sf{"main.aria", "/path/to/main.aria", "let x = 1\n"};
    EXPECT_EQ(sf.name(), "main.aria");
    EXPECT_EQ(sf.path(), "/path/to/main.aria");
    EXPECT_EQ(sf.content(), "let x = 1\n");
}

TEST(SourceFile, DefaultConstructedIsEmpty) {
    SourceFile sf;
    EXPECT_EQ(sf.name(), "");
    EXPECT_EQ(sf.path(), "");
    EXPECT_EQ(sf.content(), "");
}

TEST(SourceFile, ContentIsNullTerminated) {
    SourceFile sf{"t", "t", "abc"};
    EXPECT_EQ(sf.content().data()[sf.content().size()], '\0');
}

// ---------------------------------------------------------------------------
// from_path：基本读取
// ---------------------------------------------------------------------------

TEST(SourceFileFromPath, ReadsContent) {
    String path = write_temp_file("basic.aria", "let x = 1\n");
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
    EXPECT_EQ(sf->name(), "basic.aria");
    EXPECT_EQ(sf->path(), path);
    EXPECT_EQ(sf->content(), "let x = 1\n");
}

TEST(SourceFileFromPath, MissingFileReturnsNotFound) {
    auto sf = SourceFile::from_path("/no/such/path/xyz.aria");
    ASSERT_FALSE(sf.has_value());
    EXPECT_EQ(sf.error(), fs::FsErrCode::NotFound);
}

// ---------------------------------------------------------------------------
// from_path：BOM 剥除
// ---------------------------------------------------------------------------

TEST(SourceFileFromPath, StripsBom) {
    std::vector<u8> bytes = {0xEF, 0xBB, 0xBF};
    String          body  = "let 中 = 1\n";
    bytes.insert(bytes.end(), body.begin(), body.end());
    String path = write_temp_file("bom.aria", bytes);
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
    EXPECT_NE(static_cast<u8>(sf->content()[0]), 0xEF);
    EXPECT_EQ(sf->content(), "let 中 = 1\n");
}

TEST(SourceFileFromPath, NoBomUnchanged) {
    String path = write_temp_file("nobom.aria", "abc");
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
    EXPECT_EQ(sf->content(), "abc");
}

// ---------------------------------------------------------------------------
// from_path：CRLF / CR 归一化
// ---------------------------------------------------------------------------

TEST(SourceFileFromPath, NormalizesCrlf) {
    String path = write_temp_file("crlf.aria", "line1\r\nline2\r\n");
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
    EXPECT_EQ(sf->content().find('\r'), String::npos);
    EXPECT_EQ(sf->content(), "line1\nline2\n");
}

TEST(SourceFileFromPath, NormalizesLoneCr) {
    String path = write_temp_file("cr.aria", "a\rb\rc");
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
    EXPECT_EQ(sf->content(), "a\nb\nc");
}

TEST(SourceFileFromPath, MixedLineEndings) {
    String path = write_temp_file("mixed.aria", "a\r\nb\rc\nd");
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
    EXPECT_EQ(sf->content(), "a\nb\nc\nd");
}

// ---------------------------------------------------------------------------
// from_path：非法 UTF-8
// ---------------------------------------------------------------------------

TEST(SourceFileFromPath, InvalidUtf8ReturnsInvalidEncoding) {
    std::vector<u8> bytes = {0xFF, 0xFE, 0x00, 0xC0, 0xAF};
    String          path  = write_temp_file("bad.aria", bytes);
    auto            sf    = SourceFile::from_path(path);
    ASSERT_FALSE(sf.has_value());
    EXPECT_EQ(sf.error(), fs::FsErrCode::InvalidEncoding);
}

TEST(SourceFileFromPath, ValidUtf8Accepted) {
    String path = write_temp_file("ok.aria", "中文 \xF0\x9F\x98\x80");
    auto   sf   = SourceFile::from_path(path);
    ASSERT_TRUE(sf.has_value());
}

// ---------------------------------------------------------------------------
// line_count（遵循 wc -l + 末行未终止也算一行的惯例）
// ---------------------------------------------------------------------------

class SourceFileLines : public ::testing::Test {
protected:
    SourceFile make(StringView content) { return SourceFile{"t", "t", String{content}}; }
};

TEST_F(SourceFileLines, EmptyIsZeroLines) { EXPECT_EQ(make("").line_count(), 0u); }

TEST_F(SourceFileLines, SingleCharOneLine) { EXPECT_EQ(make("a").line_count(), 1u); }

TEST_F(SourceFileLines, TrailingNewlineOneLine) { EXPECT_EQ(make("a\n").line_count(), 1u); }

TEST_F(SourceFileLines, UnterminatedLastLineCounts) { EXPECT_EQ(make("a\nb").line_count(), 2u); }

TEST_F(SourceFileLines, TwoLinesTerminated) { EXPECT_EQ(make("a\nb\n").line_count(), 2u); }

TEST_F(SourceFileLines, LoneNewlineOneLine) { EXPECT_EQ(make("\n").line_count(), 1u); }

TEST_F(SourceFileLines, TwoNewlinesTwoLines) { EXPECT_EQ(make("\n\n").line_count(), 2u); }

// ---------------------------------------------------------------------------
// line(n)
// ---------------------------------------------------------------------------

TEST_F(SourceFileLines, LineExcludesTrailingLf) {
    auto sf = make("a\nb\n");
    EXPECT_EQ(sf.line(1), "a");
    EXPECT_EQ(sf.line(2), "b");
}

TEST_F(SourceFileLines, LineOnUnterminatedLast) {
    auto sf = make("foo\nbar");
    EXPECT_EQ(sf.line(1), "foo");
    EXPECT_EQ(sf.line(2), "bar");
}

TEST_F(SourceFileLines, LineOutOfBoundsReturnsEmpty) {
    auto sf = make("a\nb\n");
    EXPECT_EQ(sf.line(0), "");
    EXPECT_EQ(sf.line(3), "");
    EXPECT_EQ(sf.line(99), "");
}

TEST_F(SourceFileLines, EmptyLineInMiddle) {
    auto sf = make("a\n\nb");
    EXPECT_EQ(sf.line(1), "a");
    EXPECT_EQ(sf.line(2), "");
    EXPECT_EQ(sf.line(3), "b");
}

// ---------------------------------------------------------------------------
// locate：字节偏移 -> (行, 码点列)
// ---------------------------------------------------------------------------

class SourceFileLocate : public ::testing::Test {
protected:
    SourceFile make(StringView content) { return SourceFile{"t", "t", String{content}}; }
};

TEST_F(SourceFileLocate, AsciiPositions) {
    auto sf = make("a\nb\n");
    EXPECT_EQ(sf.locate(0).line, 1u);
    EXPECT_EQ(sf.locate(0).col, 1u);
    // LF 属于它所在行
    EXPECT_EQ(sf.locate(1).line, 1u);
    EXPECT_EQ(sf.locate(2).line, 2u);
    EXPECT_EQ(sf.locate(2).col, 1u);
}

TEST_F(SourceFileLocate, EofReturnsNextLineCol1) {
    auto sf = make("a\nb\n");
    auto lc = sf.locate(4); // size == 4
    EXPECT_EQ(lc.line, 3u);
    EXPECT_EQ(lc.col, 1u);
}

TEST_F(SourceFileLocate, OvershootClampsToEof) {
    auto sf = make("ab");
    auto lc = sf.locate(999);
    EXPECT_EQ(lc.line, 2u);
    EXPECT_EQ(lc.col, 1u);
}

TEST_F(SourceFileLocate, EmptyFileEof) {
    auto sf = make("");
    auto lc = sf.locate(0);
    EXPECT_EQ(lc.line, 1u);
    EXPECT_EQ(lc.col, 1u);
}

TEST_F(SourceFileLocate, CjkCodepointColumn) {
    // "中 = 1"：中占 3 字节
    auto sf = make("\xE4\xB8\xAD = 1");
    EXPECT_EQ(sf.locate(0).col, 1u); // 中 起始
    EXPECT_EQ(sf.locate(3).col, 2u); // 空格（中 之后 1 码点）
    EXPECT_EQ(sf.locate(4).col, 3u); // '='
    EXPECT_EQ(sf.locate(5).col, 4u); // 空格
    EXPECT_EQ(sf.locate(6).col, 5u); // '1'
}

TEST_F(SourceFileLocate, CjkAcrossLines) {
    // 第1行 "中\n"，第2行 "b"
    auto sf = make("\xE4\xB8\xAD\nb");
    EXPECT_EQ(sf.locate(0).line, 1u);
    EXPECT_EQ(sf.locate(0).col, 1u);
    EXPECT_EQ(sf.locate(3).line, 1u); // LF
    EXPECT_EQ(sf.locate(4).line, 2u); // b
    EXPECT_EQ(sf.locate(4).col, 1u);
}

// 验证列按码点而非字节：第二个 CJK 字符起始字节 3 -> col 2（不是字节偏移）
TEST_F(SourceFileLocate, MultiByteColNotByteCol) {
    auto sf = make("\xE4\xB8\xAD\xE6\x96\x87x"); // 中文x
    // 第二个字符 '文' 起始于字节 3
    auto lc = sf.locate(3);
    EXPECT_EQ(lc.col, 2u);
    // 'x' 起始于字节 6
    auto lcx = sf.locate(6);
    EXPECT_EQ(lcx.col, 3u);
}

// ---------------------------------------------------------------------------
// SourceSpan
// ---------------------------------------------------------------------------

TEST(SourceSpan, Length) {
    SourceSpan s{3, 7};
    EXPECT_EQ(s.length(), 4u);
}

TEST(SourceSpan, EmptyLength) {
    SourceSpan s{5, 5};
    EXPECT_EQ(s.length(), 0u);
}

// ---------------------------------------------------------------------------
// SourceLoc
// ---------------------------------------------------------------------------

TEST(SourceLoc, ToStringValid) {
    SourceFile sf{"main.aria", "/path/to/main.aria", "abc"};
    SourceLoc  loc{&sf, LineCol{3, 5}};
    EXPECT_EQ(loc.to_string(), "/path/to/main.aria:3:5");
}

TEST(SourceLoc, ToStringInvalidLine) {
    SourceFile sf{"t", "t", "abc"};
    SourceLoc  loc{&sf, LineCol{0, 5}};
    EXPECT_EQ(loc.to_string(), "t:?:5");
}

TEST(SourceLoc, ToStringInvalidCol) {
    SourceFile sf{"t", "t", "abc"};
    SourceLoc  loc{&sf, LineCol{3, 0}};
    EXPECT_EQ(loc.to_string(), "t:3:?");
}

TEST(SourceLoc, ToStringInvalidBoth) {
    SourceFile sf{"t", "t", "abc"};
    SourceLoc  loc{&sf, LineCol{0, 0}};
    EXPECT_EQ(loc.to_string(), "t:?:?");
}

TEST(SourceLoc, AccessorsAndEmptyState) {
    SourceFile sf{"t", "t", "abc"};
    SourceLoc  loc{&sf, LineCol{2, 7}};
    EXPECT_EQ(loc.source(), &sf);
    EXPECT_EQ(loc.line_col().line, 2u);
    EXPECT_EQ(loc.line_col().col, 7u);

    // 默认构造为空态（src=nullptr）：to_string 返回 "?"，source 返回 nullptr。
    SourceLoc empty;
    EXPECT_EQ(empty.source(), nullptr);
    EXPECT_EQ(empty.to_string(), "?");
}
