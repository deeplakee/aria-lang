#ifndef ARIA_FS_HPP
#define ARIA_FS_HPP

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <system_error>
#include "common.hpp"
#include "sys.hpp" // SYS_* 平台宏（本头直接使用，勿依赖 common.hpp 传递）

#if defined(SYS_WINDOWS)
    #include <windows.h>
#elif defined(SYS_LINUX)
    #include <climits>
    #include <unistd.h>
#elif defined(SYS_MACOS)
    #include <climits>
    #include <cstdlib>
    #include <mach-o/dyld.h>
#elif defined(SYS_FREEBSD)
    #include <climits>
    #include <sys/sysctl.h>
#endif

namespace aria::fs {
    namespace stdfs = std::filesystem;

    enum class FsErrCode : i32 {
        // === 通用/未分类错误 ===
        Unknown = 0, // 未知错误（兜底）

        // === 路径与名称相关 ===
        InvalidPath   = 100, // 路径格式非法（如空路径、非法字符）
        PathTooLong   = 101, // 超出系统路径长度限制
        NotFound      = 102, // 文件或目录不存在
        AlreadyExists = 103, // 创建时目标已存在

        // === 权限与安全 ===
        PermissionDenied = 200, // 权限不足
        ReadOnlyFs       = 201, // 只读文件系统

        // === I/O 与资源 ===
        IoError          = 300, // 底层读写I/O失败
        DiskFull         = 301, // 磁盘空间不足
        TooManyOpenFiles = 302, // 文件描述符耗尽
        IsDirectory      = 303, // 期望文件但遇到了目录
        NotADirectory    = 304, // 期望目录但遇到了文件
        FileInUse        = 305, // 文件被其他进程锁定/占用

        // === 符号链接相关 ===
        SymlinkLoop   = 400, // 符号链接循环
        BrokenSymlink = 401, // 悬空符号链接

        // === 操作语义错误 ===
        NotEmpty        = 500, // 删除非空目录
        CrossDeviceLink = 501, // 跨设备移动/硬链接
        UnsupportedOp   = 502, // 当前文件系统不支持该操作
        InvalidEncoding = 503, // 文件内容不是合法的 UTF-8
    };

    namespace detail {
        // 将 POSIX errno 映射到对应的 FsErrCode，未识别的归为 I/O 错误
        [[nodiscard]]
        inline FsErrCode errno_to_fserr(const int e) noexcept {
            switch (e) {
                case ENOENT:
                    return FsErrCode::NotFound;
                case EEXIST:
                    return FsErrCode::AlreadyExists;
                case EACCES:
                case EPERM:
                    return FsErrCode::PermissionDenied;
                case EROFS:
                    return FsErrCode::ReadOnlyFs;
                case ENAMETOOLONG:
                    return FsErrCode::PathTooLong;
                case EISDIR:
                    return FsErrCode::IsDirectory;
                case ENOTDIR:
                    return FsErrCode::NotADirectory;
                case ENOSPC:
                    return FsErrCode::DiskFull;
                case EMFILE:
                case ENFILE:
                    return FsErrCode::TooManyOpenFiles;
                case ELOOP:
                    return FsErrCode::SymlinkLoop;
                case ENOTEMPTY:
                    return FsErrCode::NotEmpty;
                case EXDEV:
                    return FsErrCode::CrossDeviceLink;
                case EBUSY:
                    return FsErrCode::FileInUse;
                default:
                    return FsErrCode::IoError;
            }
        }

        [[nodiscard]]
        inline FsErrCode errno_to_fserr() noexcept {
            return errno_to_fserr(errno);
        }

        // 将 std::error_code 映射到 FsErrCode：经 default_error_condition() 把平台相关错误码
        // （如 Windows 错误码）归一化为可移植的 POSIX 条件值后再映射
        [[nodiscard]]
        inline FsErrCode to_fserr(const std::error_code& ec) noexcept {
            return errno_to_fserr(ec.default_error_condition().value());
        }

