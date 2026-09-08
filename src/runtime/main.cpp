// agentty — terminal Claude Code clone built on maya.
//
// main.cpp is wiring only:
//   1. parse argv (subcommands + options)
//   2. resolve credentials
//   3. construct the Provider + Store satisfying the io concepts
//   4. install the Deps so update/cmd_factory can reach them
//   5. hand AgenttyApp to maya's runtime

// Global malloc/free and operator new/delete are routed through the mimalloc
// static library fetched by CMake when -DAGENTTY_USE_MIMALLOC=ON (the default).

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>        // CommandLineToArgvW (UTF-8 argv recovery)
#  include <mmsystem.h>          // timeBeginPeriod / timeEndPeriod
#  include <io.h>               // _setmode
#  include <fcntl.h>            // _O_BINARY
#  if defined(_MSC_VER)
//   MSVC consumes the pragma and links winmm.lib automatically. GCC
//   ignores it with a warning — we link winmm via target_link_libraries
//   in CMakeLists.txt for the MinGW build, so the pragma is pointless
//   noise there.
#    pragma comment(lib, "winmm.lib")
#  endif
#endif

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>   // isatty (headless `run` stdin detection)
#else
#include <io.h>
#include <process.h>  // _getpid
#define isatty _isatty
#define fileno _fileno
#endif

