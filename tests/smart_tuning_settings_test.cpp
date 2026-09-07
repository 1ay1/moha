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

#include "agentty/domain/smart_mode.hpp"
#include "agentty/domain/smart_tuning.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"   // apply_smart
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "agentty/runtime/settings_registry.hpp"
#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/subagent.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// The routing rows, named ONCE. Tests reach them through the registry ids —
// the same strings the table declares and the panes look up — so a rename
// shows up here as a failed find() rather than as a test quietly checking a
// row that no longer exists.
constexpr const char* kCut  = "smart.complex_threshold";
constexpr const char* kDeep = "smart.deep_margin";
constexpr const char* kBias = "smart.bias_clamp";

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// A private data root per call. Settings live under $AGENTTY_HOME (see
// util/user_root.hpp) — NOT AGENTTY_CONFIG_DIR, which this file used to set
// and which persistence does not read, so every "isolated" test was quietly
// sharing the developer's real ~/.agentty. Returns the root so a test can
// write a fixture file into it.
//
// Same uniqueness discipline as the account tests: sibling ctest processes can
// start in the same millisecond, and several cases in one binary each want a
// clean store.
fs::path isolate_config_dir() {
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
    _putenv_s("AGENTTY_HOME", dir.string().c_str());
#else
    setenv("AGENTTY_HOME", dir.string().c_str(), 1);
#endif
    return dir;
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

    const auto* cut = reg::find(kCut);
    REQUIRE(cut != nullptr);
    CHECK(cut->owner() == reg::Owner::Smart);
    CHECK(reg::is_default(s.smart, *cut));

    CHECK(reg::set(s.smart, *cut, "5"));
    CHECK(reg::get(s.smart, *cut) == "5");
    CHECK(s.smart.complex_threshold == 5);
    CHECK_FALSE(reg::is_default(s.smart, *cut));

    reg::reset(s.smart, *cut);
    CHECK(reg::is_default(s.smart, *cut));
    CHECK(s.smart.complex_threshold == tun::kComplexDefault);

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
        (d.owner() == reg::Owner::Smart ? settings_rows : rag_rows)++;
    CHECK(settings_rows == 3);      // the three routing knobs
    CHECK(rag_rows > 0);
}

TEST_CASE("smart tuning: every entry point clamps to the row's range") {
    Settings s;
    const auto* cut = reg::find(kCut);
    REQUIRE(cut != nullptr);

    // Clamped, never rejected-then-forgotten: a hand-edited settings.json must
    // not be able to put the config into a state the UI could never produce.
    (void)reg::set(s.smart, *cut, "999");
    CHECK(s.smart.complex_threshold == tun::kComplexMax);
    (void)reg::set(s.smart, *cut, "-4");
    CHECK(s.smart.complex_threshold == tun::kComplexMin);

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
    s.smart.complex_threshold = 6;
    s.smart.deep_margin       = 7;
    agentty::persistence::save_settings(s);

    const Settings back = agentty::persistence::load_settings();
    CHECK(back.smart.complex_threshold == 6);
    CHECK(back.smart.deep_margin == 7);
    // A row left alone is not written, and comes back as the shipped default.
    CHECK(back.smart.bias_clamp == tun::kBiasClampDefault);
}

TEST_CASE("smart tuning: the Retrieval pane does not carry the routing rows") {
    // They were briefly put here — the pane that happened to already walk the
    // registry — which made them reachable and unfindable at the same time.
    // A knob belongs beside the thing it governs; these govern Smart Mode.
    namespace rs = agentty::rag_settings;
    agentty::rag::embed::EmbedConfig cfg;
    const Settings s;

    for (bool advanced : {false, true}) {
        const auto f = rs::build_form(cfg, agentty::store::RagMode::On, s,
                                      advanced);
        for (const char* id : {kCut, kDeep, kBias})
            CHECK(f.find(id) == nullptr);
        // The Retrieval rows are still there — this is not "the walk broke".
        CHECK(f.find("rag.mmr") != nullptr);
    }
}

