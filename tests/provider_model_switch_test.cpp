// provider_model_switch_test — the provider/model switching state machine.
//
// Regression locks for the field complaint "model switching is not working —
// the model changes but the provider stays the same":
//
//   1. STALE-FETCH GATE. fetch_models() stamps the provider it fetched FOR;
//      the ModelsLoaded reducer drops a payload whose provider_id doesn't
//      match the provider active at DELIVERY time. Without the gate, two
//      quick provider switches interleave their slow catalog fetches and
//      provider A's late catalog is installed under provider B — the picker
//      then offers models the active backend cannot stream (pick one and the
//      request 400s, or silently streams the wrong backend's model).
//
//   2. ACCEPTED FETCH INSTALLS THE CATALOG + auto-corrects a model id that
//      the new provider doesn't offer (first-available fallback).
//
// Driven through the REAL app::update reducer, no mocks of the reducer path.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"  // app::detail::fused_rows_for_model
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/provider_rows.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/catalog_sources.hpp"
#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/io/persistence.hpp"

#include <cstdlib>
#include <unistd.h>

#include <optional>
#include <string>
#include <vector>

namespace pn = agentty::ui::panel;

using namespace agentty;

// The ModelsLoaded reducer touches deps().load_settings() (favorites) and
// deps().save_settings() (persisting an auto-corrected model). Stub them
// with an in-memory Settings.
static store::Settings g_settings;
static void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

static ModelInfo mi(const char* id, const char* prov) {
    ModelInfo m;
    m.id = ModelId{id};
    m.display_name = id;
    m.provider = prov;
    m.supports_tools = true;
    return m;
}

TEST_CASE("provider model switch") {
    install_stub_deps();
    // Make the active provider deterministic: the Anthropic path, whose
    // canonical id ("anthropic") is what active_provider_id() reports.
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── 1: a ModelsLoaded stamped for a DIFFERENT provider is dropped ──
    {
        Model m;
        m.s.models_loading = true;
        m.d.model_id = ModelId{"claude-opus-4-5"};

        ModelsLoaded stale;
        stale.models      = { mi("gpt-4o", "openai"), mi("o4-mini", "openai") };
        stale.provider_id = "openai";   // fetched for openai; anthropic active

        auto [m2, cmd] = app::update(std::move(m), Msg{std::move(stale)});
        CHECK(m2.d.available_models.empty(),
              "stale catalog must NOT be installed");
        CHECK(m2.d.model_id.value == "claude-opus-4-5",
              "active model untouched by a stale catalog");
        CHECK(m2.s.models_loading,
              "loading stays armed — the newer fetch is still in flight");
    }

    // ── 2: a ModelsLoaded stamped for the ACTIVE provider installs ──
    {
        Model m;
        m.s.models_loading = true;
        m.d.model_id = ModelId{"some-stale-model"};

        ModelsLoaded fresh;
        fresh.models      = { mi("claude-opus-4-5", "anthropic"),
                              mi("claude-sonnet-4-5", "anthropic") };
        fresh.provider_id = "anthropic";

        auto [m2, cmd] = app::update(std::move(m), Msg{std::move(fresh)});
        CHECK(m2.d.available_models.size() == 2, "fresh catalog installed");
        CHECK(!m2.s.models_loading, "loading cleared on accepted fetch");
        // The stale active model isn't in the new catalog: auto-correct to
        // the first available so the next prompt can't 400.
        CHECK(m2.d.model_id.value == "claude-opus-4-5",
              "model auto-corrected to a catalog member");
    }

    // ── 3: legacy/synthetic dispatch (empty provider_id) still accepted ──
    {
        Model m;
        m.s.models_loading = true;

        ModelsLoaded legacy;
        legacy.models = { mi("claude-opus-4-5", "anthropic") };
        // provider_id left empty

        auto [m2, cmd] = app::update(std::move(m), Msg{std::move(legacy)});
        CHECK(m2.d.available_models.size() == 1,
              "unstamped payload accepted (back-compat)");
        CHECK(!m2.s.models_loading, "loading cleared");
    }
}