// Portable process-id helper (POSIX getpid / Windows _getpid).
[[nodiscard]] inline long agentty_pid() {
#ifdef _WIN32
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

#include <maya/maya.hpp>

#include "agentty/acp/server.hpp"
#include "agentty/airgap/airgap.hpp"
#include "agentty/util/logx.hpp"   // flight recorder dump in crash handler
#include "agentty/domain/profile.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/io/http.hpp"
#include "agentty/mcp/serve.hpp"
#include "agentty/mcp/oauth.hpp"
#include "agentty/mcp/client.hpp"   // mcp::release_servers
#include "agentty/rag/rag_adapter.hpp"
#include "agentty/rag/embed_secret.hpp"
#include "agentty/util/update.hpp"
#include "agentty/util/user_root.hpp"   // user_logs_dir (stderr.log home)
#include "agentty/provider/anthropic/provider.hpp"
#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/copilot/provider.hpp"
#include "agentty/provider/kimi/provider.hpp"
#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/ollama/provider.hpp"
#include "agentty/provider/acp_provider_adapter.hpp"
#include "agentty/provider/dispatch.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/tool/hooks.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/workspace/files.hpp"     // join_workspace_prewarm
#include "agentty/workspace/symbols.hpp"   // join_workspace_symbols_prewarm
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"

// Forward decl (mcp_tools_backends.hpp pulls in <mcp/tools/host.hpp>, whose
// ::mcp namespace collides with agentty::mcp inside main.cpp's usings).
namespace agentty::tools {
std::string run_one_shot(const std::string& prompt,
                         const std::string& agent_type,
                         bool& is_error);
// Stops the RAG retriever's background warm at teardown (see call site below).
void rag_shutdown();
}

namespace {

// Compiled-in project version — populated by CMakeLists.txt's
// target_compile_definitions(agentty PRIVATE AGENTTY_VERSION=...). The fallback
// "0.0.0-dev" is only reached on a build that bypasses our CMake (e.g.
// hand-invoked compiler), which keeps the binary self-describing instead
// of a hard #error.
#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

// ── Debug crash handler ───────────────────────────────────────────────────
// In debug builds, install a SIGSEGV/SIGABRT handler that prints a
// backtrace to stdout BEFORE the process dies. This catches crashes that
// ASAN cannot intercept (e.g. mimalloc internal segfaults) and gives
// the user a stack trace without needing GDB.
#if !defined(NDEBUG) && !defined(_WIN32)
#include <execinfo.h>
#include <unistd.h>

void crash_handler(int sig) {
    // Write to STDERR, not stdout: agentty's stdout is the live TUI render,
    // so a backtrace on stdout lands mangled in the middle of the frame and
    // can't be redirected. stderr is the conventional diagnostic channel and
    // is separable with `2>crash.log`. Raw write() only — fprintf and
    // backtrace_symbols both allocate, re-entering a possibly-corrupted
    // allocator.
    if (sig == SIGSEGV) {
        const char msg[] = "\n=== agentty: SIGSEGV (segmentation fault) ===\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    } else if (sig == SIGABRT) {
        const char msg[] = "\n=== agentty: SIGABRT (abort) ===\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    // backtrace()/backtrace_symbols_fd() are not strictly async-signal-safe
    // (they can allocate), but in a debug-only handler the extra frames are
    // worth it. If they crash on a corrupted stack the default disposition
    // (chained below via maya's emergency handler, or SIG_DFL) still gives a
    // core dump.
    void* frames[32];
    int n = backtrace(frames, 32);
    if (n > 0) {
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
    // Flight recorder: the last ~256 recorded events (Warn+ always, more
    // when AGENTTY_LOG enables channels), preformatted at emit time — the
    // dump is raw write(2)s of ready bytes, async-signal-safe. This ships
    // "what was happening right before the crash" with every report even
    // when file logging was off.
    (void)agentty::logx::dump_flight_recorder(STDERR_FILENO);
    const char end[] = "==================\n";
    (void)!write(STDERR_FILENO, end, sizeof(end) - 1);
    // _exit (not exit) so no atexit handler runs in a corrupted process. Note
    // maya's Terminal installs its own sigaction-based emergency restore once
    // the TUI is up; because THIS handler is installed first (top of main),
    // maya chains to it AFTER restoring the tty, so a mid-session crash both
    // restores the terminal and prints this backtrace.
    _exit(128 + sig);
}

void install_crash_handler() {
    // Best-effort: if signal() fails we simply run without the extra
    // backtrace — maya's emergency restore (installed later) still runs.
    (void)std::signal(SIGSEGV, crash_handler);
    (void)std::signal(SIGABRT, crash_handler);
}
#else
// Release / Windows: a MINIMAL handler that only dumps the flight recorder.
// The debug handler's backtrace() can allocate (unsafe in a corrupted
// process), but logx::dump_flight_recorder is async-signal-safe by
// construction (preformatted bytes, raw write) — so even release users get
// "the last ~256 events" on a crash, which is exactly when it matters most.
// Windows: std::signal(SIGSEGV/SIGABRT) is supported by the CRT; write()
// maps via io.h in logx.cpp.
extern "C" void agentty_release_crash_handler(int sig) {
    // RE-ENTRANCY GUARD — see the matching one in maya's
    // emergency_signal_handler. maya installs its tty-restore handler for the
    // same fatal signals with SA_NODEFER and, on the way out, restores the
    // PRIOR sigaction (this function) before re-raising. So the
    // `signal(sig, SIG_DFL)` below can be overwritten by that restore, and
    // the re-raise lands back in maya's handler, which lands back here:
    // a two-handler ping-pong that never reaches the default action.
    //
    // The symptom is unmistakable and was seen in the field: thousands of
    // "=== agentty crash ===" headers sharing ONE pid and ONE timestamp,
    // each followed by another full flight-recorder dump, burying the actual
    // first crash under megabytes of duplicates.
    //
    // On the second entry, stop cooperating: take the default action
    // immediately so the process dies with the right status and can dump
    // core.
    static volatile std::sig_atomic_t in_handler = 0;
    if (in_handler) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
#if !defined(_WIN32)
        _exit(128 + sig);   // if the default was ignored, do not spin
#endif
        return;
    }
    in_handler = 1;

    static const char hdr[] = "\n=== agentty crash ===\n";
#if defined(_WIN32)
    (void)agentty::logx::dump_flight_recorder(2);
#else
    (void)!write(2, hdr, sizeof(hdr) - 1);
    (void)agentty::logx::dump_flight_recorder(2);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);   // re-raise for the default disposition (core dump etc.)
}
void install_crash_handler() {
#if defined(_WIN32)
    (void)std::signal(SIGSEGV, agentty_release_crash_handler);
    (void)std::signal(SIGABRT, agentty_release_crash_handler);
#else
    // sigaction, not std::signal: we need SA_ONSTACK, and std::signal has
    // no way to ask for it.
    //
    // A handler installed the plain way runs on the FAULTING stack. That is
    // fine for a null-deref and useless for a stack overflow — there is no
    // room left to push the handler's frame, so it faults on entry and the
    // crash report this function exists to print never appears. Measured:
    // 0 diagnostic lines on a stack-overflow SIGSEGV without an alternate
    // stack, 1 with one.
    //
    // maya installs its own altstack for the tty-restore handler; a second
    // sigaltstack() call would replace it, so ask for one only if nobody
    // has already provided it. SA_ONSTACK is then safe either way — it just
    // means "use whatever alternate stack this thread has".
    struct sigaction sa{};
    sa.sa_handler = agentty_release_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    stack_t cur{};
    const bool have_alt =
        ::sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE);
    if (have_alt) {
        sa.sa_flags |= SA_ONSTACK;
    } else {
        static char alt_stack[256 * 1024];
        stack_t ss{};
        ss.ss_sp    = alt_stack;
        ss.ss_size  = sizeof(alt_stack);
        ss.ss_flags = 0;
        if (::sigaltstack(&ss, nullptr) == 0) sa.sa_flags |= SA_ONSTACK;
    }
    (void)::sigaction(SIGSEGV, &sa, nullptr);
    (void)::sigaction(SIGABRT, &sa, nullptr);
#endif
}
#endif

void print_version() {
    std::printf("agentty %s\n", AGENTTY_VERSION);
    // Where diagnostics land, and whether they are being captured right now.
    // A user cannot send a log they cannot find, and `--version` is the first
    // thing anyone runs when asked "what are you on?" — so it is the one
    // place a bug-reporting workflow can rely on being seen.
    const auto lf = agentty::logx::log_file();
    if (!lf.empty()) {
        std::printf("log: %.*s\n", static_cast<int>(lf.size()), lf.data());
        if (const auto n = agentty::logx::redaction_count(); n > 0)
            std::printf("     (%lu secret%s redacted)\n", n, n == 1 ? "" : "s");
    } else {
        std::printf("log: off \u2014 set AGENTTY_LOG=trace to capture diagnostics\n");
    }
}

// ── `agentty diagnostics` ─────────────────────────────────────────────────
//
// One command that produces ONE file a user can attach to a bug report.
//
// The alternative — what we had — is a four-step ritual: know that
// AGENTTY_LOG exists, set it BEFORE reproducing (bugs are noticed after),
// find ~/.agentty/logs, and remember to also state your version, OS and
// provider. Every step loses people; the field evidence is that a user
// reported a wire bug by photographing their screen.
//
// Plain text, not a zip: no dependency, and the user can READ it before
// sending. That matters more than compactness — someone who can see what
// they are sharing actually sends it. Secrets are already stripped at the
// logx emit() seam, and the redaction count is printed so that is visible.
int cmd_diagnostics() {
    namespace fs = std::filesystem;
    std::string out;
    out.reserve(1 << 16);
    const auto dir = agentty::util::user_logs_dir();

    const auto section = [&](const char* name) {
        out += "\n===== "; out += name; out += " =====\n";
    };

    section("agentty");
    out += std::string{"version: "} + AGENTTY_VERSION + "\n";
    out += std::string{"os: "} +
#if defined(_WIN32)
        "windows"
#elif defined(__APPLE__)
        "macos"
#else
        "linux"
#endif
        + "  arch: " +
#if defined(__aarch64__) || defined(_M_ARM64)
        "arm64"
#else
        "x86_64"
#endif
        + "  build: " +
#if defined(NDEBUG)
        "release"
#else
        "debug"
#endif
        + "\n";

    // NOTE: no live provider section. This command runs as its OWN process,
    // so provider::active() is a default-constructed selection, not the one
    // the failing session used — printing it would state a confident lie.
    // The real provider/model is in the log's `provider.select` line, which
    // the tail below carries. One truth, from the session that mattered.
    section("retrieval (embeddings)");
    {
        // Unlike the provider section above, this is NOT a lie in a separate
        // process: the embedder is configured by settings.json + env, the same
        // two sources the failing session read, so re-deriving it here yields
        // the same value. The probe is explicitly labelled as run-now, because
        // reachability is exactly the thing a user filing "retrieval is bad"
        // needs answered — and a silent fall back to keyword-only search is
        // this subsystem's most common invisible failure.
        namespace eb = agentty::rag::embed;
        eb::EmbedConfig ec;
        eb::apply_env(ec);
        try {
            const auto s = agentty::persistence::load_settings();
            if (!s.rag.embed_backend.empty()) {
                ec.backend = eb::backend_from_id(s.rag.embed_backend);
                if (!s.rag.embed_model.empty()) ec.model = s.rag.embed_model;
                if (!s.rag.embed_host.empty())  ec.host  = s.rag.embed_host;
                if (s.rag.embed_port != 0)      ec.port  = s.rag.embed_port;
                ec.tls            = s.rag.embed_tls;
                ec.path           = s.rag.embed_path;
                ec.model_path     = s.rag.embed_model_path;
                ec.tokenizer_path = s.rag.embed_tokenizer_path;
                ec.dim            = s.rag.embed_dim;
            }
        } catch (...) { /* env config stands */ }

        out += "config: " + eb::describe(ec) + "\n";
        out += "identity: " + eb::identity_tag(ec) + "\n";
        // The credential is never printed — only whether one is present.
        if (eb::needs_api_key(ec.backend)) {
            const auto slot = eb::endpoint_key(ec);
            out += std::string{"api key: "}
                 + (eb::load_key(slot).empty() ? "not set" : "set (stored securely)") + "\n";
        }
        if (auto v = eb::validate(ec); const auto* bad = std::get_if<eb::Invalid>(&v)) {
            out += "invalid: " + bad->why + "\n";
        } else {
            if (!ec.api_key.empty()) { /* not printed */ }
            const auto r = eb::probe(ec);
            if (const auto* ok = std::get_if<eb::ProbeOk>(&r))
                out += "probe: ok · " + std::to_string(ok->dim) + "d · "
                     + std::to_string(ok->latency_ms) + "ms\n";
            else
                out += "probe: FAILED — " + std::get<eb::ProbeErr>(r).why
                     + "\n         (retrieval falls back to keyword-only/BM25)\n";
        }
    }

    section("session (from the log)");
    {
        const auto lf = agentty::logx::log_file();
        std::error_code sec;
        if (!lf.empty() && std::filesystem::exists(
                std::filesystem::path{std::string{lf}}, sec)) {
            std::ifstream in{std::string{lf}, std::ios::binary};
            std::string ln, startup, provider;
            while (std::getline(in, ln)) {
                if (ln.find("startup:") != std::string::npos)         startup  = ln;
                if (ln.find("provider.select:") != std::string::npos) provider = ln;
            }
            // These are Info-level, so a default release run (Warn+) does not
            // capture them. Say so rather than printing a bare "(not
            // captured)", which reads like a bug in the collector.
            out += (startup.empty()
                        ? std::string{"startup: (not captured \u2014 Info level; "
                                      "set AGENTTY_LOG=info for this)"}
                        : startup) + "\n";
            out += (provider.empty()
                        ? std::string{"provider: (not captured \u2014 Info level; "
                                      "set AGENTTY_LOG=info for this)"}
                        : provider) + "\n";
        } else {
            out += "(no log captured \u2014 see the NOTE below)\n";
        }
    }

    // The FLIGHT RECORDER — the part that works without any setup.
    //
    // Warn-and-above events are always kept in an in-process ring, even with
    // file logging off, so a failure that already happened was still
    // captured. Without this section the common case is useless: a release
    // user hits a bug, runs `diagnostics`, and gets a file saying "logging
    // was off" — the bug is over, and asking them to reproduce it under
    // AGENTTY_LOG is exactly the friction this command exists to remove.
    //
    // Same process, so this only covers a bug hit in THIS run. That is the
    // interactive case (hit the bug, run diagnostics from the same shell);
    // the log-file path below covers everything longer-lived.
    // Only when there is no log file: with one, the same Warn+ events are
    // already in the tail below and duplicating them just makes the report
    // harder to read.
    if (agentty::logx::log_file().empty()) {
    section("recent warnings + errors (in-memory flight recorder)");
    {
        const auto fr = dir / "agentty-flight.txt";
        std::error_code fec;
        fs::create_directories(dir, fec);
        if (agentty::logx::dump_flight_recorder_to(fr.string().c_str())) {
            std::ifstream in{fr, std::ios::binary};
            std::string body{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
            out += body.empty() ? std::string{"(nothing recorded this run)\n"}
                                : body;
            fs::remove(fr, fec);
        } else {
            out += "(flight recorder unavailable)\n";
        }
    }
    }

    section("log");
    {
        const auto lf = agentty::logx::log_file();
        if (lf.empty()) {
            out += "logging: OFF at collection time\n";
            out += "NOTE: the warnings above were captured automatically.\n"
                   "      For a FULL byte-level trace (raw provider traffic),\n"
                   "      re-run with AGENTTY_LOG=trace, reproduce, then\n"
                   "      run `agentty diagnostics` again.\n";
        } else {
            out += "file: " + std::string{lf} + "\n";
            out += "secrets redacted so far: "
                 + std::to_string(agentty::logx::redaction_count()) + "\n";
        }
    }

    // The log itself, tail-limited: a long session can run to tens of MB and
    // nobody pastes that. The tail holds the failure, which is what matters.
    section("log tail (last 4000 lines)");
    {
        const auto lf = agentty::logx::log_file();
        std::error_code ec;
        if (!lf.empty() && fs::exists(fs::path{std::string{lf}}, ec)) {
            std::ifstream in{std::string{lf}, std::ios::binary};
            std::vector<std::string> lines;
            std::string ln;
            while (std::getline(in, ln)) {
                lines.push_back(std::move(ln));
                if (lines.size() > 4000) lines.erase(lines.begin());
            }
            for (const auto& l : lines) { out += l; out += '\n'; }
        } else {
            out += "(no log file — see the NOTE above)\n";
        }
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto path = dir / "agentty-diagnostics.txt";
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    f << out;
    f.close();

    std::printf("agentty %s \u2014 diagnostics written\n", AGENTTY_VERSION);
    if (const auto n = agentty::logx::redaction_count(); n > 0)
        std::printf("  %lu secret%s redacted\n", n, n == 1 ? "" : "s");
    if (agentty::logx::log_file().empty())
        std::printf("  note: logging was OFF \u2014 for a full trace run\n"
                    "        AGENTTY_LOG=trace agentty   (reproduce, then re-run this)\n");
    std::printf("\n  %s\n\n  Attach that file to your report.\n",
                path.string().c_str());
    return 0;
}

void print_usage() {
    std::fprintf(stderr,
        "agentty %s\n"
        "\n"
        "usage: agentty [subcommand] [options]\n"
        "\n"
        "subcommands:\n"
        "  login             Authenticate (API key, or OAuth via claude.ai)\n"
        "  logout            Remove saved credentials\n"
        "  status            Show current auth status\n"
        "  airgap            Launch agentty on an air-gapped host via SSH tunnel\n"
        "                    (`agentty airgap --help` for details)\n"
        "  acp               Run as an ACP agent over stdio (for Zed et al.)\n"
        "  run [PROMPT]      Headless one-shot: run PROMPT through the full\n"
        "                    agent loop (tools, sandbox) and print the final\n"
        "                    answer to stdout. PROMPT `-` or absent reads\n"
        "                    stdin: `git diff | agentty run \"review this\"`.\n"
        "                    --agent TYPE picks the role (general default,\n"
        "                    explorer/reviewer/tester/coder). Exit 1 on error.\n"
        "  mcp-serve         Serve agentty's native tools over MCP (stdio).\n"
        "                    Point any MCP client at `agentty mcp-serve`.\n"
        "  mcp-login <srv>   Authorize an OAuth-gated MCP server from mcp.json\n"
        "                    (2026-07-28 OAuth 2.1 + PKCE via your browser).\n"
        "  mcp-logout <srv>  Remove a stored MCP server token.\n"
        "  mcp-status        List MCP servers and their authorization state.\n"
        "  diagnostics       Collect a redacted diagnostic bundle for a bug\n"
        "                    report (build info, logs, config \u2014 no secrets)\n"
        "  skills            List discovered skills with spec-lint diagnostics\n"
        "                    (exit 1 on warnings — CI-friendly validate)\n"
        "  hooks [list]      Show configured lifecycle hooks + approval state\n"
        "  hooks approve     Inspect + approve the active hooks file (hooks\n"
        "                    NEVER run unapproved; any change re-gates)\n"
        "  plugin add|list|remove|approve\n"
        "                    Manage plugins — a plugin IS an MCP server\n"
        "                    (any language, mcp.json entry): `agentty plugin\n"
        "                    add today --python today.py`, `--uvx pkg`,\n"
        "                    `--npx pkg`, `--http <url>`, or `-- cmd args`;\n"
        "                    add --project for the repo config (approve it\n"
        "                    with `plugin approve <name> --project`). Docs:\n"
        "                    /docs/plugins\n"
        "  rag-bench [dir]   Benchmark search_docs retrieval on your own corpus\n"
        "                    (recall@k / MRR / nDCG per pipeline stage)\n"
        "  update            Update agentty to the latest release\n"
        "                    (--check: only report, don't install)\n"
        "  version           Print the agentty version and exit\n"
        "  help              Show this message\n"
        "\n"
        "options:\n"
        "  -k, --key KEY       API-key override for this session\n"
        "  -m, --model ID      Model id (e.g. claude-opus-4-5)\n"
        "  -w, --workspace DIR Sandbox filesystem tools to this directory\n"
        "                      (default: cwd). Tools refuse paths outside it.\n"
        "                      Pass `--workspace /` to disable the gate.\n"
        "      --sandbox MODE  Wrap bash/diagnostics in an OS-native sandbox\n"
        "                      (Linux: bwrap, macOS: sandbox-exec).\n"
        "                      MODE = auto (default: use if available),\n"
        "                             on  (require backend; fail otherwise),\n"
        "                             off (disable wrapping).\n"
        "  -p, --profile MODE  ACP permission tier (Zed shows the prompts):\n"
        "                             ask     (default: prompt write/exec/net),\n"
        "                             minimal (also prompt reads),\n"
        "                             write   (never prompt reads).\n"
        "      --provider P    LLM backend. anthropic (default, OAuth/Pro/Max)\n"
        "                      or an OpenAI-compatible one: openai | codex | groq |\n"
        "                      openrouter | together | cerebras | deepseek | xai |\n"
        "                      mistral | gemini | fireworks | ollama | llama.cpp,\n"
        "                      or a raw host[:port] for any other\n"
        "                      OpenAI-compatible server. Reads OPENAI_API_KEY\n"
        "                      (or the provider-specific *_API_KEY) / -k for\n"
        "                      the key; local backends need no key. Persisted\n"
        "                      like -m. (Switch live in-app with Ctrl-P \xe2\x80\x94 the\n"
        "                      picker has a \"Custom host\xe2\x80\xa6\" entry too.)\n"
        "                      chatgpt talks to ChatGPT natively via the\n"
        "                      reverse-engineered OAuth login (`agentty login`\n"
        "                      \xe2\x86\x92 ChatGPT); no Codex binary is needed.\n"
        "                      copilot and kimi sign in the same way via their\n"
        "                      device-flow OAuth (`agentty login` \xe2\x86\x92 GitHub\n"
        "                      Copilot / Kimi) \xe2\x80\x94 no API key needed.\n"
        "                      Note: hosted Claude output is watermarked per\n"
        "                      the EU AI Act (invisible, no opt-out, set\n"
        "                      server-side). Local/open-weight backends\n"
        "                      (ollama, llama.cpp) carry no provider watermark.\n"
        "  -V, --version       Print the agentty version and exit.\n"
        "      --auth-header N Auth header NAME for OpenAI-compatible backends\n"
        "                      whose gateway doesn't accept `Authorization:\n"
        "                      Bearer` (e.g. X-API-Key). The key (-k /\n"
        "                      OPENAI_API_KEY) is sent raw under that name.\n"
        "                      Session-scoped like -k; default: Bearer.\n"
        "  -h, --help          Show this message.\n"
        "\n",
        AGENTTY_VERSION);
}

struct Args {
    std::string subcommand;
    std::string cli_key;
    std::string cli_model;
    std::string cli_workspace;
    std::string cli_sandbox;   // "auto" | "on" | "off"; empty = auto default
    std::string cli_profile;   // "write" | "ask" | "minimal"; ACP only
    std::string cli_provider;  // "anthropic" | "openai" | "ollama" | "llama.cpp" | host[:port]
    std::string cli_auth_header; // custom auth header NAME (e.g. "X-API-Key")
    std::string cli_bench_root;  // rag-bench: docs root override (positional)
    std::string cli_mcp_server;  // mcp-login/logout: server name (positional)
    std::string cli_mcp_metadata; // mcp-login: explicit resource-metadata URL
    std::string cli_mcp_client_id; // mcp-login: pre-registered / CIMD client_id
    std::string cli_run_prompt;    // run: the one-shot prompt (positional)
    std::string cli_run_agent;     // run: --agent explorer|reviewer|…|general
    std::vector<std::string> plugin_argv;  // plugin: verb + tail, verbatim
    int         airgap_argc = 0;
    char**      airgap_argv = nullptr;   // borrowed from main's argv
    bool        bad = false;
};

Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "login" || a == "logout" || a == "status" || a == "help"
         || a == "acp" || a == "skills" || a == "mcp-serve"
         || a == "diagnostics") {
            out.subcommand = std::move(a);
        } else if (a == "update") {
            // `agentty update [--check]` — --check rides in cli_run_agent
            // (reused scratch; update has no agent).
            out.subcommand = std::move(a);
            if (i + 1 < argc && std::string{argv[i + 1]} == "--check") {
                out.cli_run_agent = "--check";
                ++i;
            }
        } else if (a == "hooks") {
            // `agentty hooks [list|approve]` — verb rides in cli_run_agent
            // (reused scratch; hooks has no agent).
            out.subcommand = std::move(a);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                out.cli_run_agent = argv[++i];
        } else if (a == "mcp-login" || a == "mcp-logout" || a == "mcp-status") {
            // `agentty mcp-login <server> [--metadata <url>] [--client-id <id>]`
            out.subcommand = std::move(a);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                out.cli_mcp_server = argv[++i];
            while (i + 1 < argc) {
                std::string opt = argv[i + 1];
                if (opt == "--metadata" && i + 2 < argc)       out.cli_mcp_metadata  = argv[i += 2];
                else if (opt == "--client-id" && i + 2 < argc) out.cli_mcp_client_id = argv[i += 2];
                else break;
            }
        } else if (a == "rag-bench") {
            // Optional positional docs root: `agentty rag-bench [dir]`.
            out.subcommand = std::move(a);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                out.cli_bench_root = argv[++i];
        } else if (a == "run") {
            // Headless one-shot: `agentty run "prompt" [--agent TYPE]`.
            // A missing / `-` prompt reads stdin (pipe usage:
            // `git diff | agentty run -` or `… | agentty run`).
            out.subcommand = std::move(a);
            while (i + 1 < argc) {
                std::string opt = argv[i + 1];
                if (opt == "--agent" && i + 2 < argc) {
                    out.cli_run_agent = argv[i += 2];
                } else if (opt[0] != '-' || opt == "-") {
                    if (out.cli_run_prompt.empty()) out.cli_run_prompt = argv[++i];
                    else break;   // extra positional → top-level flags below
                } else {
                    break;        // -m/-w/… handled by the outer loop
                }
            }
        } else if (a == "plugin") {
            // `agentty plugin <verb> …` — hand the whole tail to the
            // plugin CLI verbatim (it owns its own flags: --uvx/--python/
            // --npx/--/--project/--force).
            out.subcommand = std::move(a);
            for (int j = i + 1; j < argc; ++j)
                out.plugin_argv.emplace_back(argv[j]);
            return out;
        } else if (a == "airgap") {
            // Hand the remaining argv tail to the airgap subcommand verbatim
            // so it can run its own flag parsing without re-implementing
            // ours.  Stop scanning — top-level flags don't apply.
            out.subcommand   = std::move(a);
            out.airgap_argc  = argc - (i + 1);
            out.airgap_argv  = argv + (i + 1);
            return out;
        } else if ((a == "-k" || a == "--key") && i + 1 < argc) {
            out.cli_key = argv[++i];
        } else if ((a == "-m" || a == "--model") && i + 1 < argc) {
            out.cli_model = argv[++i];
        } else if ((a == "-w" || a == "--workspace") && i + 1 < argc) {
            out.cli_workspace = argv[++i];
        } else if (a == "--sandbox" && i + 1 < argc) {
            out.cli_sandbox = argv[++i];
        } else if ((a == "-p" || a == "--profile") && i + 1 < argc) {
            out.cli_profile = argv[++i];
        } else if (a == "--provider" && i + 1 < argc) {
            out.cli_provider = argv[++i];
        } else if (a == "--auth-header" && i + 1 < argc) {
            out.cli_auth_header = argv[++i];
        } else if (a == "-h" || a == "--help") {
            out.subcommand = "help";
        } else if (a == "-V" || a == "--version" || a == "version") {
            // Standalone version subcommand / flag. Treated as a
            // top-level dispatch path so it short-circuits the rest
            // of argparse — `agentty --version -k garbage` shouldn't
            // complain about the unused -k.
            out.subcommand = "version";
            return out;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n\n", a.c_str());
            out.bad = true;
            return out;
        }
    }
    return out;
}

} // namespace

