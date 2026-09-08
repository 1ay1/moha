// models.cpp — the fused cross-provider model picker's reducer: open/
// filter/select, slot-assign, the MRU ring, catalog freshness, and the
// switch_to_model_ref / open_login_for helpers other arms dispatch to.

#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/core/anim_clock.hpp>   // anim_now_ms (catalog freshness clock)
#include <maya/platform/io.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/catalog_sources.hpp"
#include "agentty/auth/vault.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/provider_rows.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

namespace {
// Monotonic ms from maya's animation clock — test-controllable via
// maya::testing::advance_anim_clock_ms, so catalog freshness (loaded_at_ms) is
// reproducible / advanceable in tests.
[[nodiscard]] std::int64_t now_ms() {
    return ::maya::anim_now_ms();
}

// A fused-picker catalog older than this is refetched on the next open. Long
// enough that rapid re-opens don't re-hammer providers, short enough that a
// model added upstream shows up within a normal session.
constexpr std::int64_t kCatalogTtlMs = 60'000;   // 60s


// Defined further down (with the fused-picker MRU helpers); forward-declared
// here so the classic model-picker reducer above can feed the same ring.
void record_recent(Model& m, const std::string& provider_id,
                   const std::string& model_id);
void hydrate_recents(Model& m);
void rebuild_fused_rows(Model& m, bool sync_sources = true);
} // namespace
using maya::Cmd;


// ── Fused cross-provider model picker ────────────────────────────────────
namespace {

constexpr int kRecentCap = 6;

// Record (provider,model) at the FRONT of the MRU, deduped, capped. Persists
// to Settings.recent_models so RECENT + ^Tab survive restart. Mirrors into
// m.d.recent_models for the live picker build.
void record_recent(Model& m, const std::string& provider_id,
                   const std::string& model_id) {
    if (provider_id.empty() || model_id.empty()) return;
    ModelRef ref{provider_id, model_id};
    auto& mru = m.d.recent_models;
    // Identity for the ring is the capkey-FOLDED id: providers list the
    // same model under multiple alias spellings (Mistral's catalog carries
    // mistral-medium-3-5 AND -3.5 — both canonical mistral-medium-latest),
    // and picking two spellings at different times must not grow two RECENT
    // rows for one model. The newest pick's SPELLING wins (it replaces the
    // older entry wholesale), so the ring shows what the user last chose.
    const std::string folded = capkey::norm_row_id(model_id);
    std::erase_if(mru, [&](const ModelRef& r) {
        return r.provider_id == provider_id
            && capkey::norm_row_id(r.model_id) == folded;
    });
    mru.insert(mru.begin(), std::move(ref));
    if (static_cast<int>(mru.size()) > kRecentCap) mru.resize(kRecentCap);

    auto s = deps().load_settings();
    s.recent_models.clear();
    for (const auto& r : mru)
        s.recent_models.push_back(r.provider_id + "\t" + r.model_id);
    deps().save_settings(s);
}

// Hydrate m.d.recent_models from Settings ("<provider>\t<model>" per entry).
void hydrate_recents(Model& m) {
    if (m.d.recent_models.empty()) {
        auto s = deps().load_settings();
        for (const auto& e : s.recent_models) {
            auto tab = e.find('\t');
            if (tab == std::string::npos) continue;
            ModelRef ref{e.substr(0, tab), e.substr(tab + 1)};
            // Fold legacy duplicate SPELLINGS of one model (persisted before
            // record_recent deduped by capkey identity): first (newest)
            // spelling wins, later aliases are dropped.
            const std::string folded = capkey::norm_row_id(ref.model_id);
            bool dup = false;
            for (const auto& r : m.d.recent_models)
                if (r.provider_id == ref.provider_id
                    && capkey::norm_row_id(r.model_id) == folded) {
                    dup = true;
                    break;
                }
            if (!dup) m.d.recent_models.push_back(std::move(ref));
        }
    }
    // Always ensure the ACTIVE (provider, model) is present so ^Tab has a
    // home to return to and the ring is never a lone stale entry. Persisted
    // history may predate the current model (e.g. a fresh switch that hasn't
    // been recorded yet, or a first-ever session), so append it if missing.
    // Folded comparison: the active id being an alias spelling of a ring
    // entry must not append a visual duplicate.
    if (!m.d.model_id.value.empty()) {
        const ModelRef active{active_provider_id(), m.d.model_id.value};
        const std::string folded = capkey::norm_row_id(active.model_id);
        bool present = false;
        for (const auto& r : m.d.recent_models)
            if (r.provider_id == active.provider_id
                && capkey::norm_row_id(r.model_id) == folded) {
                present = true;
                break;
            }
        if (!present) m.d.recent_models.push_back(active);
    }
}

// Is an MRU (provider, model) ref still switchable? True when its provider is
// authed AND — if we hold that provider's catalog — the model id is still
// listed. If no catalog is loaded yet we can't disprove it, so we optimistically
// allow it (the switch's own refetch + stale-model guard self-heal). Used to
// skip dead ring entries on ^Tab so a delisted / signed-out model is never the
// switch target.
[[nodiscard]] bool mru_ref_is_live(const Model& m, const ModelRef& ref,
                                   const store::Settings& settings) {
    if (!provider::provider_is_authed(ref.provider_id, settings)) return false;
    for (const auto& c : m.d.provider_catalogs) {
        if (c.provider_id != ref.provider_id) continue;
        if (c.models.empty()) return true;          // catalog not loaded yet
        for (const auto& mi : c.models)
            if (mi.id.value == ref.model_id) return true;
        return false;                                // provider known, model gone
    }
    return true;   // no catalog for this provider yet — don't disprove
}

// Refresh the picker's SOURCES into the Model — a CHEAP, in-memory-only pass
// (one settings read + provider enumeration + stat-cached auth checks), run
// when the picker opens. It does NOT touch the network or build any provider's
// model list: the active provider is seeded from the catalog already in hand
// (available_models), every other authed provider gets an empty Loading entry
// that the async fetch (fetch_models_for) fills in a frame or two later. This
// is what keeps open INSTANT even with slow backends (Ollama / custom hosts
// whose list probe would otherwise block the UI thread for seconds).
void refresh_fused_sources(Model& m) {
    const auto settings = deps().load_settings();
    const std::string active_pid = active_provider_id();

    // Prune catalogs whose provider is no longer authed (e.g. signed out via
    // ^D while the picker is open). Without this a stale catalog lingers and
    // its models keep showing in the fused list until restart. The id overload
    // handles custom hosts too (authed iff their saved key still exists).
    std::erase_if(m.d.provider_catalogs, [&](const ProviderCatalog& c) {
        return !provider::provider_is_authed(c.provider_id, settings);
    });

    auto find_cat = [&](std::string_view id) -> ProviderCatalog* {
        for (auto& c : m.d.provider_catalogs)
            if (c.provider_id == id) return &c;
        return nullptr;
    };
    m.d.fused_offers.clear();

    // ONE pass over ONE enumeration. provider::catalog_sources() is the
    // single statement of "every backend that can answer a models query" —
    // registry presets plus saved custom hosts (raw host:port, Ollama,
    // llama.cpp, a private gateway), with adoption already resolved so a
    // spec that names a known backend appears once, as that backend.
    //
    // This used to be two hand-written loops, and for a while it was one:
    // only presets were enumerated, so a saved custom host never got a
    // catalog and its models simply never appeared. Splitting the list
    // across call sites is what let a whole source be forgotten, so the
    // list now lives in one place and every consumer reads it from there.
    for (const auto& src : provider::catalog_sources(settings)) {
        if (src.needs_signin) {
            // Un-authed preset → a QUERY-GATED sign-in offer: it never
            // clutters the browse view (build_fused_rows hides offers while
            // the query is empty), but typing "mistral" with no Mistral
            // account surfaces one "Mistral — sign in" row instead of a DEAD
            // END. Enter routes into its auth flow (origin::Models) and
            // returns here with the catalog loading — search is the single
            // verb: find model → maybe sign in → pick.
            m.d.fused_offers.push_back(SigninOffer{src.id, src.label});
            continue;
        }
        ProviderCatalog* c = find_cat(src.id);
        if (!c) {
            // Several SAVED custom hosts can share one hostname and differ
            // only in their "#name" account tag ("ollama.com#main" vs
            // "ollama.com#work"). provider_display_name() strips the URL
            // path — and with it any tag that sits past a "/" — so their
            // labels would come out IDENTICAL in the picker. Surface the
            // tag in brackets so every row stays unique and the user can
            // tell which account it points at. Preset sources skip this:
            // their id carries no tag and their registry label is already
            // unambiguous.
            std::string label = src.label;
            if (!src.is_preset) {
                if (auto h = src.id.rfind('#'); h != std::string::npos
                                                && h + 1 < src.id.size()) {
                    const std::string tag = src.id.substr(h + 1);
                    // Only append when the label doesn't ALREADY show this
                    // fragment. URL-form specs keep the "#tag" verbatim, so
                    // look for that exact "#tag" token — not a bare substring
                    // of the tag, which would wrongly suppress the bracket
                    // when the tag text also appears in the HOST (e.g.
                    // "main.ollama.com#main", where a plain find("main") hits
                    // the host and leaves the row ambiguous).
                    if (label.find('#' + tag) == std::string::npos)
                        label += " [" + tag + "]";
                }
            }
            m.d.provider_catalogs.push_back(ProviderCatalog{
                src.id, std::move(label), ProviderCatalog::State::Idle, {}, {}});
            c = &m.d.provider_catalogs.back();
        }
        // Active backend: MIRROR the live catalog we already hold
        // (available_models is the SSOT for the active provider). Re-seed on
        // EVERY refresh, not just when empty — otherwise the fused catalog
        // freezes on whatever available_models was at the FIRST open (often
        // the bundled seed, before the live /v1/models fetch landed) and
        // then diverges as available_models grows (a newly-listed flagship
        // would never appear). Everyone else stays empty + Idle so Open
        // fires a background fetch through cmd::fetch_models_for(id), which
        // resolves a preset's auth or a custom host's saved key alike; the
        // row list simply grows as each resolves.
        if (src.id == active_pid && !m.d.available_models.empty()) {
            if (c->models != m.d.available_models) {
                c->models = m.d.available_models;
                c->invalidate_derived();    // model set changed — all caches stale
            }
            c->state = ProviderCatalog::State::Ready;
        }
    }
}


} // namespace

