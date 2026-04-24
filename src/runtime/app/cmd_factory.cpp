#include "moha/runtime/app/cmd_factory.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

#include "moha/auth/auth.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/app/update/internal.hpp"
#include "moha/io/http.hpp"
#include "moha/provider/anthropic/transport.hpp"
#include "moha/tool/registry.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tool.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app::cmd {

using maya::Cmd;

Cmd<Msg> launch_stream(Model& m) {
    provider::Request req;
    req.model         = m.d.model_id.value;
    req.system_prompt = provider::anthropic::default_system_prompt();
    req.messages      = m.d.current.messages;

    if (m.d.profile != Profile::Minimal) {
        for (const auto& t : tools::registry()) {
            // Ask profile hides the mutating/executing trio from the wire
            // so the model can't even request them — it must ask the user
            // to switch profile first. Closed-set Kind dispatch: adding
            // another gated tool is an enum-arm change, not a new string
            // compare to add here.
            if (m.d.profile == Profile::Ask) {
                auto k = tools::spec::kind_of(t.name.value);
                if (k && (*k == tools::spec::Kind::Write
                       || *k == tools::spec::Kind::Edit
                       || *k == tools::spec::Kind::Bash))
                    continue;
            }
            req.tools.push_back({t.name.value, t.description, t.input_schema,
                                 t.eager_input_streaming});
        }
    }
    req.auth_header = deps().auth_header;
    req.auth_style  = deps().auth_style;

    // Mint a fresh cancel token per turn and stash it on the model so the
    // Esc handler (Msg::CancelStream) can flip it. The worker thread holds
    // its own shared_ptr via req.cancel; reassigning m.s.cancel on the
    // UI thread is safe — both sides only ever load/store the atomic flag.
    req.cancel = std::make_shared<http::CancelToken>();
    m.s.cancel = req.cancel;

    return Cmd<Msg>::task([req = std::move(req)](std::function<void(Msg)> dispatch) mutable {
        try {
            deps().stream(std::move(req), [dispatch](Msg m) {
                dispatch(std::move(m));
            });
        } catch (const std::exception& e) {
            // The stream backend threw before producing a terminal event —
            // surface it as StreamError so the UI doesn't hang on the spinner.
            dispatch(StreamError{std::string{"stream backend: "} + e.what()});
        } catch (...) {
            dispatch(StreamError{"stream backend: unknown exception"});
        }
    });
}

Cmd<Msg> run_tool(ToolCallId id, ToolName tool_name, nlohmann::json args) {
    return Cmd<Msg>::task(
        [id = std::move(id),
         name = std::move(tool_name),
         args = std::move(args)]
        (std::function<void(Msg)> dispatch) {
            // Install a thread-local progress sink *before* dispatch so the
            // subprocess runner inside the tool can stream stdout+stderr to
            // the UI as bytes arrive. RAII scope guarantees the sink is
            // cleared even if the tool throws, so the next tool run can't
            // inherit a stale dispatch lambda.
            moha::tools::progress::Scope progress_scope{
                [dispatch, id](std::string_view snapshot) {
                    dispatch(ToolExecProgress{id, std::string{snapshot}});
                }};
            try {
                auto result = tool::DynamicDispatch::execute(name.value, args);
                if (result) {
                    dispatch(ToolExecOutput{id, std::move(result->text)});
                } else {
                    dispatch(ToolExecOutput{id,
                        std::unexpected(std::move(result).error())});
                }
            } catch (const std::exception& e) {
                // DynamicDispatch already catches tool exceptions, but guard
                // against anything in the dispatch infrastructure itself so
                // the tool never gets stuck in Running with no terminal Msg.
                dispatch(ToolExecOutput{id, std::unexpected(
                    tools::ToolError::unknown(
                        std::string{"dispatch error: "} + e.what()))});
            } catch (...) {
                dispatch(ToolExecOutput{id, std::unexpected(
                    tools::ToolError::unknown("dispatch error: unknown exception"))});
            }
        });
}

