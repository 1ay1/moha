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

    // All tools, every profile. Gating is the policy layer's job
    // (`tool::DynamicDispatch::needs_permission`, called from
    // `kick_pending_tools`) — it inspects the tool's effects against
    // the active profile and decides Allow vs Prompt:
    //   Write   → all Allow  (autonomous)
    //   Ask     → ReadFs/Pure Allow; WriteFs/Exec/Net Prompt
    //   Minimal → only Pure Allow; everything else Prompt
    // Hiding tools from the wire here used to be the gate, but it
    // conflicted with what users expect from "Ask": that the model
    // can attempt anything, the *user* approves per-call. With the
    // filter in place, Ask just made write/edit/bash invisible (no
    // prompt could ever fire because the model never called them) and
    // Minimal sent zero tools at all (model became chat-only). The
    // policy layer is exhaustively static_asserted in policy.hpp; let
    // it own the decision so the three profiles read as users expect.
    for (const auto& t : tools::registry()) {
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});
    }
    req.auth_header = deps().auth_header;
    req.auth_style  = deps().auth_style;

    // Mint a fresh cancel token per turn and stash it on the model so the
    // Esc handler (Msg::CancelStream) can flip it. The worker thread holds
    // its own shared_ptr via req.cancel; reassigning m.s.cancel on the
    // UI thread is safe — both sides only ever load/store the atomic flag.
    req.cancel = std::make_shared<http::CancelToken>();
    m.s.cancel = req.cancel;

    // Reset the stall watchdog baseline at *launch* time (not StreamStarted
    // time) so the threshold counts time since *this* request was issued,
    // not time since some prior stream's last delta. Without this the
    // watchdog inherits a stale `last_event_at` from a long preceding
    // tool-execution phase and trips on the first ~20s of legitimate TTFT
    // when the new stream begins. The Tick handler also rebases while
    // non-streaming, so this is belt-and-suspenders for the brief window
    // between phase=Streaming and the first SSE byte.
    m.s.last_event_at = std::chrono::steady_clock::now();
    m.s.retry_state   = retry::Fresh{};

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
                    dispatch(ToolExecOutput{id, std::move(*result)});
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

Cmd<Msg> compact_thread(std::vector<Message> messages,
                        std::size_t first,
                        std::size_t last) {
    // Why off-thread: Haiku's response is the gating factor (≈ 1-3 s on a
    // dozen-turn window). Doing it on the UI thread freezes the spinner;
    // doing it as a Cmd::task lets the user keep typing/scrolling and
    // see a "compacting…" toast in the status bar.
    return Cmd<Msg>::task(
        [messages = std::move(messages), first, last]
        (std::function<void(Msg)> dispatch) mutable {
            using namespace std::string_literals;

            // System prompt: instruct Haiku to produce a *retrieval-
            // friendly* summary, not a press release. Bullet-style with
            // explicit file paths, decisions, error states, and any
            // open invariants the next turn might need to remember.
            constexpr const char* kCompactorPrompt =
                "You compress a coding-assistant conversation so the "
                "main agent can carry on with much less context. Output "
                "ONE plain-text summary, 250-700 words, no preamble, "
                "no markdown headings. Cover, in this order:\n"
                "  1. The user's overarching goal (1-2 sentences).\n"
                "  2. Decisions made and the rationale (briefly).\n"
                "  3. Every file path mentioned with a one-line note "
                "on what was read / written / discussed.\n"
                "  4. Any open errors, TODOs, or facts the next turn "
                "needs (commands the user is waiting on, library "
                "versions, build state, etc.).\n"
                "Skip pleasantries, skip explaining what tools you would "
                "have called, skip re-pasting code unless a 1-2 line "
                "snippet is load-bearing. Pretend you are the assistant "
                "writing a hand-off note to itself.";

            // Append a final "now summarise" instruction. Putting it in
            // a fresh user-turn keeps the conversation valid (alternation)
            // and signals Haiku that the prior turns are *input*, not its
            // own work to continue.
            Message instruction;
            instruction.role = Role::User;
            instruction.text =
                "Summarise the conversation above per the system "
                "instructions. Return ONLY the summary text."s;
            messages.push_back(std::move(instruction));

            provider::anthropic::Request req;
            // Haiku 4.5 — fastest + cheapest model in the family. The
            // summarisation task is comprehension-heavy but doesn't need
            // a frontier model; Haiku consistently produces dense,
            // accurate hand-off notes within ~2 s.
            req.model         = "claude-haiku-4-5-20251001";
            req.system_prompt = kCompactorPrompt;
            req.messages      = std::move(messages);
            req.tools         = {};                     // no tool calls during compaction
            req.max_tokens    = 4096;
            req.auth_header   = deps().auth_header;
            req.auth_style    = deps().auth_style;

            std::string summary;
            std::string error;
            bool finished = false;

            try {
                provider::anthropic::run_stream_sync(std::move(req), [&](Msg m) {
                    std::visit([&](auto&& e) {
                        using T = std::decay_t<decltype(e)>;
                        if constexpr (std::same_as<T, StreamTextDelta>) {
                            summary += e.text;
                        } else if constexpr (std::same_as<T, StreamFinished>) {
                            finished = true;
                        } else if constexpr (std::same_as<T, StreamError>) {
                            error = e.message;
                            finished = true;
                        }
                    }, m);
                }, /*cancel*/{});
            } catch (const std::exception& e) {
                error = std::string{"compactor threw: "} + e.what();
            } catch (...) {
                error = "compactor threw: unknown exception";
            }

            if (!finished && error.empty())
                error = "compactor: stream ended without a terminal event";

            if (!error.empty()) {
                dispatch(CompactCompleted{std::unexpected(std::move(error)),
                                          first, last});
                return;
            }
            // Strip leading/trailing whitespace; nothing valid is empty.
            while (!summary.empty()
                   && (summary.front() == ' ' || summary.front() == '\n'
                    || summary.front() == '\r' || summary.front() == '\t'))
                summary.erase(0, 1);
            while (!summary.empty()
                   && (summary.back() == ' ' || summary.back() == '\n'
                    || summary.back() == '\r' || summary.back() == '\t'))
                summary.pop_back();
            if (summary.empty()) {
                dispatch(CompactCompleted{
                    std::unexpected(std::string{"compactor returned empty summary"}),
                    first, last});
                return;
            }
            dispatch(CompactCompleted{std::move(summary), first, last});
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