// ^E in the model picker toggles a per-model reasoning-effort override and
// MUST give feedback: flip the catalog registry, persist to Settings, and set
// a status toast (regression guard for a use-after-move that silently dropped
// the toast). Driven through the REAL reducer via app::update.
TEST_CASE("model picker ^E toggles reasoning override + feedback") {
    namespace pick = agentty::ui::pick;
    install_stub_deps();
    g_settings = store::Settings{};
    agentty::clear_reasoning_overrides();

    // A non-chatgpt provider with a genuinely non-reasoning model (codestral,
    // a code model) highlighted in an open picker — inference does NOT light
    // it up, so ^E force-on has something to prove.
    // The picker reads its cached rows, so open it through the REAL reducer
    // (which seeds provider_catalogs + fused_rows) rather than hand-placing
    // the overlay. Hermetic auth so the catalog seeds without on-disk creds.
    g_settings.provider_keys["mistral"] = "sk-test";
    provider::select(provider::parse_selection("mistral"));
    Model m0;
    m0.d.available_models = { mi("codestral-latest", "mistral") };
    m0.d.model_id = ModelId{"codestral-latest"};
    auto [m, _open] = app::update(std::move(m0), Msg{OpenModels{}});
    REQUIRE(!m.d.fused_rows.empty());

    // Baseline: inference says NOT a reasoner, so no override, no effort.
    CHECK(agentty::reasoning_override_for("codestral-latest") == -1);
    CHECK(!agentty::resolved_caps("codestral-latest").supports_effort());

    // 1st ^E: auto -> force ON.
    auto [m1, c1] = app::update(std::move(m), Msg{ModelsToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("codestral-latest") == 1,
          "^E forces the override on");
    CHECK(agentty::resolved_caps("codestral-latest").supports_effort(),
          "effort capability now open for the model");
    CHECK(g_settings.reasoning_effort_overrides.count("codestral-latest") == 1,
          "override persisted to Settings");
    CHECK(g_settings.reasoning_effort_overrides.at("codestral-latest"),
          "persisted value is ON");
    CHECK(!m1.s.status.empty(), "a status toast is set as feedback");

    // 2nd ^E: ON -> force OFF.
    auto [m2, c2] = app::update(std::move(m1), Msg{ModelsToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("codestral-latest") == 0,
          "^E again forces the override off");
    CHECK(!agentty::resolved_caps("codestral-latest").supports_effort(),
          "effort suppressed under force-off");
    CHECK(!m2.s.status.empty(), "force-off also gives feedback");

    // 3rd ^E: OFF -> back to inference (cleared).
    auto [m3, c3] = app::update(std::move(m2), Msg{ModelsToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("codestral-latest") == -1,
          "^E a third time clears the override (auto)");
    CHECK(g_settings.reasoning_effort_overrides.count("codestral-latest") == 0,
          "cleared override removed from Settings");
    CHECK(!m3.s.status.empty(), "clear-to-auto also gives feedback");

    agentty::clear_reasoning_overrides();   // keep global state clean
}

// ^E on a FAMILY-GATED model (Claude) is a no-op for the override but still
// gives feedback (a hint), and never writes an override.
TEST_CASE("model picker ^E on family-gated model is a hinted no-op") {
    namespace pick = agentty::ui::pick;
    install_stub_deps();
    g_settings = store::Settings{};
    agentty::clear_reasoning_overrides();

    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));
    Model m0;
    m0.d.available_models = { mi("claude-opus-4-5", "anthropic") };
    m0.d.model_id = ModelId{"claude-opus-4-5"};
    auto [m, _open] = app::update(std::move(m0), Msg{OpenModels{}});
    REQUIRE(!m.d.fused_rows.empty());
    // ^E acts on the HIGHLIGHTED row. The seeded catalog carries Anthropic's
    // bundled line-up, so row 0 isn't necessarily opus — point at it.
    {
        int idx = -1;
        for (int i = 0; i < static_cast<int>(m.d.fused_rows.size()); ++i)
            if (m.d.fused_rows[static_cast<std::size_t>(i)].model.id.value
                == "claude-opus-4-5") { idx = i; break; }
        REQUIRE(idx >= 0);
        if (auto* c = m.ui.panel.get<pn::Models>()) c->index = idx;
    }

    auto [m1, c1] = app::update(std::move(m), Msg{ModelsToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("claude-opus-4-5") == -1,
          "family-gated model gets no override");
    CHECK(g_settings.reasoning_effort_overrides.empty(),
          "nothing persisted for a family-gated model");
    CHECK(!m1.s.status.empty(), "still shows a hint toast");
    CHECK(agentty::resolved_caps("claude-opus-4-5").supports_effort(),
          "Claude keeps its own family-gated effort");

    agentty::clear_reasoning_overrides();
}

TEST_CASE("provider filter: fuzzy narrows + ranks") {
    namespace P = agentty::provider;

    // Empty query = every provider, in registry order.
    {
        auto all = P::filter_provider_indices("");
        CHECK(all.size() == P::providers().size());
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            CHECK(all[static_cast<std::size_t>(i)] == i);
    }

    // A specific id floats to the front.
    {
        auto r = P::filter_provider_indices("kimi");
        CHECK(!r.empty());
        const auto ps = P::providers();
        CHECK(ps[static_cast<std::size_t>(r.front())].id == "kimi");
    }
    {
        auto r = P::filter_provider_indices("deepseek");
        CHECK(!r.empty());
        CHECK(P::providers()[static_cast<std::size_t>(r.front())].id == "deepseek");
    }

    // A label word matches even when it's not the id ("grok" -> xai row).
    {
        auto r = P::filter_provider_indices("grok");
        bool found_xai = false;
        for (int i : r) if (P::providers()[static_cast<std::size_t>(i)].id == "xai") found_xai = true;
        CHECK(found_xai);
    }

    // Gibberish matches nothing.
    CHECK(P::filter_provider_indices("zzqzzq").empty());
}

TEST_CASE("provider rows: one ordered list, filter hides non-preset rows") {
    namespace ui = agentty::ui;
    const std::vector<std::string> hosts = {"my-host.example:8443"};

    // Empty query: presets, then the custom host, then the sentinel LAST.
    {
        auto rows = ui::build_provider_rows(hosts, "");
        CHECK(!rows.empty());
        CHECK(rows.back().is_new_custom_host());
        bool saw_host = false, saw_preset = false;
        for (const auto& r : rows) {
            if (r.preset()) saw_preset = true;
            if (const auto* c = r.custom_host()) saw_host = *c == hosts[0];
        }
        CHECK(saw_preset);
        CHECK(saw_host);
    }

    // Filtered: only matching presets + the always-present sentinel; the saved
    // custom host is hidden (it isn't part of the provider text search).
    {
        auto rows = ui::build_provider_rows(hosts, "kimi");
        CHECK(rows.size() >= 2);          // >=1 preset + sentinel
        CHECK(rows.back().is_new_custom_host());
        CHECK(rows.front().preset() != nullptr);
        CHECK(rows.front().preset()->id == "kimi");
        for (const auto& r : rows)
            CHECK(r.custom_host() == nullptr);   // no saved host while filtering
    }

    // No preset matches: still exactly the sentinel, so the escape hatch
    // (open the custom-host modal) is always reachable.
    {
        auto rows = ui::build_provider_rows(hosts, "zzqzzq");
        CHECK(rows.size() == 1);
        CHECK(rows.front().is_new_custom_host());
    }
}

// Fused cross-provider picker, driven through the REAL reducer:
// open seeds catalogs + fires fetches; a same-provider Select changes the
// model in place and records the MRU; a FusedCatalogLoaded merges by id.
TEST_CASE("fused picker open, merge, same-provider switch, MRU") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    // Hermetic auth: don't depend on real on-disk Anthropic creds (absent on
    // CI). A provider_keys entry makes provider_is_authed("anthropic") true.
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic"),
                            mi("claude-opus-4", "anthropic")};

    // Open: picker opens, active provider's catalog is seeded from
    // available_models (Ready), other authed providers get Loading + a fetch.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    CHECK(m1.ui.panel.is<pn::Models>());
    bool anthropic_seeded = false;
    for (const auto& c : m1.d.provider_catalogs)
        if (c.provider_id == "anthropic") {
            anthropic_seeded = (c.state == ProviderCatalog::State::Ready
                                && c.models.size() == 2);
        }
    CHECK(anthropic_seeded);

    // The fused rows include both Anthropic models (active pinned first).
    auto rows = app::detail::fused_rows_for_model(m1);
    CHECK(rows.size() >= 2);
    CHECK(rows[0].active);
    CHECK(rows[0].model.id.value == "claude-sonnet-4-6");

    // Move to the opus row and Select: same-provider model change, no hop.
    // Find opus's index in the fused list.
    int opus_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (rows[static_cast<std::size_t>(i)].model.id.value == "claude-opus-4") {
            opus_idx = i; break;
        }
    REQUIRE(opus_idx >= 0);
    if (auto* c = m1.ui.panel.get<pn::Models>()) c->index = opus_idx;

    auto [m2, c2] = app::update(std::move(m1), Msg{ModelsSelect{}});
    CHECK(m2.d.model_id.value == "claude-opus-4");
    CHECK(!m2.ui.panel.is<pn::Models>());       // picker closed
    // MRU recorded the switch (front = the model just selected).
    REQUIRE(!m2.d.recent_models.empty());
    CHECK(m2.d.recent_models.front().provider_id == "anthropic");
    CHECK(m2.d.recent_models.front().model_id == "claude-opus-4");
    // Persisted to settings.
    CHECK(!g_settings.recent_models.empty());
}

