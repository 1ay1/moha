// `moha airgap` — one-command launcher for running moha on an air-gapped
// host through an SSH-tunneled SOCKS5 proxy.  Run from a laptop/box that
// *has* internet to reach an air-gapped host that doesn't:
//
//   moha airgap user@host                 # connect + run remote moha
//   moha airgap --setup user@host         # also: scp ~/.config/moha/credentials.json -> remote
//
// What it does:
//   - `ssh -R 1080` (port-only form) tells OpenSSH to expose a SOCKS5
//     proxy on the remote at localhost:1080.  Connections to it are
//     tunnelled back through the SSH session and dialed by *this*
//     laptop's OpenSSH.  Net: every TCP destination the remote moha
//     wants — api.anthropic.com (chat), platform.claude.com (OAuth
//     refresh), arbitrary hosts hit by web_fetch / web_search — reaches
//     the public internet via this laptop, in one tunnel, with no
//     per-host enumeration.
//   - Sets MOHA_SOCKS_PROXY=localhost:1080 in the remote shell so the
//     remote moha routes every dial through that SOCKS proxy instead
//     of trying to resolve hosts directly (which would fail on an
//     air-gapped box).
//   - TLS, cert verification, and the HTTP Host header on the remote
//     stay pinned to the real upstream — the SOCKS proxy can't MITM
//     you.
//   - With --setup: scp the laptop's credentials.json to the remote and
//     chmod 600 it, so the user doesn't have to ferry it manually.
//
// On POSIX the final step exec()s into ssh — signals (Ctrl-C, SIGWINCH)
// reach ssh directly.  On Windows the support is stubbed; OpenSSH client
// is bundled with modern Windows but we leave the orchestration to a
// future contributor with a Win32 box to test on.

#include "moha/runtime/airgap.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace moha::airgap {

namespace {

#if !defined(_WIN32)

void print_usage() {
    std::fprintf(stderr,
        "usage: moha airgap [--setup] [--remote-moha PATH] <user@host>\n"
        "\n"
        "  Opens an SSH session to <user@host> with `-R 1080`, which makes\n"
        "  OpenSSH expose a SOCKS5 proxy on the remote's localhost:1080.\n"
        "  Connections to it are tunnelled back through SSH and dialed by\n"
        "  this laptop, so the remote moha — pointed at the proxy via\n"
        "  MOHA_SOCKS_PROXY — can reach every destination it needs (chat,\n"
        "  OAuth refresh, web tools) over a single tunnel.  TLS / cert\n"
        "  verification stay pinned on the real upstream end-to-end, so\n"
        "  the proxy can't MITM you.\n"
        "\n"
        "  --setup            Copy ~/.config/moha/credentials.json from\n"
        "                     this laptop to the remote (chmod 600) before\n"
        "                     launching.  Run this once on first connect or\n"
        "                     after re-OAuthing locally.\n"
        "  --remote-moha PATH Absolute path to moha on the remote.  Default:\n"
        "                     `moha` (resolved via remote PATH).\n"
        "\n"
        "  ssh and scp must be on this laptop's PATH.  Pass extra ssh args\n"
        "  via the MOHA_AIRGAP_SSH env var (e.g. -i, -p, -J).\n");
}

// Synchronously spawn `argv[0]` with the given argv and wait for it.
// Inherits stdin/stdout/stderr.  Returns the child's exit code, or -1 on
// spawn failure.
int run_sync(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;

    // posix_spawn wants `char* const*`; build a contiguous argv buffer
    // from the std::strings without const-casting their internals.
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& s : argv) raw.push_back(const_cast<char*>(s.c_str()));
    raw.push_back(nullptr);

    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, raw[0], /*file_actions=*/nullptr,
                            /*attr=*/nullptr, raw.data(), environ);
    if (rc != 0) {
        std::fprintf(stderr,
            "moha airgap: failed to spawn `%s`: %s\n",
            raw[0], std::strerror(rc));
        return -1;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno == EINTR) continue;
        std::fprintf(stderr,
            "moha airgap: waitpid failed: %s\n", std::strerror(errno));
        return -1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Copy ~/.config/moha/credentials.json from this laptop to the remote.