TEST_CASE("smart tuning: the rows live in the SMART MODE pane") {
    // Where a setting lives is part of whether it works. These knobs decide how
    // Smart Mode routes, so they belong beside the switch that turns routing on
    // and the slots it fills — a user asking "how eagerly does it escalate?"
    // opens ^S, not the Retrieval pane. Putting them under Retrieval made them
    // technically reachable and practically invisible, which is the same
    // failure as leaving them in an env var.
    //
    // Advanced-gated: four rows that matter should not compete with three that
    // mostly should not be touched.
    namespace sf = agentty::smart_form;

    sf::Inputs in;
    in.enabled = true;
    in.smart.complex_threshold = 5;
    in.smart.deep_margin       = 6;
    in.smart.bias_clamp        = 4;

    const auto basic = sf::build_form(in);
    in.advanced = true;
    const auto adv = sf::build_form(in);

    for (const char* id : {kCut, kDeep, kBias}) {
        CHECK(basic.find(id) == nullptr);
        CHECK(adv.find(id) != nullptr);
    }

    // Real editable Number rows showing the CONFIGURED value, carrying the
    // registry's range — all of it projected from the table, none of it
    // retyped into the pane.
    const auto* cut = adv.find(kCut);
    REQUIRE(cut != nullptr);
    CHECK_FALSE(cut->locked);
    CHECK(cut->label == "Complexity cut");     // the registry's label
    const auto* num = std::get_if<agentty::form::field::Number>(&cut->value);
    REQUIRE(num != nullptr);
    CHECK(num->value == 5);
    CHECK(num->min == tun::kComplexMin);
    CHECK(num->max == tun::kComplexMax);
}

#if !defined(_WIN32)
TEST_CASE("smart tuning: an env override locks the row in the pane") {
    // Provenance and locking are the REGISTRY's, not the pane's. The pane used
    // to compute "is an env var shadowing this?" itself and pass three lock
    // strings in — a second implementation of a rule the table already
    // enforces for every other row. Walking the table means the Smart Mode
    // rows get the same treatment the Retrieval rows always had, for free.
    namespace sf = agentty::smart_form;

    clear_env();
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "5", 1);

    sf::Inputs in;
    in.enabled  = true;
    in.advanced = true;
    const auto f = sf::build_form(in);

    const auto* row = f.find(kCut);
    REQUIRE(row != nullptr);
    CHECK(row->locked);
    CHECK(row->origin == "env: AGENTTY_SMART_COMPLEX_THRESHOLD");

    // The rows NOT overridden stay editable — the lock is per row, not a mode.
    const auto* other = f.find(kDeep);
    REQUIRE(other != nullptr);
    CHECK_FALSE(other->locked);

    clear_env();
}
#endif

TEST_CASE("smart tuning: the pane advertises the key that reveals them") {
    // Hidden rows plus an unadvertised key is an env var with extra steps.
    // The note is the only thing on screen saying the rows exist, so it is
    // part of the feature rather than decoration — asserted in BOTH states so
    // the way back is visible too.
    //
    // The key is a bare letter, not ^A: ^A is the form layer's caret-home and
    // the default tmux prefix, so a chord there never reaches the app. That is
    // why this pins the TEXT rather than just "a note exists".
    namespace sf = agentty::smart_form;

    sf::Inputs in;
    in.enabled = true;

    const auto basic = sf::build_form(in);
    CHECK(basic.note.find("a") != std::string::npos);
    CHECK(basic.note.find("advanced") != std::string::npos);
    CHECK(basic.note.find("^A") == std::string::npos);

    in.advanced = true;
    const auto adv = sf::build_form(in);
    CHECK(adv.note.find("hide") != std::string::npos);
}

