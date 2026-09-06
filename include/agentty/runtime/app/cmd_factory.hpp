#pragma once
// agentty::app::cmd — factories for the side-effecting commands the runtime issues.
//
// These wrap maya's Cmd<Msg> with agentty-specific glue: kicking off a streaming
// turn, executing a tool, advancing pending tool execution after a turn ends.

#include <maya/maya.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/io/http.hpp"   // http::CancelTokenPtr (run_tool)

namespace agentty::app::cmd {

// ── The per-turn routing DECISION ────────────────────────────────
//
// Which model serves this turn, at what effort, and why. ONE value, computed
// once by resolve_turn_routing() — the 🧠 card renders it and launch_stream
// dispatches it.
//
// The two used to derive it INDEPENDENTLY from the same Model, each running
// the same scan, the same classifier and the same effort scaler. The comment
// on both said "same call as launch_stream so card == wire", and keeping that
// true meant passing every argument identically at two sites forever. It did
// not stay true: a threshold added to the classifier reached one site and not
// the other, so the card advertised a route the request never took, and the
// only thing that noticed was a test written specifically to look for it.
//
// With one value there is nothing to keep in step. card == wire stops being an
// invariant under test and becomes an identity.
struct TurnRouting {
    // Empty model ⇒ orchestration is off and the plain selection serves the
    // turn; the card is not shown at all.
    bool                    orchestrate = false;
    std::string             model;      // resolved Strategic model ("" = selection)
    Effort                  base = Effort::None;     // BEFORE complexity scaling
    Effort                  effort = Effort::None;   // AFTER complexity scaling
    smart::ComplexityScore  cx{};       // the classification that scaled it
    smart::ComplexityScore  cx_text{};  // text-only score, for lift provenance
    bool                    subagents = false;

