#include "moha/tool/util/fs_helpers.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <share.h>
#  include <process.h>
#  include <sys/stat.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
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
#ifdef _WIN32
    // Native Win32 path: CreateFileW with FILE_FLAG_SEQUENTIAL_SCAN tells
    // the cache manager to read ahead aggressively (larger prefetch window,
    // pages discarded after use). For the read / grep / edit hot path —
    // one linear pass, no seeks — this is materially faster than the CRT
    // ReadFile that std::ifstream lowers to. Also avoids the CRT's
    // per-char buffer translation overhead on large files.
    if (!ec && sz > 0) {
        HANDLE h = ::CreateFileW(p.wstring().c_str(),
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                 nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string out;
            out.resize(sz);
            char*  buf   = out.data();
            size_t total = 0;
            const size_t want = static_cast<size_t>(sz);
            while (total < want) {
                DWORD chunk = static_cast<DWORD>(
                    std::min<size_t>(want - total, 1u << 20));   // 1 MiB reads
                DWORD got = 0;
                if (!::ReadFile(h, buf + total, chunk, &got, nullptr) || got == 0)
                    break;
                total += got;
            }
            ::CloseHandle(h);
            if (total != want) out.resize(total);
            return out;
        }
        // Fall through to ifstream on CreateFileW failure (rare: locked /
        // permission-denied files where the CRT might still retry through
        // a different path). Better to degrade gracefully than refuse.
    }
#endif
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

    // Atomic write: write to a sibling temp file, fsync, rename over target.
    // Crash or power-loss mid-write leaves the original file intact; only
    // the fully-written, fsync'd copy ever becomes visible at the target
    // path. The temp must live in the same directory so the rename is a
    // single filesystem operation — cross-device rename falls back to a
    // non-atomic copy.
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = seq.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    fs::path tmp = p;
    tmp += fs::path(".moha-tmp-" + std::to_string(pid) + "-" + std::to_string(n));

    // Preserve existing mode on POSIX so the rename doesn't regress perms.
#ifndef _WIN32
    mode_t target_mode = 0644;
    bool   had_mode    = false;
    {
        struct stat st{};
        if (::stat(p.c_str(), &st) == 0) {
            target_mode = st.st_mode & 07777;
            had_mode    = true;
        }
    }
#endif

    // Drop down to the POSIX/Win32 fd so we can fsync before the rename.
    // ofstream::flush only empties the libstdc++ streambuf into the OS —
    // power-loss / crash can still lose the bytes, and on some FUSE and
    // network filesystems the data isn't readable by the next open until
    // fsync completes.
#ifdef _WIN32
    // Use the wide-char variant so Unicode paths (e.g. under a non-ASCII
    // %USERPROFILE%) round-trip correctly. `_sopen_s` takes an ANSI/MBCS
    // path which silently corrupts multi-byte sequences on some MinGW
    // ucrt configurations.
    int fd = -1;
    auto ws = tmp.wstring();
    if (::_wsopen_s(&fd, ws.c_str(),
                    _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                    _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fd < 0)
        return "cannot open '" + p.string() + "' for writing";
#else
    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::string("cannot open '") + p.string() + "' for writing: "
             + explain_errno(errno);
#endif

    auto cleanup_tmp = [&] {
        std::error_code ec;
        fs::remove(tmp, ec);
    };

    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
#ifdef _WIN32
        int n2 = _write(fd, data, static_cast<unsigned>(
            remaining > 0x7fffffff ? 0x7fffffff : remaining));
#else
        ssize_t n2 = ::write(fd, data, remaining);
        if (n2 < 0 && errno == EINTR) continue;
#endif
        if (n2 <= 0) {
            std::string err = std::string("write to '") + p.string()
                + "' failed: " + explain_errno(errno);
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
            cleanup_tmp();
            return err;
        }
        data += n2;
        remaining -= static_cast<size_t>(n2);
    }

    // fsync temp: data is durable before the rename publishes it.
#ifdef _WIN32
    (void)_commit(fd);
    _close(fd);
#else
    if (had_mode) (void)::fchmod(fd, target_mode);
    (void)::fdatasync(fd);
    ::close(fd);
#endif

    // Atomic publish. Windows MoveFileExW with REPLACE_EXISTING is atomic on
    // NTFS (single MFT update); WRITE_THROUGH flushes the directory entry.
    // POSIX rename() is atomic by spec when src/dst are on the same FS.
#ifdef _WIN32
    auto tmp_w = tmp.wstring();
    auto dst_w = p.wstring();
    if (!::MoveFileExW(tmp_w.c_str(), dst_w.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = ::GetLastError();
        cleanup_tmp();
        return "atomic rename to '" + p.string() + "' failed (GLE=" + std::to_string(e) + ")";
    }
#else
    if (::rename(tmp.c_str(), p.c_str()) != 0) {
        std::string err = std::string("atomic rename to '") + p.string()
            + "' failed: " + explain_errno(errno);
        cleanup_tmp();
        return err;
    }
    // fsync parent dir so the rename itself survives power loss.
    if (!parent.empty()) {
        int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
    }
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
