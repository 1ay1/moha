#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/registry.hpp"
#include "moha/tool/util/utf8.hpp"

#include <algorithm>
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
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
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

// In-process runner: posix_spawn the child with stdout+stderr piped back
// here, then poll(2) the read end with a deadline. Replaces the previous
// `popen("timeout ... sh -c ...")` design, which had two problems:
//   1. macOS doesn't ship GNU `timeout` (it lives in coreutils → brew),
//      so every bash call on a stock Mac failed at the shell layer.
//   2. Even on Linux it stacked an extra process layer, made stderr
//      structure invisible (forced 2>&1 in the shell), and gave us no
//      handle to actually kill the child if shell quoting went sideways.
//
// The new design owns the entire lifecycle: we hold the pid, we send
// SIGTERM at the deadline, we escalate to SIGKILL after a 2 s grace,
// and we close the read end ourselves once the child reaps so a
// grandchild that inherits the pipe can't block us forever (same edge
// case the Windows path handles via the reader-thread grace + force-
// close pattern).

namespace {

// Drain whatever's currently readable on `fd` without blocking.
// Returns true if EOF (read==0) was observed; the caller uses that to
// distinguish "more might come later" from "writer closed for good".
bool drain_pipe(int fd, std::ostringstream& out, std::size_t& total,
                std::size_t max_bytes, bool& truncated) {
    if (fd < 0) return true;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (truncated) continue;
            std::size_t room = (total < max_bytes) ? max_bytes - total : 0;
            std::size_t w = std::min<std::size_t>(static_cast<std::size_t>(n), room);
            out.write(buf, static_cast<std::streamsize>(w));
            total += w;
            if (w < static_cast<std::size_t>(n)) truncated = true;
            continue;
        }
        if (n == 0) return true;                          // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;                                       // fatal read error
    }
}

} // namespace

