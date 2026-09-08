// models_update + thread_list_update — reducers for the model and
// thread pickers (and the related async loads, ModelsLoaded / ThreadsLoaded).
// Both are list-modal pickers that the user opens with a key shortcut, moves
// through with Up/Down, and confirms with Enter; the underlying data comes
// from the store + provider so neither reducer is purely-local.

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

// ── Fresh-thread reset ────────────────────────────────────────────────────
// Swap the model over to a brand-new empty thread and return the terminal
// reset that wipes the departing thread's rendered turns off-screen.
//
// This is the SHARED core behind two entry points: `NewThread` (^N / picker
// `N`) and `ThreadListDelete` when the row removed is the active thread.
// It is deliberately the part *after* the caller's save/delete decision —
// NewThread persists the outgoing thread first, delete has just destroyed
// it — so the caller owns that policy and this owns the reset. Keeping the
// two callers on one code path is what stops them drifting (they had, and
// the delete copy was missing the phase reset + kernel release + inline
// wipe, which is exactly the machinery that makes a mid-stream swap safe).
//
// Returns the reset_inline Cmd so the caller can batch it with its own
// commands (delete also kicks a thread-list refresh + a toast).
[[nodiscard]] Cmd<Msg> reset_to_fresh_thread(Model& m) {
    // Skill activations belong to the departing thread's context; the new
    // thread must be able to re-load any skill from scratch.
    tools::skills::reset_activations();
    // Drop the whole render cache: every (tid,msg) entry belongs to the
    // thread we're leaving, whose messages will never freeze again (freeze
    // is the only per-entry drop, and it only runs on the CURRENT thread).
    // Keys embed thread_id so there's no collision — this purely reclaims
    // the old thread's staged/pinned entries so they don't linger.
    m.ui.view_cache.clear();
    m.d.current = Thread{};
    m.d.current.id = deps().new_thread_id();
    m.d.current.created_at = m.d.current.updated_at =
        std::chrono::system_clock::now();
    clear_frozen(m);
    // Close every modal that framed the OLD thread: the picker we acted
    // from, plus the palette / code-block picker whose contents belonged to
    // the departing thread's last reply.
    m.ui.panel.close<pn::ThreadList>();
    m.ui.panel.close<pn::Palette>();
    m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
    // Wipe the whole composer draft — a pasted-but-unsent image (or any
    // chip / queued message) belongs to the thread we're leaving. Leaking
    // it once carried an empty-bytes image attachment into the new thread's
    // first submit and 400'd the request.
    reset_composer_draft(m.ui.composer);
    // A fresh empty thread has no live turn — drop any streaming phase and
    // hand the kernel back so a mid-stream swap can't leave the wire running
    // against a thread that no longer exists.
    m.s.phase = phase::Idle{};
    // Smart-Mode per-THREAD routing state must not leak into the new thread:
    // complexity momentum (classify_score_with_context inherits a tier from
    // the PREVIOUS turn), the session cascade bias, and the last turn's
    // signature (outcome feedback would otherwise attribute the new thread's
    // first reply to the OLD thread's route). The learned per-workspace
    // priors (RoutingMemory) survive by design — they are cross-thread.
    m.s.smart_turn_complexity  = smart::Complexity::Standard;
    m.s.smart_effort_bias      = 0;
    release_to_kernel();
    // Re-warm the active provider's TLS socket. The launch-time prewarm in
    // main() has usually aged out of the pool by now — a user reads a reply,
    // composes, then hits ^N, and the 90 s idle TTL has evicted the warm
    // connection. Without this the FIRST turn of every new thread re-pays
    // the full DNS+TCP+TLS handshake (~150-300 ms) before its first SSE byte,
    // which reads as a per-new-thread lag. Opening the socket now overlaps
    // that cost with the user typing their first prompt. Non-blocking: spawns
    // a tracked background dial and returns immediately; a no-op when the pool
    // is already warm enough to serve the next request.
    provider::prewarm_active_provider();
    // Per maya's contract this is the ONE allowed wiring of reset_inline: an
    // explicit, user-initiated content swap. `\x1b[3J` wipes saved-lines
    // (including pre-agentty shell history), acceptable precisely because
    // the user asked to switch threads. Do NOT extend it to per-turn paths.
    return Cmd<Msg>::reset_inline();
}