// A FusedCatalogLoaded for a provider merges into provider_catalogs by id and
// flips its state to Ready.
TEST_CASE("fused catalog loaded merges by provider id") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";  // hermetic auth
    g_settings.provider_keys["openai"] = "sk-test";   // openai authed → catalog
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});

    // Simulate openai's catalog resolving.
    FusedCatalogLoaded loaded;
    loaded.provider_id = "openai";
    loaded.models = {mi("gpt-5-codex", "openai"), mi("gpt-4o", "openai")};
    loaded.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(loaded)});

    bool openai_ready = false;
    for (const auto& c : m2.d.provider_catalogs)
        if (c.provider_id == "openai")
            openai_ready = (c.state == ProviderCatalog::State::Ready
                            && c.models.size() == 2);
    CHECK(openai_ready);
}

// The fused picker caches its rows (m.d.fused_rows) for cheap per-frame /
// per-keystroke rendering: populated on open, cleared on close/select.
TEST_CASE("fused picker caches rows and clears them on close") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    g_settings.provider_keys["xai"]       = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};

    // Open is INSTANT: only the active provider (seeded from available_models)
    // has models; every other authed provider is empty + Loading and streams
    // in via the async fetch. The cache reflects exactly that on open.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    CHECK(!m1.d.fused_rows.empty());                 // active provider shows now
    bool anthropic_ready = false, xai_pending = false;
    for (const auto& c : m1.d.provider_catalogs) {
        if (c.provider_id == "anthropic")
            anthropic_ready = (c.state == ProviderCatalog::State::Ready
                               && !c.models.empty());
        if (c.provider_id == "xai")
            xai_pending = c.models.empty();          // not seeded synchronously
    }
    CHECK(anthropic_ready);
    CHECK(xai_pending);

    // xai's catalog streams in → rows rebuild to include it.
    FusedCatalogLoaded xai;
    xai.provider_id = "xai";
    xai.models = {mi("grok-4", "xai"), mi("grok-3", "xai")};
    xai.ok = true;
    auto [m1b, c1b] = app::update(std::move(m1), Msg{std::move(xai)});
    bool xai_now = false;
    for (const auto& c : m1b.d.provider_catalogs)
        if (c.provider_id == "xai") xai_now = (c.models.size() == 2);
    CHECK(xai_now);

    // Filtering rebuilds the cache in place (cursor clamped, still open).
    auto [m2, c2] = app::update(std::move(m1b),
                                Msg{ModelsFilterInput{U'c'}});
    CHECK(m2.ui.panel.is<pn::Models>());

    // Close releases the cache.
    auto [m3, c3] = app::update(std::move(m2), Msg{CloseModels{}});
    CHECK(!m3.ui.panel.is<pn::Models>());
    CHECK(m3.d.fused_rows.empty());
}

// Digits are ordinary filter input on the fused list — there is no number
// quick-select. Typing "3" searches for "3", never jumps to the 3rd row.
TEST_CASE("fused picker digits type into the filter") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    g_settings.provider_keys["xai"]       = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});

    FusedCatalogLoaded xai;
    xai.provider_id = "xai";
    xai.models = {mi("grok-4", "xai"), mi("grok-3", "xai")};
    xai.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(xai)});
    REQUIRE(m2.d.fused_rows.size() >= 3);

    // '3' on the empty query enters the query — it does NOT jump to a row.
    auto [m3, c3] = app::update(std::move(m2), Msg{ModelsFilterInput{U'3'}});
    const auto* c = m3.ui.panel.get<pn::Models>();
    REQUIRE(c != nullptr);
    CHECK(c->query == "3");           // digit went into the query

    // Digits keep appending like any other search text (so "g3" still works).
    auto [m4, c4] = app::update(std::move(m3), Msg{ModelsFilterInput{U'g'}});
    const auto* c2p = m4.ui.panel.get<pn::Models>();
    REQUIRE(c2p != nullptr);
    CHECK(c2p->query == "3g");        // both chars appended in order
}

// A filtered fused list carries per-row fuzzy-match byte offsets into the
// model NAME, so the view can highlight WHY each row matched (fzf-style).
TEST_CASE("fused rows expose name match positions for highlight") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic"),
                            mi("claude-opus-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});

    // No query → no highlight offsets.
    for (const auto& r : m1.d.fused_rows)
        CHECK(r.match_positions.empty());

    // Type "son" → the sonnet row gains match offsets into its name.
    Model m2 = std::move(m1);
    for (char ch : std::string{"son"}) {
        auto [n, c] = app::update(std::move(m2),
                                  Msg{ModelsFilterInput{static_cast<char32_t>(ch)}});
        m2 = std::move(n);
    }
    bool sonnet_has_hl = false;
    for (const auto& r : m2.d.fused_rows)
        if (r.model.id.value == "claude-sonnet-4-5" && !r.match_positions.empty())
            sonnet_has_hl = true;
    CHECK(sonnet_has_hl);
}