TEST_CASE("smart tuning: an existing settings.json still loads its pins") {
    // The in-memory shape changed from eleven flat fields to one RoleConfig.
    // The FILE format did not — and this is what proves it, because the cost of
    // being wrong is a user's pinned models silently reverting to auto on
    // upgrade, which they would discover as "why is it using the wrong model".
    //
    // Written as raw JSON on purpose: round-tripping through save() would pass
    // even if both sides changed together, which is exactly the bug.
    const auto dir = isolate_config_dir();
    clear_env();

    {
        std::ofstream f{dir / "settings.json"};
        f << R"({
          "smart": {
            "enabled": true,
            "strategic_model": "claude-opus-4-5",
            "strategic_effort": "high",
            "strategic_provider": "anthropic",
            "impl_model": "claude-sonnet-4-5",
            "utility_model": "claude-haiku-4-5",
            "complex_threshold": 6,
            "route_internal": true,
            "learn_routing": false
          }
        })";
    }

    const Settings s = agentty::persistence::load_settings();

    CHECK(s.smart.enabled);
    // A pin is model + effort + provider, and `set` is what makes resolve_role
    // honour it. Losing any one of those is the silent-revert bug.
    CHECK(s.smart.strategic.set);
    CHECK(s.smart.strategic.model == "claude-opus-4-5");
    CHECK(s.smart.strategic.provider == "anthropic");
    // A slot with a model but no explicit effort/provider still counts as
    // pinned — settings written before those fields existed must keep working.
    CHECK(s.smart.implementation.set);
    CHECK(s.smart.implementation.model == "claude-sonnet-4-5");
    CHECK(s.smart.implementation.provider.empty());
    CHECK(s.smart.utility.set);
    // The tuning row rides in the same object.
    CHECK(s.smart.complex_threshold == 6);
    // Keys from deleted layers are ignored, not fatal.
}

TEST_CASE("smart tuning: a pinned slot survives a save/load round trip") {
    isolate_config_dir();
    clear_env();

    Settings s;
    s.smart.enabled = true;
    s.smart.strategic = agentty::smart::SlotOverride{
        .model = "gpt-5", .effort = agentty::Effort::High,
        .set = true, .provider = "openai"};
    s.smart.deep_margin = 7;
    agentty::persistence::save_settings(s);

    const Settings back = agentty::persistence::load_settings();
    CHECK(back.smart.enabled);
    CHECK(back.smart.strategic.set);
    CHECK(back.smart.strategic.model == "gpt-5");
    CHECK(back.smart.strategic.effort == agentty::Effort::High);
    CHECK(back.smart.strategic.provider == "openai");
    CHECK(back.smart.deep_margin == 7);
    // Untouched slots stay unpinned rather than becoming empty pins.
    CHECK_FALSE(back.smart.utility.set);
}

TEST_CASE("smart tuning: a config change reaches all three holders") {
    // The fan-out that made this feature hard. Smart Mode config lives in
    // THREE places at once — settings.json, the Model the UI thread reads, and
    // the subagent router's worker-thread copy — and every bug in the series
    // was a call site that updated one or two of them.
    //
    // apply_smart is the only thing that knows all three. This asserts all
    // three, so a future path that open-codes "save and install" and forgets
    // the router fails here rather than in a user's `task` routing to a model
    // they un-pinned ten minutes ago.
    isolate_config_dir();
    clear_env();

    // apply_smart persists through deps(); a test binary has none installed.
    // An in-memory pair is enough and keeps the assertion honest — what comes
    // back out is what apply_smart put in, with no disk in the way.
    static agentty::store::Settings persisted;
    persisted = agentty::store::Settings{};
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](auto, auto) {},
        .save_thread   = [](const agentty::Thread&) {},
        .delete_thread = [](const auto&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const agentty::ThreadId&) {
                             return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return persisted; },
        .save_settings = [](const agentty::store::Settings& s) { persisted = s; },
        .new_thread_id = [] { return agentty::ThreadId{"t-smart"}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .auth          = {},
    });

    // A fresh, INSTALLED router copy. set_smart() no-ops while the router is
    // uninstalled (correct in production: `task` is unavailable, so there is
    // nothing to configure) — so without installing one here the third
    // assertion would pass vacuously against a config nobody kept.
    agentty::tools::subagent::install(agentty::tools::subagent::Config{
        .model = "stub", .installed = true});
    agentty::tools::subagent::set_smart(agentty::smart::RoleConfig{});
    REQUIRE_FALSE(agentty::tools::subagent::current().smart.enabled);

    agentty::Model m;
    agentty::smart::RoleConfig cfg;
    cfg.enabled = true;
    cfg.complex_threshold = 6;
    cfg.strategic = agentty::smart::SlotOverride{
        .model = "gpt-5", .effort = agentty::Effort::High,
        .set = true, .provider = "openai"};

    agentty::app::detail::apply_smart(m, cfg);

    // 1. Persisted — survives a restart.
    CHECK(persisted.smart.enabled);
    CHECK(persisted.smart.complex_threshold == 6);
    CHECK(persisted.smart.strategic.model == "gpt-5");

    // 2. Installed on the Model — what the classifier reads on the next turn.
    CHECK(m.d.smart.enabled);
    CHECK(m.d.smart.complex_threshold == 6);
    CHECK(m.d.smart.strategic.model == "gpt-5");

    // 3. Pushed to the subagent router — what `task` workers route on.
    const auto router = agentty::tools::subagent::current().smart;
    CHECK(router.enabled);
    CHECK(router.complex_threshold == 6);
    CHECK(router.strategic.model == "gpt-5");
}