#if defined(_WIN32)
// RAII guard for Windows-specific tuning that must be undone at process exit:
//   - timeBeginPeriod(1): bumps the system-wide timer interrupt from the
//     default 15.625 ms down to 1 ms. Every Sleep, WaitForSingleObject
//     timeout, and std::this_thread::sleep_for respects this floor, so
//     spinner ticks / streaming frame pacing / input-poll cadence become
//     smooth instead of stepping on a ~16 ms grid. The effect is global,
//     so we must pair it with timeEndPeriod(1) on teardown.
//   - SetPriorityClass(ABOVE_NORMAL): interactive TUI — we want our
//     render/input loop to preempt background compilation or Slack over
//     the user's CPU. Doesn't affect a quiescent process; only buys
//     contention-time responsiveness.
struct Win32PerfTuning {
    bool hi_res_timer = false;
    Win32PerfTuning() {
        if (::timeBeginPeriod(1) == TIMERR_NOERROR) hi_res_timer = true;
        ::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    }
    ~Win32PerfTuning() {
        if (hi_res_timer) ::timeEndPeriod(1);
    }
};

// Windows delivers narrow argv in the process ANSI/OEM codepage, so a
// non-ASCII path argument (`-w C:\Users\Ünïcode`, an @-mentioned unicode
// filename) arrives as mojibake and every downstream fs::path / getenv
// comparison misses. Recover the REAL command line via GetCommandLineW +
// CommandLineToArgvW (always UTF-16) and re-encode each arg to UTF-8, which
// is what the whole codebase already assumes narrow strings are. The
// returned storage owns the strings; callers point argv at c_str()s that
// live as long as it does.
struct Utf8Argv {
    std::vector<std::string> store;
    std::vector<char*>       ptrs;
    int                      argc = 0;
    char**                   argv = nullptr;
};