// ^Tab walks the WHOLE MRU ring (A→B→C→A), progressively older, without
// reordering the ring — not a single A↔B toggle.
TEST_CASE("^Tab cycles the MRU ring without reordering") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-a"};
    m.d.available_models = {mi("claude-a", "anthropic"),
                            mi("claude-b", "anthropic"),
                            mi("claude-c", "anthropic")};
    // MRU ring: active at front, then progressively older.
    m.d.recent_models = {ModelRef{"anthropic", "claude-a"},
                         ModelRef{"anthropic", "claude-b"},
                         ModelRef{"anthropic", "claude-c"}};

    auto step = [](Model mm) {
        auto [n, c] = app::update(std::move(mm), Msg{SwitchToPreviousModel{}});
        return std::move(n);
    };

    m = step(std::move(m));
    CHECK(m.d.model_id.value == "claude-b");            // A → B
    m = step(std::move(m));
    CHECK(m.d.model_id.value == "claude-c");            // B → C
    m = step(std::move(m));
    CHECK(m.d.model_id.value == "claude-a");            // C → A (wrap)

    // The ring itself never reordered — that's what makes the walk stable.
    REQUIRE(m.d.recent_models.size() == 3);
    CHECK(m.d.recent_models[0].model_id == "claude-a");
    CHECK(m.d.recent_models[1].model_id == "claude-b");
    CHECK(m.d.recent_models[2].model_id == "claude-c");
}

// Selecting a model in the CLASSIC model picker feeds the MRU ring, so ^Tab
// has more than one entry to cycle (the reported "recents only has one model,
// ^Tab does nothing" bug — previously only the fused picker recorded).
TEST_CASE("classic model picker feeds the MRU ring") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-a"};
    m.d.available_models = {mi("claude-a", "anthropic"),
                            mi("claude-b", "anthropic")};

    // Open the classic picker, move to claude-b, select it.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    if (auto* p = m1.ui.panel.get<pn::Models>()) p->index = 1;
    auto [m2, c2] = app::update(std::move(m1), Msg{ModelsSelect{}});
    CHECK(m2.d.model_id.value == "claude-b");
    // The pick landed in the ring (front = newest).
    REQUIRE(!m2.d.recent_models.empty());
    CHECK(m2.d.recent_models.front().model_id == "claude-b");

    // Now ^Tab has 2 entries to cycle — it moves off the active model.
    auto [m3, c3] = app::update(std::move(m2), Msg{SwitchToPreviousModel{}});
    CHECK(m3.d.model_id.value != "claude-b");   // cycled, not a no-op
}

// The single bundled catalog is THE offline floor for every provider — one
// source, no per-site drift. Sanity-check its shape + that the Anthropic
// flagship (Fable) and its [1m] companion are present.
TEST_CASE("bundled catalog is the single provider floor") {
    using agentty::catalog::bundled;

    const auto anth = bundled("anthropic");
    bool fable = false, fable_1m = false, opus = false;
    for (const auto& mi : anth) {
        if (mi.id.value == "claude-fable-5")     fable = true;
        if (mi.id.value == "claude-fable-5[1m]") fable_1m = true;
        if (mi.id.value == "claude-opus-4-5")    opus = true;
        CHECK(mi.provider == "anthropic");
    }
    CHECK(fable);        // the flagship the fused picker was missing
    CHECK(fable_1m);     // add_1m_variants companion
    CHECK(opus);

    // Other providers resolve through the SAME function (no separate tables).
    CHECK(!bundled("xai").empty());
    CHECK(!bundled("chatgpt").empty());
    CHECK(!bundled("copilot").empty());
    CHECK(!bundled("kimi").empty());
    // Unknown / user-defined catalogs have no floor.
    CHECK(bundled("openrouter").empty());
    CHECK(bundled("some-custom-host").empty());
}

// The active provider's fused catalog MIRRORS available_models on every open,
// so a model that appears in available_models later (e.g. the live /v1/models
// fetch lands a new flagship after the first open seeded the bundled list)
// also shows in the fused picker — it must not freeze on the first snapshot.
TEST_CASE("fused active catalog re-seeds when available_models grows") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.available_models = {mi("claude-opus-4-5", "anthropic"),
                            mi("claude-sonnet-4-5", "anthropic")};

    // First open seeds the fused catalog from the current (Fable-less) list.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    auto [m2, c2] = app::update(std::move(m1), Msg{CloseModels{}});

    // The live fetch lands a newly-listed flagship into available_models.
    m2.d.available_models.push_back(mi("claude-fable-5", "anthropic"));

    // Re-open: the active catalog must MIRROR the grown available_models.
    auto [m3, c3] = app::update(std::move(m2), Msg{OpenModels{}});
    bool fable_listed = false;
    for (const auto& cat : m3.d.provider_catalogs)
        if (cat.provider_id == "anthropic")
            for (const auto& mo : cat.models)
                if (mo.id.value == "claude-fable-5") fable_listed = true;
    CHECK(fable_listed);

    bool fable_row = false;
    for (const auto& r : m3.d.fused_rows)
        if (r.model.id.value == "claude-fable-5") fable_row = true;
    CHECK(fable_row);
}

// A live catalog landing (ModelsLoaded) while the fused picker is OPEN must
// refresh its rows in place — the active provider's models arrive via
// ModelsLoaded (not FusedCatalogLoaded), and the picker was showing a stale
// snapshot until it was reopened. This is the "fused pane doesn't update" bug.
TEST_CASE("ModelsLoaded refreshes the open fused picker") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.available_models = {mi("claude-opus-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});

    // A newer model isn't in the open picker yet.
    bool before = false;
    for (const auto& r : m1.d.fused_rows)
        if (r.model.id.value == "claude-fable-5") before = true;
    CHECK(!before);

    // The active provider's live /v1/models fetch lands a new flagship.
    ModelsLoaded ml;
    ml.provider_id = "anthropic";
    ml.models = {mi("claude-opus-4-5", "anthropic"),
                 mi("claude-fable-5", "anthropic")};
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(ml)});

    // The OPEN picker's rows refreshed in place — no reopen needed.
    CHECK(m2.ui.panel.get<pn::Models>());
    bool after = false;
    for (const auto& r : m2.d.fused_rows)
        if (r.model.id.value == "claude-fable-5") after = true;
    CHECK(after);
}

// OpenModels refreshes the ACTIVE provider immediately and defers the
// OTHER providers (FusedRefreshOthers) so the active result isn't queued
// behind slower providers. The deferred pass marks the others Loading.
TEST_CASE("fused open prioritizes active provider, defers others") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";
    g_settings.provider_keys["openai"]    = "sk-o";   // a second authed provider
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});

    // On open the non-active provider is NOT yet Loading (deferred).
    for (const auto& c : m1.d.provider_catalogs)
        if (c.provider_id == "openai")
            CHECK(c.state != ProviderCatalog::State::Loading);

    // The deferred wave marks the OTHER providers Loading, not the active one.
    auto [m2, c2] = app::update(std::move(m1), Msg{FusedRefreshOthers{}});
    for (const auto& c : m2.d.provider_catalogs) {
        if (c.provider_id == "openai")
            CHECK(c.state == ProviderCatalog::State::Loading);
    }
}