TEST_CASE("smart tuning: apply_tuning is the one resolution rule") {
    // Startup and the settings-pane save both resolve env-over-stored into
    // RoleConfig. They call the SAME function, because a rule written out at
    // both is how a pane ends up applying something different from what a
    // restart would — the user changes a knob, sees it take effect, restarts,
    // and gets a different answer.
    //
    // Without the save-side call the knobs were persisted but not LIVE: the
    // classifier reads m.d.smart, so a change appeared to do nothing until the
    // next launch.
    namespace sm = agentty::smart;

    clear_env();
    {
        sm::RoleConfig c;
        c.deep_margin = 6; c.bias_clamp = 3; c.complex_threshold = 7;
        sm::apply_tuning(c);
        CHECK(c.deep_margin == 6);
        CHECK(c.bias_clamp == 3);
        CHECK(c.complex_threshold == 7);
    }
#if !defined(_WIN32)
    // An env override beats the stored value — the same layering the locked
    // row in the pane advertises.
    {
        setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "2", 1);
        sm::RoleConfig c;
        c.deep_margin = 6; c.bias_clamp = 3; c.complex_threshold = 7;
        sm::apply_tuning(c);
        CHECK(c.complex_threshold == 2);   // env wins
        CHECK(c.deep_margin == 6);         // others untouched
        clear_env();
    }
#endif
}

#if !defined(_WIN32)
TEST_CASE("smart tuning: an env var overrides the stored value") {
    isolate_config_dir();
    clear_env();

    Settings s;
    s.smart.complex_threshold = 6;
    s.smart.deep_margin       = 7;
    agentty::persistence::save_settings(s);

    // The override the locked settings row advertises.
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "2", 1);
    const Settings with_env = agentty::persistence::load_settings();
    CHECK(with_env.smart.complex_threshold == 2);
    CHECK(with_env.smart.deep_margin == 7);      // untouched rows unaffected

    // Clamped on the way in, exactly like a UI edit.
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "999", 1);
    CHECK(agentty::persistence::load_settings().smart.complex_threshold
          == tun::kComplexMax);

    // A typo in a shell profile must not silently reset a configured value to
    // the shipped default — the least surprising response is that what the
    // user configured stands.
    setenv("AGENTTY_SMART_COMPLEX_THRESHOLD", "garbage", 1);
    CHECK(agentty::persistence::load_settings().smart.complex_threshold == 6);

    clear_env();
    CHECK(agentty::persistence::load_settings().smart.complex_threshold == 6);
}

TEST_CASE("smart tuning: an env var applies to a config with no smart block") {
    // Every config until the user changes something has no "smart" section at
    // all. An export has to take effect there too, which it did not when the
    // env walk was nested inside the block that parses it.
    isolate_config_dir();
    clear_env();

    agentty::persistence::save_settings(Settings{});   // nothing to persist

    setenv("AGENTTY_SMART_DEEP_MARGIN", "6", 1);
    CHECK(agentty::persistence::load_settings().smart.deep_margin == 6);
    clear_env();
}
#endif
