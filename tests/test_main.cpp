// agentty_tests — the single test binary.
//
// Every unit test in tests/ is compiled as a doctest TEST_CASE and linked into
// THIS one executable, which links the shared agentty object set exactly once.
// The previous model built ~70 separate executables, each statically re-linking
// the whole object set — that link fan-out was the dominant CI cost. doctest
// auto-registers every TEST_CASE, so this file only supplies main().
//
// ctest still runs and filters individual cases: doctest_discover_tests()
// registers each TEST_CASE as its own ctest entry (`ctest -j` parallelism and
// per-case failure reporting are preserved), and `agentty_tests --test-case=X`
// runs one in isolation.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <maya/core/anim_clock.hpp>

int main(int argc, char** argv) {
    // (Rendering policy that would otherwise be terminal-derived — the
    // streaming reveal — keys off AGENTTY_UNDER_TEST, set below, so the
    // suite behaves identically inside and outside tmux.)

    // ── Central user-root isolation ────────────────────────────────
    // Point the ENTIRE binary at a throwaway ~/.agentty before any test
    // runs. Tests write credentials, account registries, threads,
    // settings and caches; without this they mutate the developer's real
    // store. (That is not hypothetical: five credential tests isolated
    // via XDG_CONFIG_HOME silently lost their sandbox when the
    // single-root consolidation moved config_dir() to $AGENTTY_HOME, and
    // the suite began overwriting live credentials + accounts.)
    //
    // Per-process unique so parallel ctest workers never share state.
    // Tests that want their OWN sandbox still setenv AGENTTY_HOME
    // themselves — this is only the default floor. AGENTTY_UNDER_TEST
    // additionally arms the tripwire in util/user_root.cpp, which aborts
    // if anything ever reaches the real root despite this.
    {
        namespace fs = std::filesystem;
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto sandbox = fs::temp_directory_path() /
            ("agentty_tests_home_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(sandbox, ec);
#if defined(_WIN32)
        _putenv_s("AGENTTY_HOME", sandbox.string().c_str());
        _putenv_s("AGENTTY_UNDER_TEST", "1");
#else
        ::setenv("AGENTTY_HOME", sandbox.string().c_str(), 1);
        ::setenv("AGENTTY_UNDER_TEST", "1", 1);
#endif
    }
    // Pin the animation clock for the whole binary. Several render/seam tests
    // (midrun_*, turn_settle, reveal) drive frames synchronously and assert on
    // committed-scrollback stability; they require maya::anim_now_ms() frozen
    // so render is a pure function of the model instead of racing wall-clock.
    // Harmless for tests that don't read it. Formerly each such test froze it
    // in its own main(); with one shared binary we do it once here.
    maya::testing::freeze_anim_clock();
    return doctest::Context(argc, argv).run();
}