// The fused list stays CURRENT: a provider catalog older than the TTL (or
// Failed) is refetched on the next open; a freshly-loaded one is left alone so
// rapid re-opens don't re-hammer providers. This is the "always updated but
// fast" contract.
TEST_CASE("fused refetches stale/failed catalogs, skips fresh") {
    using namespace agentty::msg;
    maya::testing::freeze_anim_clock(1'000'000);   // non-zero base (0 = "never loaded")
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";
    g_settings.provider_keys["openai"]    = "sk-o";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    // openai loads live now (t=0).
    FusedCatalogLoaded oa; oa.provider_id = "openai";
    oa.models = {mi("gpt-5", "openai")}; oa.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(oa)});

    auto openai_state = [](const Model& mm) {
        for (const auto& c : mm.d.provider_catalogs)
            if (c.provider_id == "openai") return c.state;
        return ProviderCatalog::State::Idle;
    };
    CHECK(openai_state(m2) == ProviderCatalog::State::Ready);

    // FRESH (just loaded): the deferred wave must NOT refetch it.
    auto [m3, c3] = app::update(std::move(m2), Msg{FusedRefreshOthers{}});
    CHECK(openai_state(m3) == ProviderCatalog::State::Ready);   // not Loading

    // Advance past the TTL → now STALE → the deferred wave refetches it.
    maya::testing::advance_anim_clock_ms(61'000);
    auto [m4, c4] = app::update(std::move(m3), Msg{FusedRefreshOthers{}});
    CHECK(openai_state(m4) == ProviderCatalog::State::Loading);  // refetching

    maya::testing::unfreeze_anim_clock();
}

// ^L (ModelsRefresh) forces a full live refresh regardless of TTL: it
// resets every catalog's freshness and refetches the non-active providers now.
TEST_CASE("fused ^L forces a full refresh") {
    using namespace agentty::msg;
    maya::testing::freeze_anim_clock(1'000'000);
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";
    g_settings.provider_keys["openai"]    = "sk-o";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    FusedCatalogLoaded oa; oa.provider_id = "openai";
    oa.models = {mi("gpt-5", "openai")}; oa.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(oa)});

    auto openai_state = [](const Model& mm) {
        for (const auto& c : mm.d.provider_catalogs)
            if (c.provider_id == "openai") return c.state;
        return ProviderCatalog::State::Idle;
    };
    REQUIRE(openai_state(m2) == ProviderCatalog::State::Ready);   // fresh

    // ^L refetches even a FRESH catalog (no TTL wait).
    auto [m3, c3] = app::update(std::move(m2), Msg{ModelsRefresh{}});
    CHECK(openai_state(m3) == ProviderCatalog::State::Loading);
    for (const auto& c : m3.d.provider_catalogs)
        CHECK(c.loaded_at_ms == 0);   // freshness reset for all

    maya::testing::unfreeze_anim_clock();
}

// Sign-in offers are QUERY-GATED in the fused picker: un-authed providers
// are seeded as offers (so a query naming one can surface it — no dead end)
// but the BROWSE view (empty query) renders none of them.
TEST_CASE("fused browse view hides sign-in offers; query surfaces them") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";   // only ONE provider authed
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});

    CHECK(!m1.d.fused_offers.empty());  // un-authed providers ARE seeded…
    for (const auto& r : m1.d.fused_rows)
        CHECK(!r.is_signin_offer());    // …but browsing renders none

    // Typing an un-authed provider's name surfaces exactly its offer row.
    Model m2 = std::move(m1);
    for (char ch : std::string{"mistral"}) {
        auto [mm, cc] = app::update(std::move(m2),
                                    Msg{ModelsFilterInput{
                                        static_cast<char32_t>(ch)}});
        m2 = std::move(mm);
    }
    bool offer_shown = false;
    for (const auto& r : m2.d.fused_rows)
        if (r.is_signin_offer() && r.provider_id == "mistral")
            offer_shown = true;
    CHECK(offer_shown);
}

// Signing out of a provider while the fused picker is open prunes its catalog,
// so its models stop appearing (no stale rows until restart).
TEST_CASE("fused prunes a signed-out provider's catalog") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";
    g_settings.provider_keys["openai"]    = "sk-o";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    FusedCatalogLoaded oa; oa.provider_id = "openai";
    oa.models = {mi("gpt-5", "openai")}; oa.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(oa)});
    bool had_openai = false;
    for (const auto& c : m2.d.provider_catalogs)
        if (c.provider_id == "openai") had_openai = true;
    CHECK(had_openai);

    // Sign out of openai, then rebuild the sources (as any refresh would).
    g_settings.provider_keys.erase("openai");
    auto [m3, c3] = app::update(std::move(m2), Msg{OpenModels{}});  // re-open re-syncs
    for (const auto& c : m3.d.provider_catalogs)
        CHECK(c.provider_id != "openai");            // catalog pruned
    for (const auto& r : m3.d.fused_rows)
        CHECK(r.provider_id != "openai");            // no stale rows
}

// ^Tab skips a dead MRU entry (provider signed out, or model delisted) rather
// than switching to an id that would 400 on the next request.
TEST_CASE("^Tab skips a dead MRU entry") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";   // xai NOT authed
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-a"};
    m.d.available_models = {mi("claude-a", "anthropic"),
                            mi("claude-c", "anthropic")};
    // Ring: active claude-a, then a DEAD xai entry (not authed), then claude-c.
    m.d.recent_models = {ModelRef{"anthropic", "claude-a"},
                         ModelRef{"xai", "grok-4"},          // dead
                         ModelRef{"anthropic", "claude-c"}};

    auto [m1, c1] = app::update(std::move(m), Msg{SwitchToPreviousModel{}});
    // Must skip the dead xai entry and land on claude-c.
    CHECK(m1.d.model_id.value == "claude-c");
}