Utf8Argv recover_utf8_argv() {
    Utf8Argv out;
    int wargc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0) { if (wargv) ::LocalFree(wargv); return out; }
    out.store.reserve(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        int need = ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                         nullptr, 0, nullptr, nullptr);
        std::string s;
        if (need > 1) {
            s.resize(static_cast<size_t>(need - 1));  // drop the NUL
            ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(),
                                  need, nullptr, nullptr);
        }
        out.store.push_back(std::move(s));
    }
    ::LocalFree(wargv);
    out.ptrs.reserve(out.store.size() + 1);
    for (auto& s : out.store) out.ptrs.push_back(s.data());
    out.ptrs.push_back(nullptr);
    out.argc = static_cast<int>(out.store.size());
    out.argv = out.ptrs.data();
    return out;
}
#endif

int main(int argc, char** argv) {
    using namespace agentty;

    install_crash_handler();

#if defined(_WIN32)
    Win32PerfTuning win32_perf;
    // Swap the ANSI-codepage narrow argv for a UTF-8 one recovered from the
    // real UTF-16 command line, so non-ASCII path arguments survive. Keep the
    // storage alive for the whole of main(); argc/argv below point into it.
    Utf8Argv win32_argv = recover_utf8_argv();
    if (win32_argv.argv && win32_argv.argc > 0) {
        argc = win32_argv.argc;
        argv = win32_argv.argv;
    }
#endif

    // ── Ignore SIGPIPE process-wide ────────────────────────────────────
    // agentty writes to many pipes it doesn't fully control: spawned MCP
    // plugin servers (stdio), the sandbox/bash child procs, clipboard
    // helpers. If ANY of them dies mid-write — a plugin that crashes on its
    // startup handshake, a server pointed at a bad path that spawns then
    // exits — a write to the now-closed pipe delivers SIGPIPE, whose DEFAULT
    // action is to KILL the process. That is a real "agentty suddenly
    // crashes" cause: one flaky plugin takes down the whole app with no
    // abort message. Individual sites used MSG_NOSIGNAL / local SIG_IGN, but
    // the stdio-pipe writes (MCP transport) can't, so ignore it globally and
    // handle write failures via EPIPE return codes instead. Must run FIRST,
    // before any child is spawned.
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
    // Dev bug-marker: `kill -USR1 $(pgrep agentty)` stamps the live log with
    // a MARK banner + flight-recorder snapshot at the moment a bug is SEEN.
    // signal_mark() is async-signal-safe (static bytes, raw write(2)s).
    std::signal(SIGUSR1, [](int) { agentty::logx::signal_mark(); });
#endif

    auto args = parse_args(argc, argv);

    // Every run self-identifies in the (append-mode, multi-session) log:
    // version + build type + pid + cwd. `grep "=== agentty"` splits the file
    // into runs; a shared log names the exact binary that produced it.
    {
        std::string banner;
        banner += AGENTTY_VERSION;
#if defined(NDEBUG)
        banner += " release";
#else
        banner += " debug";
#endif
        banner += " pid=";
        banner += std::to_string(agentty_pid());
        std::error_code cwd_ec;
        const auto cwd = std::filesystem::current_path(cwd_ec);
        if (!cwd_ec) { banner += " cwd="; banner += cwd.string(); }
        agentty::logx::session_banner(banner);
    }
    if (args.bad)                    { print_usage(); return 2; }

    // ── Scrollback-gate abort: opt-in for maya developers ──────────────────
    // maya's debug-build invariant tripwires now default to SOFT-RECOVER
    // (the same non-destructive recovery a Release build performs) and only
    // std::abort() when MAYA_GATE_ABORT=1 is exported — two field SIGABRTs
    // proved the old abort-by-default killed daily-driven Debug sessions on
    // benign, self-healing gate trips. Back-compat: the previous opt-in
    // spelling was MAYA_NO_GATE_ABORT=0/false; translate it so a maya
    // developer's old launch alias still gets the loud abort.
    if (const char* g = std::getenv("MAYA_NO_GATE_ABORT");
        g && (std::string_view{g} == "0" || std::string_view{g} == "false")) {
#if defined(_WIN32)
        _putenv_s("MAYA_GATE_ABORT", "1");
#else
        setenv("MAYA_GATE_ABORT", "1", /*overwrite=*/0);
#endif
    }

    // ── Startup banner ────────────────────────────────────────────────
    // ONE line, first in every log, naming the build and the machine.
    //
    // Without it a log fragment is unattributable: the first three questions
    // on every bug report were "which version / which OS / which provider",
    // and a user who has already moved on rarely answers all three. Emitting
    // it here — after arg parsing, before any work — means every log a user
    // ever sends is self-describing, including one that ends in a crash.
    //
    // Info level: present in any enabled configuration without needing trace.
    // Warn, not Info: this is the single most useful line in any bug report
    // (version/OS/build), and the release default keeps Warn+ only. Not an
    // error — the level here is chosen for RETENTION, which is the honest
    // trade for one line per process.
    // Resolve OS / arch / build / pid with plain #if blocks here instead of
    // embedding the directives inside the AGT_LOG(...) macro args — a directive
    // within macro arguments is not portable and trips -Wpedantic.
#if defined(_WIN32)
    const char* os_name   = "windows";
#elif defined(__APPLE__)
    const char* os_name   = "macos";
#else
    const char* os_name   = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch_name = "arm64";
#else
    const char* arch_name = "x86_64";
#endif
#if defined(NDEBUG)
    const char* build_kind = "release";
#else
    const char* build_kind = "debug";
#endif
    const long long pid_ll = static_cast<long long>(
#if defined(_WIN32)
        _getpid()
#else
        ::getpid()
#endif
    );

    AGT_LOG(General, Warn, "startup",
            "agentty {} os={} arch={} build={} pid={}",
            AGENTTY_VERSION, os_name, arch_name, build_kind, pid_ll);

    if (args.subcommand == "help")    { print_usage();   return 0; }
    if (args.subcommand == "version") { print_version(); return 0; }
    if (args.subcommand == "diagnostics") return cmd_diagnostics();
    if (args.subcommand == "update") {
        const bool check_only = args.cli_run_agent == "--check";
        std::printf("agentty %s — checking for updates…\n",
                    update::current_version().c_str());
        auto c = update::check_latest(/*force=*/true);
        if (!c.error.empty()) {
            std::fprintf(stderr, "check failed: %s\n", c.error.c_str());
            return 1;
        }
        if (!c.update_available) {
            std::printf("✓ up to date (latest is v%s)\n", c.latest.c_str());
            return 0;
        }
        std::printf("⬆ v%s available (you have v%s)\n  %s\n",
                    c.latest.c_str(), c.current.c_str(), c.url.c_str());
        if (check_only) return 0;

        std::string why;
        if (!update::self_update_possible(why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
        std::printf("downloading %s…\n", update::platform_asset().c_str());
        auto err = update::perform_update(c.latest,
            [](std::size_t got, std::size_t total) {
                if (total > 0)
                    std::printf("\r  %zu / %zu KiB (%d%%)   ",
                                got / 1024, total / 1024,
                                (int)(got * 100 / total));
                else
                    std::printf("\r  %zu KiB   ", got / 1024);
                std::fflush(stdout);
            });
        std::printf("\n");
        if (!err.empty()) {
            std::fprintf(stderr, "update failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("✓ updated to v%s — restart agentty to use it\n",
                    c.latest.c_str());
        return 0;
    }
    if (args.subcommand == "login")  return auth::cmd_login();
    if (args.subcommand == "logout") return auth::cmd_logout();
    if (args.subcommand == "status") return auth::cmd_status();
    if (args.subcommand == "skills") return tools::skills::cmd_skills();
    if (args.subcommand == "hooks")  return tools::hooks::cli(args.cli_run_agent);
    if (args.subcommand == "plugin") return tools::plugin::cli(args.plugin_argv);
    if (args.subcommand == "mcp-login")
        return mcp::oauth::cmd_mcp_login(args.cli_mcp_server, args.cli_mcp_metadata,
                                         args.cli_mcp_client_id);
    if (args.subcommand == "mcp-logout")
        return mcp::oauth::cmd_mcp_logout(args.cli_mcp_server);
    if (args.subcommand == "mcp-status")
        return mcp::oauth::cmd_mcp_status();
    if (args.subcommand == "rag-bench")
        return rag::bench::run(args.cli_bench_root);
    if (args.subcommand == "airgap")
        return airgap::cmd_airgap(args.airgap_argc, args.airgap_argv);

    // Missing creds is no longer a fatal error: we install with an empty
    // auth header, init.cpp opens the in-app login modal, and the user
    // finishes signing in inside the TUI. The reducer's LoginExchanged /
    // LoginSubmit handlers call auth::update_auth() which live-swaps the
    // creds in the Deps without requiring a process restart.
    auto creds = auth::resolve(args.cli_key);

    // Persist -m as the new default — but NOT in ACP mode, where the model
    // is an ephemeral per-subprocess override (handled below) that must not
    // clobber the TUI's saved model.
    if (!args.cli_model.empty() && args.subcommand != "acp") {
        auto s = persistence::load_settings();
        s.model_id = ModelId{args.cli_model};
        persistence::save_settings(s);
    }

    // ── Filesystem sandbox boundary ─────────────────────────────────────
    // Default workspace = process cwd. Tools that touch the filesystem
    // (read/write/edit/list_dir/grep/glob/find_definition/git_*/bash's
    // `cd`) refuse paths outside this root with a clear error. Pass
    // `--workspace <dir>` to widen — `--workspace /` disables the gate
    // entirely for users who explicitly want unrestricted access.
    if (!args.cli_workspace.empty()) {
        std::filesystem::path req{args.cli_workspace};
        std::error_code ec;
        if (!std::filesystem::is_directory(req, ec)) {
            std::fprintf(stderr,
                "agentty: --workspace path is not a directory: %s\n",
                args.cli_workspace.c_str());
            return 2;
        }
        tools::util::set_workspace_root(std::move(req));
    } else {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) tools::util::set_workspace_root(std::move(cwd));
    }

    // ── Bash / diagnostics sandbox ──────────────────────────────────────
    // Wraps shell commands in bwrap (Linux) or sandbox-exec (macOS) so an
    // approved bash call can't read ~/.ssh, write /etc, or `rm -rf ~`.
    // `auto` (default): use if available, log warning otherwise. `on`:
    // fail loud if the backend is missing — for users who'd rather not
    // run unsandboxed at all. `off`: explicit opt-out for environments
    // where the user has external isolation (Docker, VM, whatever).
    {
        auto mode = tools::util::sandbox::Mode::Auto;
        if (args.cli_sandbox == "off")       mode = tools::util::sandbox::Mode::Off;
        else if (args.cli_sandbox == "on")   mode = tools::util::sandbox::Mode::On;
        else if (args.cli_sandbox == "auto"
              || args.cli_sandbox.empty())   mode = tools::util::sandbox::Mode::Auto;
        else {
            std::fprintf(stderr,
                "agentty: --sandbox must be auto, on, or off (got '%s')\n",
                args.cli_sandbox.c_str());
            return 2;
        }
        bool ok = tools::util::sandbox::init(mode);
        if (!ok) {
            std::fprintf(stderr,
                "agentty: --sandbox=on but no backend available. %s\n",
                tools::util::sandbox::describe_state().c_str());
            return 2;
        }
        // Status line so the user knows what they got. Stdout is fine —
        // maya runs after this returns, no clobbering.
        std::fprintf(stderr, "agentty: %s\n",
                     tools::util::sandbox::describe_state().c_str());
    }

    // ── Mirror the tool runtime into mcp-cpp ────────────────────────────
    // The local tool set is now served by mcp-cpp's batteries-included
    // toolset (see build_registry / mcp_tools_bridge). Mirror agentty's
    // workspace-root boundary + sandbox mode into mcp's util layer so the
    // bridged read/write/edit/bash/git tools enforce the SAME --workspace
    // gate and bwrap/sandbox-exec isolation the native tools did. Must run
    // before any tool can dispatch (TUI, ACP, and mcp-serve all reach this).
    tools::wire_mcp_runtime(args.cli_sandbox);

    // ── Resolve the active provider ──────────────────────────────
    // --provider wins; otherwise the saved setting; otherwise Anthropic.
    // "anthropic" (default) keeps the OAuth/Pro/Max path. Any other value
    // ("openai" | "groq" | "openrouter" | "ollama" | "host[:port]") routes
    // through the OpenAI-compatible transport.
    std::string provider_spec = args.cli_provider;
    if (provider_spec.empty()) {
        auto s = persistence::load_settings();
        provider_spec = s.provider;          // empty → anthropic
    } else if (args.subcommand != "acp") {
        // Canonicalise BEFORE the spec becomes a settings key: two spellings
        // of the same endpoint (trailing slash, stray whitespace) must not
        // split provider_keys / provider_models / picker rows. Same
        // normalisation as the TUI custom-host modal — CLI/TUI parity.
        provider_spec = provider::canonical_spec(provider_spec);
        // PERSIST-ON-SUCCESS for custom hosts. A KNOWN PRESET ("groq",
        // "ollama", …) can't be mistyped into a broken endpoint — persist it
        // immediately, exactly as before. A CUSTOM spec (raw host:port /
        // URL) is raw INTENT: persisting it at parse time let a typo'd
        // --provider poison settings.json so every future bare launch
        // inherited the mistake (the "skeptical of settings.json" report).
        // The session still runs on it NOW (flags are commands, never
        // blocked); the ModelsLoaded reducer persists it the moment the
        // host proves itself by answering the model fetch. -m recall gets
        // the same treatment (filed with the spec on proof).
        const bool is_preset = provider::preset_for(provider_spec) != nullptr;
        if (is_preset) {
            auto s = persistence::load_settings();
            s.provider = provider_spec;
            if (args.cli_model.empty()) {
                if (auto it = s.provider_models.find(provider_spec);
                    it != s.provider_models.end() && !it->second.empty())
                    s.model_id = ModelId{it->second};
            } else {
                // -m given alongside --provider: file the model as this
                // provider's recall so a later bare relaunch restores it.
                s.provider_models[provider_spec] = args.cli_model;
            }
            persistence::save_settings(s);
        } else {
            // Register for persist-on-success: the ModelsLoaded reducer
            // writes settings the moment this host answers a non-empty
            // model fetch. The -m recall (if any) is filed with it.
            provider::set_unproven_spec(provider_spec, args.cli_model);
        }
    }
    // Session-scoped --auth-header override; must be installed BEFORE
    // parse_selection so the initial Selection (and every live-switch
    // rebuild) stamps it onto the OpenAI-family Endpoint.
    provider::set_custom_auth_header(args.cli_auth_header);
    auto selection = provider::parse_selection(provider_spec);
    provider::select(selection);

    // Auth header per provider, registry-driven. Anthropic uses the
    // OAuth/key creds resolved above; OpenAI-family backends read their
    // provider-specific env var (GROQ_API_KEY, …) then OPENAI_API_KEY, or
    // -k; local backends (Ollama) accept an empty key. See
    // provider::credentials::resolve — the single place that knows this mapping.
    // Auth header per provider, through the central resolver — the single
    // credential model. `-k` overrides (for a one-shot key on the CLI);
    // otherwise credentials::resolve reads the provider's stored account /
    // env / provider_keys uniformly. Anthropic startup creds were already
    // saved above, so resolve("anthropic") picks them up.
    auth::AuthHeader provider_auth;
    if (!args.cli_key.empty()) {
        provider_auth = auth::AuthHeader{auth::ApiKeyHeader{args.cli_key}};
    } else {
        provider_auth = provider::credentials::resolve(provider_spec);
    }

    // ── Wire the Provider + Store seams ─────────────────────────────────
    // Both providers live on main's stack so whichever the install lambda
    // captures by reference outlives maya::run / the ACP serve loop.
    provider::anthropic::AnthropicProvider anthropic_provider;
    provider::chatgpt::ChatGptProvider chatgpt_provider;
    provider::copilot::CopilotProvider copilot_provider;
    provider::kimi::KimiProvider           kimi_provider;
    io::FsStore                            store;

    // The seam: a single std::function the runtime calls. It dispatches on
    // provider::active() AT CALL TIME (not on a frozen branch), so the
    // provider picker can live-switch the backend mid-session
    // (provider::select() + app::switch_provider()) and the very next
    // request targets the new provider — no seam rebuild, no restart. For an
    // OpenAI-family switch we rebuild the per-call endpoint from the active
    // selection so a host/path/tls change takes effect immediately.
    // The seam: a single std::function the runtime calls. `dispatch_stream`
    // (provider/dispatch.cpp) is the ONE routing point — it reads
    // provider::active() AT CALL TIME so the picker can live-switch the
    // backend mid-session and the next request targets it, with no seam
    // rebuild. The two long-lived native providers are captured by ref;
    // short-lived OpenAI-compat / Ollama transports are built per call inside
    // dispatch from the active endpoint.
    std::function<provider::StreamResult(provider::Request,
                                         provider::EventSink)> stream_fn =
        [&anthropic_provider, &chatgpt_provider, &copilot_provider, &kimi_provider]
        (provider::Request req, provider::EventSink sink) {
            provider::ProviderRouter router;
            router.set(provider::LongLived::Anthropic,
                       [&anthropic_provider](provider::Request r,
                                             provider::EventSink s) {
                           return anthropic_provider.stream(std::move(r), std::move(s));
                       })
                  .set(provider::LongLived::ChatGpt,
                       [&chatgpt_provider](provider::Request r,
                                           provider::EventSink s) {
                           return chatgpt_provider.stream(std::move(r), std::move(s));
                       })
                  .set(provider::LongLived::Copilot,
                       [&copilot_provider](provider::Request r,
                                           provider::EventSink s) {
                           return copilot_provider.stream(std::move(r), std::move(s));
                       })
                  .set(provider::LongLived::Kimi,
                       [&kimi_provider](provider::Request r,
                                        provider::EventSink s) {
                           return kimi_provider.stream(std::move(r), std::move(s));
                       });
            router.external_acp = [](const std::string& agent_id,
                                     provider::Request r, provider::EventSink s) {
                return provider::stream_external_acp(agent_id, std::move(r),
                                                     std::move(s));
            };
            return provider::dispatch_stream(router, std::move(req),
                                             std::move(sink));
        };
    app::install_deps(app::Deps{
        .stream        = stream_fn,
        .save_thread   = [&store](const Thread& t) { store.save_thread(t); },
        .delete_thread = [&store](const ThreadId& id) { store.delete_thread(id); },
        .load_threads  = [&store] { return store.load_threads(); },
        .load_thread   = [&store](const ThreadId& id) { return store.load_thread(id); },
        .load_settings = [&store] { return store.load_settings(); },
        .save_settings = [&store](const store::Settings& x) { store.save_settings(x); },
        .new_thread_id = [&store] { return store.new_id(); },
        .title_from    = [&store](std::string_view t) { return store.title_from(t); },
        .write_file    = [](const std::string& path, const std::string& contents) {
            // Diff-review reject: rewrite the file with only accepted hunks
            // kept. Best-effort — an error is surfaced by the next read, and
            // the review already applied its decision in-model.
            (void)tools::util::write_file(std::filesystem::path{path}, contents);
        },
        .auth          = provider_auth,
    });

    // ── Wire the subagent (`task` tool) seam ────────────────────
    // Process-global config the `task` tool reads to spin up an isolated
    // sub-agent loop. Resolve the model the same way the TUI / ACP paths
    // do (-m override → saved setting → built-in default). The stream fn
    // is the SAME provider dispatch Deps::stream uses (routes on
    // provider::active() at call time), so subagents work on Anthropic,
    // OpenAI-compat, and Ollama alike — not just the Anthropic transport.
    {
        auto sa_settings = persistence::load_settings();
        std::string sa_model =
            !args.cli_model.empty()       ? args.cli_model
          : !sa_settings.model_id.empty() ? sa_settings.model_id.value
          :                                 std::string{"claude-opus-4-5"};
        // Smart Mode config, resolved the same way init() does. The TUI
        // re-pushes this from init() once the Model exists (and again on any
        // overlay edit / catalog load), but `agentty run` and `agentty acp`
        // never build a Model — without this they delegated with a
        // default-constructed RoleConfig, i.e. Smart Mode silently off and
        // every pinned role slot ignored. Headless runs are exactly where
        // role routing matters most (they are all delegation).
        //
        // "The same way init() does" is now literally the same three lines,
        // because the persisted config IS a RoleConfig. This used to be a
        // second hand-written copy of the slot mapping, which is precisely how
        // the two drifted.
        smart::RoleConfig sa_smart = sa_settings.smart;
        if (auto ov = smart::tuning::enabled_override())
            sa_smart.enabled = *ov;
        settings::registry::apply_env(sa_smart);
        tools::subagent::install(tools::subagent::Config{
            .auth = provider_auth,
            .model = std::move(sa_model),
            .installed = true,
            .stream = stream_fn,
            .smart = std::move(sa_smart)});
    }

    // ── MCP server mode: serve agentty's native tools over MCP (stdio) ──
    // No maya, no terminal UI. An external MCP client (Claude Desktop, an
    // IDE, another agent) drives tools/list + tools/call over stdin/stdout;
    // diagnostics go to stderr. Reuses the provider/subagent/sandbox seams
    // wired above, so `bash`, `task`, the git_* family, etc. behave exactly
    // as they do in the TUI (filesystem tools stay sandboxed to --workspace).
    if (args.subcommand == "mcp-serve") {
        provider::prewarm_active_provider();   // `task`/web tools reuse the warm session
        int rc = mcp::serve_stdio();
        persistence::flush_pending_saves();
        return rc;
    }

    // ── Headless one-shot: `agentty run "prompt"` ───────────────────
    // No maya, no terminal UI. Runs the prompt through the SAME agent loop
    // the `task` tool uses — full toolset, sandbox, doom-loop breaker,
    // bounded turns — and prints the final report to stdout. Perfect for
    // scripting/CI: `git diff | agentty run "review this change"`.
    if (args.subcommand == "run") {
        std::string prompt = args.cli_run_prompt;
        const bool want_stdin = prompt.empty() || prompt == "-";
        std::string piped;
        // Read piped stdin when the prompt asks for it (`-` / absent) OR
        // when input is a pipe (attach `git diff | agentty run "review"`:
        // the piped bytes become context appended after the prompt).
        if (want_stdin || !isatty(fileno(stdin))) {
            std::ostringstream ss;
            ss << std::cin.rdbuf();
            piped = ss.str();
        }
        if (want_stdin) {
            prompt = std::move(piped);
            piped.clear();
        }
        // Trim: `echo "" | agentty run` pipes a lone newline — that is
        // NOT a prompt, and an accidental empty run bills a real API call.
        while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r'
                                   || prompt.back() == ' ' || prompt.back() == '\t'))
            prompt.pop_back();
        std::size_t lead = 0;
        while (lead < prompt.size() && (prompt[lead] == '\n' || prompt[lead] == '\r'
                                        || prompt[lead] == ' ' || prompt[lead] == '\t'))
            ++lead;
        prompt.erase(0, lead);
        if (!piped.empty()) {
            prompt += "\n\n<piped-input>\n";
            prompt += piped;
            prompt += "\n</piped-input>";
        }
        if (prompt.empty()) {
            std::fprintf(stderr,
                "agentty run: no prompt (pass one, or pipe stdin)\n");
            return 2;
        }
        // Slash commands work here too: `agentty run "/review src/x.cpp"`.
        if (auto expanded = tools::commands::try_expand(prompt))
            prompt = std::move(*expanded);

        provider::prewarm_active_provider();
        bool is_error = false;
        std::string report =
            tools::run_one_shot(prompt, args.cli_run_agent, is_error);
        std::fputs(report.c_str(), is_error ? stderr : stdout);
        if (!report.empty() && report.back() != '\n')
            std::fputc('\n', is_error ? stderr : stdout);
        persistence::flush_pending_saves();
        return is_error ? 1 : 0;
    }

    // ── ACP mode: run as a headless agent over stdio (Zed et al.) ───────
    // No maya, no terminal UI. stdin/stdout carry newline-delimited
    // JSON-RPC; all diagnostics go to stderr so the protocol channel stays
    // clean. Reuses the same provider/tools/sandbox wired above.
    if (args.subcommand == "acp") {
        auto settings = persistence::load_settings();
        // -m wins for this subprocess WITHOUT persisting to settings (an ACP
        // agent shouldn't clobber the TUI's saved model). Otherwise fall back
        // to the saved model, then the built-in default.
        std::string model_id =
            !args.cli_model.empty()    ? args.cli_model
          : !settings.model_id.empty() ? settings.model_id.value
          :                              std::string{"claude-opus-4-5"};

        // Prewarm TLS/DNS to the ACTIVE provider's host so the first prompt
        // reuses the SSL session + connection cache instead of paying the
        // ~150–300 ms handshake. Uniform for Anthropic and ChatGPT/Codex
        // alike. The TUI does the same before launching maya.
        provider::prewarm_active_provider();

        // Permission profile gates which tools trigger a Zed approval prompt.
        // Default Ask: prompt for write/exec/net, auto-run reads. `minimal`
        // also prompts for reads; `write` never prompts for reads.
        Profile profile = Profile::Ask;
        if      (args.cli_profile == "write")   profile = Profile::Write;
        else if (args.cli_profile == "minimal") profile = Profile::Minimal;
        else if (args.cli_profile == "ask" || args.cli_profile.empty())
                                                profile = Profile::Ask;
        else {
            std::fprintf(stderr,
                "agentty: --profile must be write, ask, or minimal (got '%s')\n",
                args.cli_profile.c_str());
            return 2;
        }

        // Fast fd transport: raw read(2)/writev(2) on stdin(0)/stdout(1).
        // Bypasses std::iostream sync + per-message flush that std::cin/cout
        // would impose. Ensure any buffered C++ stream output is flushed first
        // so it can't interleave with the raw fd writes.
        std::cout.flush();
#if defined(_WIN32)
        // On Windows fds 0/1 default to TEXT mode, which would translate every
        // '\n' the JSON-RPC framing emits into "\r\n" and mangle the wire.
        // Force BINARY so FdTransport's read(2)/write(2) see exact bytes.
        _setmode(_fileno(stdin),  _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        ::acp::FdTransport transport = ::acp::FdTransport::process();
        agentty::acp::AgentServer server(
            transport,
            stream_fn,
            provider_auth,
            std::move(model_id),
            profile);
        std::fprintf(stderr, "agentty: ACP agent ready on stdio (profile=%s)\n",
                     std::string(to_string(profile)).c_str());
        int rc = server.serve();
        persistence::flush_pending_saves();
        return rc;
    }

    // Pre-warm TLS to the ACTIVE provider's host on a detached background
    // thread. The first prompt the user types will reuse the SSL session + DNS
    // + connection cache, skipping ~150–300 ms of first-byte handshake.
    // Uniform: whether the user launched on Claude or ChatGPT/Codex, the
    // active backend gets the head start — no provider is privileged.
    provider::prewarm_active_provider();

    // ── Keep diagnostics OFF the rendered terminal ──────────────────────
    // agentty's TUI runs in INLINE mode (not the alt-screen), so anything
    // written to stderr lands directly in the viewport/scrollback and
    // corrupts maya's render — e.g. an MCP server that fails to spawn used
    // to smear "mcp::cap: exec … failed" across the frame and trip the
    // scrollback gate. Redirect stderr to a per-session log so every
    // subsystem's diagnostics (MCP, providers, sandbox) are preserved but
    // never touch the screen. Opt out with AGENTTY_NO_STDERR_REDIRECT=1
    // (e.g. when debugging startup). ACP / mcp-serve / run keep stderr as
    // their diagnostic channel and are handled above, before this point.
    if (const char* off = std::getenv("AGENTTY_NO_STDERR_REDIRECT");
        !(off && off[0] && off[0] != '0')) {
        std::error_code lec;
        std::filesystem::path logdir = util::user_logs_dir();   // ~/.agentty/logs
        std::filesystem::create_directories(logdir, lec);
        std::filesystem::path logpath = logdir / "stderr.log";
        // Append so a crash's trailing output survives across sessions;
        // truncation would lose the very lines you'd want post-mortem.
        if (std::freopen(logpath.string().c_str(), "a", stderr)) {
            std::setvbuf(stderr, nullptr, _IOLBF, 0);   // line-buffered
            std::fprintf(stderr, "\n=== agentty session %ld ===\n",
                         static_cast<long>(agentty_pid()));
        }
    }

    // fps = 0 → pure event-driven: maya only renders on Msg / input / timer.
    // The spinner-tick subscription (gated on stream.active) supplies frames
    // while streaming; idle agentty costs zero CPU.
    //
    // Grid backend: when hosted by a cooperating editor that paints cells
    // natively (AGENTTY_HOST=emacs — the agentty-mode Emacs render module —
    // or AGENTTY_HOST=vscode — the agentty-vscode webview grid renderer),
    // emit binary grid frames instead of ANSI, skipping the terminal-emulator
    // ANSI encode/reparse round trip.  Any other host falls back to ANSI.
    maya::RenderBackend backend = maya::RenderBackend::Ansi;
    if (const char* host = std::getenv("AGENTTY_HOST")) {
        const std::string_view h{host};
        if (h == "emacs" || h == "vscode") {
            // Opt-out: AGENTTY_GRID=0 (or the legacy AGENTTY_EMACS_GRID=0)
            // keeps the plain-ANSI path.
            const char* g  = std::getenv("AGENTTY_GRID");
            const char* ge = std::getenv("AGENTTY_EMACS_GRID");
            const bool off = (g && g[0] == '0') || (ge && ge[0] == '0');
            if (!off)
                backend = maya::RenderBackend::Grid;
        }
    }
    // Render leaked model PSEUDO-TAG lines (the system-prompt section tags a
    // model sometimes echoes into its reply) as dim labeled dividers that NAME
    // the tag, instead of raw `<shell>` text or a mangled HTML block. The set
    // mirrors the section tags emitted by src/provider/prompt.cpp plus the
    // common reasoning/reminder wrappers. Registered once, before maya::run.
    maya::set_markdown_pseudo_tags({
        "shell", "shell-notes", "environment",
        "file-editing", "tool-batching", "output-formatting",
        "context-economy", "big-codebases", "search-strategy",
        "in-house-languages", "memory", "memory-tools",
        "learned-memory", "local-memory", "project-memory", "user-memory",
        "provider-notes", "agents-md", "agents-md-global", "agents-md-package",
        "persisted-output", "thinking", "system-reminder", "antml:reasoning",
    });
    maya::run<app::AgenttyApp>({.title = "agentty", .fps = 0,
                               .mode = maya::Mode::Inline, .backend = backend});

    // Tear down connected MCP plugin servers FIRST — before the blocking
    // flushes below. Closing each server's stdin (→ EOF) unblocks any
    // in-flight tool-call worker still reading from it, so a tool that was
    // mid-call at quit time can't wedge the exit path. A well-behaved stdio
    // server exits on EOF in ~1ms; a wedged one is bounded by ChildProcess's
    // SIGTERM→SIGKILL deadline. Doing this promptly (not at static
    // destruction) is what makes quit feel instant instead of hanging until
    // the second Ctrl-C.
    mcp::release_servers();

    // Stop the RAG retriever's background warm NOW, not at static destruction.
    // Its warm worker (a jthread that embeds the whole corpus) is otherwise
    // joined when the function-local `static Retriever` in mcp_tools_backends
    // is destroyed AFTER main returns — blocking ^C for 4–10 s on a large
    // corpus. rag_shutdown() trips the cooperative cancel flag + joins the
    // now-interruptible worker, so exit stays instant.
    tools::rag_shutdown();

    // Join any in-flight TLS prewarm dial BEFORE the process tears down. On a
    // fast exit (e.g. immediate pipe-stdin EOF under MSYS2/mintty) the detached
    // dial would otherwise still be inside SSL_connect when the CRT/OpenSSL
    // static state is freed, corrupting the heap (Windows 0xC0000374). Trips
    // the dial's cancel token then joins; a no-op if no prewarm ran.
    http::default_client().join_prewarm();

    // Same fast-exit race for the workspace `@`/`#` prewarms: init() kicks a
    // filesystem walk and a symbol scan on background threads. On an immediate
    // pipe-EOF exit (MSYS2/mintty smoke test) they'd still be running when the
    // CRT/mimalloc atexit handlers free their captured state — a use-after-
    // free that faults on Windows (0xC0000005). Join them before teardown.
    join_workspace_prewarm();
    join_workspace_symbols_prewarm();

    // Drain the async persistence queue. The Quit reducer arm enqueues
    // a final save_thread() right before maya returns; this blocks
    // until that (and any earlier-still-queued) write lands on disk.
    persistence::flush_pending_saves();
    // Tear down any cached external ACP agent subprocesses (bounded even for a
    // wedged agent via ExternalAcpBackend's teardown watchdog).
    provider::release_acp_agents();
    return 0;
}