        [[nodiscard]]
        inline Result<String, FsErrCode> executable_path() {
#if defined(SYS_WINDOWS)
            // GetModuleFileNameW 不会告知所需缓冲区大小，需自行轮询扩容
            auto buf = std::wstring{MAX_PATH, L'\0'};
            for (;;) {
                const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                if (len == 0) {
                    return std::unexpected(FsErrCode::IoError);
                }
                if (len < buf.size()) {
                    return stdfs::path(buf.data(), buf.data() + len).string();
                }
                buf.resize(buf.size() * 2);
            }
#elif defined(SYS_LINUX)
            // /proc/self/exe 是指向可执行文件的符号链接，readlink 读取其指向
            char          buf[4096];
            const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf));
            if (len < 0)
                return std::unexpected(errno_to_fserr());
            return String(buf, static_cast<usize>(len));
#elif defined(SYS_MACOS)
            char buf[PATH_MAX];
            u32  len = sizeof(buf);
            if (_NSGetExecutablePath(buf, &len) != 0)
                return std::unexpected(FsErrCode::PathTooLong);
            // 解析路径中可能的符号链接与 "."
            char real[PATH_MAX];
            if (!::realpath(buf, real))
                return std::unexpected(errno_to_fserr());
            return String{real};
#elif defined(SYS_FREEBSD)
            int   mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
            char  buf[PATH_MAX];
            usize len = sizeof(buf);
            if (::sysctl(mib, 4, buf, &len, nullptr, 0) < 0)
                return std::unexpected(errno_to_fserr());
            return String{buf};
#else
            return std::unexpected(FsErrCode::UnsupportedOp);
#endif
        }
    } // namespace detail


    // 以 seekg(end)+tellg 探测大小后一次读入：对报告 st_size=0 的特殊文件（/proc、sysfs 等）
    // 会静默得到空串而非报错，本函数面向常规文件（源码加载）。
    [[nodiscard]]
    inline Result<String, FsErrCode> read_file(const StringView path) {
        auto file = std::ifstream{String{path}, std::ios::in | std::ios::binary};
        if (!file) {
            return std::unexpected(detail::errno_to_fserr());
        }

        file.seekg(0, std::ios::end);
        const auto end_pos = file.tellg();
        if (!file || static_cast<std::streamoff>(end_pos) < 0) {
            return std::unexpected(FsErrCode::IoError);
        }

        file.seekg(0, std::ios::beg);

        const auto size   = static_cast<usize>(end_pos);
        auto       result = String(size, '\0'); // 例外：大括号会触发 initializer_list 窄化
        if (size > 0) {
            file.read(result.data(), static_cast<std::streamsize>(size));
            if (!file) // 实际读入字节数不足或发生 I/O 错误
            {
                return std::unexpected(FsErrCode::IoError);
            }
        }
        return result;
    }


    [[nodiscard]]
    inline Result<String, FsErrCode> current_dir() {
        std::error_code ec;
        const auto      p = stdfs::current_path(ec);
        if (ec) {
            return std::unexpected(detail::to_fserr(ec));
        }
        return p.string();
    }

    [[nodiscard]]
    inline Result<String, FsErrCode> program_dir() {
        auto exe = detail::executable_path();
        if (!exe) {
            return std::unexpected(exe.error());
        }
        return stdfs::path{*exe}.parent_path().string();
    }

    // 返回绝对路径（仅按当前工作目录补全，不解析符号链接与 "."/".."）
    [[nodiscard]]
    inline Result<String, FsErrCode> absolute(const StringView path) {
        std::error_code ec;
        const auto      p = stdfs::absolute(stdfs::path{String{path}}, ec);
        if (ec) {
            return std::unexpected(detail::to_fserr(ec));
        }
        return p.string();
    }

    // B 是相对 A 的相对路径，返回 B 的绝对路径：拼接后经 weakly_canonical 规范化（解析已存在部分
    // 的符号链接、消除 "."/".."）；尚不存在的尾部仅做词法规范化，目标不存在也成功返回。
    [[nodiscard]]
    inline Result<String, FsErrCode> resolve(const StringView base, const StringView rel) {
        std::error_code ec;
        const auto      p = stdfs::weakly_canonical(stdfs::path{String{base}} / String{rel}, ec);
        if (ec) {
            return std::unexpected(detail::to_fserr(ec));
        }
        return p.string();
    }

    // 把文件路径拆为入口模块身份 {name, dir}：name = basename 去 .aria 后缀（stem 剥最后扩展名）；
    // dir = dirname(absolute(path))，使 dir + "/" + name + ".aria" 还原原文件、相对导入以同级目录为基
    // （absolute 失败退化为原路径，best effort）。name 可能为空（目录/空/无文件名），调用方据此判定加载错误。
    // 纯路径工具，不读盘、不校验存在性--配合 SourceFile::from_path 的 I/O 结果使用。
    [[nodiscard]]
    inline Pair<String, String> module_name_and_dir(const StringView path) {
        stdfs::path abs_p{String{path}};
        if (const auto abs = absolute(path)) {
            abs_p = stdfs::path{*abs};
        }
        String name = abs_p.filename().stem().string(); // 剥最后一个扩展名（.aria -> 模块名）
        String dir  = abs_p.parent_path().string();
        return {std::move(name), std::move(dir)};
    }
} // namespace aria::fs

#endif // ARIA_FS_HPP