// The provider picker's ^D (Mac-reachable stand-in for forward-Delete)
// signs out of a preset that has a saved key: first ^D arms, second commits
// (clears the key). openrouter is the reported case.
TEST_CASE("provider picker: ^D signs out of a keyed preset (two-press)") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"]  = "sk-a";
    g_settings.provider_keys["openrouter"] = "sk-or";   // keyed preset
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenProviders{}});

    // Point the cursor at the openrouter row deterministically by locating
    // it in the built row list, then set the picker index.
    auto* p = m1.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    const auto rows = ui::build_provider_rows(
        agentty::provider::saved_custom_hosts(g_settings.provider_keys), "");
    int or_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (const auto* pr = rows[static_cast<std::size_t>(i)].preset();
            pr && std::string{pr->id} == "openrouter") { or_idx = i; break; }
    REQUIRE(or_idx >= 0);
    p->index = or_idx;
    p->query.clear();
    Model m2 = std::move(m1);

    // First ^D arms the sign-out; the key is still present.
    auto [m3, c3] = app::update(std::move(m2), Msg{ProvidersDelete{}});
    CHECK(g_settings.provider_keys.count("openrouter") == 1);

    // Second ^D on the same row commits the sign-out.
    auto [m4, c4] = app::update(std::move(m3), Msg{ProvidersDelete{}});
    CHECK(g_settings.provider_keys.count("openrouter") == 0);  // signed out
    CHECK(g_settings.provider_keys.count("anthropic") == 1);   // others intact
}

// ^D on the ACTIVE provider's row must also zero the live auth header —
// otherwise the session keeps streaming with a credential the user just
// revoked, and the "signed out" toast lies.
TEST_CASE("provider picker: ^D on the ACTIVE provider zeroes live auth") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["openrouter"] = "sk-or";
    provider::select(provider::parse_selection("openrouter"));
    app::update_auth(auth::AuthHeader{auth::ApiKeyHeader{"sk-or"}});

    Model m;
    auto [m1, c1] = app::update(std::move(m), Msg{OpenProviders{}});
    auto* p = m1.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    const auto rows = ui::build_provider_rows(
        agentty::provider::saved_custom_hosts(g_settings.provider_keys), "");
    int or_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (const auto* pr = rows[static_cast<std::size_t>(i)].preset();
            pr && std::string{pr->id} == "openrouter") { or_idx = i; break; }
    REQUIRE(or_idx >= 0);
    p->index = or_idx;
    p->query.clear();

    auto [m2, c2] = app::update(std::move(m1), Msg{ProvidersDelete{}});
    auto [m3, c3] = app::update(std::move(m2), Msg{ProvidersDelete{}});
    CHECK(g_settings.provider_keys.count("openrouter") == 0);
    // The live header was zeroed — the next turn cannot reuse the dead key.
    CHECK(agentty::auth::is_empty(app::deps().auth));
}

// Enter on an ACCOUNT-CAPABLE provider that is already active opens its
// accounts drill-down (Esc from there closes the whole picker — handled by
// login_back → close_login for AccountList). Non-account providers switch.
TEST_CASE("provider picker: Enter opens accounts on active OAuth provider") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenProviders{}});

    // Land the cursor on the (active) anthropic row.
    auto* p = m1.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    const auto rows = ui::build_provider_rows(
        agentty::provider::saved_custom_hosts(g_settings.provider_keys), "");
    int a_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (const auto* pr = rows[static_cast<std::size_t>(i)].preset();
            pr && std::string{pr->id} == "anthropic") { a_idx = i; break; }
    REQUIRE(a_idx >= 0);
    p->index = a_idx;

    // Enter opens the accounts list (and closes the provider picker).
    auto [m2, c2] = app::update(std::move(m1), Msg{ProvidersSelect{}});
    CHECK(std::holds_alternative<ui::login::AccountList>(m2.ui.login));
    CHECK(!m2.ui.panel.get<pn::Providers>());  // picker closed

    // Esc from the accounts list steps BACK to the provider picker (not a
    // full close), keeping the hierarchy accounts → providers → chat.
    auto [m3, c3] = app::update(std::move(m2), Msg{LoginBack{}});
    CHECK(std::holds_alternative<ui::login::Closed>(m3.ui.login));  // accounts gone
    CHECK(m3.ui.panel.get<pn::Providers>());                 // back at providers
}

// A custom host can hold MULTIPLE saved keys (accounts): the accounts layer
// keys on the endpoint spec, storing each key as the account secret. Snapshot
// then activate must round-trip the bearer key through provider_keys[spec].
TEST_CASE("custom host supports multiple accounts") {
    using namespace agentty::msg;
    namespace acc = agentty::auth::accounts;
    // account_switch reads/writes the REAL settings + accounts.json (via
    // config_dir()/data_dir() → home_dir()), not the deps stub. Point HOME at
    // a throwaway dir so this test never touches the user's real config.
    const auto tmp = std::filesystem::temp_directory_path()
                   / ("agentty_acct_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    const char* old_home = ::getenv("HOME");
    ::setenv("HOME", tmp.c_str(), 1);

    install_stub_deps();
    const std::string spec = "my-host:8080/v1";   // not a preset ⇒ custom host

    auto set_key = [&](const std::string& k) {
        auto s = agentty::persistence::load_settings();
        s.provider_keys[spec] = k;
        agentty::persistence::save_settings(s);
    };
    auto get_key = [&]() {
        return agentty::persistence::load_settings().provider_keys[spec];
    };

    // Account A active: snapshot it into the registry.
    set_key("sk-aaaa1111");
    CHECK(acc::snapshot_active(spec, "A"));

    // Switch the active key to B and snapshot that too.
    set_key("sk-bbbb2222");
    CHECK(acc::snapshot_active(spec, "B"));

    // Both accounts are listed for this host.
    CHECK(acc::list_for(spec).size() == 2);

    // Activating A restores its key into provider_keys[spec]; then B.
    CHECK(acc::activate(spec, "A"));
    CHECK(get_key() == "sk-aaaa1111");
    CHECK(acc::activate(spec, "B"));
    CHECK(get_key() == "sk-bbbb2222");

    // Two keys that SHARE the same last-4 still derive DISTINCT labels (the
    // label mixes a prefix + suffix + length, not just the suffix) — so a
    // second key can't collide with / overwrite the first in the registry.
    set_key("sk-prefix1-SAME9999");
    const std::string l1 = acc::derive_current_label(spec);
    set_key("sk-prefix2-SAME9999");
    const std::string l2 = acc::derive_current_label(spec);
    CHECK(!l1.empty());
    CHECK(l1 != l2);

    if (old_home) ::setenv("HOME", old_home, 1); else ::unsetenv("HOME");
    std::filesystem::remove_all(tmp);
}

// The fused picker tunes reasoning effort (←/→) on the highlighted model,
// ported from the old model picker so the fused surface is complete.
TEST_CASE("fused picker cycles reasoning effort") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    // An effort-capable model (Claude Opus supports the reasoning ladder).
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.effort = Effort::None;
    m.d.available_models = {mi("claude-opus-4-5", "anthropic")};

    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    // Cursor on the active (only) model row.
    if (auto* cur = m1.ui.panel.get<pn::Models>()) cur->index = 0;
    const Effort before = m1.d.effort;
    auto [m2, c2] = app::update(std::move(m1), Msg{ModelsCycleEffort{+1}});
    // ←/→ mutates the GLOBAL m.d.effort LIVE — identical to the classic model
    // picker, so the two surfaces share one state and can't disagree.
    CHECK(m2.d.effort != before);
    CHECK(m2.ui.effort_dirty);
    const Effort after = m2.d.effort;

    // The change is already global; select just persists + switches.
    auto [m3, c3] = app::update(std::move(m2), Msg{ModelsSelect{}});
    CHECK(m3.d.effort == after);
    CHECK(!m3.ui.panel.get<pn::Models>());  // picker closed on select

    // Closing flushes a dirty effort edit (parity with the classic picker).
    auto [m4, c4] = app::update(std::move(m3), Msg{OpenModels{}});
    if (auto* cur = m4.ui.panel.get<pn::Models>()) cur->index = 0;
    auto [m5, c5] = app::update(std::move(m4), Msg{ModelsCycleEffort{+1}});
    auto [m6, c6] = app::update(std::move(m5), Msg{CloseModels{}});
    CHECK(!m6.ui.effort_dirty);            // persisted on close
}

