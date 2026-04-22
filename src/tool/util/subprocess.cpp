#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/registry.hpp"
#include "moha/tool/util/utf8.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace moha::tools::util {

namespace {

// Best-effort progress flush throttle. 80 ms keeps the UI responsive without
// drowning the event queue — the worst case (30 KB / flush) is a few hundred
// µs of UTF-8 decode work, negligible against the subprocess wall clock.
constexpr std::chrono::milliseconds kEmitGap{80};

#ifdef _WIN32

// CommandLineToArgvW-compatible quoting (see MSDN "Parsing C++ Command-Line
// Arguments"). Run of backslashes doubles if followed by `"`, literal `"`
// gets prefixed with `\`. Quoting only wraps the arg when it contains
// whitespace or `"`.
std::string win_quote_arg(const std::string& arg) {
    if (!arg.empty()
        && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out;
    out.push_back('"');
    int backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { backslashes++; continue; }
        if (c == '"') {
            out.append((size_t)backslashes * 2, '\\');
            out += "\\\"";
        } else {
            out.append((size_t)backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append((size_t)backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

// CreateProcess-based runner. Redirects the child's stdin to NUL so it
// can't steal keystrokes from the TUI or disturb the console mode. Saves +
// restores the stdin console mode as a belt-and-suspenders guard: a child
// that resets ENABLE_LINE_INPUT / ENABLE_ECHO_INPUT (bash, some shells)
// would otherwise make the next keystroke echo at the cursor instead of
// flowing into the composer.
SubprocessResult run_win32_cmdline(const std::string& cmdline,
                                   const SubprocessOptions& opts) {
    SubprocessResult r;
    HANDLE h_stdin = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD saved_in_mode = 0;
    bool  have_saved_mode =
        h_stdin != INVALID_HANDLE_VALUE && ::GetConsoleMode(h_stdin, &saved_in_mode);

    struct Restore {
        HANDLE h; DWORD mode; bool active;
        ~Restore() { if (active) ::SetConsoleMode(h, mode); }
    } restore{h_stdin, saved_in_mode, have_saved_mode};

    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!::CreatePipe(&rd, &wr, &sa, 0)) {
        r.started = false; r.start_error = "CreatePipe failed"; return r;
    }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = ::CreateFileA("NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        ::CloseHandle(rd); ::CloseHandle(wr);
        r.started = false; r.start_error = "CreateFile(NUL) failed"; return r;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = wr;
    si.hStdError  = wr;

    std::vector<char> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back('\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessA(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                               TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &si, &pi);
    ::CloseHandle(wr);
    ::CloseHandle(nul);
    if (!ok) {
        ::CloseHandle(rd);
        DWORD e = ::GetLastError();
        r.started = false;
        r.start_error = "CreateProcess failed (" + std::to_string(e) + ")";
        return r;
    }

    std::ostringstream out;
    size_t total = 0;
    char buf[4096];
    auto  last_emit = std::chrono::steady_clock::now();
    bool  emit_dirty = false;

    auto flush_progress = [&]{
        if (!emit_dirty || !opts.on_progress) { emit_dirty = false; return; }
        // Converting the full accumulated buffer each flush (not the delta)
        // is the cleanest way to handle UTF-8 boundaries — a multi-byte
        // sequence spanning two pipe reads still renders correctly once
        // the next flush sees both halves. 30 KB / 80 ms is ~300 µs.
        opts.on_progress(to_valid_utf8(out.str()));
        emit_dirty = false;
    };

    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(rd, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        if (!r.truncated) {
            size_t room = (total < (size_t)opts.max_bytes)
                        ? (size_t)opts.max_bytes - total : 0;
            size_t write = n < room ? n : room;
            out.write(buf, (std::streamsize)write);
            total += write;
            emit_dirty = true;
            if (write < (size_t)n) r.truncated = true;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_emit >= kEmitGap) {
            flush_progress();
            last_emit = now;
        }
    }
    flush_progress();  // final snapshot before the Result comes back
    ::CloseHandle(rd);

    DWORD timeout_ms = opts.timeout.count() > 0
                     ? (DWORD)opts.timeout.count() * 1000u
                     : INFINITE;
    DWORD wait = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD exit_code = 0;
    if (wait == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        r.timed_out = true;
    } else {
        ::GetExitCodeProcess(pi.hProcess, &exit_code);
        r.exit_code = (int)exit_code;
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    r.output = to_valid_utf8(out.str());
    return r;
}

#else // POSIX

std::string posix_shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// popen-based runner wrapped in GNU `timeout` so a hung command (network
// wait, infinite loop, REPL with no stdin close) can't block the worker
// thread forever and freeze the UI on the spinner.
SubprocessResult run_posix_shell(const std::string& cmd,
                                 const SubprocessOptions& opts) {
    SubprocessResult r;
    std::string wrapped = "timeout --kill-after=2s "
                        + std::to_string(opts.timeout.count())
                        + "s sh -c " + posix_shell_quote(cmd) + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
    if (!pipe) { r.started = false; r.start_error = "popen failed"; return r; }
    std::ostringstream out;
    std::array<char, 4096> buf{};
    size_t total = 0;
    auto  last_emit = std::chrono::steady_clock::now();
    bool  emit_dirty = false;
    auto flush_progress = [&]{
        if (!emit_dirty || !opts.on_progress) { emit_dirty = false; return; }
        opts.on_progress(to_valid_utf8(out.str()));
        emit_dirty = false;
    };
    while (fgets(buf.data(), (int)buf.size(), pipe)) {
        size_t n = std::strlen(buf.data());
        if (!r.truncated) {
            if (total + n > (size_t)opts.max_bytes) {
                out.write(buf.data(), (std::streamsize)((size_t)opts.max_bytes - total));
                total = (size_t)opts.max_bytes;
                r.truncated = true;
            } else {
                out << buf.data();
                total += n;
            }
            emit_dirty = true;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_emit >= kEmitGap) {
            flush_progress();
            last_emit = now;
        }
    }
    flush_progress();
    int rc = pclose(pipe);
    r.output = to_valid_utf8(out.str());
    // GNU `timeout` exits 124 on timeout, 137 on KILL after grace.
    if (rc == 124 * 256 || rc == 137 * 256) r.timed_out = true;
    else r.exit_code = (rc >> 8) & 0xff;
    return r;
}

#endif

} // namespace

SubprocessResult Subprocess::run(SubprocessOptions opts) {
    SubprocessResult r;

    // Build a final command line appropriate for this platform.
#ifdef _WIN32
    std::string cmdline;
    if (opts.shell_command) {
        // cmd.exe /S /C "…" — /S strips just the outermost quotes and
        // leaves everything else (including embedded "...") intact.
        cmdline = "cmd.exe /S /C \"" + *opts.shell_command + "\"";
    } else if (opts.argv) {
        if (opts.argv->empty()) {
            r.started = false; r.start_error = "empty command"; return r;
        }
        for (size_t i = 0; i < opts.argv->size(); ++i) {
            if (i) cmdline.push_back(' ');
            cmdline += win_quote_arg((*opts.argv)[i]);
        }
    } else {
        r.started = false; r.start_error = "no command specified"; return r;
    }
    return run_win32_cmdline(cmdline, opts);
#else
    std::string cmd;
    if (opts.shell_command) {
        cmd = *opts.shell_command;
    } else if (opts.argv) {
        if (opts.argv->empty()) {
            r.started = false; r.start_error = "empty command"; return r;
        }
        for (size_t i = 0; i < opts.argv->size(); ++i) {
            if (i) cmd.push_back(' ');
            cmd += posix_shell_quote((*opts.argv)[i]);
        }
    } else {
        r.started = false; r.start_error = "no command specified"; return r;
    }
    return run_posix_shell(cmd, opts);
#endif
}

// ── Convenience wrappers ────────────────────────────────────────────────
//
// on_progress defaults to `progress::emit`, the thread-local sink the cmd
// runner installs for tool execution. Wiring this at the wrapper level (not
// inside Subprocess::run) keeps the core runner free of app-specific state
// — other callers can skip the sink by going direct to Subprocess::run.

SubprocessResult run_command_s(const std::string& cmd, int max_bytes, int timeout_secs) {
    SubprocessOptions opts;
    opts.shell_command = cmd;
    opts.max_bytes   = max_bytes;
    opts.timeout     = std::chrono::seconds{timeout_secs};
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

SubprocessResult run_argv_s(const std::vector<std::string>& argv, int max_bytes, int timeout_secs) {
    SubprocessOptions opts;
    opts.argv        = argv;
    opts.max_bytes   = max_bytes;
    opts.timeout     = std::chrono::seconds{timeout_secs};
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

std::string legacy_format(const SubprocessResult& r, int timeout_secs) {
    if (!r.started) return "[" + r.start_error + "]";
    std::string o = r.output;
    if (r.truncated) o += "\n[output truncated]";
    if (r.timed_out) o += "\n[timed out after " + std::to_string(timeout_secs) + "s]";
    else if (r.exit_code != 0) o += "\n[exit code " + std::to_string(r.exit_code) + "]";
    return o;
}

std::string run_command(const std::string& cmd, int max_bytes, int timeout_secs) {
    return legacy_format(run_command_s(cmd, max_bytes, timeout_secs), timeout_secs);
}

std::string run_argv(const std::vector<std::string>& argv, int max_bytes, int timeout_secs) {
    return legacy_format(run_argv_s(argv, max_bytes, timeout_secs), timeout_secs);
}

} // namespace moha::tools::util
