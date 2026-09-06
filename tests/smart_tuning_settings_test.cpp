// smart_tuning_settings_test — the numeric routing knobs as PERSISTED rows.
//
// These three (Complex threshold, deep-band margin, self-correction cap) were
// reachable only through AGENTTY_SMART_* environment variables. That made them
// undiscoverable (you had to read domain/smart_tuning.hpp to learn they
// existed), session-only, and unvalidated. They are settings-registry rows
// now, which means four things have to hold at once, and this file asserts
// each of them:
//
//   1. the row reaches store::Settings, not store::RagConfig — and a row
//      belonging to the OTHER config struct is unreachable rather than
//      silently writing somewhere,
//   2. values are CLAMPED to the row's range at every entry point, so the
//      config cannot hold a value the UI could not produce,
//   3. a row survives save -> load,
//   4. an environment variable still WINS over the stored value (that is what
//      the locked row in the settings UI promises), is clamped on the way in,
//      and a malformed one leaves the configured value standing rather than
//      resetting it to the shipped default.

#include "agtest.hpp"

#include "agentty/domain/smart_tuning.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/runtime/settings_registry.hpp"
#include "agentty/store/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace {

namespace fs  = std::filesystem;
namespace reg = agentty::settings::registry;
namespace tun = agentty::smart::tuning;
using agentty::store::Settings;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// A private config dir per call. Same discipline as the account tests: sibling
// ctest processes can start in the same millisecond, and several cases in one
// binary each want a clean store.
void isolate_config_dir() {
    static std::atomic<int> seq{0};
    auto dir = fs::temp_directory_path()
             / ("agentty_smarttune_" + std::to_string(now_ms())
                + "_" + std::to_string(
#if defined(_WIN32)
                      static_cast<long>(::_getpid())
#else
                      static_cast<long>(::getpid())
#endif
                  )
                + "_" + std::to_string(seq.fetch_add(1)));
    fs::create_directories(dir);
#if defined(_WIN32)
    _putenv_s("AGENTTY_CONFIG_DIR", dir.string().c_str());
#else
    setenv("AGENTTY_CONFIG_DIR", dir.string().c_str(), 1);
#endif
}

void clear_env() {
#if !defined(_WIN32)
    unsetenv("AGENTTY_SMART_COMPLEX_THRESHOLD");
    unsetenv("AGENTTY_SMART_DEEP_MARGIN");
    unsetenv("AGENTTY_SMART_BIAS_CLAMP");
#endif
}

} // namespace

TEST_CASE("smart tuning: rows bind to the config struct that owns them") {
    Settings s;

    const auto* cut = reg::find("smart.complex_threshold");
    REQUIRE(cut != nullptr);
    CHECK(cut->owner() == reg::Owner::Settings);
    CHECK(reg::is_default(s, *cut));

    CHECK(reg::set(s, *cut, "5"));
    CHECK(reg::get(s, *cut) == "5");
    CHECK(s.smart_complex_threshold == 5);
    CHECK_FALSE(reg::is_default(s, *cut));

    reg::reset(s, *cut);
    CHECK(reg::is_default(s, *cut));
    CHECK(s.smart_complex_threshold == tun::kComplexDefault);

    // The other half of the table is unreachable through Settings, and the
    // barrier is stronger than a runtime check: `reg::set(settings, rag_row,
    // …)` does not COMPILE. get/set are overloaded per owner, so a call
    // naming the wrong struct has no viable overload — the mistake cannot
    // reach a test at all.
    //
    // What remains worth asserting is the tag the walkers branch on, and the
    // fact that both halves are non-empty (an owner() that answered `Rag` for
    // everything would still compile and would silently drop every routing
    // row from save, load and apply_env).
    const auto* rag_row = reg::find("rag.mmr");
    REQUIRE(rag_row != nullptr);
    CHECK(rag_row->owner() == reg::Owner::Rag);

    int settings_rows = 0, rag_rows = 0;
    for (const auto& d : reg::kSettings)
        (d.owner() == reg::Owner::Settings ? settings_rows : rag_rows)++;
    CHECK(settings_rows == 3);      // the three routing knobs
    CHECK(rag_rows > 0);
}

TEST_CASE("smart tuning: every entry point clamps to the row's range") {
    Settings s;
    const auto* cut = reg::find("smart.complex_threshold");
    REQUIRE(cut != nullptr);

    // Clamped, never rejected-then-forgotten: a hand-edited settings.json must
    // not be able to put the config into a state the UI could never produce.
    (void)reg::set(s, *cut, "999");
    CHECK(s.smart_complex_threshold == tun::kComplexMax);
    (void)reg::set(s, *cut, "-4");
    CHECK(s.smart_complex_threshold == tun::kComplexMin);

    // The registry's ranges and smart::tuning's are the same numbers — there
    // is a static_assert for this, but assert it dynamically too so a reader
    // of this file sees the invariant stated.
    CHECK(cut->min == tun::kComplexMin);
    CHECK(cut->max == tun::kComplexMax);
}

TEST_CASE("smart tuning: a configured value survives save and load") {
    isolate_config_dir();
    clear_env();

    Settings s;
    s.smart_complex_threshold = 6;
    s.smart_deep_margin       = 7;
    agentty::persistence::save_settings(s);

    const Settings back = agentty::persistence::load_settings();
    CHECK(back.smart_complex_threshold == 6);
    CHECK(back.smart_deep_margin == 7);
    // A row left alone is not written, and comes back as the shipped default.
    CHECK(back.smart_bias_clamp == tun::kBiasClampDefault);
}

#if !defined(_WIN32)
TEST_CASE("smart tuning: an env var overrides the stored value") {
    isolate_config_dir();
    clear_env();

    Settings s;
    s.smart_complex_threshold = 6;
    s.smart_deep_margin       = 7;
    agentty::persistence::save_settings(s);

    // The override the locked settings row advertises.
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "2", 1);
    const Settings with_env = agentty::persistence::load_settings();
    CHECK(with_env.smart_complex_threshold == 2);
    CHECK(with_env.smart_deep_margin == 7);      // untouched rows unaffected

    // Clamped on the way in, exactly like a UI edit.
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "999", 1);
    CHECK(agentty::persistence::load_settings().smart_complex_threshold
          == tun::kComplexMax);

    // A typo in a shell profile must not silently reset a configured value to
    // the shipped default — the least surprising response is that what the
    // user configured stands.
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "garbage", 1);
    CHECK(agentty::persistence::load_settings().smart_complex_threshold == 6);

    clear_env();
    CHECK(agentty::persistence::load_settings().smart_complex_threshold == 6);
}

TEST_CASE("smart tuning: an env var applies to a config with no smart block") {
    // Every config until the user changes something has no "smart" section at
    // all. An export has to take effect there too, which it did not when the
    // env walk was nested inside the block that parses it.
    isolate_config_dir();
    clear_env();

    agentty::persistence::save_settings(Settings{});   // nothing to persist

    setenv("AGENTTY_SMART_DEEP_MARGIN", "6", 1);
    CHECK(agentty::persistence::load_settings().smart_deep_margin == 6);
    clear_env();
}
#endif
