// In-app login modal reducer arms. Lives outside update.cpp because the
// OAuth flow drags in `auth/auth.hpp` + `cmd_factory.hpp` worth of
// dependencies that the rest of update.cpp doesn't need.
//
// The modal is a closed sum (`ui::login::State`): Closed | Picking |
// OAuthCode | OAuthExchanging | ApiKeyInput | Failed. Every arm here
// either dispatches via `std::visit` into the active alternative or
// short-circuits when the modal isn't in a state that accepts the Msg —
// the typed state machine is what guarantees we never read OAuthCode
// fields from an ApiKeyInput modal, etc.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"   // app::update (LoginBack re-entry)

#include <chrono>
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/io/clipboard.hpp"
#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/auth/vault.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/subagent.hpp"

namespace agentty::app::detail {

using maya::Cmd;
using maya::overload;
namespace login = agentty::ui::login;

namespace {

// Persist + live-install credentials, then close the modal. Single
// point so OAuth and ApiKey paths can't drift — both end here.
void install_and_close(Model& m, auth::Credentials creds,
                       bool as_new_account = false) {
    auth::save_credentials(creds);
    agentty::app::update_auth(auth::make_auth_header(creds));

    // Capture this login as a named account so it's switchable in-app.
    //
    // OAuth / API-key credentials carry NO stable per-account identity
    // (the tokens rotate; there's no email/subject on the wire), so the
    // derived label IS the account's identity here. A re-login — an
    // expired-token refresh, or just re-testing — therefore produces the
    // SAME derived label ("OAuth login", "API key") and must UPDATE the
    // existing slot in place, not spawn "OAuth login 2/3/…". The old
    // suffix-until-unique loop turned every re-auth into a fresh junk
    // account and could leave the active-label pointer aimed at a slot
    // whose live credential no longer matched — the reported
    // accumulation + wedge.
    //
    // snapshot_active(label) upserts by (provider, label): reusing the
    // derived label overwrites the same account's stored secret and
    // re-marks it active, which is exactly right for a re-login. A
    // DELIBERATE second account on the same provider is created through
    // the account manager's "+ Add another account…" flow, which the
    // user labels explicitly — that path already carries its own label
    // and never lands here with a colliding one.
    {
        namespace acc = agentty::auth::accounts;
        const std::string provider = "anthropic";
        std::string base = acc::derive_current_label(provider);
        if (base.empty()) base = "account";
        std::string label = base;
        if (as_new_account) {
            // DELIBERATE add of a separate account whose derived label
            // collides (two "OAuth login"s — Anthropic OAuth carries no
            // distinguishing identity). Suffix until unique so both slots
            // survive and the user can flip between them.
            for (int n = 2; acc::get(provider, label).has_value() && n < 100; ++n)
                label = base + " " + std::to_string(n);
        }
        // Plain sign-in (as_new_account == false): reuse the derived-label
        // slot. snapshot_active upserts by (provider, label), so a re-auth
        // overwrites the same account's secret in place instead of spawning
        // "OAuth login 2/3/…" — the reported accumulation + wedge.
        acc::snapshot_active(provider, label);
    }

    m.ui.login = login::Closed{};
    m.s.status = "logged in";
    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{4};
}

} // namespace

// Start a native device-flow OAuth login for `provider` (registry id) with the
// given display `label`. Sets the DeviceWaiting modal state and returns the
// worker Cmd. One place so the picker, the login menu, and the account manager
// all launch device login identically.
Step launch_device_login(Model m, std::string provider, std::string label) {
    const auto attempt_id = cmd::next_codex_login_attempt_id();
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m.ui.login = login::DeviceWaiting{
        .provider = provider,
        .provider_label = label,
        .attempt_id = attempt_id,
        .cancel = cancel,
    };
    return {std::move(m),
            cmd::device_login_async(std::move(provider), std::move(label),
                                    attempt_id, std::move(cancel))};
}

Step open_login(Model m) {
    m.ui.login = login::Picking{};
    return done(std::move(m));
}

Step close_login(Model m) {
    if (auto* waiting = std::get_if<login::ChatGptWaiting>(&m.ui.login);
        waiting && waiting->cancel) {
        waiting->cancel->store(true, std::memory_order_release);
    }
    if (auto* waiting = std::get_if<login::DeviceWaiting>(&m.ui.login);
        waiting && waiting->cancel) {
        waiting->cancel->store(true, std::memory_order_release);
    }
    m.ui.login = login::Closed{};
    return done(std::move(m));
}

// Esc from a login sub-modal: pop ONE level using the sub-state's recorded
// Origin — a first-class parent frame, so each pop rebuilds the parent with
// its FULL context (which provider's accounts, which typed host). One visit,
// no side-channels. States with no parent (Nowhere) or async-waiting states
// (Esc = cancel) fall through to close_login.
Step login_back(Model m) {
    // Reconstruct the recorded parent. `pop` OWNS the origin, so it can move a
    // provider/spec out of it into the rebuilt state.
    auto pop = [&](login::Origin origin) -> Step {
        return std::visit([&](auto&& o) -> Step {
            using T = std::decay_t<decltype(o)>;
            namespace og = login::origin;
            if constexpr (std::is_same_v<T, og::Providers>) {
                m.ui.login = login::Closed{};
                return agentty::app::update(std::move(m), Msg{OpenProviders{}});
            } else if constexpr (std::is_same_v<T, og::Models>) {
                m.ui.login = login::Closed{};
                return agentty::app::update(std::move(m), Msg{OpenModels{}});
            } else if constexpr (std::is_same_v<T, og::Accounts>) {
                // Return to the SPECIFIC provider's account list — the frame
                // carries the provider, so this can't drift to the active one.
                m.ui.login = login::Closed{};
                return agentty::app::update(
                    std::move(m), Msg{OpenAccounts{std::move(o.provider)}});
            } else if constexpr (std::is_same_v<T, og::Method>) {
                login::Picking p;
                p.provider = std::move(o.provider);
                // A provider-scoped method menu is only ever entered from that
                // provider's account list, so restore that as ITS parent — the
                // next Esc keeps walking the same stack instead of closing.
                if (!p.provider.empty())
                    p.origin = login::origin::Accounts{p.provider};
                m.ui.login = std::move(p);
                return done(std::move(m));
            } else if constexpr (std::is_same_v<T, og::HostInput>) {
                // Restore the typed spec so backing out of the key/probe prompt
                // doesn't discard the host the user just entered.
                login::CustomHostInput ch;
                ch.host_input = std::move(o.spec);
                ch.cursor     = static_cast<int>(ch.host_input.size());
                ch.origin     = login::origin::Providers{};
                m.ui.login    = std::move(ch);
                return done(std::move(m));
            } else {   // og::Nowhere
                return close_login(std::move(m));
            }
        }, std::move(origin));
    };

    if (auto* ch = std::get_if<login::CustomHostInput>(&m.ui.login))
        return pop(std::move(ch->origin));
    if (auto* hp = std::get_if<login::HostProbing>(&m.ui.login)) {
        // Esc during a probe: abandon it (the attempt id makes any late
        // HostProbed a no-op) and put the typed spec back in the input,
        // keeping the probe's own parent frame for the NEXT Esc.
        auto spec   = std::move(hp->spec);
        auto origin = std::move(hp->origin);
        login::CustomHostInput ch;
        ch.host_input = std::move(spec);
        ch.cursor     = static_cast<int>(ch.host_input.size());
        ch.origin     = std::move(origin);
        m.ui.login    = std::move(ch);
        return done(std::move(m));
    }
    if (auto* api = std::get_if<login::ApiKeyInput>(&m.ui.login))
        return pop(std::move(api->origin));
    if (auto* p = std::get_if<login::Picking>(&m.ui.login))
        return pop(std::move(p->origin));
    // The account list is always reached FROM the provider picker (Enter on a
    // provider row), so Esc steps BACK there — keeping accounts → providers →
    // chat hierarchical.
    if (std::holds_alternative<login::AccountList>(m.ui.login))
        return pop(login::origin::Providers{});
    // Every other sub-state (OAuth waits, failures): Esc keeps its original
    // meaning — cancel/close outright.
    return close_login(std::move(m));
}

// Async probe result for a keyless custom host. Success: persist the host
// (empty key — the picker's saved-row store) and commit the switch with the
// DETECTED dialect. Failure: back to the input with the spec restored and
// the reason in the status line. Stale results (Esc'd / resubmitted probes)
// are dropped by attempt-id mismatch.
Step host_probed(Model m, HostProbed r) {
    auto* hp = std::get_if<login::HostProbing>(&m.ui.login);
    if (!hp || hp->attempt_id != r.attempt_id) return done(std::move(m));
    auto origin = std::move(hp->origin);
    if (!r.ok) {
        login::CustomHostInput ch;
        ch.host_input = std::move(r.spec);
        ch.cursor     = static_cast<int>(ch.host_input.size());
        ch.origin     = std::move(origin);
        m.ui.login    = std::move(ch);
        auto toast = set_status_toast(m, "host check failed: " + r.error,
                                      std::chrono::seconds{6});
        return {std::move(m), std::move(toast)};
    }

    const std::string spec = std::move(r.spec);
    // PERSIST the keyless host (empty key) so it has a picker row next
    // session — saved_custom_hosts() derives rows from provider_keys.
    {
        auto settings = deps().load_settings();
        if (settings.provider_keys.find(spec) == settings.provider_keys.end()) {
            settings.provider_keys[spec] = "";
            deps().save_settings(settings);
        }
    }
    auth::AuthHeader new_auth = provider::credentials::resolve(spec);
    m.ui.login = login::Closed{};
    auto step = commit_provider_switch(std::move(m), spec, std::move(new_auth),
                                       provider::provider_display_name(
                                           provider::parse_selection(spec)));
    // Enrich the switch toast with what the probe FOUND — the "it just
    // works" moment: dialect, model count, latency.
    auto found = set_status_toast(step.first,
        std::string{"\xe2\x9c\x93 "} + std::to_string(r.model_count)
        + (r.model_count == 1 ? " model" : " models") + " \xc2\xb7 "
        + (r.native_api ? "ollama native" : "openai-compatible") + " \xc2\xb7 "
        + std::to_string(r.latency_ms) + "ms \xc2\xb7 " + r.models_path,
        std::chrono::seconds{5});
    return {std::move(step.first),
            maya::Cmd<Msg>::batch(std::move(step.second), std::move(found))};
}

// Canonical account/registry id for a provider selection — mirrors the
// account_provider_id() helper below but is self-contained so sign_out
// (defined above it) needs no forward reference across linkage boundaries.
static std::string signout_provider_id(const provider::Selection& sel) {
    // The carried registry row IS the identity. Only an unrecognised custom
    // host has no row, and it keys its account on the endpoint label.
    if (const auto id = sel.provider_id(); !id.empty()) return std::string{id};
    if (sel.kind == provider::Kind::OpenAI) return sel.openai_endpoint.label;
    return {};
}

Step sign_out(Model m) {
    // Clear the ACTIVE provider's credentials, so "Sign out" targets whatever
    // the user is currently signed in to. WHICH store that is (Anthropic's
    // credentials.json, an OAuth token file, a provider_keys entry) is the
    // vault's question — one descriptor per provider, no per-provider ladder
    // here (see auth/vault.hpp).
    const auto& sel = provider::active();
    const std::string pid = signout_provider_id(sel);
    std::string what = pid.empty() ? std::string{"credentials"}
                                   : provider::provider_display_name(sel);
    // SettingsKey stores (hosted keys + custom hosts) clear through the
    // INJECTED settings door so the write is visible to tests' in-memory
    // stubs and rides the same persistence path as every reducer write.
    // File-backed stores (Anthropic / OAuth token files) are the vault's.
    if (!pid.empty()) {
        if (auth::vault::of(pid).kind == auth::vault::Kind::SettingsKey) {
            auto settings = deps().load_settings();
            if (settings.provider_keys.erase(pid) > 0)
                deps().save_settings(settings);
        } else {
            auth::vault::sign_out(pid);
        }
    }

    if (sel.kind == provider::Kind::Anthropic) {
        // NOTE: nothing to re-arm here any more. The 1M entitlement fact is
        // keyed by (provider, ACCOUNT, model) — see domain/entitlement.hpp —
        // so the account being dropped never speaks for the next one. This
        // used to clear a global bool, which was lossy in the other
        // direction: ping-ponging between an entitled and an unentitled
        // account re-discovered the same 400 on every hop.
        //
        // Genuine sign-OUT (not a switch) is handled where the account is
        // removed, via entitlement::forget_account.
    }

    // Zero the live auth header so the very next turn can't reuse a
    // now-revoked credential.
    agentty::app::update_auth(auth::AuthHeader{});

    // If ANOTHER provider is still authed, fall back to it rather than
    // dumping the user at a sign-in modal — signing out of one of several
    // accounts should keep you working, not strand you. Prefer the most
    // recently used still-authed provider (MRU order), else the first authed
    // registry row. Only when nothing is left do we open the sign-in modal.
    {
        const std::string just_left = signout_provider_id(sel);
        auto settings = deps().load_settings();
        // Only fall back to a provider with a REAL credential (saved or env) —
        // never silently hop onto an always-on local backend (Ollama) the
        // user never chose. That would be a surprising "I signed out but I'm
        // suddenly on some local model" jump.
        auto is_usable = [&](std::string_view pid) {
            if (pid == just_left || pid.empty()) return false;
            const auto* p = provider::preset_for(pid);
            if (!p) return true;   // saved custom host (has a provider_keys entry)
            const auto src = provider::auth_source(*p, settings);
            return src == provider::AuthSource::Saved
                || src == provider::AuthSource::Env;
        };
        std::string fallback;
        for (const auto& ref : m.d.recent_models)   // MRU, newest first
            if (is_usable(ref.provider_id)) { fallback = ref.provider_id; break; }
        if (fallback.empty())
            for (const auto& p : provider::providers())
                if (is_usable(p.id)) { fallback = std::string{p.id}; break; }
        if (!fallback.empty()) {
            m.ui.login = login::Closed{};
            m.s.status = "signed out of " + what + " \xe2\x80\x94 switched to "
                       + provider::provider_display_name(
                             provider::parse_selection(fallback));
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds{5};
            auth::AuthHeader fb_auth = provider::credentials::resolve(fallback);
            return commit_provider_switch(
                std::move(m), fallback, std::move(fb_auth),
                provider::provider_display_name(
                    provider::parse_selection(fallback)));
        }
    }

    // Nothing else authed — drop the user straight into sign-in.
    m.ui.login = login::Picking{};
    m.s.status = "signed out of " + what + " \xe2\x80\x94 sign in to continue";
    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{5};
    return done(std::move(m));
}

namespace {

// Canonical registry id for the active provider's accounts. Anthropic,
// ChatGPT, and GitHub Copilot have file-backed credential stores the account
// layer can snapshot; OpenAI-family keys already switch per-endpoint via the
// picker.
std::string account_provider_id(const provider::Selection& sel) {
    if (const auto id = sel.provider_id(); !id.empty()) return std::string{id};
    // Any OpenAI-family endpoint — a hosted API-key PRESET (mistral/groq/…) OR
    // a user-added CUSTOM HOST — keys its account(s) on the endpoint label ==
    // the provider id under which its bearer key(s) live in provider_keys.
    // Both support multiple accounts through the same account manager now.
    if (sel.kind == provider::Kind::OpenAI) {
        const std::string label = sel.openai_endpoint.label;
        // Keyless local servers (ollama/llama.cpp) have no account to manage.
        // `is_local` now means exactly "runs on this machine", so the old
        // `&& !oauth_native` guard (which existed only because ChatGPT
        // mislabelled itself local) is gone.
        const auto* p = provider::preset_for(label);
        if (p && p->is_local) return {};
        return label;
    }
    return {};   // no account switching for this provider (local/ACP)
}

// Build the AccountList state for the active provider, auto-registering the
// current live login as "default" the first time so it appears as a row.
login::AccountList build_account_list(const provider::Selection& sel) {
    namespace acc = agentty::auth::accounts;
    login::AccountList al;
    al.provider       = account_provider_id(sel);
    al.provider_label = provider::provider_display_name(sel);
    if (al.provider.empty()) return al;

    auto saved = acc::list_for(al.provider);
    if (saved.empty()) {
        // Legacy single-login: capture whatever is signed in right now under
        // a derived name so the user has a switchable, removable row.
        std::string label = acc::derive_current_label(al.provider);
        if (!label.empty() && acc::snapshot_active(al.provider, label))
            saved = acc::list_for(al.provider);
    }
    const std::string active = acc::active_label(al.provider);
    for (auto& a : saved) {
        login::AccountRow row;
        row.provider = a.provider;
        row.label    = a.label;
        row.active   = (a.label == active);
        al.rows.push_back(std::move(row));
    }
    // Land the cursor on the active row so "open, hit enter" is a no-op.
    for (int i = 0; i < static_cast<int>(al.rows.size()); ++i)
        if (al.rows[static_cast<std::size_t>(i)].active) { al.cursor = i; break; }
    return al;
}

} // namespace

Step open_accounts(Model m, const std::string& provider_id = {}) {
    // Target the requested provider if given (Enter on a provider row), else
    // the active one. Building from a parsed Selection means we DON'T have to
    // switch to the provider first — no model-picker pop — the account list
    // shows immediately for whatever row the user pressed Enter on.
    const auto sel = provider_id.empty() ? provider::active()
                                         : provider::parse_selection(provider_id);
    auto al = build_account_list(sel);
    if (al.provider.empty()) {
        // Provider has no switchable accounts — fall back to the normal
        // sign-in / add-key flow rather than showing an empty list.
        m.ui.login = login::Picking{};
        return done(std::move(m));
    }
    m.ui.login = std::move(al);
    return done(std::move(m));
}

Step account_move(Model m, int delta) {
    if (auto* al = std::get_if<login::AccountList>(&m.ui.login)) {
        const int n = static_cast<int>(al->rows.size()) + 1;   // +1 add-new row
        if (n > 0) al->cursor = ((al->cursor + delta) % n + n) % n;
        al->confirm_remove.clear();
    }
    return done(std::move(m));
}

Step account_select(Model m) {
    auto* al = std::get_if<login::AccountList>(&m.ui.login);
    if (!al) return done(std::move(m));
    const int add_row = static_cast<int>(al->rows.size());

    // Trailing "+ Add another account…" row. Keep the continuation scoped
    // to the provider the user was managing: ChatGPT and Copilot each have
    // one native OAuth method, while Anthropic offers its API-key/OAuth choices.
    if (al->cursor >= add_row) {
        const std::string provider = al->provider;
        // UNIFORM add-account routing: the credential layer says HOW this
        // provider takes a new account (API key vs OAuth device vs none).
        switch (provider::credentials::add_method(provider)) {
        case provider::credentials::AddMethod::ApiKey:
            // Paste a key for this endpoint (hosted key preset OR custom host).
            // The ApiKeyInput submit snapshots the current key first, so this
            // ADDS rather than replaces.
            m.ui.login = login::ApiKeyInput{
                .provider       = provider,
                .provider_label = al->provider_label,
                // Frame carries THIS provider — Esc provably returns to ITS
                // account list, never the active provider's.
                .origin         = login::origin::Accounts{provider},
            };
            return done(std::move(m));
        case provider::credentials::AddMethod::None:
            // Local server — no account to add; stay put.
            return done(std::move(m));
        case provider::credentials::AddMethod::OAuthDevice:
            break;   // handled below (provider-specific launch)
        }
        // OAuth login, routed off REGISTRY CAPABILITIES rather than provider
        // names. `oauth_native` marks the bespoke ChatGPT/Codex flow (it
        // negotiates device-vs-browser at runtime); `device_login` marks the
        // providers that share the generic launcher; `method_menu` marks the
        // one that offers a choice of method. A new OAuth provider sets a
        // flag on its row and lands here with no edit to this function.
        const auto* prow = provider::preset_for(provider);
        if (prow && prow->oauth_native) {
            const auto attempt_id = cmd::next_codex_login_attempt_id();
            auto cancel = std::make_shared<std::atomic_bool>(false);
            m.ui.login = login::ChatGptWaiting{
                .attempt_id = attempt_id,
                .cancel = cancel,
                .device_auth = provider::chatgpt::codex_device_auth_preferred(),
            };
            return {std::move(m), cmd::codex_login_async(attempt_id, std::move(cancel))};
        }
        if (prow && prow->device_login)
            return launch_device_login(std::move(m), provider,
                                       std::string{prow->label});
        // Otherwise: the method menu (OAuth subscription vs API key).
        m.ui.login = login::Picking{
            .provider = provider,
            .origin   = login::origin::Accounts{provider}};
        return done(std::move(m));
    }

    const auto& row = al->rows[static_cast<std::size_t>(al->cursor)];
    if (row.active) {                         // already this account
        m.ui.login = login::Closed{};
        return done(std::move(m));
    }

    namespace acc = agentty::auth::accounts;
    const std::string provider = row.provider;
    const std::string label    = row.label;
    if (!acc::activate(provider, label)) {
        m.ui.login = login::Failed{"could not switch to \"" + label + "\""};
        return done(std::move(m));
    }

    // If the chosen account belongs to a provider that ISN'T currently active
    // (Enter on a non-active provider row → its account list), selecting the
    // account SWITCHES to that provider too — provider + account + auth +
    // model recall + refetch, all atomically — and returns straight to chat
    // (open_panel=false, so no model-picker pop). The recalled model is
    // pre-stashed so commit_provider_switch doesn't open the picker.
    {
        const std::string active_pid =
            provider::active().kind == provider::Kind::OpenAI
                ? provider::active().openai_endpoint.label
                : std::string{provider::default_provider_id()};
        // The active provider's id, for non-OpenAI kinds, is the registry
        // default — compare ids, not names. (The old form special-cased
        // "anthropic" because active_pid falls back to the default id when
        // the active kind isn't OpenAI; that IS the default id, so the
        // comparison already covers it.)
        const bool provider_changed = provider != active_pid;
        if (provider_changed) {
            m.ui.login = login::Closed{};
            const auto* p = provider::preset_for(provider);
            const std::string plabel = p ? std::string{p->label} : provider;
            std::string recalled = deps().load_settings().provider_models.count(provider)
                ? deps().load_settings().provider_models.at(provider) : std::string{};
            return commit_provider_switch(std::move(m), provider,
                                          provider::credentials::resolve(provider),
                                          plabel, recalled, /*open_panel=*/false);
        }
    }
    // Re-install the live auth header from the now-swapped active store.
    // If the account we're switching TO has an OAuth token that's expired or
    // about to lapse (the "switched to the other, long-idle account and it
    // doesn't work" bug), kick a background refresh the same way startup and
    // the composer do — install the on-disk header now, then let the async
    // refresh replace it via TokenRefreshed. Non-blocking: no UI-thread
    // network round trip, and submit_message queues sends while the refresh
    // is in flight (oauth_refresh_in_flight), so the first turn on the new
    // account can't fire with the stale bearer.
    maya::Cmd<Msg> refresh_cmd = maya::Cmd<Msg>::none();
    // UNIFORM: install the live header for the now-active account through the
    // central resolver. Anthropic/custom-host resolve a real header; the
    // oauth-native transports (ChatGPT/Copilot/Kimi) read their token from the
    // just-swapped store on the next turn, so resolve() returns empty and that
    // empty CLEARS the cached header — exactly the "force a fresh read" the old
    // per-provider branch did.
    agentty::app::update_auth(provider::credentials::resolve(provider));

    // Anthropic-SPECIFIC extras (legitimately not uniform): a long-idle OAuth
    // token may be stale — kick a background proactive refresh so the first
    // turn on the switched-to account can't fire with a lapsed bearer; and
    // re-arm 1M-context discovery (the block was learned for the PREVIOUS
    // account; this one may be entitled).
    if (const auto* prow = provider::preset_for(provider);
        prow && prow->oauth_proactive_refresh) {
        // Guard against a concurrent kick (composer's 30s probe / init):
        // two refresh workers would exchange back-to-back — harmless since
        // the file lock serializes them and the second re-reads the freshest
        // token, but wasteful and it double-toggles the in-flight flag.
        if (auto tok = auth::oauth_proactive_refresh_token();
            tok && !m.s.oauth_refresh_in_flight) {
            m.s.oauth_refresh_in_flight = true;
            refresh_cmd = cmd::refresh_oauth(std::move(*tok));
        }
        // No entitlement re-arm here: facts are keyed by (provider,
        // ACCOUNT, model), so the account we just left cannot speak for the
        // one we just entered — and, unlike the old global-bool reset, what
        // we learned about the OUTGOING account survives for when the user
        // switches back. See domain/entitlement.hpp.
    }
    const std::string provider_label = al->provider_label;
    m.ui.login = login::Closed{};
    m.s.status = m.s.oauth_refresh_in_flight
               ? "switched " + provider_label + " to " + label
                     + " \xE2\x80\x94 refreshing token\xE2\x80\xA6"
               : "switched " + provider_label + " to " + label;
    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{4};
    return {std::move(m), std::move(refresh_cmd)};
}

Step account_remove(Model m) {
    auto* al = std::get_if<login::AccountList>(&m.ui.login);
    if (!al) return done(std::move(m));
    const int add_row = static_cast<int>(al->rows.size());
    if (al->cursor >= add_row) return done(std::move(m));   // add-new row: nothing to remove

    namespace acc = agentty::auth::accounts;
    const auto row = al->rows[static_cast<std::size_t>(al->cursor)];

    // Destructive actions are deliberately two-step. A stray Backspace or
    // vim `d` must never erase a saved refresh token with no way back.
    if (al->confirm_remove != row.label) {
        al->confirm_remove = row.label;
        return done(std::move(m));
    }

    const bool was_active = row.active;
    const int old_cursor = al->cursor;
    const std::string provider_label = al->provider_label;
    acc::remove(row.provider, row.label);

    // Entitlement facts learned for THIS account can never be consulted
    // again — and a later re-login may land on a different subscription
    // tier, so keeping them would be wrong as well as dead. This is the one
    // place forgetting is correct: removal, not a switch.
    {
        auto s = deps().load_settings();
        const auto before = s.entitlements.size();
        domain::entitlement::forget_account(s.entitlements, row.provider,
                                            row.label);
        if (s.entitlements.size() != before) deps().save_settings(s);
    }

    // If we removed the account we're currently authed as, the newest
    // remaining one (promoted to active by remove()) becomes live.
    if (was_active) {
        if (auto next = acc::get(row.provider, acc::active_label(row.provider))) {
            acc::activate(row.provider, next->label);
            const auto* rrow = provider::preset_for(row.provider);
            if (rrow && rrow->oauth_proactive_refresh) {
                // Anthropic-shaped: credentials live in the shared store and
                // resolve to a real header.
                if (auto c = auth::load_credentials())
                    agentty::app::update_auth(auth::make_auth_header(*c));
            } else if (rrow && rrow->token_in_transport) {
                // The transport owns the token and reads it per turn; the
                // cached header must be CLEARED, not replaced.
                agentty::app::update_auth(auth::AuthHeader{});
            } else {
                // CUSTOM HOST: activate() wrote the promoted key into
                // provider_keys[spec]; re-resolve the live header through the
                // central resolver so the promoted account is used next turn.
                agentty::app::update_auth(
                    provider::credentials::resolve(row.provider));
            }
            m.s.status = "removed " + row.label + " \xc2\xb7 switched to " + next->label;
        } else {
            // The registry is empty. Clear the underlying live credential too
            // (file OR provider_keys[spec]) through the ONE central sign-out;
            // otherwise build_account_list() would rediscover and silently
            // resurrect the account the user just removed.
            provider::credentials::clear_active(row.provider);
            agentty::app::update_auth(auth::AuthHeader{});

            login::AccountList empty;
            empty.provider = row.provider;
            empty.provider_label = provider_label;
            m.ui.login = std::move(empty);  // stays on "+ Add another account…"
            m.s.status = "removed the last " + provider_label + " account";
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds{4};
            return done(std::move(m));
        }
    } else {
        m.s.status = "removed " + row.label;
    }

    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{4};

    // Rebuild the list in place and keep the cursor near the removed row.
    auto rebuilt = build_account_list(provider::active());
    rebuilt.cursor = std::min(old_cursor,
                              static_cast<int>(rebuilt.rows.size()));
    m.ui.login = std::move(rebuilt);
    return done(std::move(m));
}

Step login_pick_method(Model m, char32_t key) {
    const auto* picking = std::get_if<login::Picking>(&m.ui.login);
    if (!picking && !std::holds_alternative<login::Failed>(m.ui.login))
        return done(std::move(m));
    // Does this provider offer a CHOICE of auth method? Registry flag, not a
    // name: a second OAuth-or-key provider gets the menu by setting it.
    const auto* mrow = picking ? provider::preset_for(picking->provider) : nullptr;
    const bool anthropic_only = mrow && mrow->method_menu;
    if (key == U'2') {
        // OAuth: mint PKCE pair, open browser, transition to OAuthCode.
        // The URL lives in state so the modal can show it as a fallback
        // if the system browser opener fails silently (broken xdg-open,
        // headless SSH session, etc.).
        //
        // random_urlsafe throws if the OpenSSL CSPRNG is unavailable
        // (astronomically rare, but a pure reducer must not propagate an
        // exception into maya's update loop). Fail closed into the login
        // modal's Failed state instead of minting a weak/empty secret.
        try {
            auth::PkceVerifier verifier{auth::random_urlsafe(128)};
            auth::OAuthState   state{auth::random_urlsafe(32)};
            std::string url = auth::oauth_authorize_url(verifier, state);
            login::OAuthCode oc;
            oc.verifier      = std::move(verifier);
            oc.state         = std::move(state);
            oc.authorize_url = url;
            m.ui.login = std::move(oc);
            return {std::move(m), cmd::open_browser_async(std::move(url))};
        } catch (const std::exception& e) {
            m.ui.login = login::Failed{
                std::string{"could not start secure login: "} + e.what()};
            return done(std::move(m));
        }
    }
    if (key == U'1') {
        login::ApiKeyInput api;
        // Esc = back to THIS method menu, scoped to the same provider.
        api.origin = login::origin::Method{
            picking ? picking->provider : std::string{}};
        m.ui.login = std::move(api);
        return done(std::move(m));
    }
    if (key == U'3' && !anthropic_only) {
        // Native ChatGPT OAuth. Local terminals use the browser + loopback
        // callback; SSH terminals automatically use OpenAI device auth and
        // receive a one-time code through CodexDeviceCodeReady.
        const auto attempt_id = cmd::next_codex_login_attempt_id();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        m.ui.login = login::ChatGptWaiting{
            .attempt_id = attempt_id,
            .cancel = cancel,
            .device_auth = provider::chatgpt::codex_device_auth_preferred(),
        };
        return {std::move(m), cmd::codex_login_async(attempt_id, std::move(cancel))};
    }
    if (key == U'4' && !anthropic_only) {
        // Custom OpenAI-compatible host (llama.cpp, vLLM, LM Studio,
        // or any remote server). Reuses the same CustomHostInput flow
        // that the provider picker's "Custom host…" row opens — the
        // user types a host[:port], TLS hosts prompt for an API key,
        // local hosts commit keylessly. The model picker opens
        // automatically when the provider switch commits.
        {
            login::CustomHostInput ch;
            // Esc = back to THIS method menu, scoped to the same provider.
            ch.origin = login::origin::Method{
                picking ? picking->provider : std::string{}};
            m.ui.login = std::move(ch);
        }
        return done(std::move(m));
    }
    if (key == U'5' && !anthropic_only) {
        // Native GitHub Copilot OAuth (device flow).
        return launch_device_login(std::move(m), "copilot", "GitHub Copilot");
    }
    if (key == U'6' && !anthropic_only) {
        // Native Kimi OAuth (device flow).
        return launch_device_login(std::move(m), "kimi", "Kimi");
    }
    return done(std::move(m));
}

Step login_char_input(Model m, char32_t ch) {
    auto utf8 = ui::utf8_encode(ch);
    std::visit(overload{
        [&](login::OAuthCode& s) {
            s.code_input.insert(s.cursor, utf8);
            s.cursor += static_cast<int>(utf8.size());
        },
        [&](login::ApiKeyInput& s) {
            s.key_input.insert(s.cursor, utf8);
            s.cursor += static_cast<int>(utf8.size());
        },
        [&](login::CustomHostInput& s) {
            s.host_input.insert(s.cursor, utf8);
            s.cursor += static_cast<int>(utf8.size());
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_backspace(Model m) {
    std::visit(overload{
        [](login::OAuthCode& s) {
            if (s.cursor > 0 && !s.code_input.empty()) {
                int p = ui::utf8_prev(s.code_input, s.cursor);
                s.code_input.erase(p, s.cursor - p);
                s.cursor = p;
            }
        },
        [](login::ApiKeyInput& s) {
            if (s.cursor > 0 && !s.key_input.empty()) {
                int p = ui::utf8_prev(s.key_input, s.cursor);
                s.key_input.erase(p, s.cursor - p);
                s.cursor = p;
            }
        },
        [](login::CustomHostInput& s) {
            if (s.cursor > 0 && !s.host_input.empty()) {
                int p = ui::utf8_prev(s.host_input, s.cursor);
                s.host_input.erase(p, s.cursor - p);
                s.cursor = p;
            }
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_paste(Model m, std::string text) {
    std::visit(overload{
        [&](login::OAuthCode& s) {
            s.code_input.insert(s.cursor, text);
            s.cursor += static_cast<int>(text.size());
        },
        [&](login::ApiKeyInput& s) {
            s.key_input.insert(s.cursor, text);
            s.cursor += static_cast<int>(text.size());
        },
        [&](login::CustomHostInput& s) {
            s.host_input.insert(s.cursor, text);
            s.cursor += static_cast<int>(text.size());
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_cursor_left(Model m) {
    std::visit(overload{
        [](login::OAuthCode& s) {
            s.cursor = ui::utf8_prev(s.code_input, s.cursor);
        },
        [](login::ApiKeyInput& s) {
            s.cursor = ui::utf8_prev(s.key_input, s.cursor);
        },
        [](login::CustomHostInput& s) {
            s.cursor = ui::utf8_prev(s.host_input, s.cursor);
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_cursor_right(Model m) {
    std::visit(overload{
        [](login::OAuthCode& s) {
            s.cursor = ui::utf8_next(s.code_input, s.cursor);
        },
        [](login::ApiKeyInput& s) {
            s.cursor = ui::utf8_next(s.key_input, s.cursor);
        },
        [](login::CustomHostInput& s) {
            s.cursor = ui::utf8_next(s.host_input, s.cursor);
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_submit(Model m) {
    if (auto* ch = std::get_if<login::CustomHostInput>(&m.ui.login)) {
        // Canonicalise (trim + strip trailing '/') via the ONE shared helper
        // — the same normalisation the CLI --provider path applies, so the
        // two entry points can never mint different settings keys for the
        // same endpoint (the root cause behind the custom-host PR series).
        std::string spec = provider::canonical_spec(ch->host_input);
        if (spec.empty()) {
            m.ui.login = login::Failed{"no host entered"};
            return done(std::move(m));
        }

        // Remote (TLS) custom hosts need an API key — local servers
        // (http://, bare host:port) conventionally don't. For TLS hosts,
        // hand off to the ApiKeyInput modal instead of committing immediately:
        // the user pastes a key, it's saved to provider_keys[spec], and the
        // existing ApiKeyInput arm commits the switch. Esc at the key prompt
        // dispatches CloseLogin → no switch (matching how Esc works for
        // every other login sub-state). For non-TLS hosts, fall through to
        // the keyless commit path below.
        const bool needs_key = provider::parse_selection(spec)
                                   .openai_endpoint.use_tls;
        if (needs_key) {
            std::string label = provider::provider_display_name(
                provider::parse_selection(spec));
            // Pre-fill the key field with any key already saved for this
            // spec so re-entering a known host shows its current key (masked)
            // for confirmation/edit, not a blank field.
            std::string existing_key;
            {
                auto settings = deps().load_settings();
                if (auto it = settings.provider_keys.find(spec);
                    it != settings.provider_keys.end())
                    existing_key = it->second;
            }
            // Capture size BEFORE the move: designated initializers evaluate
            // in declaration order (key_input before cursor), so
            // std::move(existing_key) into .key_input would leave
            // existing_key moved-from when .cursor reads .size().
            const int key_len = static_cast<int>(existing_key.size());
            m.ui.login = login::ApiKeyInput{
                .key_input      = std::move(existing_key),
                .cursor         = key_len,
                .provider       = spec,
                .provider_label = std::move(label),
                // Esc = re-edit the host; the frame carries the TYPED spec so
                // nothing the user entered is lost.
                .origin         = login::origin::HostInput{spec},
            };
            return done(std::move(m));
        }

        // Non-TLS (local) host: no key needed. PROBE before committing —
        // dial the model list on a worker (configured path → /v1/models →
        // Ollama /api/tags) and only commit on an answer, with the DETECTED
        // dialect. The modal shows "probing…"; failure returns here with
        // the spec restored and the reason named. No more committing blind
        // to a dead endpoint and discovering it at the first prompt.
        {
            auth::AuthHeader probe_auth = provider::credentials::resolve(spec);
            const auto attempt_id = cmd::next_codex_login_attempt_id();
            auto origin = std::move(ch->origin);
            m.ui.login = login::HostProbing{
                .spec = spec, .attempt_id = attempt_id,
                .origin = std::move(origin)};
            return {std::move(m),
                    cmd::probe_host_async(spec, attempt_id,
                                          std::move(probe_auth))};
        }
    }
    if (auto* api = std::get_if<login::ApiKeyInput>(&m.ui.login)) {
        std::string key = std::move(api->key_input);
        const std::string provider = api->provider;
        const std::string provider_label = api->provider_label;
        // Trim trailing whitespace — paste handlers may include a stray
        // newline depending on terminal pasting behaviour.
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n'
                              || key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (key.empty()) {
            m.ui.login = login::Failed{"no key entered"};
            return done(std::move(m));
        }

        // OpenAI-family key: persist under Settings.provider_keys[id], then
        // commit the live provider switch the picker deferred. The Anthropic
        // path (empty provider) keeps using credentials.json below.
        if (!provider.empty()) {
            // Persist the pasted key as this provider's active account (snapshots
            // any prior key first, so it ADDS, not replaces), and mark the
            // provider active. add_key is the single implementation of the
            // "paste a key" flow, shared with credentials::.
            provider::credentials::add_key(provider, key);
            {
                auto settings = deps().load_settings();
                settings.provider = provider;
                deps().save_settings(settings);
            }
            auth::AuthHeader new_auth = provider::credentials::resolve(provider);
            m.ui.login = login::Closed{};
            // Commit through the ONE shared switch path like every other entry;
            // commit_provider_switch opens the model picker for us.
            return commit_provider_switch(std::move(m), provider,
                                          std::move(new_auth), provider_label);
        }

        install_and_close(m, auth::Credentials{auth::cred::ApiKey{std::move(key)}},
                          std::holds_alternative<login::origin::Accounts>(api->origin));
        return done(std::move(m));
    }
    if (auto* oc = std::get_if<login::OAuthCode>(&m.ui.login)) {
        std::string code_raw = std::move(oc->code_input);
        while (!code_raw.empty() && (code_raw.back() == '\r' || code_raw.back() == '\n'
                                   || code_raw.back() == ' ' || code_raw.back() == '\t'))
            code_raw.pop_back();
        if (code_raw.empty()) {
            // Stay in OAuthCode — leaving the verifier intact so the user
            // can re-paste without reopening the browser.
            return done(std::move(m));
        }
        auto verifier = std::move(oc->verifier);
        auto state    = std::move(oc->state);
        // OAuthCode carries no origin, so a deliberate "+ Add another
        // account" OAuth login can't be distinguished from a re-login
        // at exchange time. Default to REUSE (as_new_account = false):
        // the common case is a re-auth of the same account, and two
        // Anthropic OAuth logins are content-indistinguishable anyway
        // (no email/subject on the wire) — so collapsing them onto one
        // "OAuth login" slot is honest, not lossy. Multi-account with
        // real identity is served by API keys / ChatGPT.
        m.ui.login = login::OAuthExchanging{/*as_new_account=*/false};
        return {std::move(m),
            cmd::oauth_exchange(auth::OAuthCode{std::move(code_raw)},
                                std::move(verifier), std::move(state))};
    }
    return done(std::move(m));
}

Step login_copy_auth_url(Model m) {
    // Copy the verification URL. Works from the OAuth-code screen and from any
    // device-flow modal (Copilot/Kimi) — one path, so every provider behaves
    // the same.
    std::string url;
    if (auto* oc = std::get_if<login::OAuthCode>(&m.ui.login)) url = oc->authorize_url;
    else if (auto* dw = std::get_if<login::DeviceWaiting>(&m.ui.login))
        url = dw->browser_url.empty() ? dw->authorize_url : dw->browser_url;
    if (url.empty()) return done(std::move(m));
    (void)write_clipboard_text(url);   // native pbcopy/wl-copy/xclip
    auto write_cmd = Cmd<Msg>::write_clipboard(url);
    auto toast = set_status_toast(m, "authorize URL copied to clipboard",
                                  std::chrono::seconds{3});
    return {std::move(m), Cmd<Msg>::batch(std::move(write_cmd), std::move(toast))};
}

Step login_copy_code(Model m) {
    // Copy the one-time CODE (what the user types into the browser). Device
    // flow only — terminal text-selection can't grab it because the modal
    // re-renders every poll tick, wiping any selection, so this keystroke is
    // the reliable way onto the clipboard. One path for every device provider.
    auto* dw = std::get_if<login::DeviceWaiting>(&m.ui.login);
    if (!dw || dw->user_code.empty()) return done(std::move(m));
    auto code = dw->user_code;
    (void)write_clipboard_text(code);
    auto write_cmd = Cmd<Msg>::write_clipboard(code);
    auto toast = set_status_toast(m, "code " + code + " copied to clipboard",
                                  std::chrono::seconds{3});
    return {std::move(m), Cmd<Msg>::batch(std::move(write_cmd), std::move(toast))};
}

Step login_open_browser_again(Model m) {
    std::string url;
    if (auto* oc = std::get_if<login::OAuthCode>(&m.ui.login)) url = oc->authorize_url;
    else if (auto* dw = std::get_if<login::DeviceWaiting>(&m.ui.login))
        url = dw->browser_url.empty() ? dw->authorize_url : dw->browser_url;
    if (url.empty()) return done(std::move(m));
    auto open_cmd = cmd::open_browser_async(std::move(url));
    auto toast = set_status_toast(m,
        "opening browser\xe2\x80\xa6",
        std::chrono::seconds{2});
    return {std::move(m),
        Cmd<Msg>::batch(std::move(open_cmd), std::move(toast))};
}

Step login_exchanged(Model m, auth::TokenResult result) {
    auto* xchg = std::get_if<login::OAuthExchanging>(&m.ui.login);
    if (!xchg)
        return done(std::move(m));
    const bool as_new = xchg->as_new_account;
    if (!result) {
        m.ui.login = login::Failed{result.error().render()};
        return done(std::move(m));
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& tok = *result;
    install_and_close(m, auth::Credentials{auth::cred::OAuth{
        std::move(tok.access_token),
        std::move(tok.refresh_token),
        tok.expires_in_s ? now_ms + tok.expires_in_s * 1000 : 0,
    }}, as_new);
    return done(std::move(m));
}

Step login_codex_device_code_ready(Model m, std::uint64_t attempt_id,
                                   std::string verification_url,
                                   std::string user_code) {
    auto* waiting = std::get_if<login::ChatGptWaiting>(&m.ui.login);
    if (!waiting || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    waiting->device_auth = true;
    waiting->authorize_url = std::move(verification_url);
    waiting->user_code = std::move(user_code);
    return done(std::move(m));
}

Step login_codex_done(
    Model m, std::uint64_t attempt_id,
    std::expected<provider::chatgpt::CodexCredentials, auth::OAuthError> result)
{
    auto* waiting = std::get_if<login::ChatGptWaiting>(&m.ui.login);
    if (!waiting || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    if (waiting->cancel)
        waiting->cancel->store(true, std::memory_order_release);
    if (!result) {
        m.ui.login = login::Failed{result.error().render()};
        return done(std::move(m));
    }
    if (!provider::chatgpt::save_codex_credentials(*result)) {
        m.ui.login = login::Failed{
            "signed in, but encrypted credentials could not be saved"};
        return done(std::move(m));
    }
    // Persistence happens only after the attempt identity check above. An
    // abandoned or superseded worker can therefore neither switch provider
    // nor overwrite the active credential store.
    m.ui.login = login::Closed{};
    m.s.status = "signed in to ChatGPT";
    m.s.status_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    return commit_provider_switch(std::move(m), "chatgpt",
                                  auth::AuthHeader{}, "ChatGPT");
}

Step login_device_code_ready(Model m, std::string provider,
                             std::uint64_t attempt_id,
                             std::string verification_url,
                             std::string browser_url,
                             std::string user_code) {
    auto* waiting = std::get_if<login::DeviceWaiting>(&m.ui.login);
    if (!waiting || waiting->provider != provider
                 || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    waiting->authorize_url = std::move(verification_url);   // bare (code field)
    if (browser_url.empty()) browser_url = waiting->authorize_url;
    waiting->browser_url = browser_url;
    waiting->user_code = std::move(user_code);
    // Best-effort: open the PRE-FILLED url so the user doesn't have to type the
    // code. Harmless if it can't (SSH/headless) — the panel shows the bare url
    // + code for manual entry.
    return {std::move(m), cmd::open_browser_async(std::move(browser_url))};
}

Step login_device_done(Model m, std::string provider, std::string provider_label,
                       std::uint64_t attempt_id,
                       std::optional<std::string> error) {
    auto* waiting = std::get_if<login::DeviceWaiting>(&m.ui.login);
    if (!waiting || waiting->provider != provider
                 || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    if (waiting->cancel)
        waiting->cancel->store(true, std::memory_order_release);
    if (error) {
        m.ui.login = login::Failed{std::move(*error)};
        return done(std::move(m));
    }
    // The worker already persisted the token; the transport reads it lazily on
    // the first turn. Switch the active provider now.
    m.ui.login = login::Closed{};
    m.s.status = "signed in to " + provider_label;
    m.s.status_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    return commit_provider_switch(std::move(m), std::move(provider),
                                  auth::AuthHeader{}, std::move(provider_label));
}

Step token_refreshed(Model m, auth::TokenResult result) {
    // Background-refresh result. Distinct from login_exchanged: this
    // path was kicked off either by `init()` (stale-but-refreshable
    // token on disk) or by the StreamError handler reacting to a
    // mid-session 401 (see stream.cpp, ErrorClass::Auth branch). The
    // modal state doesn't change here either way; the stream ctx
    // parked in retry::Scheduled is what tells us we owe a RetryStream.
    m.s.oauth_refresh_in_flight = false;

    // Was a stream parked waiting for this refresh? If so, we either
    // resume it (success) or tear it down to Idle (failure). Detected
    // structurally via retry::Scheduled on the active ctx — the only
    // way the phase reaches that state without a RetryStream already
    // in flight is the auth-refresh branch in stream.cpp.
    const bool stream_parked = m.s.in_scheduled();

    if (!result) {
        // BENIGN RACE, not a failure: the refresh landed after an account
        // switch replaced the credential store. The switched-to account's
        // token is already live (activate installed it) and the refreshed
        // token belongs to the switched-AWAY account — installing it would
        // cross-wire the accounts, and an error toast would alarm the user
        // over a race they can't see. If a stream was parked on this
        // refresh, retry it: it will pick up the live (switched-to) header.
        if (result.error().kind == auth::OAuthErrorKind::Superseded) {
            AGT_LOG(Auth, Info, "auth.refresh.superseded_dropped",
                    "stream_parked={}", stream_parked ? 1 : 0);
            if (stream_parked)
                return {std::move(m),
                        Cmd<Msg>::after(std::chrono::milliseconds{0},
                                        Msg{RetryStream{}})};
            return {std::move(m), Cmd<Msg>::none()};
        }
        // Refresh failed — surface the typed error in the bottom row.
        // The "error:" prefix triggers shortcut_row.cpp's danger
        // styling. 6s gives the user time to read before the toast
        // expires; the Cmd::after sentinel auto-clears so a later
        // status write doesn't get pre-empted.
        std::string text = std::string{"error: token refresh failed: "}
                         + result.error().render();

        // If a stream was parked on this refresh, tear it down: there's
        // no fresh token coming, so retrying would just 401 again.
        // Drop to Idle and finalise any in-flight tool calls so the
        // session is cleanly recoverable via the login modal.
        if (stream_parked) {
            auto now = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& last = m.d.current.messages.back();
                last.error = text;
                for (auto& tc : last.tool_calls) {
                    if (!tc.is_terminal()) {
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            "auth refresh failed"};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
                if (last.text.empty() && last.tool_calls.empty()) {
                    m.d.current.messages.pop_back();
                }
            }
            m.s.phase = phase::Idle{};
        }

        auto cmd = set_status_toast(m, std::move(text),
                                    std::chrono::seconds{6});
        // Leave any queued composer text alone — the user can resubmit
        // (after re-authenticating via the login modal) without
        // retyping. The first manual send in that state will hit the
        // stale-token 401 path, but the in-app login modal is the
        // recovery surface.
        return {std::move(m), std::move(cmd)};
    }

    // Refresh OK — install fresh creds into Deps so the next stream uses
    // the new bearer.
    //
    // Do NOT save here. The refresh worker (refresh_access_token_locked)
    // already persisted under the cross-process file lock — and crucially,
    // its save preserves the previous refresh token when the server chose
    // not to rotate (empty refresh_token in the response). A second save
    // here wrote `tok.refresh_token` VERBATIM — possibly empty — wiping the
    // on-disk refresh token, so the NEXT refresh had nothing to exchange
    // and the account was stuck until re-login ("switch refuses to refresh"
    // when the wiped slot was later snapshotted/activated). One writer, one
    // policy: the locked worker owns persistence; the reducer only installs.
    //
    // LINEAGE before install: an account switch may have replaced the store
    // after the worker saved. Installing this (older account's) bearer into
    // Deps would cross-wire the live header against the registry-active
    // account. The store is the truth — install only if it still carries
    // the access token this refresh produced.
    auto& tok = *result;
    {
        auto on_disk = auth::load_credentials();
        const auto* o = on_disk ? std::get_if<auth::cred::OAuth>(&*on_disk)
                                : nullptr;
        if (!o || o->access_token != tok.access_token) {
            AGT_LOG(Auth, Info, "auth.refresh.stale_install_dropped",
                    "store no longer carries the refreshed token "
                    "(account switched); keeping live header as-is");
            if (stream_parked)
                return {std::move(m),
                        Cmd<Msg>::after(std::chrono::milliseconds{0},
                                        Msg{RetryStream{}})};
            return {std::move(m), Cmd<Msg>::none()};
        }
        agentty::app::update_auth(auth::make_auth_header(*on_disk));
    }

    auto toast_cmd = set_status_toast(m, "OAuth token refreshed",
                                      std::chrono::seconds{3});

    // A stream was parked waiting for this refresh (the StreamError
    // handler's Auth branch left the phase in Streaming{retry::Scheduled}).
    // Resume by dispatching RetryStream — the existing RetryStream arm
    // flips retry back to Fresh and calls launch_stream, which picks
    // up the freshly-installed bearer from Deps.
    if (stream_parked) {
        return {std::move(m),
            Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(toast_cmd),
                Cmd<Msg>::after(std::chrono::milliseconds{0},
                                Msg{RetryStream{}})})};
    }

    // Drain any text the user queued while the refresh was in flight.
    // Mirrors the stream-finish drain at update/stream.cpp:617 — pull
    // the front off `composer.queued`, hand it to submit_message, and
    // batch its Cmd alongside the toast so the user's first turn fires
    // the moment fresh creds are live.
    if (m.s.is_idle() && !m.ui.composer.queued.empty()) {
        auto& head = m.ui.composer.queued.front();
        m.ui.composer.text        = std::move(head.text);
        m.ui.composer.attachments = std::move(head.attachments);
        m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return {std::move(m),
            Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(toast_cmd), std::move(sub_cmd)})};
    }
    return {std::move(m), std::move(toast_cmd)};
}

// ============================================================================
// login_update — reducer for `msg::LoginMsg`
// ============================================================================
// Thin dispatch over the per-arm helpers above; the typed state-machine
// guarantees the helpers see a modal in the right state.

Step login_update(Model m, msg::LoginMsg lm) {
    return std::visit(overload{
        [&](OpenLogin)              -> Step { return open_login(std::move(m)); },
        [&](CloseLogin)             -> Step { return close_login(std::move(m)); },
        [&](LoginBack)              -> Step { return login_back(std::move(m)); },
        [&](HostProbed& e)          -> Step { return host_probed(std::move(m), std::move(e)); },
        [&](SignOut)                -> Step { return sign_out(std::move(m)); },
        [&](OpenAccounts& e)        -> Step { return open_accounts(std::move(m), e.provider); },
        [&](AccountMove& e)         -> Step { return account_move(std::move(m), e.delta); },
        [&](AccountSelect)          -> Step { return account_select(std::move(m)); },
        [&](AccountRemove)          -> Step { return account_remove(std::move(m)); },
        [&](LoginPickMethod& e)     -> Step { return login_pick_method(std::move(m), e.key); },
        [&](LoginCharInput& e)      -> Step { return login_char_input(std::move(m), e.ch); },
        [&](LoginBackspace)         -> Step { return login_backspace(std::move(m)); },
        [&](LoginPaste& e)          -> Step { return login_paste(std::move(m), std::move(e.text)); },
        [&](LoginCursorLeft)        -> Step { return login_cursor_left(std::move(m)); },
        [&](LoginCursorRight)       -> Step { return login_cursor_right(std::move(m)); },
        [&](LoginSubmit)            -> Step { return login_submit(std::move(m)); },
        [&](LoginCopyAuthUrl)       -> Step { return login_copy_auth_url(std::move(m)); },
        [&](LoginCopyCode)          -> Step { return login_copy_code(std::move(m)); },
        [&](LoginOpenBrowserAgain)  -> Step { return login_open_browser_again(std::move(m)); },
        [&](LoginExchanged& e)      -> Step { return login_exchanged(std::move(m), std::move(e.result)); },
        [&](CodexDeviceCodeReady& e) -> Step {
            return login_codex_device_code_ready(std::move(m), e.attempt_id,
                std::move(e.verification_url), std::move(e.user_code));
        },
        [&](CodexLoginDone& e)      -> Step {
            return login_codex_done(std::move(m), e.attempt_id,
                                    std::move(e.result));
        },
        [&](DeviceCodeReady& e) -> Step {
            return login_device_code_ready(std::move(m), std::move(e.provider),
                e.attempt_id, std::move(e.verification_url),
                std::move(e.browser_url), std::move(e.user_code));
        },
        [&](DeviceLoginDone& e)    -> Step {
            return login_device_done(std::move(m), std::move(e.provider),
                std::move(e.provider_label), e.attempt_id, std::move(e.error));
        },
        [&](TokenRefreshed& e)      -> Step { return token_refreshed(std::move(m), std::move(e.result)); },
    }, lm);
}

} // namespace agentty::app::detail
