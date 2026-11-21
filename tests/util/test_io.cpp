#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

#include "sys.hpp"
#include "util/io.hpp"

#ifndef SYS_WINDOWS
    #include <cerrno>
    #include <csignal>
    #include <tuple>
    #include <unistd.h>
#endif

using aria::i32;
namespace io = aria::io;

TEST(Io, ReadsLinesAndRemainingTextFromFile) {
    const auto path   = (std::filesystem::path{testing::TempDir()} / "aria_io_reads_lines_and_remaining.txt").string();
    const auto output = std::fopen(path.c_str(), "wb");
    ASSERT_NE(output, nullptr);
    EXPECT_TRUE(io::write_text(output, "first\r\r\n\nremaining\r\n"));
    EXPECT_TRUE(io::write_line(output, "last"));
    EXPECT_TRUE(io::flush(output));
    EXPECT_EQ(std::fclose(output), 0);

    // Windows 文本模式会转换 CRLF，读操作须自行切换模式。
    const auto input = std::fopen(path.c_str(), "r");
    ASSERT_NE(input, nullptr);
    const auto first = io::read_line(input);
    EXPECT_TRUE(first && *first == "first\r");
    const auto empty = io::read_line(input);
    EXPECT_TRUE(empty && *empty == "");
    const auto remaining = io::read_all(input);
    EXPECT_TRUE(remaining && *remaining == "remaining\r\nlast\n");
    const auto end = io::read_line(input);
    EXPECT_TRUE(end && !*end);
    EXPECT_EQ(std::fclose(input), 0);
    std::filesystem::remove(path);
}

#ifndef SYS_WINDOWS
TEST(Io, ReadsAfterAnEarlierStreamError) {
    i32 descriptors[2];
    ASSERT_EQ(pipe(descriptors), 0);
    const auto stream = fdopen(descriptors[0], "r");
    EXPECT_EQ(write(descriptors[1], "first\nsecond\n", 13), 13);
    EXPECT_EQ(close(descriptors[1]), 0);
    if (stream == nullptr) {
        std::ignore = close(descriptors[0]);
    }
    ASSERT_NE(stream, nullptr);

    // 宿主对只读流的写入失败会留下错误标志。
    EXPECT_EQ(std::fputc('x', stream), EOF);
    EXPECT_NE(std::ferror(stream), 0);
    const auto line = io::read_line(stream);
    EXPECT_TRUE(line && *line == "first");

    EXPECT_EQ(std::fputc('x', stream), EOF);
    EXPECT_NE(std::ferror(stream), 0);
    const auto remaining = io::read_all(stream);
    EXPECT_TRUE(remaining && *remaining == "second\n");
    EXPECT_EQ(std::fclose(stream), 0);
}

TEST(Io, WritesAfterAnEarlierStreamError) {
    i32 descriptors[2];
    ASSERT_EQ(pipe(descriptors), 0);
    const auto stream = fdopen(descriptors[1], "w");
    if (stream == nullptr) {
        std::ignore = close(descriptors[0]);
        std::ignore = close(descriptors[1]);
    }
    ASSERT_NE(stream, nullptr);

    EXPECT_EQ(std::fgetc(stream), EOF);
    EXPECT_NE(std::ferror(stream), 0);
    EXPECT_TRUE(io::write_text(stream, "x"));
    EXPECT_EQ(std::fclose(stream), 0);

    char received = '\0';
    EXPECT_EQ(read(descriptors[0], &received, 1), 1);
    EXPECT_EQ(received, 'x');
    EXPECT_EQ(close(descriptors[0]), 0);
}

TEST(Io, FlushDiscardsBrokenPipeBufferWithSignalIgnored) {
    i32 descriptors[2];
    ASSERT_EQ(pipe(descriptors), 0);
    const auto stream = fdopen(descriptors[1], "w");
    EXPECT_EQ(close(descriptors[0]), 0);
    if (stream == nullptr) {
        std::ignore = close(descriptors[1]);
    }
    ASSERT_NE(stream, nullptr);

    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    EXPECT_EQ(sigemptyset(&ignored.sa_mask), 0);
    struct sigaction previous{};
    if (sigaction(SIGPIPE, &ignored, &previous) != 0) {
        std::ignore = std::fclose(stream);
        FAIL() << "sigaction failed";
    }

    char buffer[BUFSIZ];
    EXPECT_EQ(std::setvbuf(stream, buffer, _IOFBF, sizeof(buffer)), 0);
    EXPECT_TRUE(io::write_text(stream, "buffered"));
    EXPECT_FALSE(io::flush(stream));
    EXPECT_EQ(std::fclose(stream), 0);
    EXPECT_EQ(sigaction(SIGPIPE, &previous, nullptr), 0);
}

    #ifdef SYS_MACOS
TEST(Io, FlushPreservesUnrelatedSigpipe) {
    const auto write_with_io_error = [](void* cookie, const char* buffer, const i32 count) -> i32 {
        std::ignore = std::raise(SIGPIPE);
        errno       = EIO;
        return -1;
    };
    const auto stream = funopen(nullptr, nullptr, write_with_io_error, nullptr, nullptr);
    ASSERT_NE(stream, nullptr);

    static volatile std::sig_atomic_t was_sigpipe_delivered = 0;
    struct sigaction                  action{};
    action.sa_handler = [](i32 signal_number) { was_sigpipe_delivered = 1; };
    EXPECT_EQ(sigemptyset(&action.sa_mask), 0);
    struct sigaction previous{};
    if (sigaction(SIGPIPE, &action, &previous) != 0) {
        std::ignore = std::fclose(stream);
        FAIL() << "sigaction failed";
    }

    char buffer[BUFSIZ];
    EXPECT_EQ(std::setvbuf(stream, buffer, _IOFBF, sizeof(buffer)), 0);
    EXPECT_NE(std::fputs("buffered", stream), EOF);
    was_sigpipe_delivered = 0;
    EXPECT_FALSE(io::flush(stream));
    EXPECT_TRUE(was_sigpipe_delivered != 0);
    std::ignore = std::fclose(stream);
    EXPECT_EQ(sigaction(SIGPIPE, &previous, nullptr), 0);
}
    #endif
#endif
