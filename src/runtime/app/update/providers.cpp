// providers.cpp — the provider picker's reducer: selecting a row
// live-switches the active backend (no restart). Split from panels.cpp;
// self-contained (none of the fused/MRU machinery).

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/auth/vault.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/runtime/login.hpp"
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
using maya::Cmd;
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

} // namespace agentty::app::detail