// Shared builder used by BOTH this reducer and the fused_picker view, so the
// row list they act on can never disagree (SSOT). Declared in internal.hpp.
// PURE: builds only from the already-refreshed sources (provider_catalogs +
// fused_offers) + the live query — no disk, no enumeration, so it is cheap
// enough to run on every keystroke.
std::vector<FusedRow> fused_rows_for_model(const Model& m) {
    ui::FusedInputs in;
    in.catalogs   = &m.d.provider_catalogs;
    in.offers     = &m.d.fused_offers;
    in.recents    = &m.d.recent_models;
    in.active     = ModelRef{active_provider_id(), m.d.model_id.value};
    in.recent_cap = kRecentCap;
    // The canonical, provider-uniform label so the fused rows read AND
    // match identically to the per-provider picker (ui::model_display_label).
    in.label_fn   = &ui::model_display_label;
    // Smart Mode slot-assign pins a model that will be dispatched to the
    // ACTIVE provider, so only its models may appear. See the Select arm.
    if (auto* c = m.ui.panel.get<pn::Models>()) {
        if (c->assign_slot) in.only_provider = active_provider_id();
        in.query = c->query;
        // ^/ scope: restrict the list to one provider's models. Smart-assign
        // scoping (above) wins when both are set — the slot constraint is
        // stronger than a user's drill-in.
        if (!c->provider_scope.empty() && in.only_provider.empty())
            in.only_provider = c->provider_scope;
    }
    return ui::build_fused_rows(in);
}

