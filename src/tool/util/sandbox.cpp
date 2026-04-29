#include "moha/tool/util/sandbox.hpp"

#include "moha/tool/util/fs_helpers.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace moha::tools::util::sandbox {

namespace fs = std::filesystem;

namespace {

// Single-process state. Set by init() at startup, read by every bash
// call afterwards. Atomics aren't strictly needed (set once, never
// flipped after main has handed off to maya), but they cost nothing
// and document the read-many lifecycle.
std::atomic<Mode>    g_mode{Mode::Auto};
std::atomic<Backend> g_backend{Backend::None};

// Only the POSIX backends (Linux bwrap, macOS sandbox-exec) need the
// "can we run this binary?" probe — the Windows/unsupported branch
// just hard-codes Backend::None.
#if defined(__linux__) || defined(__APPLE__)

// One-shot probe: try to spawn `<exe> --version` and observe the
// outcome. Replaces a fragile PATH walk with the actual semantic
// check we care about ("can we run this binary?"). The result is
// thrown away — exit code 0 just means the binary exists and starts.
[[nodiscard]] bool can_invoke(const char* exe) {
    SubprocessOptions opts;
    opts.argv = std::vector<std::string>{exe, "--version"};
    opts.timeout = std::chrono::seconds{2};
    opts.max_bytes = 4096;
    auto r = Subprocess::run(std::move(opts));
    return r.started && r.exit_code == 0;
}

#endif // posix backends

#if defined(__linux__)

[[nodiscard]] Backend probe() {
    return can_invoke("bwrap") ? Backend::Bwrap : Backend::None;
}

// Build the bwrap argv prefix. Workspace gets read-write bound to
// itself; system dirs are bound read-only so the shell can find
// /bin/sh, libc, /etc/resolv.conf, etc.; /tmp is a fresh tmpfs (no
// leakage into / from the host /tmp); /proc and /dev are minimal;
// network namespace is shared (so `git push` / `npm install` / `curl`
// keep working — sandboxing those would break legitimate agent flows
// users expect to work).
//
// The intent is "bash can do anything WITHIN your workspace, plus
// network, plus read system libs — but it cannot mutate /etc,
// /home/<other-projects>, ~/.ssh, /opt, etc." That's the threat model
// we're actually defending against: a compromised or sloppy model
// running `rm -rf ~` or `cat ~/.ssh/id_rsa` after one user approval.
//
// `--die-with-parent` ensures the sandbox dies if moha dies — no
// detached zombies. `--unshare-pid` gives the child a clean PID
// namespace so kills work cleanly. `--new-session` so the child can't
// steal the controlling tty.
[[nodiscard]] std::vector<std::string> build_bwrap_argv(std::string_view shell_cmd) {
    std::string ws = workspace_root().string();
    std::vector<std::string> argv = {"bwrap"};

    auto push = [&](const char* a) { argv.emplace_back(a); };
    auto push_pair = [&](const char* k, std::string v) {
        argv.emplace_back(k);
        argv.emplace_back(std::move(v));
    };
    auto push_bind = [&](const char* k, const char* p) {
        argv.emplace_back(k);
        argv.emplace_back(p);
        argv.emplace_back(p);
    };

    // System dirs first: read-only. `--ro-bind-try` skips silently if
    // the path doesn't exist on this distro (e.g. /lib64 on Alpine).
    push_bind("--ro-bind",     "/usr");
    push_bind("--ro-bind",     "/bin");
    push_bind("--ro-bind",     "/etc");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/lib"); argv.emplace_back("/lib");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/lib64"); argv.emplace_back("/lib64");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/sbin"); argv.emplace_back("/sbin");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/opt"); argv.emplace_back("/opt");

    // Pseudo-fs
    push_pair("--proc", "/proc");
    push_pair("--dev",  "/dev");

    // Fresh /tmp inside the sandbox. MUST come before the workspace
    // bind: when workspace lives under /tmp (common in test setups),
    // bwrap applies args in order and a later --tmpfs would wipe the
    // workspace overlay. Bind workspace LAST so it always wins.
    push_pair("--tmpfs", "/tmp");

    // Workspace: read-write. Bound LAST so it overlays any earlier
    // --tmpfs / --ro-bind that touches the same prefix.
    argv.emplace_back("--bind");
    argv.emplace_back(ws);
    argv.emplace_back(ws);

    // Network: keep it. Removing this breaks git push / package
    // installs / curl — flows users explicitly want to work.
    push("--share-net");

    // Process / session hardening
    push("--unshare-pid");
    push("--new-session");
    push("--die-with-parent");

    // Pass through the workspace-relative cwd so the shell starts where
    // the user expects. Default cwd would be / inside the sandbox.
    argv.emplace_back("--chdir");
    argv.emplace_back(ws);

    // The actual shell command
    argv.emplace_back("--");
    argv.emplace_back("/bin/sh");
    argv.emplace_back("-c");
    argv.emplace_back(std::string{shell_cmd});
    return argv;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.argv = build_bwrap_argv(cmd);
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