// ^/ toggles between the fused (all-providers) picker and the classic
// single-provider picker: opening one tears the other down cleanly.
// There is ONE model picker: re-issuing OpenModels must re-open it
// cleanly (fresh query, reseeded rows) rather than stacking a second overlay
// or leaving the previous one's cache behind. This used to be a toggle
// between the fused and the classic picker; the classic one is gone.
TEST_CASE("the model picker re-opens cleanly, never stacks") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};

    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    CHECK(m1.ui.panel.is<pn::Models>(), "^/ opens the picker");
    REQUIRE(!m1.d.fused_rows.empty());
    // Type a query, then re-open: the overlay is replaced, not stacked, and
    // the stale query does not survive.
    if (auto* c = m1.ui.panel.get<pn::Models>()) c->query = "zzz";
    auto [m2, c2] = app::update(std::move(m1), Msg{OpenModels{}});
    CHECK(m2.ui.panel.is<pn::Models>(), "still exactly one picker");
    if (auto* c = m2.ui.panel.get<pn::Models>())
        CHECK(c->query.empty(), "re-open resets the filter");
    CHECK(!m2.d.fused_rows.empty(), "rows reseeded on re-open");

    // Esc closes it and releases the row cache.
    auto [m3, c3] = app::update(std::move(m2), Msg{CloseModels{}});
    CHECK(!m3.ui.panel.is<pn::Models>(), "Esc closes the picker");
    CHECK(m3.d.fused_rows.empty(), "closing releases the row cache");
}

// Signing out of ONE provider when others are still authed should fall back
// to a still-authed provider — not strand the user at the sign-in modal.
TEST_CASE("SignOut falls back to another authed provider") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["openrouter"] = "sk-or";
    g_settings.provider_keys["groq"]       = "sk-gr";
    provider::select(provider::parse_selection("openrouter"));

    Model m;
    m.d.recent_models = { ModelRef{"groq", "llama-3.3-70b"} };  // MRU fallback
    auto [m1, c1] = app::update(std::move(m), Msg{LoginMsg{SignOut{}}});

    // openrouter's key was dropped…
    CHECK(g_settings.provider_keys.count("openrouter") == 0);
    // …and we did NOT strand the user at the sign-in modal.
    CHECK(!std::holds_alternative<agentty::ui::login::Picking>(m1.ui.login));
    // The switch targeted the still-authed fallback (groq).
    CHECK(provider::active().openai_endpoint.label == "groq");
}

// Signing out of the ONLY authed provider opens the sign-in modal. (When the
// machine running the test happens to have real on-disk Anthropic creds, the
// fallback correctly finds them instead — so assert the CONTRACT: either the
// modal opened, or we switched to a genuinely-authed different provider, but
// NEVER left openrouter's dropped key as the active credential.)
TEST_CASE("SignOut with no saved fallback opens sign-in") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["openrouter"] = "sk-or";
    provider::select(provider::parse_selection("openrouter"));

    Model m;
    auto [m1, c1] = app::update(std::move(m), Msg{LoginMsg{SignOut{}}});
    CHECK(g_settings.provider_keys.count("openrouter") == 0);
    const bool opened_signin =
        std::holds_alternative<agentty::ui::login::Picking>(m1.ui.login);
    const bool switched_away =
        provider::active().openai_endpoint.label != "openrouter";
    CHECK((opened_signin || switched_away));
}

// ── Regression: custom-host models in the fused picker ───────────────────────
// cb5847aa retired the classic per-provider model picker (which read the
// active provider's list straight out of available_models) and made the
// fused cross-provider picker the ONLY models menu. But the fused picker
// reads provider_catalogs, and refresh_fused_sources enumerated ONLY
// registry presets — a saved custom host never got a catalog, so its live
// /v1/models fetch landed in available_models and the menu still showed
// nothing. These cases pin the restored behaviour through the REAL reducer:
// the active custom host's catalog mirrors available_models (rows appear),
// and a non-active saved custom host gets a catalog that a FusedCatalogLoaded
// can merge into.
TEST_CASE("fused picker shows an active custom host's models") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    // A saved custom host (provider_keys key that is not a registry preset)
    // is the ACTIVE provider, with a live catalog already in hand.
    g_settings.provider_keys["my-box.lan:8080"] = "";   // keyless local host
    provider::select(provider::parse_selection("my-box.lan:8080"));

    Model m;
    m.d.model_id = ModelId{"qwen3:32b"};
    m.d.available_models = {mi("qwen3:32b", "my-box.lan:8080"),
                            mi("llama3.3:70b", "my-box.lan:8080")};

    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    CHECK(m1.ui.panel.is<pn::Models>());

    // The custom host has a catalog, it is the ACTIVE one, and it mirrors
    // the live list (Ready, non-empty) — the exact seeding contract the
    // presets get on open.
    bool host_ready = false;
    for (const auto& c : m1.d.provider_catalogs) {
        if (c.provider_id == "my-box.lan:8080") {
            host_ready = c.state == ProviderCatalog::State::Ready
                       && c.models.size() == 2;
        }
    }
    CHECK(host_ready, "active custom host catalog mirrors available_models");

    // …and therefore ROWS: the models menu actually lists the host's models.
    CHECK(!m1.d.fused_rows.empty());
    bool saw_qwen = false, saw_llama = false;
    for (const auto& r : m1.d.fused_rows) {
        if (r.provider_id == "my-box.lan:8080") {
            if (r.model.id.value == "qwen3:32b") saw_qwen = true;
            if (r.model.id.value == "llama3.3:70b") saw_llama = true;
        }
    }
    CHECK(saw_qwen);
    CHECK(saw_llama);
}