namespace {

// Rebuild the cached row list into m.d.fused_rows. Called by the reducer ONLY
// at the points its inputs change (open / filter / catalog-loaded / favorite)
// so the view + cursor math never re-enumerate providers or re-read
// settings.json per frame or per keystroke.
//
// `sync_sources` controls the EXPENSIVE preamble (settings read + provider
// enumeration + per-provider auth stat + prune + active-catalog mirror). It is
// only needed when AUTH or available_models may have changed — on open, on a
// ModelsLoaded (active provider), or a favorite toggle. A plain async
// catalog-arrival (FusedCatalogLoaded) for a NON-active provider changed only
// that catalog's models; passing sync_sources=false there re-ranks + rebuilds
// keys without the auth-stat churn, so a burst of providers resolving doesn't
// re-enumerate + re-stat auth N times.
void rebuild_fused_rows(Model& m, bool sync_sources) {
    // Snapshot ONLY the highlighted row's identity for the cursor re-anchor
    // below — NOT the whole row list. The old full deep copy (every row's
    // strings + ModelInfo) ran per keystroke; two strings carry the same
    // information.
    std::string prev_provider, prev_model;
    if (auto* c = m.ui.panel.get<pn::Models>();
        c && c->index >= 0
        && c->index < static_cast<int>(m.d.fused_rows.size())) {
        const auto& r = m.d.fused_rows[static_cast<std::size_t>(c->index)];
        prev_provider = r.provider_id;
        prev_model    = r.model.id.value;
    }
    // Re-sync the sources FIRST: mirror the active provider's live
    // available_models into its catalog + pick up any newly-authed provider.
    // Without this a rebuild re-ranks the STALE catalog, so a model the live
    // /v1/models fetch just added (ModelsLoaded) never reaches the open
    // picker — the "fused pane doesn't update" bug.
    if (sync_sources) refresh_fused_sources(m);
    // Keep each catalog's precomputed, lowercased fuzzy keys in sync with its
    // model set. This is the SINGLE place the filter consumes catalogs, so
    // keys built here are reused across every keystroke — the per-key filter
    // never re-allocates or re-lowercases a haystack per model. A model-set
    // change clears search_keys at the mutation site (size mismatch here), so
    // this rebuild runs O(models) only when a catalog actually changed.
    for (auto& c : m.d.provider_catalogs) {
        if (c.search_keys.size() != c.models.size()) {
            c.search_keys.clear();
            c.search_keys.reserve(c.models.size());
            // Folded row identities in the same pass (same invalidation):
            // build_fused_rows' alias-dedup probes these instead of calling
            // capkey::norm_row_id per (candidate × seen) pair per keystroke.
            c.row_keys.clear();
            c.row_keys.reserve(c.models.size());
            c.display_labels.clear();
            c.display_labels.reserve(c.models.size());
            for (const auto& mi : c.models) {
                // Canonical label (ui::model_display_label) as the name
                // segment — the SAME string build_fused_rows scores and
                // the view renders — so a cached haystack can't drift from
                // what's on screen.
                std::string label =
                    ui::model_display_label(mi.id.value, mi.display_name);
                std::string key = ui::detail::fused_haystack(
                    c.label, label, mi);
                for (char& ch : key)
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                c.search_keys.push_back(std::move(key));
                c.row_keys.push_back(capkey::norm_row_id(mi.id.value));
                c.display_labels.push_back(std::move(label));
            }
        }
        // Memoise the per-model reasoning chip: resolved_caps is 3 registry
        // lookups behind a shared_mutex — far too hot for the per-keystroke
        // row build over large catalogs. Rebuild when the model set changed
        // OR any capability registry wrote since we last built (caps_epoch:
        // models.dev refresh, learned rejection, ^E override).
        const std::uint64_t epoch = caps_epoch();
        if (c.reason_flags.size() != c.models.size()
            || c.reason_epoch != epoch) {
            c.reason_flags.clear();
            c.reason_flags.reserve(c.models.size());
            for (const auto& mi : c.models)
                c.reason_flags.push_back(effort_capable(
                    resolved_caps(mi.id.value, c.provider_id)));
            c.reason_epoch = epoch;
        }
    }
    m.d.fused_rows = fused_rows_for_model(m);

    // Keep the CURSOR on the SAME model across a rebuild. A rebuild can re-rank
    // the rows (an async catalog landed, or the query changed), so the row at
    // the old index may now be a DIFFERENT model — a subsequent ^E/^F/effort
    // edit would hit the wrong one. Re-find the snapshotted (provider, model)
    // after the rebuild; fall back to a clamped index.
    if (auto* c = m.ui.panel.get<pn::Models>()) {
        const int n = static_cast<int>(m.d.fused_rows.size());
        if (n == 0) { c->index = 0; return; }
        if (!prev_model.empty()) {
            for (int i = 0; i < n; ++i) {
                const auto& r = m.d.fused_rows[static_cast<std::size_t>(i)];
                if (r.model.id.value == prev_model
                    && r.provider_id == prev_provider) {
                    c->index = i; return;
                }
            }
        }
        if (c->index >= n) c->index = n - 1;
        if (c->index < 0)  c->index = 0;
    }
}

// Resolve the AuthHeader for switching to `spec` (an ALREADY-AUTHED provider,
// since the fused picker only surfaces authed rows). Delegates to the same
// resolver the provider picker uses: Anthropic OAuth/key from disk, hosted
// Resolve auth for switching to `spec` through the ONE central resolver, so
// the picker uses the same credential model as everything else (no local
// re-implementation, no anthropic side-channel). oauth-native / local resolve
// empty here and their transports supply the token.
auth::AuthHeader resolve_switch_auth(const std::string& spec) {
    return provider::credentials::resolve(spec);
}

// THE atomic switch: make (provider, model) active.
//   • same provider  → change the model in place (effort re-clamp, persist,
//     refetch), no provider hop.
//   • cross provider  → commit_provider_switch with the model PRE-STASHED so
//     the funnel installs exactly it (atomic provider+model+auth).
// Records the target in the MRU either way (unless `record` is false, e.g. a
// ^Tab ring-walk which must not reorder the MRU mid-cycle), and fires the
// switch toast.
Step switch_to_model_ref(Model m, const ModelRef& ref, bool record = true) {
    const std::string cur_pid = active_provider_id();

    if (ref.provider_id == cur_pid) {
        // Same provider — pure model change.
        m.d.model_id    = ModelId{ref.model_id};
        m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
        for (const auto& mi : m.d.available_models)
            if (mi.id == m.d.model_id && mi.context_window > 0) {
                m.s.context_max = mi.context_window; break;
            }
        if (!m.d.model_id.value.empty())
            m.d.effort = clamp_effort(m.d.effort,
                                      resolved_caps(m.d.model_id.value,
                                                    ref.provider_id));
        tools::subagent::set_model(m.d.model_id.value);
        persist_settings(m);
        if (record) record_recent(m, ref.provider_id, ref.model_id);
        auto toast = set_status_toast(m,
            ui::pretty_model_label(m.d.model_id.value) + " \xc2\xb7 "
                + provider::provider_display_name(provider::active()),
            std::chrono::seconds{3});
        return {std::move(m), std::move(toast)};
    }

    // Cross-provider — atomic switch through the ONE funnel, model pre-stashed.
    const provider::ProviderPreset* p = provider::preset_for(ref.provider_id);
    const std::string label = p ? std::string{p->label} : ref.provider_id;
    auth::AuthHeader auth = resolve_switch_auth(ref.provider_id);
    if (record) record_recent(m, ref.provider_id, ref.model_id);
    return commit_provider_switch(std::move(m), ref.provider_id,
                                  std::move(auth), label, ref.model_id);
}

// Route to the login flow for `provider_id`, popping to `origin` on Esc.
// Used by a fused sign-in offer (un-authed provider row). Mirrors the entry
// points ProvidersSelect uses for each auth style.
Step open_login_for(Model m, const std::string& provider_id,
                    const std::string& label, ui::login::Origin origin) {
    const provider::ProviderPreset* p = provider::preset_for(provider_id);
    if (p && p->oauth_native) {
        // ChatGPT/Copilot/Kimi: OAuth device/browser flow via the method menu
        // scoped to this provider.
        ui::login::Picking pk;
        pk.provider = provider_id;
        pk.origin   = std::move(origin);
        m.ui.login = std::move(pk);
        return {std::move(m), maya::Cmd<Msg>::none()};
    }
    // Hosted API-key (or Anthropic key): the API-key input, returning to the
    // fused picker on success.
    m.ui.login = ui::login::ApiKeyInput{
        .provider       = provider_id,
        .provider_label = label,
        .origin         = std::move(origin),
    };
    return {std::move(m), maya::Cmd<Msg>::none()};
}

} // namespace