// argv form: arguments passed verbatim, no shell. shell_command form:
// `/bin/sh -c <cmd>`. Either way, env is inherited.
SubprocessResult run_posix(const std::vector<std::string>& argv_in,
                           bool                           use_shell,
                           const SubprocessOptions&       opts) {
    SubprocessResult r;

    // Pipe (child stdout+stderr → parent). Set the parent end non-blocking
    // so drain_pipe returns instead of stalling on partial reads. Use
    // pipe2(O_CLOEXEC) where available so the fd doesn't leak to nested
    // posix_spawns; fall back to fcntl on macOS.
    int pipefd[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
        r.started = false; r.start_error = "pipe2 failed: " + std::string{std::strerror(errno)};
        return r;
    }
#else
    if (::pipe(pipefd) != 0) {
        r.started = false; r.start_error = "pipe failed: " + std::string{std::strerror(errno)};
        return r;
    }
    (void)::fcntl(pipefd[0], F_SETFD, ::fcntl(pipefd[0], F_GETFD) | FD_CLOEXEC);
    (void)::fcntl(pipefd[1], F_SETFD, ::fcntl(pipefd[1], F_GETFD) | FD_CLOEXEC);
#endif
    (void)::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

    // file_actions: redirect stdin from /dev/null (so the child can't
    // steal terminal input from the TUI), and stdout+stderr to the pipe
    // write end. Close the read end in the child so grandchildren that
    // inherit fds don't hold it open after the child itself exits.
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        r.started = false; r.start_error = "posix_spawn_file_actions_init failed";
        return r;
    }
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
        "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_adddup2 (&actions, pipefd[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2 (&actions, pipefd[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[0]);

    // Build child argv. Shell form goes through `/bin/sh -c <cmd>` so
    // pipes / redirects / globs work; argv form bypasses the shell for
    // exact-arg fidelity (matters for `git commit -m "msg with $vars"`
    // and similar where the shell would mangle the message).
    std::vector<std::string> argv_storage;
    if (use_shell) {
        if (argv_in.empty()) {
            ::posix_spawn_file_actions_destroy(&actions);
            ::close(pipefd[0]); ::close(pipefd[1]);
            r.started = false; r.start_error = "empty shell command";
            return r;
        }
        argv_storage = {"sh", "-c", argv_in[0]};
    } else {
        argv_storage = argv_in;
    }
    std::vector<char*> arg_ptrs;
    arg_ptrs.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) arg_ptrs.push_back(s.data());
    arg_ptrs.push_back(nullptr);

    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, arg_ptrs[0], &actions, nullptr,
                            arg_ptrs.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);   // parent never writes
    pipefd[1] = -1;

    if (rc != 0) {
        ::close(pipefd[0]);
        r.started = false;
        r.start_error = "spawn failed: " + std::string{std::strerror(rc)};
        return r;
    }

    using clock = std::chrono::steady_clock;
    const auto start    = clock::now();
    const auto deadline = (opts.timeout.count() > 0)
        ? start + std::chrono::seconds(opts.timeout.count())
        : clock::time_point::max();
    constexpr auto kKillGrace = std::chrono::milliseconds{2000};

    auto last_emit  = start;
    auto kill_at    = clock::time_point::max();
    bool sent_term  = false;
    bool sent_kill  = false;
    bool timed_out  = false;
    bool eof        = false;
    int  wait_status = 0;

    std::ostringstream out;
    std::size_t total     = 0;
    bool        truncated = false;

    auto emit_progress = [&]{
        if (!opts.on_progress) return;
        opts.on_progress(to_valid_utf8(out.str()));
        last_emit = clock::now();
    };

    // Main poll loop. Exits as soon as we've got both EOF on the pipe AND
    // a reaped child — the order can vary (child can exit before its
    // last bytes drain on a heavily-buffered pipe; pipe can EOF before
    // waitpid completes if the child is being reparented).
    bool reaped = false;
    while (!eof || !reaped) {
        auto now = clock::now();

        // Deadline state machine. SIGTERM first (cooperative shutdown),
        // SIGKILL after a 2 s grace if still alive. timed_out is sticky
        // so we report it even if the child happened to exit cleanly
        // moments after we sent the signal.
        if (!sent_term && now >= deadline) {
            ::kill(pid, SIGTERM);
            sent_term = true;
            timed_out = true;
            kill_at   = now + kKillGrace;
        }
        if (sent_term && !sent_kill && now >= kill_at) {
            ::kill(pid, SIGKILL);
            sent_kill = true;
        }

        // Compute the next event we care about and bound the poll
        // accordingly. 100 ms ceiling so we still revisit the kill
        // state even when the child is silent and no timer is close.
        auto next = last_emit + kEmitGap;
        if (!sent_term && deadline < next) next = deadline;
        if (sent_term && !sent_kill && kill_at < next) next = kill_at;
        auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           next - now).count();
        if (wait_ms < 0)   wait_ms = 0;
        if (wait_ms > 100) wait_ms = 100;

        if (!eof) {
            struct pollfd pfd{pipefd[0], POLLIN, 0};
            int pn = ::poll(&pfd, 1, static_cast<int>(wait_ms));
            if (pn > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                if (drain_pipe(pipefd[0], out, total, opts.max_bytes, truncated))
                    eof = true;
            } else if (pn < 0 && errno != EINTR) {
                eof = true;   // poll error → stop trying to read
            }
        } else {
            // Pipe is done; just sleep briefly until reap completes.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min<long>(wait_ms, 20)));
        }

        // Non-blocking reap. If the child is gone but the pipe's still
        // open (grandchild inherited stdout), force-close the read end
        // so the next loop iteration sees eof=true and we exit cleanly
        // instead of waiting forever on a phantom writer.
        if (!reaped) {
            int status = 0;
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                wait_status = status;
                reaped = true;
                if (!eof) {
                    // One last best-effort drain of buffered bytes the
                    // child wrote between its final flush and exit.
                    drain_pipe(pipefd[0], out, total, opts.max_bytes, truncated);
                    ::close(pipefd[0]);
                    pipefd[0] = -1;
                    eof = true;
                }
            } else if (w < 0 && errno != EINTR && errno != ECHILD) {
                // Genuinely unexpected — bail rather than spin.
                reaped = true;
            }
        }

        now = clock::now();
        if (opts.on_progress && now - last_emit >= kEmitGap)
            emit_progress();
    }

    if (pipefd[0] >= 0) ::close(pipefd[0]);
    emit_progress();   // final flush so the UI sees the last bytes

    if      (WIFEXITED  (wait_status)) r.exit_code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) r.exit_code = 128 + WTERMSIG(wait_status);
    r.timed_out = timed_out;
    r.truncated = truncated;
    r.output    = to_valid_utf8(out.str());
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
    if (opts.shell_command) {
        // Shell form: pass the whole command string as the single sh -c
        // argument. The runner sets up sh "-c" "<cmd>" itself; no
        // additional quoting needed (and we don't want any — the user
        // passed shell syntax expecting it to be parsed verbatim).
        return run_posix({*opts.shell_command}, /*use_shell=*/true, opts);
    }
    if (opts.argv) {
        if (opts.argv->empty()) {
            r.started = false; r.start_error = "empty command"; return r;
        }
        // argv form: exec directly with no shell in the loop. Preserves
        // every byte of every arg, which is what callers like git_commit
        // (commit messages with $vars / quotes / newlines) actually need.
        return run_posix(*opts.argv, /*use_shell=*/false, opts);
    }
    r.started = false; r.start_error = "no command specified";
    return r;
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