Cmd<Msg> kick_pending_tools(Model& m) {
    if (m.d.current.messages.empty()) return Cmd<Msg>::none();
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return Cmd<Msg>::none();

    std::vector<Cmd<Msg>> cmds;
    bool any_pending = false;

    // Effect-based scheduler. When the assistant emits multiple tool
    // calls in one turn they all start Pending; maya's BG worker pool
    // runs Task cmds on independent threads, so any set of tools we
    // dispatch in this tick runs concurrently. `is_parallel_safe`
    // (moha/tool/effects.hpp) encodes the capability rule: Pure /
    // ReadFs / Net compose freely; WriteFs and Exec demand exclusive
    // access. We accumulate `active_effects` across the sweep so each
    // candidate is checked against *everything* already running AND
    // the siblings we've just promoted within this same batch.
    //
    // Deferred (conflicted) tools stay Pending. When the blocking
    // tool's ToolExecOutput lands, update.cpp re-fires
    // kick_pending_tools and the now-unblocked siblings advance.
    tools::EffectSet active_effects;
    auto effects_of = [](const ToolName& n) -> tools::EffectSet {
        if (const auto* sp = tools::spec::lookup(n.value)) return sp->effects;
        // Unknown tool: treat as if it requires exclusive access so we
        // never parallelise something whose effects we can't reason about.
        return tools::EffectSet{{tools::Effect::Exec}};
    };
    for (const auto& tc : last.tool_calls)
        if (tc.is_running()) active_effects |= effects_of(tc.name);

    for (auto& tc : last.tool_calls) {
        // Approved: user already granted permission; advance it exactly
        // like a Pending-but-no-permission-needed tool, minus the
        // permission check. Keeps the effect-parallel gate as the single
        // source of truth for scheduling.
        const bool ready = tc.is_pending() || tc.is_approved();
        if (ready) {
            const bool needs_perm = tc.is_approved()
                ? false
                : tool::DynamicDispatch::needs_permission(tc.name.value, m.d.profile);
            if (needs_perm && !m.d.pending_permission) {
                m.d.pending_permission = PendingPermission{
                    tc.id, tc.name,
                    "Tool " + tc.name.value + " needs permission under "
                        + std::string{ui::profile_label(m.d.profile)} + " profile"};
                m.s.phase = phase::AwaitingPermission{};
                return Cmd<Msg>::none();
            }
            if (!needs_perm) {
                // Effect-compatibility gate. Defer this tool if another
                // running sibling demands exclusive access or this tool
                // itself demands exclusive access while anything is
                // already running. kick_pending_tools re-fires on every
                // terminal ToolExecOutput, so deferred siblings advance
                // automatically without explicit requeueing.
                const auto want = effects_of(tc.name);
                if (!tools::is_parallel_safe(active_effects, want)) {
                    any_pending = true;
                    continue;
                }

                // started_at was stamped at StreamToolUseStart so the
                // timer covers the full card lifetime (args streaming +
                // execution). Preserve it as we move into Running.
                tc.status = ToolUse::Running{tc.started_at(), {}};
                active_effects |= want;
                cmds.push_back(run_tool(tc.id, tc.name, tc.args));

                // Wall-clock watchdog, configured per-tool from the spec
                // catalog (`spec::lookup(name)->max_seconds`). The worker
                // thread can't be pre-empted from here (no portable
                // thread cancellation), but we CAN move the UI on if the
                // worker is wedged in a blocking syscall (slow NFS, dead
                // FUSE mount, hung network FS). After the timeout fires,
                // the tool's Running state is force-flipped to Failed;
                // the late ToolExecOutput from the eventual unwind is
                // discarded by apply_tool_output's idempotent guard.
                //
                // max_seconds == 0 means "no overlay timeout" — bash /
                // diagnostics run via subprocess.cpp which has its own
                // strict timeout, and stacking ours on top would
                // truncate legitimate long commands. The spec catalog's
                // static_assert guarantees only those two have 0.
                if (const auto* sp = tools::spec::lookup(tc.name.value);
                    sp && sp->max_seconds > std::chrono::seconds{0}) {
                    cmds.push_back(Cmd<Msg>::after(
                        sp->max_seconds,
                        Msg{ToolTimeoutCheck{tc.id}}));
                }

                // ExecutingTool is non-Idle, so active() flips on automatically:
                // the Tick subscription stays armed, the spinner advances, the
                // view's live elapsed timer keeps ticking. Without that the
                // UI looks frozen on long-running bash commands.
                m.s.phase = phase::ExecutingTool{};
                any_pending = true;
            }
        } else if (tc.is_running()) {
            any_pending = true;
        }
    }

    if (!any_pending) {
        const bool has_results = std::ranges::any_of(last.tool_calls, [](const auto& tc){
            return tc.is_terminal();
        });
        if (has_results) {
            // Tool results are going back to the model — a fresh sub-turn
            // is about to start on the same assistant message. This is a
            // natural slice point: tool-heavy turns can grow the transcript
            // several messages in one logical "turn" from the user's POV,
            // and the submit_message virtualization hook never sees those.
            // Slicing here keeps the live canvas bounded even inside a
            // single long multi-tool assistant turn.
            auto virt = detail::maybe_virtualize(m);
            m.s.phase = phase::Streaming{};
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            if (!virt.is_none()) cmds.push_back(std::move(virt));
            cmds.push_back(launch_stream(m));
        } else {
            // Idle drops active() to false, stopping the Tick subscription.
            m.s.phase = phase::Idle{};
        }
    }
    return Cmd<Msg>::batch(std::move(cmds));
}

Cmd<Msg> fetch_models() {
    return Cmd<Msg>::task([](std::function<void(Msg)> dispatch) {
        try {
            auto models = provider::anthropic::list_models(deps().auth_header, deps().auth_style);
            dispatch(ModelsLoaded{std::move(models)});
        } catch (const std::exception& e) {
            dispatch(StreamError{std::string{"models fetch: "} + e.what()});
        } catch (...) {
            dispatch(StreamError{"models fetch: unknown exception"});
        }
    });
}

Cmd<Msg> open_browser_async(std::string url) {
    return Cmd<Msg>::task([url = std::move(url)](std::function<void(Msg)>) {
        // No dispatch — the reducer doesn't care whether the browser
        // launched. The user can always paste auth_url manually from
        // the modal if their default opener is broken.
        auth::open_browser(url);
    });
}

Cmd<Msg> oauth_exchange(auth::OAuthCode    code,
                        auth::PkceVerifier verifier,
                        auth::OAuthState   state) {
    return Cmd<Msg>::task(
        [code = std::move(code),
         verifier = std::move(verifier),
         state = std::move(state)]
        (std::function<void(Msg)> dispatch) {
            try {
                auto r = auth::exchange_code(code, verifier, state);
                dispatch(LoginExchanged{std::move(r)});
            } catch (const std::exception& e) {
                dispatch(LoginExchanged{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    std::string{"exchange threw: "} + e.what()})});
            } catch (...) {
                dispatch(LoginExchanged{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    "exchange threw: unknown exception"})});
            }
        });
}

} // namespace moha::app::cmd