Step models_update(Model m, msg::ModelsMsg pm) {
    using namespace agentty::msg;
    auto done = [](Model mm) -> Step { return {std::move(mm), maya::Cmd<Msg>::none()}; };

    // Clamp the cursor to the current row count after any list change.
    auto clamp_cursor = [](Model& mm) {
        if (auto* c = mm.ui.panel.get<pn::Models>()) {
            const int n = static_cast<int>(mm.d.fused_rows.size());
            if (n == 0) { c->index = 0; return; }
            if (c->index < 0)  c->index = 0;
            if (c->index >= n) c->index = n - 1;
        }
    };

    return std::visit(overload{
        [&](OpenModels) -> Step {
            hydrate_recents(m);
            if (auto* c = m.ui.panel.get<pn::Models>()) {
                // Already open (the smart-mode hand-off descend()s the panel
                // and THEN dispatches OpenModels; or ^/ re-pressed): keep the
                // instance — its assign_slot and `from` snapshot — but reset
                // the pick state, so a re-open never shows a stale filter.
                c->index = 0;
                c->query.clear();
            } else {
                // Cold open: descend, adopting whatever is open as parent.
                m.ui.panel.descend(pn::Models{{0, ""}});
            }
            // ONE expensive pass: enumerate providers, read settings, seed
            // every authed provider's catalog from its bundled list so the
            // picker opens instantly full.
            refresh_fused_sources(m);
            rebuild_fused_rows(m);       // seed the cache the view reads
            // PRIORITIZE the ACTIVE provider: refetch ITS live catalog now,
            // alone, so the models you're most likely to pick refresh fast
            // (→ ModelsLoaded rebuilds the open rows). The other authed
            // providers refresh LAZILY a beat later (FusedRefreshOthers), so
            // the active result isn't queued behind slower providers on the
            // bounded worker pool. Mark the others Loading only when that
            // deferred pass fires — here they keep showing their bundled seed.
            // Skip the active refetch if its catalog is still fresh (rapid
            // re-open), but always schedule the lazy wave for the others.
            const std::string apid0 = active_provider_id();
            bool active_fresh = false;
            for (const auto& cat : m.d.provider_catalogs)
                if (cat.provider_id == apid0) {
                    active_fresh = cat.loaded_at_ms != 0
                                && (now_ms() - cat.loaded_at_ms) <= kCatalogTtlMs;
                    break;
                }
            std::vector<maya::Cmd<Msg>> boot;
            if (!active_fresh) boot.push_back(cmd::fetch_models());
            boot.push_back(maya::Cmd<Msg>::after(std::chrono::milliseconds{120},
                                                 Msg{FusedRefreshOthers{}}));
            return {std::move(m), maya::Cmd<Msg>::batch(std::move(boot))};
        },
        [&](ModelsRefresh) -> Step {
            // ^L — force a full live refresh: reset every catalog's freshness so
            // the active + deferred waves all refetch, regardless of TTL. The
            // manual escape hatch when the user wants the very latest now.
            if (!m.ui.panel.get<pn::Models>()) return done(std::move(m));
            for (auto& c : m.d.provider_catalogs) c.loaded_at_ms = 0;
            const std::string apid = active_provider_id();
            std::vector<maya::Cmd<Msg>> boot;
            boot.push_back(cmd::fetch_models());   // active now
            for (auto& c : m.d.provider_catalogs) {
                if (c.provider_id == apid) continue;
                c.state = ProviderCatalog::State::Loading;
                boot.push_back(cmd::fetch_models_for(c.provider_id));
            }
            auto toast = set_status_toast(m, "refreshing models\xe2\x80\xa6",
                                          std::chrono::seconds{2});
            boot.push_back(std::move(toast));
            return {std::move(m), maya::Cmd<Msg>::batch(std::move(boot))};
        },
        [&](FusedRefreshOthers) -> Step {
            // Lazy second wave: refresh every OTHER authed provider whose live
            // catalog is STALE (older than the TTL), FAILED, or never fetched.
            // A recently-fetched Ready catalog is left alone so re-opening the
            // picker rapidly doesn't re-hammer every provider — but a catalog
            // that's been Ready a while IS refetched, so the list stays current
            // instead of freezing after its first load. No-op if closed.
            if (!m.ui.panel.get<pn::Models>()) return done(std::move(m));
            const std::string active_pid = active_provider_id();
            const std::int64_t t = now_ms();
            std::vector<maya::Cmd<Msg>> fetches;
            for (auto& c : m.d.provider_catalogs) {
                if (c.provider_id == active_pid) continue;   // done on open
                if (c.state == ProviderCatalog::State::Loading) continue;
                const bool never = c.loaded_at_ms == 0;
                const bool failed = c.state == ProviderCatalog::State::Failed;
                const bool stale = c.loaded_at_ms != 0
                                && (t - c.loaded_at_ms) > kCatalogTtlMs;
                if (!never && !failed && !stale) continue;   // still fresh
                c.state = ProviderCatalog::State::Loading;
                fetches.push_back(cmd::fetch_models_for(c.provider_id));
            }
            if (fetches.empty()) return done(std::move(m));
            return {std::move(m), maya::Cmd<Msg>::batch(std::move(fetches))};
        },
        [&](CloseModels) -> Step {
            m.d.fused_rows.clear();       // release the cache while closed
            if (m.ui.effort_dirty) { persist_settings(m); m.ui.effort_dirty = false; }
            // Esc unwinds one level — and that ONE path now covers slot-assign
            // too: the assign-mode picker's `from` snapshot IS the SmartMode
            // pane (form, advanced, nested chain), so ascend() restores it
            // verbatim; a cold-opened picker restores the palette or closes.
            // The abandoned assign mode dies WITH the picker value — nothing
            // to reset. Revalidation (the form may be stale) is ascend()'s
            // job, one place for every SmartMode restore.
            ascend(m);
            return done(std::move(m));
        },
        [&](ModelsMove e) -> Step {
            if (auto* c = m.ui.panel.get<pn::Models>()) {
                c->index += e.delta;
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](ModelsJump e) -> Step {
            if (auto* c = m.ui.panel.get<pn::Models>()) {
                const int n = static_cast<int>(m.d.fused_rows.size());
                // Page by a full viewport so PageUp/Down lands a screen away
                // (matches the other pickers' kPage), not a fixed 10 that
                // under-shoots the ~14-row viewport.
                constexpr int page = 14;  // matches kViewportH in pickers.cpp
                using W = ModelsJump::Where;
                switch (e.where) {
                    case W::Home:     c->index = 0; break;
                    case W::End:      c->index = n - 1; break;
                    case W::PageUp:   c->index -= page; break;
                    case W::PageDown: c->index += page; break;
                }
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](ModelsFilterInput e) -> Step {
            if (auto* c = m.ui.panel.get<pn::Models>()) {
                // Every printable — digits included — types into the query.
                // ASCII only — model/provider ids are ASCII in practice.
                if (e.ch >= 0x20 && e.ch < 0x7f) {
                    c->query.push_back(static_cast<char>(e.ch));
                    c->index = 0;
                    // Query changed — re-rank only; auth is unchanged so skip
                    // the source re-sync (prune + per-provider auth stat).
                    rebuild_fused_rows(m, /*sync_sources=*/false);
                    clamp_cursor(m);
                }
            }
            return done(std::move(m));
        },
        [&](ModelsFilterBackspace) -> Step {
            if (auto* c = m.ui.panel.get<pn::Models>(); c && !c->query.empty()) {
                c->query.pop_back();
                c->index = 0;
                rebuild_fused_rows(m, /*sync_sources=*/false);   // re-rank only
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](FusedCatalogLoaded e) -> Step {
            // Merge in place, guarded by provider_id (a provider signed out
            // mid-fetch is simply not in the list anymore).
            for (auto& c : m.d.provider_catalogs) {
                if (c.provider_id != e.provider_id) continue;
                if (e.ok && !e.models.empty()) {
                    c.models = std::move(e.models);
                    c.invalidate_derived();  // ids changed — all caches stale
                    c.state  = ProviderCatalog::State::Ready;
                    c.loaded_at_ms = now_ms();   // mark fresh
                } else {
                    c.state  = e.ok ? ProviderCatalog::State::Ready
                                    : ProviderCatalog::State::Failed;
                    if (e.ok) c.loaded_at_ms = now_ms();
                }
                break;
            }
            // Only the fused picker's own list depends on the merged catalogs;
            // rebuild it (cheap, once per resolving provider) if it's open. A
            // catalog arrival didn't change AUTH, so skip the source re-sync
            // (prune + per-provider auth stat) — just re-rank + rebuild keys.
            if (m.ui.panel.is<pn::Models>()) {
                rebuild_fused_rows(m, /*sync_sources=*/false);
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](ModelsToggleFavorite) -> Step {
            auto* c = m.ui.panel.get<pn::Models>();
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            if (row.is_signin_offer()) return done(std::move(m));
            auto s = deps().load_settings();
            ModelId mid = row.model.id;
            auto it = std::find(s.favorite_models.begin(),
                                s.favorite_models.end(), mid);
            const bool now_fav = (it == s.favorite_models.end());
            if (now_fav) s.favorite_models.push_back(mid);
            else         s.favorite_models.erase(it);
            deps().save_settings(s);
            // Live feedback: flip the star on every cached row for this model
            // (no re-sort — keep the cursor where it is).
            for (auto& r : m.d.fused_rows)
                if (r.model.id == mid) r.model.favorite = now_fav;
            return done(std::move(m));
        },
        [&](ModelsCycleEffort e) -> Step {
            // ←/→ walks the reasoning-effort ladder of the HIGHLIGHTED model
            // (off → low → medium → high … within its caps), mutating the
            // GLOBAL m.d.effort LIVE — exactly like the classic model picker's
            // ModelsCycleEffort. Both surfaces share m.d.effort, so a
            // change here shows everywhere at once (no staging split, no
            // "off in one, on in the other"). Persisted lazily via effort_dirty.
            auto* c = m.ui.panel.get<pn::Models>();
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            if (row.is_signin_offer()) return done(std::move(m));
            // Resolve under the ROW's provider — a Groq row highlighted while
            // Mistral is active must walk Groq's ladder, not Mistral's.
            const auto caps = resolved_caps(row.model.id.value,
                                            row.provider_id);
            if (!effort_capable(caps)) return done(std::move(m));
            m.d.effort = cycle_effort(clamp_effort(m.d.effort, caps),
                                      e.delta, caps);
            m.ui.effort_dirty = true;
            return done(std::move(m));
        },
        [&](ModelsToggleReasoning) -> Step {
            // ^E flips the highlighted model's per-model reasoning OVERRIDE
            // through its tri-state (auto → ON → OFF → auto). Mirrors the
            // model picker so tuning survives the move to the fused surface.
            auto* c = m.ui.panel.get<pn::Models>();
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            if (row.is_signin_offer()) return done(std::move(m));
            const std::string id = row.model.id.value;
            // Claude/GPT are FAMILY-GATED: their effort ladder is decoded from
            // the model family and is not user-editable, so an override here
            // would be a stored no-op that silently shadows the real ladder.
            // Hint and bail instead — ←/→ is the control that works for them.
            // (The classic picker had this gate; it was dropped when the arm
            // was ported, letting ^E persist junk overrides for Opus/GPT.)
            {
                const auto base = ModelCapabilities::from_id(id);
                if (base.is_known_family()
                    || base.family == ModelCapabilities::Family::Gpt) {
                    auto toast = set_status_toast(m,
                        "reasoning effort is model-managed here "
                        "(\xe2\x86\x90/\xe2\x86\x92 to set the tier)");
                    return {std::move(m), std::move(toast)};
                }
            }
            const int cur = reasoning_override_for(id);   // -1 auto, 0 off, 1 on
            auto s = deps().load_settings();
            const char* label = nullptr;
            if (cur < 0) {
                s.reasoning_effort_overrides[id] = true;
                set_reasoning_override(id, true);
                label = "reasoning: forced ON for this model";
            } else if (cur == 1) {
                s.reasoning_effort_overrides[id] = false;
                set_reasoning_override(id, false);
                label = "reasoning: forced OFF for this model";
            } else {
                s.reasoning_effort_overrides.erase(id);
                clear_reasoning_override(id);
                label = "reasoning: auto (catalog default)";
            }
            deps().save_settings(s);
            // Clamp against the ROW's provider scope, not the ambient one:
            // the fused picker lists models from providers that are NOT
            // active, and capability facts are keyed "provider/model".
            // Using the ambient scope here (as this arm used to) resolved
            // a non-active row's effort ladder against the CURRENT
            // provider's facts — the sibling ModelsCycleEffort arm
            // already passes row.provider_id for exactly this reason.
            m.d.effort = clamp_effort(m.d.effort,
                                      resolved_caps(id, row.provider_id));
            auto toast = set_status_toast(m, label);
            return {std::move(m), std::move(toast)};
        },
        [&](SwitchToPreviousModel) -> Step {
            // ^Tab MRU cycle: walk the recent ring to progressively OLDER
            // models — A → B → C → D → A — not a single A↔B toggle. The switch
            // does NOT reorder the ring (record=false), so finding the active
            // model's index each press naturally advances the walk one step;
            // a full lap returns you home. (A TUI can't see Ctrl release, and
            // there's no idle tick, so a stable no-reorder walk is the robust
            // way to do Alt-Tab semantics here — no commit deadline needed.)
            hydrate_recents(m);
            const auto& ring = m.d.recent_models;
            if (ring.size() < 2) return done(std::move(m));  // nothing to cycle
            const ModelRef active{active_provider_id(), m.d.model_id.value};
            const int n = static_cast<int>(ring.size());
            int cur = 0;
            for (int i = 0; i < n; ++i)
                if (ring[static_cast<std::size_t>(i)] == active) { cur = i; break; }
            // Walk forward to the next LIVE entry — skip a ring member whose
            // provider is no longer authed or whose model was delisted, so
            // ^Tab never switches to a dead id (which every request 400s).
            // At most one full lap; stop if we come back to the active row.
            const auto settings = deps().load_settings();
            ModelRef target;
            for (int step = 1; step <= n; ++step) {
                const ModelRef& cand =
                    ring[static_cast<std::size_t>((cur + step) % n)];
                if (cand == active) break;                 // lapped, none live
                if (cand.empty()) continue;
                if (mru_ref_is_live(m, cand, settings)) { target = cand; break; }
            }
            if (target.empty()) return done(std::move(m));
            return switch_to_model_ref(std::move(m), target, /*record=*/false);
        },
        [&](ModelsSelect) -> Step {
            auto* c = m.ui.panel.get<pn::Models>();
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const FusedRow row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            // Copy the assign mode out BEFORE any close: `c` points into the
            // variant, and closing destroys the alternative (the old
            // ordering read it through the dangling pointer — the same shape
            // as the palette's o->index bug).
            const std::optional<smart::ModelRole> assigning = c->assign_slot;
            m.d.fused_rows.clear();
            // ←/→ already mutated m.d.effort live;
            // the switch below persists settings, so no separate apply needed.
            m.ui.effort_dirty = false;

            if (row.is_signin_offer()) {
                // Route to login for that provider, returning here after.
                m.ui.panel.close<pn::Models>();
                return open_login_for(std::move(m), row.provider_id,
                                      row.label,
                                      ui::login::origin::Models{});
            }

            // Smart Mode slot-assign mode: write the chosen model into the
            // target role slot instead of switching the active model, then
            // pop back to the parent Smart Mode picker.
            //
            // Slot models are ACTIVE-PROVIDER scoped: smart::resolve_role
            // hands the pinned id straight to the turn's request, which is
            // dispatched to whatever provider is active. Pinning a row from a
            // different provider would therefore send an unknown model id to
            // the active endpoint. Slot-assign mode filters the row list to
            // the active provider (see rebuild_fused_rows) so such a row is
            // never selectable in the first place — unrepresentable beats
            // validated.
            if (assigning) {
                const smart::ModelRole assigned = *assigning;
                // One role->field mapping, shared with every other reader
                // and writer (RoleConfig::slot). The switch on 0/1/2 that
                // used to live here was a third copy of it.
                smart::RoleConfig cfg = m.d.smart;
                smart::SlotOverride& slot = cfg.slot(assigned);
                slot.model = row.model.id.value;
                slot.set   = true;
                // Stamp the provider this pin was made under. A model id
                // only means something to the endpoint that serves it, so
                // resolve_role replays the pin ONLY under this provider
                // (see SlotOverride::provider). Prefer the row's own
                // provider — in slot-assign mode the list is already
                // filtered to the active one, so they agree, but the row
                // is the more direct truth.
                slot.provider = row.provider_id.empty()
                                  ? active_provider_id()
                                  : row.provider_id;
                cfg.enabled = true;   // pinning a slot means "on"

                // ONE entry point: persists, installs on the UI thread and
                // pushes to the subagent router. Pinning a model and having
                // `task` keep using the old one is exactly what the open-coded
                // persist_settings here used to do.
                apply_smart(m, std::move(cfg));
                m.d.fused_rows.clear();
                // Pop back to the parent Smart Mode pane — the picker's
                // `from` snapshot, restored + revalidated by ascend() (which
                // rebuilds the form from the config just applied) — cursor
                // on the slot we just set. You probably want the sibling
                // slots too; re-opening Smart Mode per slot was the tedium
                // this fixes.
                ascend(m);
                if (auto* sm = m.ui.panel.get<pn::SmartMode>())
                    smart_form::focus_role(sm->form, assigned);
                auto toast = set_status_toast(m, "Smart Mode slot set");
                return {std::move(m), std::move(toast)};
            }

            // Ordinary pick: a COMPLETED selection means "done" — close
            // outright rather than ascend. (Esc is the "back" gesture; a
            // pick that popped you back into the palette would feel like
            // the selection hadn't taken.)
            m.ui.panel.close<pn::Models>();
            return switch_to_model_ref(std::move(m), row.ref());
        },
        [&](ModelsLoaded& e) -> Step {
            // STALENESS GATE: only accept a payload fetched FOR the provider
            // that is active NOW. Two quick switches interleave their slow
            // fetches; without this, provider A's late catalog lands under
            // provider B and the picker offers models B cannot stream.
            // (Empty provider_id = legacy/synthetic dispatch — accept.)
            if (!e.provider_id.empty()
                && e.provider_id != active_provider_id()) {
                return done(std::move(m));   // keep models_loading: the
                                             // newer fetch is still in flight
            }
            // The fetch finished (success OR failure) — always clear the
            // in-flight flag so the picker leaves "Loading models…".
            m.s.models_loading = false;
            // A failed fetch surfaces its reason as a transient toast —
            // never as a StreamError, which would feed the live turn's
            // retry machinery (see the ModelsLoaded msg comment).
            if (!e.error.empty()) {
                auto toast = set_status_toast(m, std::move(e.error),
                                              std::chrono::seconds{6});
                return {std::move(m), std::move(toast)};
            }
            if (e.models.empty()) return done(std::move(m));
            auto settings = deps().load_settings();
            // PERSIST-ON-SUCCESS: a custom --provider spec registered at
            // startup as unproven becomes sticky NOW — the host answered a
            // non-empty model fetch, so it's a real endpoint, not a typo.
            // Presets persisted at parse time as always; this only fires
            // for raw host/URL specs, at most once per process.
            if (auto proven = provider::take_unproven_spec(
                    active_provider_id())) {
                settings.provider = proven->first;
                if (!proven->second.empty())
                    settings.provider_models[proven->first] = proven->second;
                deps().save_settings(settings);
            }
            m.d.available_models.clear();
            for (auto& mi : e.models) {
                // DISCOVERED entitlement: this account already 400'd on the
                // context-1m beta ("long context beta is not available for
                // this subscription"), so offering the `[1m]` rows would
                // just sell a window the wire will reject. OAuth alone can't
                // tell us (the token carries no entitlement field) — the
                // flag is learned from the first rejection and cleared on
                // sign-out/account switch.
                if (entitlement_blocked(
                        settings, domain::entitlement::Fact::Context1M,
                        wire_model_id(mi.id.value))
                    && mi.id.value.find("[1m]") != std::string::npos)
                    continue;
                for (const auto& fav : settings.favorite_models)
                    if (mi.id == fav) mi.favorite = true;
                m.d.available_models.push_back(std::move(mi));
            }
            // Refresh the subagent router's candidate pool so read-only roles
            // route to the cheapest capable model THIS provider offers. Done
            // on every load (startup, provider switch, refetch) so routing
            // never uses a stale provider's list.
            tools::subagent::set_candidates(m.d.available_models);
            // Keep the subagent role-router in sync with Smart Mode (Layer 3b).
            tools::subagent::set_smart(m.d.smart);
            tools::subagent::set_provider(active_provider_id());
            // If the active model isn't offered by this provider (e.g. just
            // switched to Ollama with no recall, or a stale saved id), fall
            // back to the first available model so the user is never pointed
            // at a model that 400s on the first prompt. Persist the pick so
            // it sticks as this provider's recall.
            bool active_present = false;
            for (const auto& mi : m.d.available_models)
                if (mi.id == m.d.model_id) { active_present = true; break; }
            if (!active_present && !m.d.available_models.empty()) {
                m.d.model_id = m.d.available_models.front().id;
                m.s.context_max =
                    ui::context_max_for_model(m.d.model_id.value);
                // The auto-selected model may not support the effort tier that
                // rode over from the previous provider — clamp it so the picker
                // chip and the wire agree (commit_provider_switch couldn't do
                // this yet: the model id was empty until this refetch landed).
                // Uniform across every provider INCLUDING ChatGPT: gpt-5.x
                // decodes through Family::Gpt with an exact ladder, so the
                // same clamp keeps chip == wire (a stale `max` on a model
                // capped at xhigh must degrade in the CHIP too, not just be
                // silently rewritten at request time). Empty id = unknown
                // model → skip (clamping would wipe the tier, not no-op).
                if (!m.d.model_id.value.empty())
                    m.d.effort = clamp_effort(
                        m.d.effort, resolved_caps(m.d.model_id.value));
                tools::subagent::set_model(m.d.model_id.value);
                persist_settings(m);
            }
            // The active model may have remained valid, in which case the
            // old branch did not refresh its context cap. Codex publishes a
            // 272K window (rather than Agentty's generic 200K fallback), and
            // the status-bar gauge must reflect that immediately.
            for (const auto& mi : m.d.available_models) {
                if (mi.id == m.d.model_id && mi.context_window > 0) {
                    m.s.context_max = mi.context_window;
                    break;
                }
            }
            // If the FUSED picker is open, the active provider's catalog just
            // changed (available_models is its source), so its rows are stale
            // — rebuild them. rebuild_fused_rows re-mirrors available_models
            // into the active catalog and re-ranks, so a newly-listed model
            // (e.g. one the live /v1/models fetch just added) appears without
            // reopening the picker. Clamp the cursor to the new row count.
            if (auto* c = m.ui.panel.get<pn::Models>()) {
                // The active provider's live catalog just completed — mark its
                // catalog fresh so the TTL refresh doesn't immediately refetch
                // it, then rebuild the open rows.
                const std::string apid = active_provider_id();
                for (auto& cat : m.d.provider_catalogs)
                    if (cat.provider_id == apid) { cat.loaded_at_ms = now_ms(); break; }
                rebuild_fused_rows(m);
                const int n = static_cast<int>(m.d.fused_rows.size());
                if (n == 0) c->index = 0;
                else if (c->index >= n) c->index = n - 1;
                else if (c->index < 0) c->index = 0;
            }
            return done(std::move(m));
        },
        [&](ModelsToggleShowReasoning&) -> Step {
            // Flip whether the model's reasoning/thinking is SHOWN. Global (all
            // providers): renders the transcript reasoning block AND makes the
            // Anthropic transport request VISIBLE thinking. Persisted so it
            // survives restarts. Mirrors the ToggleChangesStrip pattern.
            m.d.show_reasoning = !m.d.show_reasoning;
            auto s = deps().load_settings();
            s.show_reasoning = m.d.show_reasoning;
            deps().save_settings(s);
            // Anthropic caveat: visible thinking is only REQUESTED when an
            // effort tier is active (the transport gates thinking mode on
            // req.effort). With effort off, ^R would silently show nothing —
            // tell the user what to flip instead of leaving a dead toggle.
            const auto caps = resolved_caps(m.d.model_id.value);
            const bool claude_no_effort =
                caps.family != ModelCapabilities::Family::Unknown
                && caps.family != ModelCapabilities::Family::Gpt
                && !caps.reasoning_compat
                && m.d.effort == Effort::None;
            auto toast = set_status_toast(m, !m.d.show_reasoning
                ? "reasoning: hidden (existing blocks fold away too)"
                : claude_no_effort
                    ? "reasoning: shown — needs an effort tier on this model "
                      "(\xe2\x86\x90/\xe2\x86\x92 in the picker)"
                    : "reasoning: shown (live thinking + \xe2\x9c\xa6 summary)");
            return {std::move(m), std::move(toast)};
        },
        [&](ModelsScopeProvider&) -> Step {
            // ^/ — restrict the list to ONLY the highlighted row's provider,
            // or clear the scope if it is already active. The selected row's
            // provider is "the current selection's provider" the user means.
            auto* c = m.ui.panel.get<pn::Models>();
            if (!c) return done(std::move(m));
            if (!c->provider_scope.empty()) {
                // Already scoped — toggle back to all providers.
                c->provider_scope.clear();
                rebuild_fused_rows(m);
                clamp_cursor(m);
                auto toast = set_status_toast(m, "scope: all providers");
                return {std::move(m), std::move(toast)};
            }
            const int n = static_cast<int>(m.d.fused_rows.size());
            if (c->index < 0 || c->index >= n) return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            // Sign-in offers / rows without a provider can't be scoped to.
            if (row.provider_id.empty()) return done(std::move(m));
            const std::string pid   = row.provider_id;
            const std::string label = row.label.empty() ? pid : row.label;
            // (row is now dangling once rebuild_fused_rows reallocates the
            // vector — pid/label are copies, so we no longer touch it.)
            c->provider_scope = pid;
            // Scoping shrinks the list — the old cursor may now point past the
            // end. Land on the first row so the selection is always valid.
            c->index = 0;
            rebuild_fused_rows(m);
            clamp_cursor(m);
            auto toast = set_status_toast(
                m, "scope: " + label + " only (^/ to clear)");
            return {std::move(m), std::move(toast)};
        },
    }, pm);
}

} // namespace agentty::app::detail
