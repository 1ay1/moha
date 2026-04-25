// moha — terminal Claude Code clone built on maya.
//
// main.cpp is wiring only:
//   1. parse argv (subcommands + options)
//   2. resolve credentials
//   3. construct the Provider + Store satisfying the io concepts
//   4. install the Deps so update/cmd_factory can reach them
//   5. hand MohaApp to maya's runtime

// Route global operator new/delete through mimalloc. Must live in exactly
// one TU of the final executable — main.cpp is the natural home. Enabled
// by -DMOHA_USE_MIMALLOC=ON at configure time (default ON, silently off if
// the package isn't available).
#if defined(MOHA_USE_MIMALLOC)
#  include <mimalloc-new-delete.h>
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmsystem.h>          // timeBeginPeriod / timeEndPeriod
#  pragma comment(lib, "winmm.lib")
#endif

#include <cstdio>
#include <string>
#include <utility>

#include <maya/maya.hpp>

#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/app/program.hpp"
#include "moha/auth/auth.hpp"
#include "moha/io/persistence.hpp"
#include "moha/memory/file_card_store.hpp"
#include "moha/memory/hot_files.hpp"
#include "moha/memory/memo_store.hpp"
#include "moha/version.hpp"
#include "moha/provider/anthropic/provider.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/sandbox.hpp"