    // The prompt the decision was made about. Held so the card can NAME why a
    // tier was lifted (payload / continuation / correction) without re-walking
    // the transcript and risking a scan that drifts from the one above.
    std::string             prompt;
};

// THE routing decision for the turn about to launch. Pure: reads the
// transcript, the catalog and m.d.smart, and writes nothing.
//
// Safe to call at submit and read at launch even on the DEFERRED proactive
// path, where retrieval splices a context message in between: the newest-user
// scan skips proactive-context, 🧠-card and fork-note messages, which is
// everything that can land in that window. smart_routing_card_test pins it.
[[nodiscard]] TurnRouting resolve_turn_routing(const Model& m);

// Mutates `m` to install a fresh cancel token in m.stream.cancel, then
// dispatches the streaming task on a worker. Esc (CancelStream) flips the
// token to abort the in-flight stream.
[[nodiscard]] maya::Cmd<Msg> launch_stream(Model& m);

// Build the Smart Mode ROUTING CARD for the turn about to launch, or
// std::nullopt when orchestration is off (nothing to show). A synthetic,
// wire-inert Message (smart_routing=true) the caller inserts into the
// transcript just before the assistant placeholder so the user sees, as a
// first-class thread event, which model + effort the turn was routed to, the
// classified complexity that scaled it, and which layers are active. Pure;
// mints a fresh MessageId. Renders `routing` — the SAME value launch_stream
// dispatches.
[[nodiscard]] std::optional<Message> build_smart_routing_card(const Model& m);

// What the model actually sees on the next request: applies any
// Thread::CompactionRecord substitution (latest record's summary
// replaces messages[0..up_to_index) on the wire). Mirrors what
// launch_stream's normal-turn branch ships. Callers use this when
// they need to reason about the wire payload size or shape — the
// auto-compaction triggers in particular need to estimate the
// COMPACTED prefix, not the raw transcript, otherwise they re-fire
// immediately after every compaction.
[[nodiscard]] std::vector<Message> wire_messages_for(const Thread& t);

// Bytes-based prefix token estimate computed against the wire view
// (i.e. with compaction substitution applied). Same approximation as
// `estimate_prefix_tokens(Thread)` but the right denominator for
// auto-compaction logic and the context-gauge.
[[nodiscard]] int estimate_wire_tokens(const Thread& t);

[[nodiscard]] maya::Cmd<Msg> run_tool(ToolCallId id,
                                      ToolName tool_name,
                                      nlohmann::json args,
                                      http::CancelTokenPtr cancel = {});

// Inspect the latest assistant turn and either fire off pending tool calls,
// request permission, or kick the follow-up stream once tool results are in.
// Mutates `m` (sets phase, may push a placeholder assistant message).
[[nodiscard]] maya::Cmd<Msg> kick_pending_tools(Model& m);

// Resolve every PENDING *salvaged* tool call in the back assistant message
// that byte-duplicates a call already terminal earlier in the same agent
// turn, marking it Failed-without-side-effects instead of letting it run a
// second time. Salvaged calls (synthetic `call_salvaged_` ids minted by the
// OpenAI-compat transport when a weak local model leaks a tool call into the
// `content` channel) are the only ones deduped — structured calls are the
// model's deliberate intent. Returns the number deduped. Called by
// kick_pending_tools before any promotion to Running; exposed for tests.
std::size_t dedup_releaked_salvage_calls(Model& m);

// ── Path-aware parallel tool scheduling (pure; exposed for tests) ────────────
// Given a batch of tool calls (the model's emission for one turn) and the set
// already RUNNING, decide which currently-pending calls may be promoted to run
// CONCURRENTLY this tick. The decision refines the coarse effect-only rule
// (is_parallel_safe) with PATH analysis: two writers to disjoint files — or a
// read of a.c alongside a write of b.c — run in parallel instead of
// serialising. Conflicts (overlapping paths, any Exec, or a writer whose path
// can't be extracted) stay pending and advance on the next kick once the
// blocker settles. Submission order is preserved: a call never jumps ahead of
// an earlier conflicting call. `running` and `pending` index into the same
// logical batch; returns the subset of `pending` indices safe to start now.
struct SchedDecision {
    std::vector<std::size_t> promote;   // pending indices to start this tick
};
[[nodiscard]] SchedDecision schedule_parallel_batch(
    const std::vector<ToolUse>& batch);

// ── Doom-loop circuit breaker (pure; exposed for tests) ──────────────────────
// Weak local models (qwen2.5-coder, codellama, …) routinely fall into a
// non-converging tool loop: they pick the wrong tool for a goal (e.g. `read`
// on a URL or a file that doesn't exist), get an error result, and re-issue a
// near-identical call indefinitely. With no native completion signal the main
// agent loop would spin until the user hits Esc — the symptom behind the
// "tool usage is fucked" reports. Mirrors the iteration / repeated-failure
// caps every serious local-agent framework ships (Qwen-Agent, aider, cline).
//
// Given the full message history of the CURRENT agent turn (the run since the
// last real User message), returns a non-empty nudge string when the loop
// should be force-stopped, or std::nullopt to keep going. Two triggers:
//   (1) REPEAT: the same (tool, args) call appears >= kRepeatLimit times and
//       its results were failures — the model is stuck re-trying a dead call.
//       ALWAYS enforced (every production tool does this: aider's
//       max_reflections, MindStudio's "2–3 attempts then stop"). A capable
//       model that genuinely loops on a dead call benefits from it too.
//   (2) RUNAWAY: the run has made >= kMaxToolTurns assistant tool-call turns
//       without ever producing a plain-text answer — unbounded spend. Gated
//       behind `enforce_step_cap`. No production tool applies a hard
//       successful-step cap to a CAPABLE model by default: Claude Code's
//       max_turns is UNLIMITED by default, aider never step-caps. So the
//       caller passes enforce_step_cap=true ONLY for weak local models
//       (qwen2.5-coder/codellama) and false for Claude / capable hosted
//       models, which run until they finish on their own.
// The returned text is surfaced to the model as the final assistant turn so
// it can recover gracefully (and the user sees why the loop stopped).
struct LoopBreak {
    std::string reason;     // user/model-facing explanation
};
[[nodiscard]] std::optional<LoopBreak> agent_loop_should_break(
    const std::vector<Message>& messages,
    bool enforce_step_cap = true);

[[nodiscard]] maya::Cmd<Msg> fetch_models();

// Fetch a SPECIFIC provider's catalog without switching to it (fused picker
// fan-out). Dispatches FusedCatalogLoaded{spec, models, ok}.
[[nodiscard]] maya::Cmd<Msg> fetch_models_for(std::string spec);

// ── Self-update ─────────────────────────────────────────────
// Background release check (24h-cached, never blocks a frame): dispatches
// UpdateCheckDone. check_for_update() is safe to fire on every launch.
[[nodiscard]] maya::Cmd<Msg> check_for_update();
// Background models.dev capability-snapshot refresh (24h disk cache); pushes
// declared reasoning + exact effort enums into the catalog registries. Fire
// and forget — dispatches no Msg (registries are read at render/wire time).
[[nodiscard]] maya::Cmd<Msg> refresh_modelsdev();
// Download + atomically install the given version; dispatches UpdateApplied.
[[nodiscard]] maya::Cmd<Msg> perform_self_update(std::string version);

// ── In-app login modal ──────────────────────────────────────────────────
// Fire-and-forget: shells out to the platform browser opener. Wrapped in
// Cmd::task so a wedged xdg-open / open / ShellExecute can never block
// the reducer tick.
[[nodiscard]] maya::Cmd<Msg> open_browser_async(std::string url);

// Run the OAuth code-exchange HTTP POST off the UI thread. Dispatches
// LoginExchanged{result} on completion regardless of success/failure —
// the reducer matches on `expected<OAuthToken, OAuthError>` to decide
// whether to install creds or transition to Failed.
[[nodiscard]] maya::Cmd<Msg> oauth_exchange(auth::OAuthCode    code,
                                            auth::PkceVerifier verifier,
                                            auth::OAuthState   state);

// Run the OAuth refresh HTTP POST off the UI thread. Dispatched from
// `AgenttyApp::init()` when `auth::take_pending_refresh()` returned a
// stashed token (i.e. on-disk creds were expired but had a refresh
// token). The TUI is already drawn by the time this runs, so the user
// sees a sticky "refreshing OAuth token…" toast in the bottom row
// instead of the old pre-TUI stderr line, and startup is no longer
// gated on the network round trip.
[[nodiscard]] maya::Cmd<Msg> refresh_oauth(std::string refresh_token);

// Allocate a process-unique identity for a ChatGPT login attempt. Async
// progress/completion must carry it so an abandoned attempt cannot mutate a
// newer login modal.
[[nodiscard]] std::uint64_t next_codex_login_attempt_id() noexcept;

// Connect-probe a custom host off the UI thread: dial its model list
// (configured path → /v1/models → Ollama /api/tags), detect the dialect,
// dispatch HostProbed. Shares next_codex_login_attempt_id() so a stale
// probe result (user Esc'd / resubmitted) is dropped by the reducer.
[[nodiscard]] maya::Cmd<Msg> probe_host_async(
    std::string spec, std::uint64_t attempt_id, auth::AuthHeader auth);

// Kick native ChatGPT OAuth off the UI thread. The attempt id correlates all
// async messages; `cancel` is tripped when Esc closes that exact modal.
[[nodiscard]] maya::Cmd<Msg> codex_login_async(
    std::uint64_t attempt_id, std::shared_ptr<std::atomic_bool> cancel);

// Kick a native OAuth device-flow login off the UI thread — provider-generic
// (GitHub Copilot, Kimi, …). `provider` is the registry id, `provider_label`
// the display name for the success toast. Shares next_codex_login_attempt_id()
// for correlation; dispatches DeviceCodeReady then DeviceLoginDone.
[[nodiscard]] maya::Cmd<Msg> device_login_async(
    std::string provider, std::string provider_label,
    std::uint64_t attempt_id, std::shared_ptr<std::atomic_bool> cancel);

// Walk ~/.agentty/threads/ and parse every thread JSON off the UI thread.
// Dispatches `ThreadsLoaded{vec}` on completion. The directory walk +
// parse can take seconds with hundreds of multi-MB files in real-world
// use, so it runs as a background task instead of blocking startup;
// `init()` returns immediately with an empty thread list.
[[nodiscard]] maya::Cmd<Msg> load_threads_async();

// Connect/snapshot the MCP servers off the UI thread and dispatch
// `PluginsUpdated{model}` when done — the reducer stores it in m.ui.plugins,
// the single source the Plugins panel renders. reconnect=true respawns +
// re-handshakes (add/remove/toggle/first open); reconnect=false just
// snapshots the live pool. Mirrors load_threads_async.
[[nodiscard]] maya::Cmd<Msg> load_plugins_async(bool reconnect);

// Parse a single thread's JSON off the UI thread. Dispatched from the
// thread picker's Enter handler so the synchronous ~30ms-per-thread
// parse doesn't land between the keypress and the next paint.
// Dispatches `ThreadLoaded{thread}` on success; on failure (file
// vanished, parse error) dispatches a `ThreadLoaded` with an empty
// Thread so the reducer can no-op gracefully without leaving the
// `thread_loading` flag stuck.
[[nodiscard]] maya::Cmd<Msg> load_thread_async(ThreadId id);

} // namespace agentty::app::cmd
