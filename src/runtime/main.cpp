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
#include "moha/provider/anthropic/provider.hpp"

namespace {

void print_usage() {
    std::fprintf(stderr,
        "usage: moha [subcommand] [options]\n"
        "\n"
        "subcommands:\n"
        "  login             Authenticate (OAuth via claude.ai or API key)\n"
        "  logout            Remove saved credentials\n"
        "  status            Show current auth status\n"
        "  help              Show this message\n"
        "\n"
        "options:\n"
        "  -k, --key KEY     API-key override for this session\n"
        "  -m, --model ID    Model id (e.g. claude-opus-4-5)\n"
        "\n");
}

struct Args {
    std::string subcommand;
    std::string cli_key;
    std::string cli_model;
    bool        bad = false;
};

Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "login" || a == "logout" || a == "status" || a == "help") {
            out.subcommand = std::move(a);
        } else if ((a == "-k" || a == "--key") && i + 1 < argc) {
            out.cli_key = argv[++i];
        } else if ((a == "-m" || a == "--model") && i + 1 < argc) {
            out.cli_model = argv[++i];
        } else if (a == "-h" || a == "--help") {
            out.subcommand = "help";
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
    if (args.subcommand == "login")  return auth::cmd_login();
    if (args.subcommand == "logout") return auth::cmd_logout();
    if (args.subcommand == "status") return auth::cmd_status();

    auto creds = auth::resolve(args.cli_key);
    if (!auth::is_valid(creds)) {
        std::fprintf(stderr,
            "moha: not authenticated.\n"
            "  run:  moha login\n"
            "  or:   export ANTHROPIC_API_KEY=sk-ant-...\n"
            "  or:   export CLAUDE_CODE_OAUTH_TOKEN=...\n");
        return 1;
    }

    if (!args.cli_model.empty()) {
        auto s = persistence::load_settings();
        s.model_id = ModelId{args.cli_model};
        persistence::save_settings(s);
    }

    // ── Wire the Provider + Store seams ─────────────────────────────────
    provider::anthropic::AnthropicProvider provider;
    io::FsStore                            store;
    app::install(provider, store, auth::header_value(creds), auth::style(creds));

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
