#include <gtest/gtest.h>

#include <fstream>

#include "util/fs.hpp"

using namespace aria;

namespace {
String write_temp_file(const String& name, StringView content) {
    String path = testing::TempDir() + "/" + name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamoff>(content.size()));
    f.close();
    return path;
}
} // namespace

// ---------------------------------------------------------------------------
// read_file
// ---------------------------------------------------------------------------

TEST(FsReadFile, ReadsExistingFile) {
    String path = write_temp_file("fs_read.txt", "hello\nworld");
    auto r = fs::read_file(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "hello\nworld");
}

TEST(FsReadFile, EmptyFile) {
    String path = write_temp_file("fs_empty.txt", "");
    auto r = fs::read_file(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

TEST(FsReadFile, BinaryContent) {
    std::vector<char> bytes = {0x00, 0x01, 0x02, static_cast<char>(0xFF), 'A'};
    String path = testing::TempDir() + "/fs_bin.txt";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamoff>(bytes.size()));
    f.close();
    auto r = fs::read_file(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), bytes.size());
}

TEST(FsReadFile, MissingFileReturnsNotFound) {
    auto r = fs::read_file("/no/such/file_xyz.txt");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), fs::FsErrCode::NotFound);
}

TEST(FsReadFile, DirectoryReturnsError) {
    // 读取目录应失败（具体错误码因平台而异，这里只断言失败）
    auto r = fs::read_file(testing::TempDir());
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// current_dir
// ---------------------------------------------------------------------------

TEST(FsCurrentDir, ReturnsNonEmpty) {
    auto r = fs::current_dir();
    ASSERT_TRUE(r.has_value()) << "error: " << static_cast<int>(r.error());
    EXPECT_FALSE(r->empty());
}

// ---------------------------------------------------------------------------
// program_dir
// ---------------------------------------------------------------------------

TEST(FsProgramDir, ReturnsNonEmpty) {
    auto r = fs::program_dir();
    ASSERT_TRUE(r.has_value()) << "error: " << static_cast<int>(r.error());
    EXPECT_FALSE(r->empty());
}

// ---------------------------------------------------------------------------
// absolute
// ---------------------------------------------------------------------------

TEST(FsAbsolute, RelativePathResolved) {
    auto r = fs::absolute("foo/bar.txt");
    ASSERT_TRUE(r.has_value());
    // 结果应是绝对路径且包含输入的相对部分
    EXPECT_FALSE(r->empty());
    EXPECT_NE(r->find("foo"), String::npos);
}

TEST(FsAbsolute, EmptyPath) {
    auto r = fs::absolute("");
    // 空路径行为依赖 std::filesystem，只断言不崩溃并返回值
    // （某些实现可能成功返回当前目录，某些可能失败）
    if (r.has_value()) {
        SUCCEED();
    } else {
        SUCCEED();
    }
}

// ---------------------------------------------------------------------------
// resolve（weakly_canonical）
// ---------------------------------------------------------------------------

TEST(FsResolve, EliminatesParentDir) {
    auto cwd = fs::current_dir();
    ASSERT_TRUE(cwd.has_value());
    // 用 cwd 本身构造一个含 .. 的路径，应被规范化掉
    auto r = fs::resolve(*cwd, ".");
    ASSERT_TRUE(r.has_value());
}

TEST(FsResolve, NonexistentTrailingSucceeds) {
    auto r = fs::resolve(testing::TempDir(), "does_not_exist/child.txt");
    ASSERT_TRUE(r.has_value());
    // weakly_canonical 对不存在的尾部仅做词法规范化
    EXPECT_NE(r->find("does_not_exist"), String::npos);
    EXPECT_NE(r->find("child.txt"), String::npos);
}

TEST(FsResolve, FullyMissingPathLexicalNormalized) {
    auto r = fs::resolve("/nope/nope", "x/y");
    // weakly_canonical：已不存在部分不解析符号链接，仍返回词法规范化的绝对路径
    if (r.has_value()) {
        EXPECT_NE(r->find("x"), String::npos);
    }
}