// Three steps: ensure the remote directory exists, scp the file, fix
// perms.  Each step is run synchronously; we abort on the first failure
// so the user sees exactly which step blew up.
int copy_credentials(const std::string& remote) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        std::fprintf(stderr, "moha airgap: HOME is unset.\n");
        return 1;
    }
    fs::path local = fs::path{home} / ".config" / "moha" / "credentials.json";
    std::error_code ec;
    if (!fs::exists(local, ec) || ec) {
        std::fprintf(stderr,
            "moha airgap: no local credentials at %s\n"
            "             run `moha login` on this machine first.\n",
            local.string().c_str());
        return 1;
    }

    std::fprintf(stderr, "moha airgap: copying credentials -> %s …\n",
                 remote.c_str());

    // 1) mkdir + chmod 700 for the remote config dir.  -p is idempotent.
    if (int rc = run_sync({
            "ssh", remote,
            "mkdir -p ~/.config/moha && chmod 700 ~/.config/moha",
        }); rc != 0) {
        std::fprintf(stderr,
            "moha airgap: remote mkdir failed (ssh exit %d).\n", rc);
        return rc < 0 ? 1 : rc;
    }

    // 2) scp the credentials file.
    {
        std::string dest = remote + ":.config/moha/credentials.json";
        if (int rc = run_sync({"scp", "-q", local.string(), dest});
            rc != 0) {
            std::fprintf(stderr,
                "moha airgap: scp failed (exit %d).\n", rc);
            return rc < 0 ? 1 : rc;
        }
    }

    // 3) chmod 600 — scp doesn't reliably preserve mode across hosts and
    //    the loader expects 0600 for the OAuth token.
    if (int rc = run_sync({
            "ssh", remote,
            "chmod 600 ~/.config/moha/credentials.json",
        }); rc != 0) {
        std::fprintf(stderr,
            "moha airgap: remote chmod failed (ssh exit %d).\n", rc);
        return rc < 0 ? 1 : rc;
    }

    std::fprintf(stderr, "moha airgap: credentials copied.\n");
    return 0;
}