TEST_CASE("fused picker gives a non-active saved custom host a mergeable catalog") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    // Anthropic active; the custom host is saved but NOT active. provider_keys
    // membership is the auth test for a custom host, so it must appear as a
    // source — Idle, empty, ready for cmd::fetch_models_for to fill.
    g_settings.provider_keys["anthropic"]     = "sk-test";
    g_settings.provider_keys["api.my-gw.com"] = "sk-gw";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};

    auto [m1, c1] = app::update(std::move(m), Msg{OpenModels{}});
    bool gw_idle = false;
    for (const auto& c : m1.d.provider_catalogs) {
        if (c.provider_id == "api.my-gw.com")
            gw_idle = c.state == ProviderCatalog::State::Idle
                   && c.models.empty();
    }
    CHECK(gw_idle, "saved non-active custom host has an Idle catalog");

    // The deferred wave's fetch lands: merge by provider_id, then rows show.
    FusedCatalogLoaded gw;
    gw.provider_id = "api.my-gw.com";
    gw.models = {mi("some-model-x", "api.my-gw.com")};
    gw.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(gw)});
    bool gw_ready = false;
    for (const auto& c : m2.d.provider_catalogs)
        if (c.provider_id == "api.my-gw.com")
            gw_ready = c.state == ProviderCatalog::State::Ready
                    && c.models.size() == 1;
    CHECK(gw_ready);
    bool saw_gw_model = false;
    for (const auto& r : m2.d.fused_rows)
        if (r.provider_id == "api.my-gw.com"
            && r.model.id.value == "some-model-x") saw_gw_model = true;
    CHECK(saw_gw_model, "merged custom-host catalog yields rows");
}

// ── One enumeration: adoption cannot produce a duplicate ─────────────────
// "api.githubcopilot.com" is a provider_keys entry with no preset SPEC, but
// parse_selection() adopts it onto the copilot ROW so the Copilot endpoint
// and auth apply. saved_custom_hosts() used to test the raw spec, so the
// host came back as "custom" AS WELL AS being a preset — and every consumer
// listed it twice. The provider picker showed two "GitHub Copilot" rows, and
// once the fused picker gained custom-host catalogs it grew a second catalog
// and duplicate models with it.
//
// The test is on the RESOLVED id now. These pin that, at both consumers.
TEST_CASE("an adopted host is not also a custom host") {
    store::Settings s;
    s.provider_keys["api.githubcopilot.com"] = "tok";
    s.provider_keys["api.my-gw.com"]         = "k";   // genuinely custom

    const auto hosts = provider::saved_custom_hosts(s.provider_keys);
    CHECK(hosts.size() == 1, "only the genuine custom host is 'custom'");
    CHECK(hosts.front() == "api.my-gw.com");

    // The provider picker lists it once, as the preset it resolves to.
    const auto rows = ui::build_provider_rows(hosts, "");
    int copilot_rows = 0, gw_rows = 0;
    for (const auto& r : rows) {
        if (auto* p = std::get_if<ui::ProviderRow::Preset>(&r.kind))
            if (std::string_view{p->preset->id} == "copilot") ++copilot_rows;
        if (auto* c = std::get_if<ui::ProviderRow::CustomHost>(&r.kind)) {
            if (provider::parse_selection(c->spec).provider_id() == "copilot")
                ++copilot_rows;
            if (c->spec == "api.my-gw.com") ++gw_rows;
        }
    }
    CHECK(copilot_rows == 1, "provider picker shows Copilot exactly once");
    CHECK(gw_rows == 1, "the genuine custom host still gets its row");
}

// The catalog enumeration is the SAME list, so it cannot disagree with the
// picker about which backends exist. This is the property that makes
// "we forgot to enumerate X" unrepresentable rather than merely fixed.
TEST_CASE("catalog_sources enumerates presets and custom hosts, once each") {
    store::Settings s;
    s.provider_keys["api.githubcopilot.com"] = "tok";   // adopted → copilot
    s.provider_keys["api.my-gw.com"]         = "k";     // genuine custom
    s.provider_keys["localhost:11434"]       = "x";     // local endpoint
    // Regression (issue #30): a FULL-URL custom host with a multi-segment
    // path (z.ai's GLM Coding Plan) must be its own catalog source — it is
    // not a preset and does not adopt onto one, so the picker showed nothing.
    s.provider_keys["https://api.z.ai/api/coding/paas/v4"] = "zkey";

    const auto srcs = provider::catalog_sources(s);

    auto count_id = [&](std::string_view id) {
        int n = 0;
        for (const auto& c : srcs) if (c.id == id) ++n;
        return n;
    };
    CHECK(count_id("copilot") == 1, "adopted host does not double the preset");
    CHECK(count_id("api.githubcopilot.com") == 0, "adopted spec is not its own source");
    CHECK(count_id("api.my-gw.com") == 1, "a custom gateway is a source");
    CHECK(count_id("localhost:11434") == 1, "a LOCAL endpoint is a source too");
    CHECK(count_id("https://api.z.ai/api/coding/paas/v4") == 1,
          "a full-URL custom host is a source");

    // Every id is unique — the property a per-caller enumeration kept losing.
    std::set<std::string> ids;
    for (const auto& c : srcs) {
        CHECK(ids.insert(c.id).second, "catalog source ids are unique");
        CHECK(!c.label.empty(), "every source has a label to render");
    }
    // Presets are still all present (the enumeration didn't drop the easy half).
    for (const auto& p : provider::providers())
        CHECK(count_id(p.id) == 1);
    // Custom hosts are never sign-in offers: holding a saved key IS auth.
    for (const auto& c : srcs)
        if (!c.is_preset) CHECK(!c.needs_signin);
}