// argv-form: wrap with the same bwrap prefix as the shell form, then
// append the user's argv after the `--` separator. No `sh -c`
// indirection — the args reach the child process exactly as given.
[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    // Build prefix with no shell command, then splice the user's argv.
    auto wrapped = build_bwrap_argv("");
    // Pop the trailing 4 elements added by build_bwrap_argv ("--",
    // "/bin/sh", "-c", ""), then append user argv directly.
    wrapped.resize(wrapped.size() - 4);
    wrapped.emplace_back("--");
    for (const auto& a : user_argv) wrapped.push_back(a);

    SubprocessOptions opts;
    opts.argv = std::move(wrapped);
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

#elif defined(__APPLE__)

[[nodiscard]] Backend probe() {
    return can_invoke("sandbox-exec") ? Backend::SandboxExec : Backend::None;
}

// Generate a minimal sandbox-exec profile. Allows reads broadly,
// limits writes to workspace + tmp + system caches, allows network
// (same rationale as bwrap: agent-typical commands need it).
//
// Apple deprecated `sandbox-exec` in public docs but the binary keeps
// working. The profile language is Scheme-ish; we keep it small so a
// future Apple removal is easy to spot.
[[nodiscard]] std::string build_profile(std::string_view workspace) {
    std::string p;
    p += "(version 1)\n";
    p += "(deny default)\n";
    // Process / signals
    p += "(allow process-exec)\n";
    p += "(allow process-fork)\n";
    p += "(allow signal (target same-sandbox))\n";
    // Reads: broad — same rationale as bwrap's ro-bind on system dirs.
    p += "(allow file-read*)\n";
    // Writes: workspace + tmp/cache regions only.
    p += "(allow file-write* (subpath \"" + std::string{workspace} + "\"))\n";
    p += "(allow file-write* (subpath \"/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/var/folders\"))\n";   // user caches
    p += "(allow file-write* (subpath \"/dev/null\"))\n";
    p += "(allow file-write* (subpath \"/dev/tty\"))\n";
    // Network: open. Restricting would break git push / curl / npm.
    p += "(allow network*)\n";
    p += "(allow system-socket)\n";
    p += "(allow mach-lookup)\n";
    p += "(allow iokit-open)\n";
    p += "(allow sysctl-read)\n";
    return p;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    SubprocessOptions opts;
    auto profile = build_profile(workspace_root().string());
    opts.argv = std::vector<std::string>{
        "sandbox-exec", "-p", std::move(profile),
        "/bin/sh", "-c", std::string{cmd}
    };
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    SubprocessOptions opts;
    auto profile = build_profile(workspace_root().string());
    std::vector<std::string> argv{"sandbox-exec", "-p", std::move(profile)};
    for (const auto& a : user_argv) argv.push_back(a);
    opts.argv = std::move(argv);
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

#else // Windows / unsupported

[[nodiscard]] Backend probe() { return Backend::None; }

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    // Should never be called — is_active() returns false on this
    // platform — but defensively fall through to the unsandboxed path.
    return run_command_s(std::string{cmd}, max_bytes, timeout);
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    return run_argv_s(user_argv, max_bytes, timeout);
}

#endif

} // namespace

bool init(Mode requested) {
    g_mode.store(requested, std::memory_order_release);
    auto found = (requested == Mode::Off) ? Backend::None : probe();
    g_backend.store(found, std::memory_order_release);
    if (requested == Mode::On && found == Backend::None) {
        // Strict mode + no backend = init failure. Caller decides
        // whether to abort startup or downgrade silently.
        return false;
    }
    return true;
}

Mode    requested_mode()   noexcept { return g_mode.load(std::memory_order_acquire); }
Backend detected_backend() noexcept { return g_backend.load(std::memory_order_acquire); }

bool is_active() noexcept {
    return requested_mode() != Mode::Off
        && detected_backend() != Backend::None;
}

std::string describe_state() {
    auto m = requested_mode();
    auto b = detected_backend();
    if (m == Mode::Off) return "sandbox: off";
    const char* tag = nullptr;
    switch (b) {
        case Backend::Bwrap:       tag = "bwrap";        break;
        case Backend::SandboxExec: tag = "sandbox-exec"; break;
        case Backend::None:        tag = nullptr;        break;
    }
    if (tag) return std::string{"sandbox: active ("} + tag + ")";
    if (m == Mode::On)
        return "sandbox: requested but no backend "
#if defined(__linux__)
               "(install bubblewrap)";
#elif defined(__APPLE__)
               "(sandbox-exec missing — system integrity issue)";
#else
               "(unsupported on this platform)";
#endif
    // Mode::Auto + no backend → falling through unsandboxed
    return "sandbox: unavailable, running unsandboxed "
#if defined(__linux__)
           "(install bubblewrap to enable)";
#elif defined(__APPLE__)
           "(sandbox-exec missing)";
#else
           "(no backend on this platform)";
#endif
}

SubprocessResult run_shell_command(std::string_view cmd,
                                   std::size_t max_bytes,
                                   std::chrono::seconds timeout) {
    if (!is_active())
        return run_command_s(std::string{cmd}, max_bytes, timeout);
    return run_wrapped(cmd, max_bytes, timeout);
}

SubprocessResult run_argv(const std::vector<std::string>& argv,
                          std::size_t max_bytes,
                          std::chrono::seconds timeout) {
    if (!is_active())
        return run_argv_s(argv, max_bytes, timeout);
    return run_wrapped_argv(argv, max_bytes, timeout);
}

} // namespace moha::tools::util::sandbox