namespace {

void print_usage() {
    std::fprintf(stderr,
        "%s %s\n"
        "usage: moha [subcommand] [options]\n"
        "\n"
        "subcommands:\n"
        "  login             Authenticate (OAuth via claude.ai or API key)\n"
        "  logout            Remove saved credentials\n"
        "  status            Show current auth status\n"
        "  version           Print version + exit\n"
        "  help              Show this message\n"
        "\n"
        "options:\n"
        "  -k, --key KEY       API-key override for this session\n"
        "  -m, --model ID      Model id (e.g. claude-opus-4-5)\n"
        "  -w, --workspace DIR Sandbox filesystem tools to this directory\n"
        "                      (default: cwd). Tools refuse paths outside it.\n"
        "      --sandbox MODE  Wrap bash/diagnostics in an OS-native sandbox\n"
        "                      (Linux: bwrap, macOS: sandbox-exec).\n"
        "                      MODE = auto (default: use if available),\n"
        "                             on  (require backend; fail otherwise),\n"
        "                             off (disable wrapping).\n"
        "  -v, --version       Print version + exit\n"
        "  -h, --help          Show this message\n"
        "\n",
        std::string{moha::kName}.c_str(),
        std::string{moha::kVersion}.c_str());
}

struct Args {
    std::string subcommand;
    std::string cli_key;
    std::string cli_model;
    std::string cli_workspace;
    std::string cli_sandbox;   // "auto" | "on" | "off"; empty = auto default
    bool        bad = false;
};

Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "login" || a == "logout" || a == "status" || a == "help"
         || a == "version") {
            out.subcommand = std::move(a);
        } else if ((a == "-k" || a == "--key") && i + 1 < argc) {
            out.cli_key = argv[++i];
        } else if ((a == "-m" || a == "--model") && i + 1 < argc) {
            out.cli_model = argv[++i];
        } else if ((a == "-w" || a == "--workspace") && i + 1 < argc) {
            out.cli_workspace = argv[++i];
        } else if (a == "--sandbox" && i + 1 < argc) {
            out.cli_sandbox = argv[++i];
        } else if (a == "-h" || a == "--help") {
            out.subcommand = "help";
        } else if (a == "-v" || a == "--version") {
            out.subcommand = "version";
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
#endif

int main(int argc, char** argv) {
    using namespace moha;

#if defined(_WIN32)
    Win32PerfTuning win32_perf;
#endif

    auto args = parse_args(argc, argv);
    if (args.bad)                    { print_usage(); return 2; }
    if (args.subcommand == "help")   { print_usage(); return 0; }
    if (args.subcommand == "version") {
        std::printf("%s %s\n",
            std::string{moha::kName}.c_str(),
            std::string{moha::kVersion}.c_str());
        return 0;
    }
    if (args.subcommand == "login")  return auth::cmd_login();
    if (args.subcommand == "logout") return auth::cmd_logout();
    if (args.subcommand == "status") return auth::cmd_status();

    // Missing creds is no longer a fatal error: we install with an empty
    // auth header, init.cpp opens the in-app login modal, and the user
    // finishes signing in inside the TUI. The reducer's LoginExchanged /
    // LoginSubmit handlers call auth::update_auth() which live-swaps the
    // creds in the Deps without requiring a process restart.
    auto creds = auth::resolve(args.cli_key);

    if (!args.cli_model.empty()) {
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
                "moha: --workspace path is not a directory: %s\n",
                args.cli_workspace.c_str());
            return 2;
        }
        tools::util::set_workspace_root(std::move(req));
    } else {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) tools::util::set_workspace_root(std::move(cwd));
    }

    // Bind the persistent memo store to the active workspace so the
    // <learned-about-this-workspace> block in the system prompt is
    // populated from <workspace>/.moha/memos.json on the very first
    // turn. Without this the first request of a session never sees
    // prior memos even though they're sitting on disk.
    moha::memory::shared().set_workspace(tools::util::workspace_root());
    // Same for the per-file knowledge cards. The card store also
    // needs the auth header so its background worker can call Haiku
    // to distill new files. We bind the auth right after the
    // provider is wired below.
    moha::memory::shared_cards().set_workspace(tools::util::workspace_root());
    // Hot-files index — recent-activity tracking via git log + mtime
    // for the `<recent-activity>` block in the system prompt.
    moha::memory::shared_hot_files().set_workspace(tools::util::workspace_root());

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
                "moha: --sandbox must be auto, on, or off (got '%s')\n",
                args.cli_sandbox.c_str());
            return 2;
        }
        bool ok = tools::util::sandbox::init(mode);
        if (!ok) {
            std::fprintf(stderr,
                "moha: --sandbox=on but no backend available. %s\n",
                tools::util::sandbox::describe_state().c_str());
            return 2;
        }
        // Print the status line ONLY when it carries actionable signal:
        //   * Sandbox actively engaged (the user wants to know).
        //   * User explicitly passed --sandbox=on or =off (confirm intent).
        // On Windows in the default `auto` mode with no backend this is
        // ALWAYS "unavailable, no backend on this platform" — printing it
        // every launch is noise the user can't act on, and the stderr
        // line above maya's inline frame can confuse the very first
        // render's cursor positioning. Silent unsandboxed-fallback is
        // the right default; users who care can `--sandbox=on`.
        const bool noisy_state = tools::util::sandbox::is_active()
                              || !args.cli_sandbox.empty();
        if (noisy_state) {
            std::fprintf(stderr, "moha: %s\n",
                         tools::util::sandbox::describe_state().c_str());
        }
        // Drain any stdio buffer to the console BEFORE maya enters raw
        // mode. fprintf goes through C runtime buffers; maya's writes
        // go through WriteFile. Mixing them with no flush in between
        // can leave the console cursor at an inconsistent position
        // when maya paints the first inline frame, which in turn made
        // the initial UI invisible until the first keystroke forced a
        // re-paint. Belt-and-suspenders for the silent path too.
        std::fflush(stderr);
        std::fflush(stdout);
    }

    // ── Wire the Provider + Store seams ─────────────────────────────────
    provider::anthropic::AnthropicProvider provider;
    io::FsStore                            store;
    app::install(provider, store, auth::header_value(creds), auth::style(creds));

    // Hand the same auth handle to the file-card store so its
    // background worker can call Haiku to distill files. Without
    // this, card generation silently no-ops (the worker checks for
    // an empty header and skips the request).
    {
        auto style_str = (auth::style(creds) == auth::Style::Bearer)
                       ? std::string{"Bearer"} : std::string{"ApiKey"};
        moha::memory::shared_cards().set_auth(
            auth::header_value(creds), std::move(style_str));
    }

    // Pre-warm TLS to api.anthropic.com on a detached background thread.
    // The first prompt the user types will reuse the SSL session + DNS +
    // connection cache, skipping ~150–300 ms of first-byte handshake.
    auth::prewarm_anthropic();

    // fps = 0 → pure event-driven: maya only renders on Msg / input / timer.
    // The spinner-tick subscription (gated on stream.active) supplies frames
    // while streaming; idle moha costs zero CPU.
    maya::run<app::MohaApp>({.title = "moha", .fps = 0, .mode = maya::Mode::Inline});
    return 0;
}
