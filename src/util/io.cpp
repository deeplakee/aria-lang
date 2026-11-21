#include "util/io.hpp"

#include <cerrno>
#include <tuple>

#include "sys.hpp"

#ifdef SYS_WINDOWS
    #include <fcntl.h>
    #include <io.h>
#endif

namespace aria::io {

    namespace {

        class StreamLock {
        public:
            explicit StreamLock(std::FILE* stream) : stream_{stream} {
#ifdef SYS_WINDOWS
                _lock_file(stream_);
#else
                flockfile(stream_);
#endif
            }

            ~StreamLock() {
#ifdef SYS_WINDOWS
                _unlock_file(stream_);
#else
                funlockfile(stream_);
#endif
            }

            StreamLock(const StreamLock&)            = delete;
            StreamLock& operator=(const StreamLock&) = delete;

        private:
            std::FILE* stream_;
        };

        class InputMode {
        public:
#ifdef SYS_WINDOWS
            explicit InputMode(std::FILE* stream) : descriptor_{_fileno(stream)} {
                if (descriptor_ >= 0 && !_isatty(descriptor_)) {
                    // 重定向输入须保留原始行尾，由读行操作识别 LF 与 CRLF。
                    previous_mode_ = _setmode(descriptor_, _O_BINARY);
                }
            }

            ~InputMode() {
                if (previous_mode_ > 0 && previous_mode_ != _O_BINARY) {
                    std::ignore = _setmode(descriptor_, previous_mode_);
                }
            }

            InputMode(const InputMode&)            = delete;
            InputMode& operator=(const InputMode&) = delete;

            [[nodiscard]]
            bool is_ready() const noexcept {
                return descriptor_ >= 0 && previous_mode_ != -1;
            }

        private:
            i32 descriptor_;
            i32 previous_mode_ = 0;
#else
            explicit InputMode(std::FILE* stream) noexcept {}

            ~InputMode() = default;

            InputMode(const InputMode&)            = delete;
            InputMode& operator=(const InputMode&) = delete;

            [[nodiscard]]
            bool is_ready() const noexcept {
                return true;
            }
#endif
        };

        void clear_previous_error(std::FILE* stream) noexcept {
            // 正常到达 EOF 后，后续读取沿用 stdio 的 EOF 状态。
            if (std::ferror(stream)) {
                std::clearerr(stream);
            }
        }

        void discard_broken_pipe_buffer(std::FILE* stream, const i32 error_number) noexcept {
#if defined(SYS_MACOS) || defined(SYS_IOS) || defined(SYS_FREEBSD)
            if (error_number == EPIPE) {
                // stdio 可能保留断管时写不出的缓冲，并在关闭流时重试。
                std::ignore = fpurge(stream);
            }
#endif
        }

        bool write_text_unlocked(std::FILE* stream, const StringView text) {
            clear_previous_error(stream);
            if (text.empty()) {
                return true;
            }

            errno              = 0;
            const auto written = std::fwrite(text.data(), 1, text.size(), stream);
            if (written == text.size() && !std::ferror(stream)) {
                return true;
            }

            discard_broken_pipe_buffer(stream, errno);
            std::clearerr(stream);
            return false;
        }

    } // namespace

    Result<Opt<String>, std::errc> read_line(std::FILE* stream) {
        const StreamLock lock{stream};
        const InputMode  mode{stream};
        if (!mode.is_ready()) {
            std::clearerr(stream);
            return std::unexpected(std::errc::io_error);
        }
        clear_previous_error(stream);

        auto line = String{};
        auto byte = std::getc(stream);
        while (byte != EOF && byte != '\n') {
            line.push_back(static_cast<char>(byte));
            byte = std::getc(stream);
        }
        if (std::ferror(stream)) {
            std::clearerr(stream);
            return std::unexpected(std::errc::io_error);
        }
        if (byte == EOF && line.empty()) {
            return std::nullopt;
        }
        if (byte == '\n' && !line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    }

    Result<String, std::errc> read_all(std::FILE* stream) {
        const StreamLock lock{stream};
        const InputMode  mode{stream};
        if (!mode.is_ready()) {
            std::clearerr(stream);
            return std::unexpected(std::errc::io_error);
        }
        clear_previous_error(stream);

        auto  text = String{};
        char  buffer[4096];
        usize count;
        do {
            count = std::fread(buffer, 1, sizeof(buffer), stream);
            text.append(buffer, count);
        } while (count == sizeof(buffer));
        if (std::ferror(stream)) {
            std::clearerr(stream);
            return std::unexpected(std::errc::io_error);
        }
        return text;
    }

    bool write_text(std::FILE* stream, const StringView text) {
        const StreamLock lock{stream};
        return write_text_unlocked(stream, text);
    }

    bool write_line(std::FILE* stream, const StringView text) {
        const StreamLock lock{stream};
        if (!text.empty() && !write_text_unlocked(stream, text)) {
            return false;
        }
        return write_text_unlocked(stream, "\n");
    }

    bool flush(std::FILE* stream) {
        const StreamLock lock{stream};
        if (std::fflush(stream) == EOF) {
            discard_broken_pipe_buffer(stream, errno);
            std::clearerr(stream);
            return false;
        }
        return true;
    }

} // namespace aria::io
