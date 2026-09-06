#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/mcp/client.hpp"   // mcp_config_present()
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/workspace/files.hpp"
#include "agentty/workspace/symbols.hpp"
#include "agentty/util/modelsdev.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/tool/subagent.hpp"   // set_smart: push pins to the task router

#include <cstdlib>
#include <vector>

namespace agentty::app {

namespace {
std::vector<ModelInfo> seed_models() {
    // The launch floor is the single bundled catalog (base rows + their `[1m]`
    // companions), so the picker is full IMMEDIATELY on launch — not only
    // after the first /models fetch (which the live catalog supersedes
    // anyway). One source shared with the transport fallback, so seed and
    // live catalog can't drift. Entitlement self-heals downstream
    // (context_1m_blocked strips the [1m] rows if the account can't stream 1M).
    return catalog::bundled("anthropic");
}
} // namespace

std::pair<Model, maya::Cmd<Msg>> init() {
    Model m;
    // Seed the composer idle-blink clock at launch so the 15 s
    // blink-stop countdown starts now, not on the first keystroke. A
    // freshly-opened, never-touched agentty is exactly the idle-CPU case
    // (a forever-blinking painted cursor keeps a GPU terminal compositing),
    // so it must time out even if the user never types.
    m.ui.composer.last_edit_ms = maya::anim::default_clock().now_ms();
    // Thread history is the single largest startup cost: a real-world
    // history of hundreds of multi-MB thread JSONs serializes into
    // seconds of synchronous parse work before the first frame can
    // render. Defer to a background task; ThreadsLoaded fills the list
    // when it lands. The thread picker reads `m.s.threads_loading` to
    // show a "loading…" hint until then.
    m.s.threads_loading  = true;
    m.d.available_models = seed_models();

    auto settings = deps().load_settings();
    // ONE-TIME MIGRATION: the legacy account-blind `context_1m_blocked`
    // bool becomes a keyed fact for the CURRENTLY ACTIVE account. That is
    // the best available attribution — the bool never recorded whose block
    // it was, and the active account is who it was last observed under.
    // Attributing it (rather than dropping it) preserves the user's learned
    // state across the upgrade; the next save drops the bool for good.
    if (settings.context_1m_blocked) {
        settings.context_1m_blocked = false;
        if (detail::entitlement_record_blocked(
                settings, domain::entitlement::Fact::Context1M,
                wire_model_id(settings.model_id.value), "anthropic"))
            deps().save_settings(settings);
    }
    // DISCOVERED entitlement: this account 400'd on the context-1m beta in a
    // prior session. Strip the seeded `[1m]` rows up front so the picker
    // doesn't briefly offer them before the first live ModelsLoaded fetch
    // (which also filters) — selecting one would just re-trigger the fallback.
    if (!m.d.available_models.empty()) {
        std::erase_if(m.d.available_models, [&](const ModelInfo& mi) {
            return mi.id.value.find("[1m]") != std::string::npos
                && detail::entitlement_blocked(settings,
                                       domain::entitlement::Fact::Context1M,
                                       wire_model_id(mi.id.value));
        });
    }
    // DISCOVERED entitlement: this account 400'd on the context-1m beta in a
    // prior session. A persisted `[1m]` model id would re-send the beta on
    // the very first turn and dead-end again — strip the marker up front.
    if (settings.model_id.value.find("[1m]") != std::string::npos
        && detail::entitlement_blocked(settings, domain::entitlement::Fact::Context1M,
                               wire_model_id(settings.model_id.value)))
        settings.model_id = ModelId{wire_model_id(settings.model_id.value)};
    if (!settings.model_id.empty()) {
        // Guard against a cross-provider model id collision. A persisted
        // model id belongs to whatever provider was active when it was
        // saved; relaunching on a DIFFERENT provider (or with no
        // --provider, falling back to Anthropic) must not carry that id
        // along — e.g. a leftover "qwen2.5-coder:7b" sent to Anthropic
        // 404s on the first prompt. For Anthropic, only honour the saved
        // id if it's a known Claude id; otherwise drop it and let the
        // seed default (model_id's built-in) stand. The OpenAI side
        // self-corrects via the eager fetch_models() round trip below,
        // so it keeps the saved id and lets ModelsLoaded validate it.
        const bool anthropic_active =
            provider::active().kind == provider::Kind::Anthropic;
        bool honour = true;
        if (anthropic_active) {
            // Honour the saved id if it's a known seeded model OR any
            // `claude-*` id. The seed list is a small hardcoded snapshot
            // (opus/sonnet/haiku 4.5); a NEWER Claude the user selected
            // (e.g. claude-opus-4-8) is a perfectly valid Anthropic id but
            // isn't seeded, and rejecting it here is exactly why the last
            // picked model was forgotten every launch — we fell back to the
            // built-in default. Any `claude-` prefix is an Anthropic id and
            // is safe to send; only a foreign id (e.g. a leftover Ollama
            // "qwen2.5-coder:7b") must be dropped so it doesn't 404.
            honour = settings.model_id.value.starts_with("claude-");
            if (!honour) {
                for (const auto& mi : m.d.available_models)
                    if (mi.id == settings.model_id) { honour = true; break; }
            }
        }
        if (honour) {
            m.d.model_id = settings.model_id;
            // Ensure the honoured id is in the picker list (a newer Claude
            // won't be in the seed snapshot) so the model picker shows the
            // active model selected instead of nothing.
            bool present = false;
            for (const auto& mi : m.d.available_models)
                if (mi.id == m.d.model_id) { present = true; break; }
            if (!present)
                m.d.available_models.insert(
                    m.d.available_models.begin(),
                    ModelInfo{m.d.model_id, m.d.model_id.value,
                              "anthropic", 200000, false});
        }
    }
    // Set the per-model context window now (before any stream runs) so
    // the ctx % bar uses the right denominator from frame 1, not after
    // the user's first message lands.
    m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
    m.d.profile = settings.profile;
    m.d.effort  = effort_from_wire(settings.effort);
    // Publish the user's per-model reasoning-effort overrides into the catalog
    // registry so resolved_caps() (and thus supports_effort / the picker /
    // the wire) honor them from frame 1. Mirrors set_custom_auth_header.
    set_reasoning_overrides(settings.reasoning_effort_overrides);
    // Hydrate the LEARNED effort sets (exact per-model reasoning_effort
    // contracts discovered from provider rejections in past sessions) so the
    // ladder / clamps / wire are correct from frame 1 — the reject→learn→
    // retry loop only ever pays its one-time cost once per model, ever.
    set_learned_effort_sets(settings.learned_effort_sets);

    // Smart Mode: rehydrate role config from settings. A slot counts as
    // "set" once the user pinned a model for it.
    m.d.smart.enabled = settings.smart_enabled;
    // Session pin: AGENTTY_SMART_MODE overrides the persisted master switch
    // for THIS process (scripted runs, benchmarks, bisecting).
    // persist_settings skips the field while pinned, so the user's saved
    // preference survives the session untouched.
    if (auto ov = smart::tuning::enabled_override())
        m.d.smart.enabled = *ov;
    auto load_slot = [](smart::SlotOverride& slot,
                        const std::string& model, const std::string& eff,
                        const std::string& provider) {
        if (!model.empty()) {
            slot.model    = model;
            slot.effort   = effort_from_wire(eff);
            slot.set      = true;
            // "" for settings written before pins were provider-scoped —
            // resolve_role treats unknown provenance as "honour everywhere",
            // so an upgrading user sees no behaviour change.
            slot.provider = provider;
        }
    };
    load_slot(m.d.smart.strategic,      settings.smart_strategic_model, settings.smart_strategic_effort, settings.smart_strategic_provider);
    load_slot(m.d.smart.implementation, settings.smart_impl_model,      settings.smart_impl_effort,      settings.smart_impl_provider);
    load_slot(m.d.smart.utility,        settings.smart_utility_model,   settings.smart_utility_effort,   settings.smart_utility_provider);

    // Numeric routing policy: env override wins, else the persisted setting.
    // RESOLVED ONCE, here, so the classifier and the effort scaler read a
    // plain int rather than calling getenv() per turn — and so a value the
    // user changed in the settings UI is actually the value that routes.
    // settings_registry::apply_env has already clamped the stored fields to
    // each row's range, so no clamp is needed a second time.
    m.d.smart.deep_margin =
        smart::tuning::deep_margin_env().value_or(settings.smart_deep_margin);
    m.d.smart.bias_clamp =
        smart::tuning::bias_clamp_env().value_or(settings.smart_bias_clamp);
    m.d.smart.complex_threshold =
        smart::tuning::complex_threshold_env().value_or(
            settings.smart_complex_threshold);
    // Push the rehydrated config down to the subagent router NOW.
    //
    // `task` runs on a worker thread with no access to this Model, so the
    // router keeps its own process-global snapshot (tools::subagent::Config::
    // smart). main() installs that config before settings are read, with a
    // DEFAULT-CONSTRUCTED RoleConfig — enabled=false, no slots. Until
    // something pushes the real one down, every delegation resolves as if
    // Smart Mode were off, silently ignoring the user's pins.
    //
    // The two paths that used to do it are both conditional: persist_settings
    // (only if you EDIT Smart Mode this session) and the ModelsLoaded reducer
    // arm (only on a SUCCESSFUL catalog fetch — it early-returns on a stale
    // provider id, a fetch error, and an empty list). A user with pins saved
    // from a previous session whose fetch fails therefore delegates on the
    // auto-router's pick, which on Copilot can be a Responses-only
    // `gpt-5.x-codex` id that 400s on the Chat endpoint. Unconditional here.
    tools::subagent::set_smart(m.d.smart);
    // The provider those pins are scoped to. Without it a pin rehydrated from
    // settings would be honoured under whatever provider happens to be active
    // at launch, which is exactly the cross-provider dispatch the scoping is
    // there to prevent.
    tools::subagent::set_provider(detail::active_provider_id());
    // Same reasoning for the candidate pool the auto-router ranks over: it is
    // otherwise only ever set from the ModelsLoaded arm, so a failed fetch
    // left workers ranking over an EMPTY list. Seed it with what we have now
    // (the bundled/seeded catalog); ModelsLoaded refreshes it on success.
    tools::subagent::set_candidates(m.d.available_models);
    // …and the PARENT model, for the same reason. main() installs whatever
    // settings.json held, but the cross-provider guard above can REJECT that
    // id (a leftover "qwen2.5-coder:7b" relaunched on Anthropic) and fall back
    // to the seed default. Without this the snapshot keeps the rejected id, so
    // a write-role worker — which inherits the parent model — dispatches the
    // exact id the UI just refused to use. init() is the single source of
    // truth for the resolved model; make the router agree with it.
    tools::subagent::set_model(m.d.model_id.value);
    // Review UI: whether the persistent changes strip renders after edits.
    m.d.show_changes_strip = settings.show_changes_strip;
    m.d.show_reasoning     = settings.show_reasoning;
    // Rehydrate persisted "always allow" tool grants (Zed's always_allow
    // rules). PermissionApproveAlways appends to this list; loading it here
    // means a grant given last week still suppresses the prompt today.
    for (const auto& g : settings.always_allow_tools)
        m.d.session_grants.insert(g);
    for (auto& mi : m.d.available_models)
        for (const auto& fav : settings.favorite_models)
            if (mi.id == fav) mi.favorite = true;

    m.d.current.id  = deps().new_thread_id();
    m.s.status = "ready";

    // Deferred startup Cmds (declared early: the first-run branch below may
    // queue an OpenProviderPicker dispatch).
    std::vector<maya::Cmd<Msg>> cmds;

    // No credentials installed yet → main() invoked install() with an
    // empty header. Open the login modal so the user can authenticate
    // without leaving the TUI; subscribe.cpp routes all input there
    // until they finish (or Esc out, leaving agentty unauth'd — they'll
    // hit a stream error on first send and can /login from the palette).
    //
    // Gate on the active provider being Anthropic: the modal is
    // Anthropic-specific (OAuth / sk-ant key). An OpenAI-family backend
    // authenticates via an env var resolved at startup, so an empty
    // header there means "no key in env" — popping the Anthropic OAuth
    // modal would be nonsensical. Those users get a stream error naming
    // the missing key on first send instead.
    //
    // FIRST-RUN CREDENTIAL DETECTION: before defaulting to the Anthropic
    // modal, scan the registry for a provider that ALREADY has a credential
    // (a pasted key from a previous run, or an env var like GROQ_API_KEY
    // exported in the shell). If one exists, open the PROVIDER PICKER
    // instead — its rows badge exactly where each credential came from
    // ("● key from GROQ_API_KEY"), so the user's literally-first
    // interaction is "oh, it found my key → Enter", not an Anthropic
    // sign-in they never wanted. Plain first-runs (no creds anywhere)
    // keep the classic Anthropic modal — the majority path is unchanged.
    if (auth::is_empty(deps().auth)
        && provider::active().kind == provider::Kind::Anthropic) {
        bool cred_elsewhere = false;
        for (const auto& p : provider::providers()) {
            // Skip the provider we already know is unauthed (the active one).
            if (p.id == provider::default_provider_id()) continue;
            const auto src = provider::auth_source(p, settings);
            if (src == provider::AuthSource::Saved
                || src == provider::AuthSource::Env) {
                cred_elsewhere = true;
                break;
            }
        }
        if (cred_elsewhere)
            cmds.push_back(maya::Cmd<Msg>::after(
                std::chrono::milliseconds{0}, Msg{OpenProviderPicker{}}));
        else
            m.ui.login = ui::login::Picking{};
    }

    // Codex (ChatGPT) is a first-class provider too: if it's the active
    // backend and no ChatGPT credential is saved, land the user directly on
    // the sign-in modal — exactly like Anthropic — rather than failing on the
    // first send. The modal's option 3 runs the native loopback OAuth.
    {
        const auto& sel = provider::active();
        const bool codex_active = sel.is_chatgpt();
        if (codex_active && !provider::chatgpt::responses_available())
            m.ui.login = ui::login::Picking{};
        // Same courtesy for GitHub Copilot: launched with --provider copilot
        // but no GitHub credential yet → open the sign-in modal instead of
        // failing on the first send.
        if (sel.is_copilot() && !provider::copilot::signed_in())
            m.ui.login = ui::login::Picking{.provider = "copilot"};
        if (sel.is_kimi() && !provider::kimi::signed_in())
            m.ui.login = ui::login::Picking{.provider = "kimi"};
    }

    cmds.push_back(cmd::load_threads_async());
    // Connect MCP servers (plugins) at startup on a worker, exactly like
    // threads. This warms the tool surface for the first turn AND lands the
    // snapshot in m.ui.plugins so the Plugins panel is populated the instant
    // it opens — the connection is loop-driven, never a lazy side effect.
    if (mcp::mcp_config_present())
        cmds.push_back(cmd::load_plugins_async(/*reconnect=*/true));

    // OpenAI-family backends (Ollama, llama.cpp, groq, …) have no fixed
    // built-in model list — seed_models() only knows Claude ids. A saved
    // or --provider-restored model id may not be served by the active
    // endpoint (e.g. "qwen2.5-coder:7b" persisted but the daemon never
    // pulled it, or the id belongs to a different backend), which 404s
    // on the very first prompt. Fetch /v1/models eagerly at startup so
    // the ModelsLoaded reducer can swap to a real, served model id
    // BEFORE the user sends anything — the same validation the model
    // picker does, just not gated on the user opening it. Anthropic has
    // a trustworthy built-in seed list, so it skips this round trip.
    if (provider::active().kind == provider::Kind::OpenAI) {
        m.s.models_loading = true;
        cmds.push_back(cmd::fetch_models());
    }

    // Background OAuth refresh handoff. `auth::resolve()` parked a
    // refresh token here when it found expired-but-refreshable creds
    // on disk; pick it up and dispatch the network round trip on a
    // worker so the TUI is interactive immediately. Sticky toast (no
    // expiry) — TokenRefreshed clears it on completion. The
    // oauth_refresh_in_flight flag gates submit_message so the user
    // can't fire a stream with the stale auth_header still in Deps.
    if (auto refresh = auth::take_pending_refresh()) {
        m.s.oauth_refresh_in_flight = true;
        m.s.status = "refreshing OAuth token…";
        m.s.status_until = {};
        cmds.push_back(cmd::refresh_oauth(std::move(*refresh)));
    }

    // Background release check — 24h-cached (the fast path is one small
    // file read on a worker), so this is effectively free on most
    // launches; when a newer release exists, the status bar grows an
    // unobtrusive "⬆ vX.Y.Z" chip and the palette gains "Update agentty".
    // AGENTTY_NO_UPDATE_CHECK=1 (or airgap mode) disables it entirely.
    if (!std::getenv("AGENTTY_NO_UPDATE_CHECK"))
        cmds.push_back(cmd::check_for_update());

    // models.dev capability snapshot: parse the cached copy on a BACKGROUND
    // thread, then refresh over the network (24h-cached fetch of
    // https://models.dev/api.json). The cache is ~4.4 MB of JSON (~7.5k
    // models); parsing it inline here cost the first frame tens of
    // milliseconds of main-thread time — measured as the single largest
    // pre-paint block. The capability registries are mutex-guarded and
    // epoch-versioned (caps_epoch), and every consumer (ladder, badges,
    // wire clamp) re-resolves per frame/request, so facts landing a few
    // frames after first paint are picked up automatically — the same
    // contract the background refresh below has always relied on.
    // Ordering: load_cached FIRST, then refresh — sequenced inside one
    // task so the network copy can never lose to the disk copy.
    // Community-maintained metadata absorbs provider drift between
    // releases; the learned-from-rejection registry still outranks it.
    // Same opt-outs as the release check (airgap installs must not dial
    // out).
    const bool no_net = std::getenv("AGENTTY_NO_UPDATE_CHECK") != nullptr;
    cmds.push_back(maya::Cmd<Msg>::task_isolated(
        [no_net](std::function<void(Msg)>) {
            agentty::modelsdev::load_cached();
            if (!no_net) {
                try { (void)agentty::modelsdev::refresh(); }
                catch (const std::exception& e) {
                    util::dbglog("modelsdev.refresh", e.what());
                } catch (...) {
                    util::dbglog("modelsdev.refresh", "non-std exception");
                }
            }
        }));

    // Prewarm the composer's `@` (files) and `#` (symbols) indices on
    // background threads NOW, so by the time the user types either trigger
    // the picker opens instantly instead of blocking the UI thread on a
    // multi-thousand-path walk / multi-second regex scan.
    prewarm_workspace_files();
    prewarm_workspace_symbols();

    return {std::move(m), maya::Cmd<Msg>::batch(std::move(cmds))};
}

} // namespace agentty::app
