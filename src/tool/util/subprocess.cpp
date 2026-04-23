#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/registry.hpp"
#include "moha/tool/util/utf8.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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

// UTF-8 → UTF-16. Win32 process APIs are UTF-16 natively; anything narrower
// routes through the ANSI code page and silently corrupts non-ASCII bytes.
std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

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

// Return true when the argv path resolves to a .bat / .cmd file on PATH.
// CreateProcess cannot natively spawn batch files — they require cmd.exe —
// but tools like npx / npm / yarn ship as .cmd on Windows, and the model
// will reach for them. SearchPathW honors PATHEXT so .cmd is found even
// when the caller wrote just "npx".
bool resolves_to_batch(const std::string& exe) {
    auto we = utf8_to_wide(exe);
    wchar_t out[MAX_PATH * 2]{};
    DWORD n = ::SearchPathW(nullptr, we.c_str(), L".exe",
                            (DWORD)std::size(out), out, nullptr);
    if (n == 0 || n >= std::size(out)) {
        // .exe miss — try default PATHEXT order (batch files second).
        n = ::SearchPathW(nullptr, we.c_str(), nullptr,
                          (DWORD)std::size(out), out, nullptr);
        if (n == 0 || n >= std::size(out)) return false;
    }
    std::wstring_view resolved{out, n};
    auto dot = resolved.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    auto ext = resolved.substr(dot);
    auto ieq = [](wchar_t a, wchar_t b) {
        return (a >= L'A' && a <= L'Z' ? a - L'A' + L'a' : a)
            == (b >= L'A' && b <= L'Z' ? b - L'A' + L'a' : b);
    };
    auto equal_i = [&](std::wstring_view s, std::wstring_view t) {
        if (s.size() != t.size()) return false;
        for (size_t i = 0; i < s.size(); ++i) if (!ieq(s[i], t[i])) return false;
        return true;
    };
    return equal_i(ext, L".cmd") || equal_i(ext, L".bat");
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
    // 64 KiB pipe buffer instead of the default (4 KiB). A chatty child
    // (cmake/clang/msbuild, test runners, npm) fills a 4 KiB pipe in a
    // single printf and blocks on write until our reader thread drains it,
    // serializing the child's stdout with our UI loop. 64 KiB soaks a full
    // compile-step's output so the child keeps running while we read.
    if (!::CreatePipe(&rd, &wr, &sa, 64 * 1024)) {
        r.started = false; r.start_error = "CreatePipe failed"; return r;
    }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        ::CloseHandle(rd); ::CloseHandle(wr);
        r.started = false; r.start_error = "CreateFile(NUL) failed"; return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = wr;
    si.hStdError  = wr;

    // CreateProcessW takes a mutable LPWSTR — widen the UTF-8 cmdline and
    // give it a writable buffer.
    std::wstring wcmd = utf8_to_wide(cmdline);
    std::vector<wchar_t> mutable_cmdline(wcmd.begin(), wcmd.end());
    mutable_cmdline.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, mutable_cmdline.data(),
                               nullptr, nullptr,
                               TRUE,
                               CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                               nullptr, nullptr,
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

    // Reader thread drains the pipe into `shared_buf` under a mutex. The
    // previous single-thread loop read synchronously with no deadline —
    // a child that spawned a detached grandchild (and so kept the write
    // end of the pipe open after its own exit) or that wedged without
    // emitting output would pin ReadFile forever, because the outer
    // WaitForSingleObject only ran *after* the read loop closed.
    //
    // With the reader split off:
    //   • main thread does WaitForSingleObject(pi.hProcess, …) with the
    //     full timeout in short chunks, flushing progress between chunks;
    //   • timeout fires → TerminateProcess → child's write end closes →
    //     reader's ReadFile returns 0 → reader thread exits cleanly;
    //   • grandchild-keeps-pipe-alive edge case → after the terminate,
    //     we CloseHandle(rd) to force-unblock the reader.
    std::mutex          buf_mu;
    std::ostringstream  shared_buf;
    std::size_t         shared_total = 0;
    bool                shared_truncated = false;
    std::atomic<bool>   reader_done{false};

    std::thread reader([&, rd]{
        char tmp[4096];
        for (;;) {
            DWORD n = 0;
            if (!::ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) || n == 0) break;
            std::lock_guard lk(buf_mu);
            if (shared_truncated) continue;
            std::size_t room = (shared_total < (std::size_t)opts.max_bytes)
                             ? (std::size_t)opts.max_bytes - shared_total : 0;
            std::size_t w = n < room ? n : room;
            shared_buf.write(tmp, (std::streamsize)w);
            shared_total += w;
            if (w < (std::size_t)n) shared_truncated = true;
        }
        reader_done.store(true, std::memory_order_release);
    });

    auto snapshot = [&]{
        std::lock_guard lk(buf_mu);
        return shared_buf.str();
    };

    auto now_ms = []{
        return std::chrono::steady_clock::now();
    };

    const bool has_deadline = opts.timeout.count() > 0;
    const auto deadline = has_deadline
        ? now_ms() + std::chrono::milliseconds(opts.timeout.count() * 1000)
        : std::chrono::steady_clock::time_point::max();
    auto last_emit = now_ms();

    bool timed_out = false;
    for (;;) {
        auto now = now_ms();
        auto remaining_deadline = has_deadline
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            : std::chrono::milliseconds::max();
        if (has_deadline && remaining_deadline.count() <= 0) {
            timed_out = true;
            break;
        }
        auto to_emit = kEmitGap - std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_emit);
        if (to_emit.count() < 0) to_emit = std::chrono::milliseconds{0};
        auto sleep_ms = std::min<std::chrono::milliseconds>(
            to_emit, has_deadline ? remaining_deadline : std::chrono::milliseconds{1000});

        DWORD w = ::WaitForSingleObject(
            pi.hProcess,
            (DWORD)std::max<std::chrono::milliseconds::rep>(sleep_ms.count(), 0));
        if (w == WAIT_OBJECT_0) break;   // child exited

        auto after = now_ms();
        if (opts.on_progress && (after - last_emit) >= kEmitGap) {
            opts.on_progress(to_valid_utf8(snapshot()));
            last_emit = after;
        }
    }

    DWORD exit_code = 0;
    if (timed_out) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        r.timed_out = true;
    } else {
        ::GetExitCodeProcess(pi.hProcess, &exit_code);
        r.exit_code = (int)exit_code;
    }

    // Give the reader a grace window to drain the remaining pipe bytes
    // after the child exited (its write end closed → ReadFile is returning).
    // Grandchildren that inherited the pipe can hold it open past the
    // parent's death — if the reader is still blocked, force-close rd.
    const auto grace_deadline = now_ms() + std::chrono::milliseconds(500);
    while (!reader_done.load(std::memory_order_acquire)
           && now_ms() < grace_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::CloseHandle(rd);   // safe: even if reader is mid-ReadFile, it returns false
    reader.join();

    if (opts.on_progress) {
        opts.on_progress(to_valid_utf8(snapshot()));
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    {
        std::lock_guard lk(buf_mu);
        r.truncated = shared_truncated;
        r.output    = to_valid_utf8(shared_buf.str());
    }
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
        // If argv[0] is a .cmd / .bat on PATH, wrap through cmd.exe — raw
        // CreateProcess refuses batch files (they need the interpreter).
        // Checked once up-front so we don't quote twice on the happy path.
        const bool needs_cmd_shell = resolves_to_batch((*opts.argv)[0]);
        for (size_t i = 0; i < opts.argv->size(); ++i) {
            if (i) cmdline.push_back(' ');
            cmdline += win_quote_arg((*opts.argv)[i]);
        }
        if (needs_cmd_shell)
            cmdline = "cmd.exe /S /C \"" + cmdline + "\"";
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

SubprocessResult run_command_s(const std::string& cmd,
                               std::size_t max_bytes,
                               std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.shell_command = cmd;
    opts.max_bytes   = max_bytes;
    opts.timeout     = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

SubprocessResult run_argv_s(const std::vector<std::string>& argv,
                            std::size_t max_bytes,
                            std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.argv        = argv;
    opts.max_bytes   = max_bytes;
    opts.timeout     = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

std::string legacy_format(const SubprocessResult& r, std::chrono::seconds timeout) {
    if (!r.started) return "[" + r.start_error + "]";
    std::string o = r.output;
    if (r.truncated) o += "\n[output truncated]";
    if (r.timed_out) o += "\n[timed out after " + std::to_string(timeout.count()) + "s]";
    else if (r.exit_code != 0) o += "\n[exit code " + std::to_string(r.exit_code) + "]";
    return o;
}

std::string run_command(const std::string& cmd,
                        std::size_t max_bytes,
                        std::chrono::seconds timeout) {
    return legacy_format(run_command_s(cmd, max_bytes, timeout), timeout);
}

std::string run_argv(const std::vector<std::string>& argv,
                     std::size_t max_bytes,
                     std::chrono::seconds timeout) {
    return legacy_format(run_argv_s(argv, max_bytes, timeout), timeout);
}

} // namespace moha::tools::util
