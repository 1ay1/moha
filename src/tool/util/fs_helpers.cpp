#include "moha/tool/util/fs_helpers.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace moha::tools::util {

namespace {
// Translate the most common filesystem errno values to a sentence the
// model can act on. Raw `strerror` reads as "Permission denied" /
// "No such file or directory" — fine for humans, but the LLM responds
// better to the longer form ("you don't have write permission to X")
// when it's deciding whether to retry as a different path or surface a
// human ask. The caller appends the path/operation context.
std::string explain_errno(int e) {
    switch (e) {
        case EACCES:        return "permission denied";
        case EPERM:         return "operation not permitted (privileged op)";
        case ENOENT:        return "path not found";
        case ENOTDIR:       return "expected a directory but found a file";
        case EISDIR:        return "expected a file but found a directory";
        case ENOSPC:        return "out of disk space";
        case EROFS:         return "filesystem is read-only";
        case EMFILE:
        case ENFILE:        return "too many open files (process FD limit hit)";
        case ELOOP:         return "symlink loop";
        case ENAMETOOLONG:  return "path is too long";
        case EBUSY:         return "file is busy / locked by another process";
#ifdef EDQUOT
        case EDQUOT:        return "disk quota exceeded";
#endif
        default:            return std::strerror(e);
    }
}
} // namespace

std::string read_file(const fs::path& p) {
    // Size-then-read: avoid the ifstream→ostringstream→.str() chain, which
    // double-allocates and copies through the streambuf. One stat, one
    // malloc, one read. Fallback to streambuf drain if the size isn't known
    // (e.g. /proc, pipes) — those produce file_size==0 or throw.
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    if (!ec && sz > 0) {
        std::string out;
        out.resize(sz);
        ifs.read(out.data(), static_cast<std::streamsize>(sz));
        if (auto got = ifs.gcount(); static_cast<uintmax_t>(got) != sz)
            out.resize(static_cast<size_t>(got));
        return out;
    }
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}

std::string write_file(const fs::path& p, std::string_view content) {
    auto parent = p.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return "failed to create directory '" + parent.string() + "': " + ec.message();
    }
    // Drop down to the POSIX/Win32 fd so we can fsync before returning
    // success. ofstream::flush only empties the libstdc++ streambuf into the
    // OS — power-loss / crash can still lose the bytes, and on some FUSE
    // and network filesystems the data isn't readable by the next open until
    // fsync completes. Tools report "wrote N bytes" *after* the data is safe.
#ifdef _WIN32
    int fd = -1;
    auto s = p.string();
    if (_sopen_s(&fd, s.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                 _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fd < 0)
        return "cannot open '" + p.string() + "' for writing";
#else
    int fd = ::open(p.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::string("cannot open '") + p.string() + "' for writing: "
             + explain_errno(errno);
#endif
    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
#ifdef _WIN32
        int n = _write(fd, data, static_cast<unsigned>(
            remaining > 0x7fffffff ? 0x7fffffff : remaining));
#else
        ssize_t n = ::write(fd, data, remaining);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) {
            std::string err = std::string("write to '") + p.string()
                + "' failed: " + explain_errno(errno);
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
            return err;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }
    // fsync: data is durable before we report success. Ignore errors on
    // filesystems that don't support it (e.g. /tmp on tmpfs fdatasync is
    // cheap; /proc returns EINVAL — harmless).
#ifdef _WIN32
    (void)_commit(fd);
    _close(fd);
#else
    (void)::fdatasync(fd);
    ::close(fd);
#endif
    return {};
}

fs::path normalize_path(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))  s.remove_suffix(1);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                          || (s.front() == '\'' && s.back() == '\''))) {
        s.remove_prefix(1);
        s.remove_suffix(1);
    }
    fs::path p{s};
    std::error_code ec;
    if (!p.is_absolute()) p = fs::absolute(p, ec);
    return p;
}

bool should_skip_dir(std::string_view name) noexcept {
    static const std::vector<std::string_view> skip = {
        ".git", "node_modules", "build", "target", "__pycache__",
        ".cache", "vendor", "dist", "out", ".next", ".venv",
        "cmake-build-debug", "cmake-build-release", ".idea", ".vscode",
        "_deps", "third_party", "thirdparty", "3rdparty", "external",
    };
    for (auto s : skip) if (name == s) return true;
    return false;
}

bool is_binary_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return true;
    char buf[512];
    ifs.read(buf, sizeof(buf));
    auto n = ifs.gcount();
    for (int i = 0; i < n; ++i)
        if (buf[i] == '\0') return true;
    return false;
}

} // namespace moha::tools::util