// Single-quote `v` so it survives the remote shell as one literal token.
// Embedded `'` becomes `'\''` (close-quote, escaped quote, reopen-quote).
std::string sh_squote(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('\'');
    for (char c : v) {
        if (c == '\'') out.append("'\\''");
        else           out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// Replace this process with `ssh -t -R 1080 user@host <remote-cmd>`.
// Never returns on success.  argv constructed on the heap because execvp
// modifies its argv slot.
[[noreturn]] void exec_ssh(const std::string& remote,
                           const std::string& remote_moha) {
    // Remote command: point moha at the tunnelled SOCKS5 proxy, exec it.
    // `exec` matters — without it the user's $SHELL stays around as a
    // parent process and signal forwarding gets one extra hop wrong.
    //
    // MAYA_FORCE_SYNC=1: force maya to emit DEC 2026 (begin-sync-update)
    // around every inline frame.  We also forward terminal-identifying
    // env vars below so maya's auto-detect can fire, but plenty of
    // common terminals (bare xterm, st, urxvt, custom builds) leave no
    // identifying marker and fall through env-detect even though they
    // honor 2026 just fine.  Modern terminals silently ignore unknown
    // private CSIs, so forcing it here is no-op on the unlucky few and
    // a cure for the visible "new turn appears at the bottom then
    // realigns" flicker on the rest.
    std::string remote_cmd = "MOHA_SOCKS_PROXY=localhost:1080 MAYA_FORCE_SYNC=1";

    // Forward terminal-identifying env vars from this laptop to the
    // remote shell so the remote moha can tell whether the user's
    // terminal supports DEC 2026 (synchronized output / "begin sync
    // update").  Without sync, compose_inline_frame's per-cell rewrite
    // is rendered byte-by-byte and the user sees flicker every time the
    // composer's row gets repainted.  SSH doesn't propagate these vars
    // by default — `SendEnv` requires the server's sshd_config to
    // explicitly `AcceptEnv` each one — so we pass them on the command
    // line where no sshd config change is needed.  If a var is unset
    // locally, we skip it; an empty value would set the env var to ""
    // on the remote, which env-detect treats as "absent" anyway.
    static constexpr const char* kTerminalMarkers[] = {
        // Identification by program name / version.
        "TERM_PROGRAM", "TERM_PROGRAM_VERSION",
        // Per-terminal unambiguous markers.
        "KITTY_WINDOW_ID",
        "ALACRITTY_LOG", "ALACRITTY_WINDOW_ID",
        "GHOSTTY_RESOURCES_DIR",
        "WEZTERM_EXECUTABLE",
        "WT_SESSION",          // Windows Terminal
        "KONSOLE_VERSION",
        "VTE_VERSION",          // GNOME Terminal, Tilix, etc.
        "ITERM_SESSION_ID",     // iTerm.app
        // TERM is typically already propagated by SSH, but force it in
        // case the user's sshd_config doesn't pass it through.
        "TERM",
        // COLORTERM signals truecolor support — useful for moha's
        // colour rendering.
        "COLORTERM",
    };
    for (const char* name : kTerminalMarkers) {
        if (const char* v = std::getenv(name); v && *v) {
            remote_cmd += ' ';
            remote_cmd += name;
            remote_cmd += '=';
            remote_cmd += sh_squote(v);
        }
    }

    remote_cmd += " exec " + remote_moha;

    std::vector<std::string> argv;
    argv.push_back("ssh");
    argv.push_back("-t");                       // moha is interactive
    // -R <port> with no `host:hostport` tail tells OpenSSH to expose a
    // SOCKS proxy on the remote's <port> and resolve+dial connections
    // it receives via *this* (laptop) side.  Requires OpenSSH ≥ 7.6 on
    // both ends — practically every modern install.
    argv.push_back("-R"); argv.push_back("1080");

    // Liveness defaults.  All passed *before* MOHA_AIRGAP_SSH so a user-
    // supplied `-o Foo=bar` later silently wins (OpenSSH applies the last
    // `-o` for any given key).
    //
    //   ServerAliveInterval=30   Carrier-TCP keepalive on the SSH session —
    //                            keeps the channel from going stale during
    //                            quiet stretches between turns.
    //   ServerAliveCountMax=3    Tolerate 3 missed keepalives before
    //                            tearing down.
    //   TCPKeepAlive=yes         Kernel-level TCP keepalive on the carrier.
    //   ConnectTimeout=10        Bound the initial TCP connect so a flaky
    //                            network doesn't hang the user for 2 min.
    //   ExitOnForwardFailure=yes Bail loudly if the -R 1080 reverse listener
    //                            can't bind on the remote (something else
    //                            already on 1080), instead of silently
    //                            exposing no SOCKS proxy and letting moha
    //                            fail later with a confusing getaddrinfo.
    //
    // Compression intentionally NOT enabled by default.  Inline-mode frames
    // are small bursty deltas (~200-2 KiB per render); zlib is tuned for
    // bulk and adds latency to small chunks because the receiver waits for
    // sync markers — perceived UI lag was the dominant complaint.  Bulk
    // file payloads (web_fetch, large bash outputs) compress less than the
    // UI frames lose, on net.  Users on bandwidth-constrained links can
    // opt in: `MOHA_AIRGAP_SSH="-o Compression=yes" moha airgap …`.
    auto add_o = [&](const char* spec) {
        argv.push_back("-o"); argv.push_back(spec);
    };
    add_o("ServerAliveInterval=30");
    add_o("ServerAliveCountMax=3");
    add_o("TCPKeepAlive=yes");
    add_o("ConnectTimeout=10");
    add_o("ExitOnForwardFailure=yes");

    // MOHA_AIRGAP_SSH lets the user inject extra flags (`-i`, `-p`, `-J`,
    // or override any of the defaults above with a later `-o ...`).
    // We split on whitespace — primitive but enough for the common case.
    if (const char* extra = std::getenv("MOHA_AIRGAP_SSH"); extra && *extra) {
        std::string buf;
        for (const char* p = extra; ; ++p) {
            if (*p == ' ' || *p == '\t' || *p == '\0') {
                if (!buf.empty()) { argv.push_back(std::move(buf)); buf.clear(); }
                if (*p == '\0') break;
            } else {
                buf.push_back(*p);
            }
        }
    }

    argv.push_back(remote);
    argv.push_back(std::move(remote_cmd));

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (auto& s : argv) raw.push_back(s.data());
    raw.push_back(nullptr);

    ::execvp("ssh", raw.data());
    // Only reachable on execvp failure (PATH miss, etc.).
    std::fprintf(stderr,
        "moha airgap: failed to exec `ssh`: %s\n"
        "             ensure OpenSSH client is installed and on PATH.\n",
        std::strerror(errno));
    std::_Exit(1);
}

#endif // !_WIN32

} // namespace

int cmd_airgap(int argc, char** argv) {
#if defined(_WIN32)
    (void)argc; (void)argv;
    std::fprintf(stderr,
        "moha airgap: not yet supported on Windows.\n"
        "             until Win32 plumbing lands, do it manually:\n"
        "               ssh -R 1080 user@host\n"
        "             then on the host:\n"
        "               $env:MOHA_SOCKS_PROXY = \"localhost:1080\"\n"
        "               moha\n");
    return 1;
#else
    bool        setup_mode = false;
    std::string remote_moha = "moha";
    std::string remote;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--setup")       { setup_mode = true; }
        else if (a == "--remote-moha" && i + 1 < argc) {
            remote_moha = argv[++i];
        }
        else if (!a.empty() && a[0] != '-' && remote.empty()) {
            remote = std::move(a);
        }
        else {
            std::fprintf(stderr, "moha airgap: unrecognized argument: %s\n\n",
                         a.c_str());
            print_usage();
            return 64;
        }
    }

    if (remote.empty()) { print_usage(); return 64; }

    if (setup_mode) {
        if (int rc = copy_credentials(remote); rc != 0) return rc;
    }

    exec_ssh(remote, remote_moha);  // [[noreturn]]
#endif
}

} // namespace moha::airgap