// ── Provider picker ────────────────────────────────────────────────────────
// Selecting a row live-switches the active backend: parse the preset id
// into a Selection, install it (process-global), persist it, swap the
// Deps auth to the new provider's resolved credentials, and kick a fresh
// model fetch so the model list reflects the new backend. No restart.
Step providers_update(Model m, msg::ProvidersMsg pm) {
    // The picker's rows are ONE ordered list (presets + ACP agents + saved
    // custom hosts + "Custom host…" sentinel), built once from the current
    // search query. The cursor is an index into THIS list — no offset math,
    // and the same list the view renders (see build_provider_rows).
    const std::string query = [&] {
        const auto* p = m.ui.panel.get<pn::Providers>();
        return p ? p->query : std::string{};
    }();
    auto settings = deps().load_settings();
    const std::vector<std::string> saved_custom_hosts =
        provider::saved_custom_hosts(settings.provider_keys);
    const auto rows = ui::build_provider_rows(saved_custom_hosts, query);
    const int n = static_cast<int>(rows.size());

    return std::visit(overload{
        [&](OpenProviders) -> Step {
            // Close the model picker if the user cross-hopped here from it
            // (^P in the model picker). Without this the model picker stays
            // open and wins pick_panel's priority order (checked first), so
            // the hop would render nothing new. Flush any pending effort-tier
            // change first — the same persist CloseModels does on Esc,
            // so a hop doesn't silently drop it.
            if (m.ui.effort_dirty) {
                persist_settings(m);
                m.ui.effort_dirty = false;
            }
            // Abandon a pending Smart-Mode slot assignment: hopping away from
            // the model picker mid-assign must not leave the mode armed. The
            // mode lives ON the picker value now, so closing it is the reset
            // — nothing separate to clear. (The SmartMode snapshot in its
            // `from` is dropped with it: hopping to providers is a deliberate
            // exit from the assign flow.)
            m.ui.panel.close<pn::Models>();
            // Open at the row matching the currently-active provider. Fresh
            // rows with an empty query (so every provider is present to match).
            const auto fresh = ui::build_provider_rows(saved_custom_hosts, "");
            const auto& sel = provider::active();
            const std::string active_label =
                sel.kind == provider::Kind::ExternalAcp ? sel.acp_agent_id
                : sel.kind == provider::Kind::OpenAI    ? sel.openai_endpoint.label
                : std::string{provider::default_provider_id()};
            int idx = 0;
            for (int i = 0; i < static_cast<int>(fresh.size()); ++i) {
                const auto& row = fresh[static_cast<std::size_t>(i)];
                if (const auto* pr = row.preset(); pr && pr->id == active_label) { idx = i; break; }
                if (const auto* ag = row.acp();    ag && ag->id == active_label) { idx = i; break; }
                if (const auto* ch = row.custom_host(); ch && *ch == active_label) { idx = i; break; }
            }
            m.ui.panel = pn::Providers{{idx}};
            return done(std::move(m));
        },
        [&](CloseProviders) -> Step {
            ascend(m);   // Esc: back to whatever opened this, or close
            return done(std::move(m));
        },
        [&](ProvidersMove& e) -> Step {
            auto* p = m.ui.panel.get<pn::Providers>();
            if (!p || n == 0) return done(std::move(m));
            p->confirm_remove.clear();   // navigating disarms a pending delete
            p->index = (p->index + e.delta + n) % n;
            return done(std::move(m));
        },
        [&](ProvidersJump& e) -> Step {
            auto* p = m.ui.panel.get<pn::Providers>();
            if (!p || n == 0) return done(std::move(m));
            p->confirm_remove.clear();   // navigating disarms a pending delete
            using W = ProvidersJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = n - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(n - 1, p->index + kPage); break;
            }
            return done(std::move(m));
        },
        [&](ProvidersFilterInput& e) -> Step {
            auto* p = m.ui.panel.get<pn::Providers>();
            if (!p) return done(std::move(m));
            p->confirm_remove.clear();   // typing disarms a pending ^D delete
            // Append the typed codepoint (UTF-8) and reset the cursor to the
            // top of the freshly-narrowed list.
            char32_t cp = e.codepoint;
            if (cp < 0x80) { p->query.push_back(static_cast<char>(cp)); }
            else {
                // Minimal UTF-8 encode for multibyte input (rare in provider
                // names, but never corrupt the buffer).
                if (cp < 0x800) {
                    p->query.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    p->query.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    p->query.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    p->query.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    p->query.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    p->query.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    p->query.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    p->query.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    p->query.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            p->index = 0;
            return done(std::move(m));
        },
        [&](ProvidersFilterBackspace) -> Step {
            auto* p = m.ui.panel.get<pn::Providers>();
            if (!p || p->query.empty()) return done(std::move(m));
            // Pop one UTF-8 codepoint (trim continuation bytes then the lead).
            while (!p->query.empty()
                   && (static_cast<unsigned char>(p->query.back()) & 0xC0) == 0x80)
                p->query.pop_back();
            if (!p->query.empty()) p->query.pop_back();
            p->index = 0;
            return done(std::move(m));
        },
        [&](ProvidersDelete) -> Step {
            auto* p = m.ui.panel.get<pn::Providers>();
            if (!p || p->index < 0 || p->index >= n)
                return done(std::move(m));
            const ui::ProviderRow& row =
                rows[static_cast<std::size_t>(p->index)];

            // Two removable things share the Del key:
            //  (a) a SAVED CUSTOM HOST — remove the host entirely.
            //  (b) a PRESET with a SAVED API KEY (e.g. openrouter) — the
            //      preset itself is built-in and stays, but Del clears its
            //      saved key (a sign-out), which is what "delete openrouter"
            //      means in practice. Presets with no saved key, ACP agents,
            //      and the "Custom host…" sentinel are not removable.
            std::string target;          // settings key to erase
            bool is_custom_host = false;
            if (const std::string* spec = row.custom_host()) {
                target = *spec;
                is_custom_host = true;
            } else if (const auto* pr = row.preset()) {
                auto s = deps().load_settings();
                const std::string pid{pr->id};
                if (s.provider_keys.count(pid)) target = pid;  // has a saved key
            }
            if (target.empty()) {        // nothing removable on this row
                p->confirm_remove.clear();
                return done(std::move(m));
            }
            // Two-press: first press ARMS (marks confirm_remove on this
            // target), second press on the SAME row COMMITS. Mirrors
            // ThreadListDelete / AccountRemove.
            if (p->confirm_remove != target) {
                p->confirm_remove = target;
                return done(std::move(m));
            }
            const std::string removed = target;
            {
                auto s = deps().load_settings();
                s.provider_keys.erase(removed);
                if (is_custom_host) s.provider_models.erase(removed);
                deps().save_settings(s);
            }
            // Also drop any stored account credentials for a preset sign-out
            // (custom hosts keep everything in provider_keys, handled above).
            if (!is_custom_host)
                for (const auto& acc : auth::accounts::list_for(removed))
                    auth::accounts::remove(removed, acc.label);
            p->confirm_remove.clear();
            // Signing out of the ACTIVE provider must not leave the session
            // pointing at a dead credential — the palette SignOut zeroes the
            // live header and prompts re-auth, and this path is the same
            // action spelled differently. Zero the header (the next turn
            // must not reuse the erased key) and say what to do next; the
            // picker stays open so the user can pick another provider (or
            // re-enter this one to sign back in).
            const bool was_active = (removed == active_provider_id());
            if (was_active) app::update_auth(auth::AuthHeader{});
            // Rebuild the row list so a removed custom host is gone; clamp.
            auto s2 = deps().load_settings();
            const auto fresh = ui::build_provider_rows(
                provider::saved_custom_hosts(s2.provider_keys), p->query);
            if (!fresh.empty() && p->index >= static_cast<int>(fresh.size()))
                p->index = static_cast<int>(fresh.size()) - 1;
            auto toast = set_status_toast(m,
                was_active
                    ? "signed out of " + removed
                          + " (active) — pick a provider to continue"
                    : (is_custom_host ? "removed custom host: "
                                      : "signed out of ") + removed);
            return {std::move(m), std::move(toast)};
        },
        [&](ProvidersSelect) -> Step {
            // Capture the cursor before closing: assigning Closed destroys the
            // OpenAt alternative, so keeping a pointer into it would dangle.
            const auto* p = m.ui.panel.get<pn::Providers>();
            const int selected = p ? p->index : -1;
            m.ui.panel.close<pn::Providers>();
            if (selected < 0 || selected >= n) return done(std::move(m));
            const ui::ProviderRow& chosen = rows[static_cast<std::size_t>(selected)];

            // "Custom host…" sentinel: hand off to the free-text endpoint modal.
            if (chosen.is_new_custom_host()) {
                ui::login::CustomHostInput ch;
                ch.origin = ui::login::origin::Providers{};  // Esc = one step back
                m.ui.login = std::move(ch);
                return done(std::move(m));
            }

            // An external ACP agent row: agentty drives the agent subprocess,
            // which does its OWN auth — no key resolution here.
            if (const provider::AcpAgentSpec* agent = chosen.acp()) {
                return commit_provider_switch(std::move(m), agent->id,
                                              auth::AuthHeader{}, agent->id);
            }

            // A saved custom OpenAI-compatible host row: the spec string is the
            // key into Settings.provider_keys. Resolve the saved key and commit
            // directly — no re-entry, because the key is already on disk.
            if (const std::string* spec_ptr = chosen.custom_host()) {
                const std::string spec = *spec_ptr;
                // If this custom host is ALREADY the active provider, Enter
                // opens its accounts drill-down (a custom host can hold
                // multiple saved keys, switchable like the OAuth providers —
                // account_provider_id returns the spec for a custom OpenAI
                // endpoint). Esc from that list steps back to this picker.
                const auto& active = provider::active();
                const bool is_active =
                    active.kind == provider::Kind::OpenAI
                    && active.openai_endpoint.label == spec;
                if (is_active)
                    return agentty::app::update(std::move(m), Msg{OpenAccounts{}});

                // Not active — switch to it via the central resolver (the key
                // is already on disk; resolve reads provider_keys[spec]).
                auth::AuthHeader new_auth = provider::credentials::resolve(spec);
                return commit_provider_switch(std::move(m), spec,
                                              std::move(new_auth), spec);
            }

            const auto& preset = *chosen.preset();
            // Enter on an ACCOUNT-CAPABLE provider opens its account manager
            // DIRECTLY — UNIFORM across every provider that has accounts (OAuth:
            // Anthropic/ChatGPT/Copilot/Kimi; hosted API key: Mistral/Groq/…;
            // custom hosts), whether or not it's the active provider. No
            // switch-first (that popped the model picker); OpenAccounts targets
            // this provider by id and builds its list from a parsed Selection.
            // Only keyless local servers (add_method == None) skip straight to
            // the switch. Esc from the account list steps back to this picker.
            if (provider::credentials::add_method(preset.id)
                    != provider::credentials::AddMethod::None) {
                m.ui.panel.close<pn::Providers>();
                return agentty::app::update(
                    std::move(m), Msg{OpenAccounts{std::string{preset.id}}});
            }

            const std::string spec{preset.id};

            // Resolve the new backend's credentials BEFORE committing through
            // the ONE central resolver, so we can refuse a switch that would
            // land in a silently-broken state — same credential model as every
            // other switch site.
            auth::AuthHeader new_auth = provider::credentials::resolve(spec);

            // A hosted (non-local) OpenAI-family provider with no resolvable
            // key can't stream. Open the in-app key-entry modal for THIS
            // provider instead of a dead-end error; login_submit commits the
            // switch once the key lands.
            const bool needs_key =
                preset.kind() == provider::Kind::OpenAI && !preset.is_local
                && preset.auth != provider::AuthStyle::None;
            if (needs_key && auth::is_empty(new_auth)) {
                m.ui.login = ui::login::ApiKeyInput{
                    .key_input      = {},
                    .cursor         = 0,
                    .provider       = spec,
                    .provider_label = std::string{preset.label},
                    .origin         = ui::login::origin::Providers{},
                };
                return done(std::move(m));
            }

            // OAuth providers: if not signed in, launch the login flow rather
            // than switching to a backend that would fail on the first turn.
            //
            // Routed off registry capabilities + the auth vault, not provider
            // names. `oauth_native` picks the bespoke ChatGPT/Codex flow;
            // `device_login` picks the shared device launcher; vault::signed_in
            // answers "has this provider a live token" for every OAuth row
            // through one table. A new OAuth provider needs no edit here.
            if (preset.token_in_transport
                && !auth::vault::signed_in(std::string{spec})) {
                const auto attempt_id = cmd::next_codex_login_attempt_id();
                auto cancel = std::make_shared<std::atomic_bool>(false);
                if (preset.oauth_native) {
                    m.ui.login = ui::login::ChatGptWaiting{
                        .attempt_id = attempt_id,
                        .cancel = cancel,
                        .device_auth =
                            provider::chatgpt::codex_device_auth_preferred(),
                    };
                    return {std::move(m),
                            cmd::codex_login_async(attempt_id, std::move(cancel))};
                }
                m.ui.login = ui::login::DeviceWaiting{
                    .provider = std::string{spec},
                    .provider_label = std::string{preset.label},
                    .attempt_id = attempt_id, .cancel = cancel,
                };
                return {std::move(m),
                        cmd::device_login_async(std::string{spec},
                                                std::string{preset.label},
                                                attempt_id, std::move(cancel))};
            }

            // Every entry point funnels the actual switch through the ONE
            // helper so provider + per-provider model recall + effort clamp +
            // auth swap + refetch can never drift between call sites.
            return commit_provider_switch(std::move(m), spec, std::move(new_auth),
                                          std::string{preset.label});
        },
    }, pm);
}

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

Step thread_list_update(Model m, msg::ThreadListMsg tm) {
    return std::visit(overload{
        [&](OpenThreadList) -> Step {
            // Refresh in the background if no load is in flight — the
            // walk + parse is too slow (seconds, with hundreds of
            // multi-MB thread files) to do synchronously here. The
            // picker opens immediately against the cached list; new
            // entries fade in when ThreadsLoaded lands.
            Cmd<Msg> cmd = Cmd<Msg>::none();
            if (!m.s.threads_loading) {
                m.s.threads_loading = true;
                cmd = cmd::load_threads_async();
            }
            // Open AT the current thread, not row 0 — the user's mental
            // anchor is "where am I", and cycling from there (↑ newer /
            // ↓ older) mirrors the Alt+←/→ quick-cycle order.
            int at = 0;
            for (int i = 0; i < static_cast<int>(m.d.threads.size()); ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    at = i;
                    break;
                }
            m.ui.panel = pn::ThreadList{{at}};
            return {std::move(m), std::move(cmd)};
        },
        [&](CloseThreadList) -> Step {
            ascend(m);   // Esc: back to whatever opened this, or close
            return done(std::move(m));
        },
        [&](ThreadListMove& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return done(std::move(m));
            p->confirm_remove.clear();   // moving disarms a pending `d`
            int sz = static_cast<int>(m.d.threads.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListJump& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return done(std::move(m));
            p->confirm_remove.clear();   // jumping disarms a pending `d`
            int sz = static_cast<int>(m.d.threads.size());
            using W = ThreadListJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = sz - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(sz - 1, p->index + kPage); break;
            }
            return done(std::move(m));
        },
        // ── Model swap: commit overflow before swapping ──────────────
        //
        // ThreadListSelect and NewThread replace m.d.current wholesale.
        // Before the swap we dispatch Cmd::commit_scrollback_overflow()
        // — NOT force_redraw (see history below).
        //
        // Why commit-overflow is required:
        //   maya's inline diff treats rows [0, prev_rows - term_h) as
        //   committed scrollback ("updatable_start" in serialize.cpp).
        //   When the old thread overflowed (prev_rows > term_h) those
        //   rows are skipped by the diff scan and per-row emit. After
        //   a wholesale model swap the new thread's canvas rows at
        //   those Y positions are entirely different content — but
        //   the diff still considers them "scrollback, untouchable"
        //   and never emits them. Result: visible seam mid-viewport
        //   where the wire still holds old-thread bytes against the
        //   new-thread canvas, manifesting as two unrelated text
        //   fragments on adjacent rows.
        //
        //   commit_scrollback_overflow() calls into maya's
        //   commit_inline_overflow which advances prev_cells by
        //   max(0, prev_rows - term_h) rows. After it runs,
        //   prev_rows ≤ term_h, updatable_start drops to 0, and the
        //   diff scans the full common range — every visible row
        //   gets correctly emitted against the new thread.
        //
        //   The rows that scroll out of prev_cells are bytes the
        //   terminal already committed to its native scrollback
        //   anyway (they were emitted via bottom-edge \r\n's during
        //   streaming). commit just acknowledges that fact — zero
        //   wire effect.
        //
        // Why NOT force_redraw:
        //   Cmd::force_redraw demotes Synced → Stale, routing the
        //   next render through compose case (B). Case (B)'s
        //   scroll-to-fit branch (scroll_n > 0) emits \n at the
        //   viewport bottom when the new frame is taller than the
        //   old cursor's offset from viewport top — each \n there
        //   scrolls a row of whatever was on screen (old thread
        //   tail + host shell history above it) up into
        //   terminal-owned scrollback, permanently. History: commit
        //   8becb88 did exactly that and reverted in 0b24148.
        [&](ThreadListSelect) -> Step {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            Cmd<Msg> cmd = Cmd<Msg>::none();
            if (p) p->confirm_remove.clear();   // selecting disarms a pending `d`
            if (p && !m.d.threads.empty() && !m.s.thread_loading) {
                // Re-clamp: p->index can be stale if an async refresh shrank
                // the list since the last navigation (see ThreadListDelete).
                p->index = std::clamp(p->index, 0,
                                      static_cast<int>(m.d.threads.size()) - 1);
                const Thread& meta = m.d.threads[static_cast<std::size_t>(p->index)];
                // Same-thread re-select — closing the picker is the
                // only useful action. No async load: would just
                // reparse the same bytes and flash.
                if (meta.id == m.d.current.id) {
                    m.ui.panel.close<pn::ThreadList>();
                    return done(std::move(m));
                }
                m.s.thread_loading = true;
                // Warm the socket now so the first turn in the thread the user
                // is switching INTO doesn't re-pay the handshake (the pool's
                // idle TTL has usually evicted it during composer breathing
                // room). Non-blocking; no-op if already warm.
                provider::prewarm_active_provider();
                cmd = cmd::load_thread_async(meta.id);
            }
            m.ui.panel.close<pn::ThreadList>();
            return {std::move(m), std::move(cmd)};
        },
        [&](ThreadListDelete) -> Step {
            // `d` / `D` in the thread picker — two-press delete with
            // confirm_remove, mirroring SettingsListRemove / AccountRemove.
            // First press on a row marks it pending (⚠ badge in the view);
            // second press on the SAME row commits via deps().delete_thread().
            // Any move/jump/select/new/close disarms the pending state.
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || m.d.threads.empty()) return done(std::move(m));
            // Bounds-guard the cursor before indexing. Navigation handlers
            // clamp p->index on every move, but the thread list can be
            // mutated out from under the picker by an async refresh (or a
            // prior delete) that shrinks it, leaving a stale index that
            // points past the new end. Reading m.d.threads[idx] then is an
            // out-of-bounds access; the erase(begin()+idx) below would
            // compound it. Re-clamp into range instead of trusting p->index.
            const int sz_now = static_cast<int>(m.d.threads.size());
            const int idx = std::clamp(p->index, 0, sz_now - 1);
            p->index = idx;
            const Thread& target = m.d.threads[static_cast<std::size_t>(idx)];
            // Use the thread id as the confirm key — stable across title edits.
            const std::string key = target.id.value;
            if (p->confirm_remove != key) {
                p->confirm_remove = key;
                return done(std::move(m));
            }
            // Second press — commit. Snapshot everything we need OUT of the
            // vector element BEFORE erase(): the erase invalidates `target`,
            // so reading target.title / target.id afterward is a
            // use-after-free. Copy them here while the reference is live.
            const ThreadId  target_id = target.id;
            const bool      was_current = (target_id == m.d.current.id);
            const std::string label =
                target.title.empty() ? "(untitled)" : target.title;

            p->confirm_remove.clear();
            deps().delete_thread(target_id);
            m.d.threads.erase(m.d.threads.begin() + idx);
            // Clamp the cursor so it stays valid after removal.
            const int sz = static_cast<int>(m.d.threads.size());
            if (sz == 0) {
                p->index = 0;
            } else if (p->index >= sz) {
                p->index = sz - 1;
            }
            std::string msg = "deleted \"" + label + "\"";
            if (was_current) msg += " \xe2\x80\x94 started a new thread";
            auto toast = set_status_toast(m, std::move(msg));
            // Deleting the ACTIVE thread leaves m.d.current pointing at a
            // thread whose file no longer exists — swap to a fresh empty
            // thread through the SAME core NewThread uses. That single code
            // path is what guarantees the phase reset + kernel release (so a
            // mid-stream delete can't leave the wire running against a dead
            // thread), the modal/skill/cache teardown, and the reset_inline
            // that wipes the deleted thread's rendered turns off-screen.
            if (was_current) {
                auto reset = reset_to_fresh_thread(m);
                return {std::move(m),
                        Cmd<Msg>::batch(cmd::load_threads_async(),
                                        std::move(reset), std::move(toast))};
            }
            return {std::move(m), std::move(toast)};
        },
        [&](ThreadCycle& e) -> Step {
            // Alt+←/→ — jump to the adjacent thread without the picker.
            // Recency order (same as ^J): index 0 = newest; +1 = older,
            // -1 = newer, wrapping at both ends. Gated on an idle
            // session — swapping m.d.current under an active stream
            // would strand the in-flight ctx's writes.
            if (m.s.active()) {
                auto cmd = set_status_toast(m,
                    "wait for the reply to finish before switching threads");
                return {std::move(m), std::move(cmd)};
            }
            if (m.s.thread_loading) return done(std::move(m));
            const int sz = static_cast<int>(m.d.threads.size());
            if (sz == 0) {
                // History not loaded yet (or genuinely empty) — kick a
                // refresh so the NEXT press works, and say so.
                Cmd<Msg> cmd = Cmd<Msg>::none();
                if (!m.s.threads_loading) {
                    m.s.threads_loading = true;
                    cmd = cmd::load_threads_async();
                }
                auto toast = set_status_toast(m, "no other threads yet");
                return {std::move(m),
                        Cmd<Msg>::batch(std::move(cmd), std::move(toast))};
            }
            // Locate the current thread in the recency list. A fresh
            // unsaved thread isn't in it — treat "newest" as the anchor
            // so the first press lands on the most recent saved thread.
            int cur = -1;
            for (int i = 0; i < sz; ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    cur = i;
                    break;
                }
            int target;
            if (cur < 0) {
                target = (e.delta >= 0) ? 0 : sz - 1;
            } else {
                if (sz == 1) {
                    auto toast = set_status_toast(m, "only one thread");
                    return {std::move(m), std::move(toast)};
                }
                target = ((cur + e.delta) % sz + sz) % sz;
            }
            const Thread& meta = m.d.threads[static_cast<std::size_t>(target)];
            if (meta.id == m.d.current.id) return done(std::move(m));
            // Preserve the thread being left — same courtesy NewThread
            // extends. finalize_turn saves per turn, but a title edit or
            // an un-persisted tail shouldn't be lost to a quick cycle.
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            m.s.thread_loading = true;
            // Warm the socket for the switched-into thread's first turn.
            provider::prewarm_active_provider();
            // "thread k/N · title" — the positional readout that makes
            // repeated Alt+←/→ presses feel like flipping through a
            // deck rather than teleporting blind. Survives the swap
            // because ThreadLoaded doesn't touch m.s.status.
            auto toast = set_status_toast(m,
                "thread " + std::to_string(target + 1) + "/"
                    + std::to_string(sz) + " \xc2\xb7 "
                    + (meta.title.empty() ? "(untitled)" : meta.title));
            return {std::move(m),
                    Cmd<Msg>::batch(cmd::load_thread_async(meta.id),
                                    std::move(toast))};
        },
        [&](NewThread) -> Step {
            // Persist the outgoing thread before we drop it (delete's
            // active-row path does the opposite — it just removed the
            // thread, so it must NOT save). The shared reset below owns
            // everything after this policy decision.
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            auto reset = reset_to_fresh_thread(m);
            return {std::move(m), std::move(reset)};
        },
        [&](ThreadsLoaded& e) -> Step {
            m.d.threads = std::move(e.threads);
            m.s.threads_loading = false;
            // If the thread picker is open, its cursor may now point past the
            // end of the freshly-loaded (possibly shorter) list. Re-clamp so
            // the view and every ThreadList* handler index safely.
            if (auto* p = m.ui.panel.get<pn::ThreadList>()) {
                const int sz = static_cast<int>(m.d.threads.size());
                p->index = sz > 0 ? std::clamp(p->index, 0, sz - 1) : 0;
            }
            return done(std::move(m));
        },
        [&](ThreadLoaded& e) -> Step {
            // Result of the async single-thread load kicked off by
            // ThreadListSelect. Empty Thread (default ThreadId) means
            // the disk read or parse failed; just clear the spinner
            // and leave the current thread in place.
            m.s.thread_loading = false;
            if (e.thread.id.value.empty()) return done(std::move(m));
            // Old thread's skill activations leave context with it.
            tools::skills::reset_activations();
            // Smart-Mode per-thread routing state belongs to the departing
            // thread too — same reset as reset_to_fresh_thread (momentum,
            // cascade bias, outcome-feedback signature). Without it the
            // loaded thread's first turn inherits the OLD thread's tier
            // momentum and its first follow-up trains the old signature.
            m.s.smart_turn_complexity  = smart::Complexity::Standard;
            m.s.smart_effort_bias      = 0;
            // Optional timing probe. AGENTTY_LOAD_PROF=1 keeps surfacing
            // the synchronous portion of the load (rehydrate +
            // release_to_kernel) that still lives on the UI thread.
            const bool prof = []{
                static const bool on = [] {
                    const char* e = std::getenv("AGENTTY_LOAD_PROF");
                    return e && *e && *e != '0';
                }();
                return on;
            }();
            std::FILE* prof_out = nullptr;
            if (prof) prof_out = std::fopen("/tmp/agentty-load-prof.log", "a");
            auto stamp = [&](const char* tag, auto t0) {
                if (!prof_out) return;
                auto dt = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::fprintf(prof_out, "[load-async] %s: %.2f ms\n", tag, dt);
                std::fflush(prof_out);
            };
            m.d.current = std::move(e.thread);
            // Drop the whole render cache — same rationale as NewThread:
            // the entries belong to the thread being left, which won't
            // freeze again. The loaded thread rebuilds its frozen prefix
            // via rehydrate_frozen below and repopulates the cache lazily.
            m.ui.view_cache.clear();
            // Wipe the composer draft — same rationale as NewThread: a
            // pasted-but-unsent image / chip / queued message belongs to
            // the thread being left, and the leftover image Attachment has
            // empty bytes (drained into a prior Message), which serializes
            // an empty image block and 400s the next submit.
            reset_composer_draft(m.ui.composer);
            auto t1 = std::chrono::steady_clock::now();
            rehydrate_frozen(m);
            stamp("rehydrate_frozen", t1);
            // Frozen scrollback was just built from cold; the very
            // first render() would otherwise pay full layout+paint
            // over every frozen Turn. Flip the warmup flag so maya's
            // run loop pre-warms the component cache before the
            // wire-bound render — see Program::needs_warmup hook.
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            // Arm the one-shot post-paint trim: the rehydrate budget used
            // ESTIMATED heights; the first paint records real ones into the
            // ledger, and the Tick arm re-trims against those. The Tick
            // subscription gates on this flag (subscribe.cpp) until it fires.
            m.ui.pending_rehydrate_trim = !m.ui.frozen.empty();
            auto t2 = std::chrono::steady_clock::now();
            release_to_kernel();
            stamp("release_to_kernel", t2);
            if (prof_out) {
                const auto _ts = maya::platform::query_terminal_size(
                    maya::platform::stdout_handle());
                std::fprintf(prof_out,
                    "[load-async] msgs=%zu frozen=%zu frozen_rows=%zu "
                    "frozen_through=%zu term_h=%d\n",
                    m.d.current.messages.size(),
                    m.ui.frozen.size(),
                    m.ui.frozen.row_total(),
                    m.ui.frozen_through,
                    _ts.height.value);
                std::fflush(prof_out);
                std::fclose(prof_out);
            }
            // Wholesale model swap into the loaded thread. Same
            // rationale as NewThread above: the previous thread's
            // overflow rows are committed to native scrollback and only
            // reset_inline (which emits `\x1b[2J\x1b[3J\x1b[H`) can
            // erase them. Without it the previous thread's tail turns
            // are visible above the rehydrated thread's first turn.
            //
            // Per maya/app/app.hpp reset_inline() docs: this is the
            // sanctioned recovery for thread switch / new thread. The
            // `\x1b[3J` cost (wipes the user's pre-agentty shell
            // scrollback) is acceptable because the user explicitly
            // asked for the content swap (picker select).
            return {std::move(m), Cmd<Msg>::reset_inline()};
        },
    }, tm);
}

} // namespace agentty::app::detail
