// moha::app::update — pure (Model, Msg) -> (Model, Cmd<Msg>) reducer.
//
// This file is the orchestrator: a single std::visit with one overload per
// Msg variant, grouped by domain. The heavy lifting lives in
// update/{stream,modal,tool}.cpp (see update/internal.hpp for declarations)
// so this file stays walkable as a top-level map of every event the runtime
// can react to.

#include "moha/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>

#include "moha/provider/error_class.hpp"
#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/picker.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/tool/spec.hpp"

namespace moha::app {

using maya::Cmd;
using maya::overload;
using json = nlohmann::json;
namespace pick = moha::ui::pick;

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    return std::visit(overload{
        // ── Composer ────────────────────────────────────────────────────
        [&](ComposerCharInput e) -> Step {
            auto utf8 = ui::utf8_encode(e.ch);
            m.ui.composer.text.insert(m.ui.composer.cursor, utf8);
            m.ui.composer.cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](ComposerBackspace) -> Step {
            if (m.ui.composer.cursor > 0 && !m.ui.composer.text.empty()) {
                int p = ui::utf8_prev(m.ui.composer.text, m.ui.composer.cursor);
                m.ui.composer.text.erase(p, m.ui.composer.cursor - p);
                m.ui.composer.cursor = p;
            }
            return done(std::move(m));
        },
        [&](ComposerEnter)  { return detail::submit_message(std::move(m)); },
        [&](ComposerSubmit) { return detail::submit_message(std::move(m)); },
        [&](ComposerNewline) -> Step {
            m.ui.composer.text.insert(m.ui.composer.cursor, "\n");
            m.ui.composer.cursor += 1;
            m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerToggleExpand) -> Step {
            m.ui.composer.expanded = !m.ui.composer.expanded;
            return done(std::move(m));
        },
        [&](ComposerCursorLeft) -> Step {
            m.ui.composer.cursor = ui::utf8_prev(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorRight) -> Step {
            m.ui.composer.cursor = ui::utf8_next(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorHome) -> Step {
            m.ui.composer.cursor = 0;
            return done(std::move(m));
        },
        [&](ComposerCursorEnd) -> Step {
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            return done(std::move(m));
        },
        [&](ComposerPaste& e) -> Step {
            m.ui.composer.text.insert(m.ui.composer.cursor, e.text);
            m.ui.composer.cursor += static_cast<int>(e.text.size());
            if (e.text.find('\n') != std::string::npos) m.ui.composer.expanded = true;
            return done(std::move(m));
        },

        // ── Stream events ───────────────────────────────────────────────
        // Every event handler bumps `last_event_at` so the Tick-based
        // stall watchdog can tell "stream is alive but quiet" from
        // "stream is stalled." A small helper keeps the bump uniform.
        [&](StreamStarted) -> Step {
            auto now = std::chrono::steady_clock::now();
            m.s.started          = now;
            m.s.last_event_at    = now;
            m.s.retry_state      = retry::Fresh{};       // fresh stream → re-arm watchdog
            // Fresh stream is alive — wipe any leftover toast (retry
            // countdown, "error: …", "cancelled") from the previous
            // attempt so the status row doesn't show a stale message
            // on top of a healthy connection.
            m.s.status.clear();
            m.s.status_until = {};
            // Reset the live-rate accumulator so each sub-turn (post-tool)
            // measures its own generation speed instead of polluting the
            // average with the previous turn's bytes.
            m.s.live_delta_bytes = 0;
            m.s.first_delta_at = {};
            // Wipe the sparkline ring so each fresh stream starts with an
            // empty bar. The Tick handler refills it as bytes arrive.
            m.s.rate_history.fill(0.0f);
            m.s.rate_history_pos = 0;
            m.s.rate_history_full = false;
            m.s.rate_last_sample_at = {};
            m.s.rate_last_sample_bytes = 0;
            return done(std::move(m));
        },
        [&](StreamTextDelta& e) -> Step {
            m.s.last_event_at = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& st = m.d.current.messages.back().streaming_text;
                if (st.size() < detail::kMaxStreamingBytes) {
                    const auto room = detail::kMaxStreamingBytes - st.size();
                    if (e.text.size() <= room) st += e.text;
                    else                       st.append(e.text, 0, room);
                }
            }
            if (!e.text.empty()) {
                if (m.s.first_delta_at.time_since_epoch().count() == 0)
                    m.s.first_delta_at = m.s.last_event_at;
                m.s.live_delta_bytes += e.text.size();
            }
            return done(std::move(m));
        },
        [&](StreamToolUseStart& e) -> Step {
            m.s.last_event_at = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                ToolUse tc;
                tc.id   = e.id;
                tc.name = e.name;
                tc.args = json::object();
                // Stamp start now so the card shows a live timer during the
                // args-streaming phase too — lets the user tell "model hasn't
                // started emitting" from "execution is slow" at a glance.
                tc.status = ToolUse::Pending{m.s.last_event_at};
                m.d.current.messages.back().tool_calls.push_back(std::move(tc));
            }
            return done(std::move(m));
        },
        [&](StreamToolUseDelta& e) -> Step {
            m.s.last_event_at = std::chrono::steady_clock::now();
            if (!e.partial_json.empty()) {
                if (m.s.first_delta_at.time_since_epoch().count() == 0)
                    m.s.first_delta_at = m.s.last_event_at;
                m.s.live_delta_bytes += e.partial_json.size();
            }
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant
                && !m.d.current.messages.back().tool_calls.empty()) {
                auto& tc = m.d.current.messages.back().tool_calls.back();
                // Bounded append — beyond the cap we drop further bytes so
                // the salvage path at StreamToolUseEnd runs on whatever
                // scalars sniffed cleanly.
                if (tc.args_streaming.size() < detail::kMaxStreamingBytes) {
                    const auto room = detail::kMaxStreamingBytes - tc.args_streaming.size();
                    if (e.partial_json.size() <= room) tc.args_streaming += e.partial_json;
                    else tc.args_streaming.append(e.partial_json, 0, room);
                }
                // Throttle the live preview. First delta runs unconditionally
                // so the header paints immediately, then subsequent re-parses
                // are spaced ~120 ms. StreamToolUseEnd always runs the full
                // parse, so the final state is exact.
                using clock = std::chrono::steady_clock;
                constexpr auto kPreviewInterval = std::chrono::milliseconds{120};
                auto now = clock::now();
                if (tc.last_preview_at.time_since_epoch().count() == 0
                    || now - tc.last_preview_at >= kPreviewInterval) {
                    detail::update_stream_preview(tc);
                    tc.last_preview_at = now;
                }
            }
            return done(std::move(m));
        },
        [&](StreamToolUseEnd) -> Step {
            m.s.last_event_at = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant
                && !m.d.current.messages.back().tool_calls.empty()) {
                auto& tc = m.d.current.messages.back().tool_calls.back();
                // Empty args_streaming is legitimate for argumentless tools;
                // args was seeded to {} at StreamToolUseStart.
                if (!tc.args_streaming.empty()) {
                    try {
                        tc.args = json::parse(tc.args_streaming);
                        tc.mark_args_dirty();
                        std::string{}.swap(tc.args_streaming);
                    } catch (const std::exception& ex) {
                        // Parse failed — typically an SSE cutoff mid-content.
                        // Salvage whatever scalar fields we can so the tool
                        // still has a shot at running instead of nuking the
                        // whole turn.
                        auto salvaged = detail::salvage_args(tc);
                        if (!salvaged.empty()) {
                            tc.args = std::move(salvaged);
                            tc.mark_args_dirty();
                            std::string{}.swap(tc.args_streaming);
                        } else {
                            auto now = std::chrono::steady_clock::now();
                            tc.status = ToolUse::Failed{
                                tc.started_at(), now,
                                std::string{"invalid tool arguments: "} + ex.what()
                                    + "\nraw: " + tc.args_streaming};
                            std::string{}.swap(tc.args_streaming);
                        }
                    }
                }
                // Required-field check is deferred to finalize_turn so the
                // turn-level retry logic owns the single decision point.
            }
            return done(std::move(m));
        },
        [&](StreamUsage& e) -> Step {
            m.s.last_event_at = std::chrono::steady_clock::now();
            // `input_tokens` from Anthropic is the FULL prefix for this
            // request, NOT the delta. Accumulating across turns triple-counted
            // by turn 5. Replace, don't add. Cache fields are excluded from
            // `input_tokens` per the API but still occupy the context window,
            // so the true "tokens in context" is the sum.
            if (e.input_tokens || e.cache_read_input_tokens
                || e.cache_creation_input_tokens) {
                m.s.tokens_in = e.input_tokens
                                   + e.cache_read_input_tokens
                                   + e.cache_creation_input_tokens;
            }
            if (e.output_tokens) m.s.tokens_out = e.output_tokens;
            return done(std::move(m));
        },
        [&](StreamHeartbeat) -> Step {
            // Wire-alive signal from the transport (SSE `ping` or
            // `thinking_delta`). No payload, no UI effect — we just
            // bump last_event_at so the stall watchdog knows the
            // connection is healthy. Critical during extended-thinking
            // passes where the model reasons silently for 60-120 s
            // between visible deltas; without this the watchdog would
            // fire on every non-trivial opus turn.
            m.s.last_event_at = std::chrono::steady_clock::now();
            return done(std::move(m));
        },
        [&](StreamFinished e) -> Step {
            auto cmd = detail::finalize_turn(m, e.stop_reason);
            return {std::move(m), std::move(cmd)};
        },
        [&](StreamError& e) -> Step {
            // Dedupe: when the stall watchdog fired, it tripped the
            // cancel token, which causes the worker thread to unwind
            // and emit its own StreamError("cancelled") shortly after.
            // The first error already scheduled a retry; ignore any
            // subsequent ones that arrive before that retry runs,
            // otherwise we'd race two worker threads into the same
            // session.
            if (m.s.in_scheduled()) return done(std::move(m));

            // Worker thread is unwinding; drop the token so the next turn
            // (or scheduled retry) mints a fresh one.
            m.s.cancel.reset();

            // Move any partial streaming_text into the message body so
            // the assistant's in-flight output isn't lost regardless of
            // what we do next (retry or terminal).
            Message* last = nullptr;
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                last = &m.d.current.messages.back();
                if (!last->streaming_text.empty()) {
                    if (last->text.empty()) last->text = std::move(last->streaming_text);
                    else                    last->text += std::move(last->streaming_text);
                    std::string{}.swap(last->streaming_text);
                }
            }

            // Classify and decide retry vs terminal.
            auto klass = provider::classify(e.message);
            // If the stall watchdog fired this turn, the worker thread
            // will eventually unwind and emit StreamError("cancelled")
            // — that's our doing, not the user's. Force-classify it as
            // Transient so the retry machinery treats it as a recoverable
            // upstream stall, not a user cancel.
            if (m.s.in_stall_fired()
                && klass == provider::ErrorClass::Cancelled) {
                klass = provider::ErrorClass::Transient;
            }

            // "Committed work" gating for retry: only Done/Running tool
            // calls + finalized text body block a retry. A Pending tool
            // (StreamToolUseStart fired, args may have been mid-streaming
            // when the stall hit) is NOT committed — re-running gives
            // the model a chance to re-emit it cleanly. Same definition
            // the truncation-retry path uses (see update/stream.cpp).
            bool has_committed = false;
            if (last) {
                has_committed = !last->text.empty() ||
                    std::ranges::any_of(last->tool_calls, [](const auto& tc) {
                        return tc.is_done() || tc.is_running();
                    });
            }
            bool can_retry = (klass == provider::ErrorClass::Transient
                           || klass == provider::ErrorClass::RateLimit)
                          && m.s.transient_retries < provider::kMaxRetries
                          && !has_committed;

            if (can_retry) {
                int attempt = m.s.transient_retries++;
                auto delay = provider::backoff(klass, attempt);
                // Round up to whole seconds for the user-visible countdown.
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    delay + std::chrono::milliseconds{999}).count();
                m.s.status = std::string{provider::to_string(klass)}
                           + " — retrying in " + std::to_string(secs) + "s…";
                // Toast: auto-clear shortly after the retry fires so the
                // banner doesn't linger once the new stream is healthy.
                // The retry itself overwrites `status` on StreamStarted
                // if it succeeds; this stamp is the fallback if it
                // doesn't (e.g. the next stream also stalls).
                m.s.status_until = std::chrono::steady_clock::now()
                                 + delay + std::chrono::milliseconds{1500};
                // Phase=Streaming keeps active() true → Tick subscription
                // stays armed, the user sees the countdown / can Esc.
                m.s.phase = phase::Streaming{};
                m.s.retry_state = retry::Scheduled{};  // dedupes follow-up StreamErrors
                // Replace the half-formed turn with a fresh placeholder.
                // launch_stream's contract is "caller has appended an
                // Assistant placeholder" — every stream-event handler
                // checks `back().role == Assistant` and silently drops
                // events otherwise. Without this swap, the retry stream
                // fires into a User-tail thread, every event is dropped,
                // and the user stares at a permanently-blank turn.
                if (last) m.d.current.messages.pop_back();
                Message placeholder;
                placeholder.role = Role::Assistant;
                m.d.current.messages.push_back(std::move(placeholder));
                return {std::move(m),
                    Cmd<Msg>::after(delay, Msg{RetryStream{}})};
            }

            // Terminal path — phase=Idle drops active() to false.
            m.s.phase = phase::Idle{};
            // Cancellation gets a clean "cancelled" status, no "error:".
            if (klass == provider::ErrorClass::Cancelled) {
                m.s.status = "cancelled";
            } else {
                m.s.status = "error: " + e.message;
            }
            if (last) {
                // Record the failure as the message's error field, NOT
                // appended to text — keeps the partial body and the
                // failure reason rendering distinctly. Cancellation
                // doesn't mark the message as errored (the user's own
                // intent isn't a failure).
                if (klass != provider::ErrorClass::Cancelled)
                    last->error = e.message;
                // Pending tool calls will never dispatch; mark Failed.
                for (auto& tc : last->tool_calls) {
                    if (tc.is_pending()) {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            "stream ended before tool args closed"};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
            }
            // Toast: terminal banners auto-dismiss after a few seconds.
            // The per-message `error` field keeps the detail in the
            // transcript, so the status bar doesn't need to hold it.
            // Schedule via Cmd::after so the banner disappears even
            // though the Tick subscription shuts off with phase=Idle.
            {
                auto now = std::chrono::steady_clock::now();
                auto ttl = std::chrono::seconds{
                    klass == provider::ErrorClass::Cancelled ? 3 : 6};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}})};
            }
        },
        [&](CompactRequested) -> Step {
            return detail::request_compact(std::move(m));
        },
        [&](CompactCompleted& e) -> Step {
            return detail::apply_compact(std::move(m), std::move(e));
        },
        [&](RetryStream) -> Step {
            // Scheduled retry fired. Transition back to Fresh so the
            // freshly-launched stream's own errors flow through the
            // normal classifier path. If the user cancelled during the
            // wait (Esc → CancelStream dropped phase to Idle), do nothing.
            m.s.retry_state = retry::Fresh{};
            if (m.s.is_idle()) return done(std::move(m));
            // Re-launch on the same context. launch_stream will mint a
            // new cancel token, append a placeholder assistant message,
            // and re-issue the request.
            return {std::move(m), cmd::launch_stream(m)};
        },
        [&](CancelStream) -> Step {
            // Trip the token; the http worker notices within ~200 ms and
            // unwinds, eventually dispatching StreamError("cancelled") which
            // does the actual phase/state cleanup. Don't touch phase here —
            // doing so would race the in-flight stream's last few events.
            //
            // ALSO covers the retry-backoff window: if the user hits Esc
            // while we're sleeping for a scheduled retry, this clears
            // active so the RetryStream Msg becomes a no-op when it fires.
            if (m.s.cancel) m.s.cancel->cancel();
            m.s.phase = phase::Idle{};
            m.s.status = "cancelled";
            {
                auto now = std::chrono::steady_clock::now();
                auto ttl = std::chrono::seconds{3};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                m.s.retry_state = retry::Fresh{};   // drop any Scheduled/StallFired
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}})};
            }
        },

        // ── Live tool progress (streaming subprocess output) ────────────
        // Arrives from the subprocess runner every ~80 ms with the full
        // accumulated output so far. We just set it — no Cmd to return —
        // and rely on the existing Tick subscription (active during
        // ExecutingTool) to re-render. Ignore if the tool has already
        // finalised (a late snapshot racing the terminal ToolExecOutput).
        [&](ToolExecProgress& e) -> Step {
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == e.id) {
                        if (auto* r = std::get_if<ToolUse::Running>(&tc.status))
                            r->progress_text = std::move(e.snapshot);
                    }
            return done(std::move(m));
        },

        // ── Per-tool wall-clock watchdog ────────────────────────────────
        // Scheduled by kick_pending_tools when a tool transitions to
        // Running, with the deadline pulled from the spec catalog
        // (`spec::lookup(name)->max_seconds`). Force-fails the tool
        // if it's still Running when this fires — the worker thread
        // keeps going (we can't safely cancel a blocking syscall from
        // here), but the UI moves on. apply_tool_output's idempotent
        // guard discards the late result if the worker eventually
        // unwinds. The error message names the actual deadline so the
        // user knows whether to expect a quick recovery or to retry.
        [&](ToolTimeoutCheck& e) -> Step {
            bool flipped = false;
            for (auto& msg_ : m.d.current.messages) {
                for (auto& tc : msg_.tool_calls) {
                    if (tc.id == e.id && tc.is_running()) {
                        auto now = std::chrono::steady_clock::now();
                        const auto* sp = tools::spec::lookup(tc.name.value);
                        auto secs = sp ? sp->max_seconds : std::chrono::seconds{0};
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            "tool execution exceeded " + std::to_string(secs.count())
                            + " s wall-clock — likely hung on a blocking "
                            "syscall (slow/dead filesystem mount, network "
                            "freeze, or worker deadlock). The tool's worker "
                            "thread may continue in the background; its "
                            "result will be discarded if it ever returns."};
                        flipped = true;
                    }
                }
            }
            if (!flipped) return done(std::move(m));
            // Force-failure means kick_pending_tools needs to pick up
            // the next pending tool, send results back to the model,
            // or settle to idle. Same path ToolExecOutput uses.
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Step {
            // todo's side effect on the UI's todo modal — runs only when
            // the call actually succeeded; failures don't synthesise a
            // todo list. The expected<> shape makes that one `if` cover
            // both the discriminator and the success-value extraction.
            if (e.result) {
                for (const auto& msg_ : m.d.current.messages)
                    for (const auto& tc : msg_.tool_calls)
                        if (tc.id == e.id && tc.name == "todo") {
                            auto todos = tc.args.value("todos", json::array());
                            m.ui.todo.items.clear();
                            for (const auto& td : todos) {
                                TodoItem item;
                                item.content = td.value("content", "");
                                auto st = td.value("status", "pending");
                                item.status = st == "completed"   ? TodoStatus::Completed
                                            : st == "in_progress" ? TodoStatus::InProgress
                                                                  : TodoStatus::Pending;
                                m.ui.todo.items.push_back(std::move(item));
                            }
                        }
            }
            detail::apply_tool_output(m, e.id, std::move(e.result));
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Permission ──────────────────────────────────────────────────
        [&](PermissionApprove) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == id) {
                        // Mark the approval as type state: Pending → Approved.
                        // kick_pending_tools then treats Approved as "permission
                        // already granted" and routes it through the same
                        // effect-parallel gate as a non-permissioned tool —
                        // so if a sibling ReadFs is still running, a freshly
                        // approved WriteFs/Exec waits for it to finish instead
                        // of racing.
                        tc.status = ToolUse::Approved{tc.started_at()};
                    }
            m.d.pending_permission.reset();
            return {std::move(m), cmd::kick_pending_tools(m)};
        },
        [&](PermissionReject) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            detail::mark_tool_rejected(m, id, "User rejected this tool call.");
            m.d.pending_permission.reset();
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },
        [&](PermissionApproveAlways) -> Step {
            // MVP: same as Approve. Sticky grants TBD.
            return update(std::move(m), PermissionApprove{});
        },

        // ── Model picker ────────────────────────────────────────────────
        [&](OpenModelPicker) -> Step {
            int idx = 0;
            for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                if (m.d.available_models[i].id == m.d.model_id) idx = i;
            m.ui.model_picker = pick::OpenAt{idx};
            return {std::move(m), cmd::fetch_models()};
        },
        [&](ModelsLoaded& e) -> Step {
            if (e.models.empty()) return done(std::move(m));
            auto settings = deps().load_settings();
            m.d.available_models.clear();
            for (auto& mi : e.models) {
                for (const auto& fav : settings.favorite_models)
                    if (mi.id == fav) mi.favorite = true;
                m.d.available_models.push_back(std::move(mi));
            }
            if (auto* p = pick::opened(m.ui.model_picker)) {
                p->index = 0;
                for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                    if (m.d.available_models[i].id == m.d.model_id) p->index = i;
            }
            return done(std::move(m));
        },
        [&](CloseModelPicker) -> Step {
            m.ui.model_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ModelPickerMove& e) -> Step {
            if (m.d.available_models.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            int sz = static_cast<int>(m.d.available_models.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ModelPickerSelect) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (p && !m.d.available_models.empty()) {
                m.d.model_id = m.d.available_models[p->index].id;
                // Update the per-model context cap so the status-bar ctx
                // % bar reflects the right denominator for the new model
                // (1 M for `[1m]` variants, 200 K otherwise).
                m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
                detail::persist_settings(m);
            }
            m.ui.model_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ModelPickerToggleFavorite) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (p && !m.d.available_models.empty()) {
                auto& mi = m.d.available_models[p->index];
                mi.favorite = !mi.favorite;
            }
            return done(std::move(m));
        },

        // ── Thread list ─────────────────────────────────────────────────
        [&](OpenThreadList) -> Step {
            m.d.threads = deps().load_threads();
            m.ui.thread_list = pick::OpenAt{0};
            return done(std::move(m));
        },
        [&](CloseThreadList) -> Step {
            m.ui.thread_list = pick::Closed{};
            return done(std::move(m));
        },
        [&](ThreadListMove& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.thread_list);
            if (!p) return done(std::move(m));
            int sz = static_cast<int>(m.d.threads.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListSelect) -> Step {
            auto* p = pick::opened(m.ui.thread_list);
            if (p && !m.d.threads.empty())
                m.d.current = m.d.threads[p->index];
            m.ui.thread_list = pick::Closed{};
            // A freshly-loaded thread has no prev-frame correspondence in
            // the inline buffer — render everything that fits in the window.
            int total = static_cast<int>(m.d.current.messages.size());
            m.ui.thread_view_start = std::max(0, total - detail::kViewWindow);
            return done(std::move(m));
        },
        [&](NewThread) -> Step {
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            m.d.current = Thread{};
            m.d.current.id = deps().new_thread_id();
            m.d.current.created_at = m.d.current.updated_at = std::chrono::system_clock::now();
            m.ui.thread_list = pick::Closed{};
            m.ui.command_palette = palette::Closed{};
            m.ui.composer.text.clear();
            m.ui.composer.cursor = 0;
            m.s.phase = phase::Idle{};
            m.ui.thread_view_start = 0;
            return done(std::move(m));
        },

        // ── Command palette ─────────────────────────────────────────────
        [&](OpenCommandPalette) -> Step {
            m.ui.command_palette = palette::Open{};
            return done(std::move(m));
        },
        [&](CloseCommandPalette) -> Step {
            m.ui.command_palette = palette::Closed{};
            return done(std::move(m));
        },
        [&](CommandPaletteInput& e) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (o && static_cast<uint32_t>(e.ch) < 0x80) {
                o->query.push_back(static_cast<char>(e.ch));
                // Reset cursor to the top of the (newly filtered) list so
                // the previous index doesn't point at a now-hidden row.
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](CommandPaletteBackspace) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (o && !o->query.empty()) {
                o->query.pop_back();
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](CommandPaletteMove& e) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (!o) return done(std::move(m));
            // Clamp against the *visible* row count, not kCommands.size().
            // Without the upper bound the cursor used to walk off-screen
            // and Enter would silently fall through to the no-match path.
            int sz = static_cast<int>(filtered_commands(o->query).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](CommandPaletteSelect) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (!o) return done(std::move(m));
            // Resolve cursor → typed Command via the SAME filtered list
            // the view rendered. The previous design switched on the raw
            // o->index against the unfiltered enum, which silently fired
            // the wrong command whenever any query was active.
            auto matches = filtered_commands(o->query);
            m.ui.command_palette = palette::Closed{};
            if (matches.empty()
                || o->index < 0
                || o->index >= static_cast<int>(matches.size()))
                return done(std::move(m));
            switch (matches[static_cast<std::size_t>(o->index)]->id) {
                case Command::NewThread:     return update(std::move(m), NewThread{});
                case Command::ReviewChanges: return update(std::move(m), OpenDiffReview{});
                case Command::AcceptAll:     return update(std::move(m), AcceptAllChanges{});
                case Command::RejectAll:     return update(std::move(m), RejectAllChanges{});
                case Command::CycleProfile:  return update(std::move(m), CycleProfile{});
                case Command::OpenModels:    return update(std::move(m), OpenModelPicker{});
                case Command::OpenThreads:   return update(std::move(m), OpenThreadList{});
                case Command::OpenPlan:      return update(std::move(m), OpenTodoModal{});
                case Command::Compact:       return update(std::move(m), CompactRequested{});
                case Command::Quit:          return update(std::move(m), Quit{});
            }
            return done(std::move(m));
        },

        // ── Todo modal ──────────────────────────────────────────────────
        [&](OpenTodoModal) -> Step {
            m.ui.todo.open = pick::OpenModal{};
            return done(std::move(m));
        },
        [&](CloseTodoModal) -> Step {
            m.ui.todo.open = pick::Closed{};
            return done(std::move(m));
        },
        [&](UpdateTodos& e) -> Step {
            m.ui.todo.items = std::move(e.items);
            return done(std::move(m));
        },

        // ── In-app login modal ──────────────────────────────────────────
        [&](OpenLogin)              -> Step { return detail::open_login(std::move(m)); },
        [&](CloseLogin)             -> Step { return detail::close_login(std::move(m)); },
        [&](LoginPickMethod& e)     -> Step { return detail::login_pick_method(std::move(m), e.key); },
        [&](LoginCharInput& e)      -> Step { return detail::login_char_input(std::move(m), e.ch); },
        [&](LoginBackspace)         -> Step { return detail::login_backspace(std::move(m)); },
        [&](LoginPaste& e)          -> Step { return detail::login_paste(std::move(m), std::move(e.text)); },
        [&](LoginCursorLeft)        -> Step { return detail::login_cursor_left(std::move(m)); },
        [&](LoginCursorRight)       -> Step { return detail::login_cursor_right(std::move(m)); },
        [&](LoginSubmit)            -> Step { return detail::login_submit(std::move(m)); },
        [&](LoginExchanged& e)      -> Step { return detail::login_exchanged(std::move(m), std::move(e.result)); },

        // ── Profile ─────────────────────────────────────────────────────
        [&](CycleProfile) -> Step {
            m.d.profile = m.d.profile == Profile::Write   ? Profile::Ask
                      : m.d.profile == Profile::Ask     ? Profile::Minimal
                                                       : Profile::Write;
            detail::persist_settings(m);
            return done(std::move(m));
        },

        // ── Diff review ─────────────────────────────────────────────────
        [&](OpenDiffReview) -> Step {
            // Tell the user when there's nothing to review instead of
            // silently doing nothing — opening an empty pane would just
            // flicker the screen and leave them confused about whether
            // their keystroke registered.
            if (m.d.pending_changes.empty()) {
                auto cmd = detail::set_status_toast(m, "no pending changes to review");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.diff_review = ui::pick::TwoAxis{pick::OpenAtCell{0, 0}};
            return done(std::move(m));
        },
        [&](CloseDiffReview) -> Step {
            m.ui.diff_review = pick::Closed{};
            return done(std::move(m));
        },
        [&](DiffReviewMove& e) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            auto& fc = m.d.pending_changes[c->file_index];
            int sz = static_cast<int>(fc.hunks.size());
            if (sz == 0) return done(std::move(m));
            c->hunk_index = (c->hunk_index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](DiffReviewNextFile) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index + 1) % sz;
            c->hunk_index = 0;
            return done(std::move(m));
        },
        [&](DiffReviewPrevFile) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index - 1 + sz) % sz;
            c->hunk_index = 0;
            return done(std::move(m));
        },
        [&](AcceptHunk) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (c && !m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[c->file_index];
                if (!fc.hunks.empty())
                    fc.hunks[c->hunk_index].status = Hunk::Status::Accepted;
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (c && !m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[c->file_index];
                if (!fc.hunks.empty())
                    fc.hunks[c->hunk_index].status = Hunk::Status::Rejected;
            }
            return done(std::move(m));
        },
        [&](AcceptAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = detail::set_status_toast(m, "no pending changes to accept");
                return {std::move(m), std::move(cmd)};
            }
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) {
                    h.status = Hunk::Status::Accepted;
                    ++hunks;
                }
            auto cmd = detail::set_status_toast(m,
                "accepted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks"));
            return {std::move(m), std::move(cmd)};
        },
        [&](RejectAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = detail::set_status_toast(m, "no pending changes to reject");
                return {std::move(m), std::move(cmd)};
            }
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) {
                    h.status = Hunk::Status::Rejected;
                    ++hunks;
                }
            m.d.pending_changes.clear();
            m.ui.diff_review = pick::Closed{};
            auto cmd = detail::set_status_toast(m,
                "rejected " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks"));
            return {std::move(m), std::move(cmd)};
        },

        // ── Misc ────────────────────────────────────────────────────────
        [&](RestoreCheckpoint&) -> Step {
            m.s.status = "checkpoint restore not implemented yet";
            return done(std::move(m));
        },
        [&](ScrollThread& e) -> Step {
            m.ui.thread_scroll = std::max(0, m.ui.thread_scroll + e.delta);
            return done(std::move(m));
        },
        [&](ToggleToolExpanded& e) -> Step {
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == e.id) tc.expanded = !tc.expanded;
            return done(std::move(m));
        },
        [&](Tick) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (m.s.last_tick.time_since_epoch().count() == 0) m.s.last_tick = now;
            auto tick_gap = now - m.s.last_tick;
            float dt = std::chrono::duration<float>(tick_gap).count();
            m.s.last_tick = now;
            if (m.s.active()) m.s.spinner.advance(dt);

            // ── Stream-stall watchdog ──────────────────────────────────
            // The transport now emits `StreamHeartbeat` on SSE `ping`
            // frames (every 10-15 s while the connection is up) and on
            // `thinking_delta` blocks (fired continuously while the
            // model reasons silently). So a healthy stream bumps
            // last_event_at at least every ~15 s regardless of what
            // the model is doing. 120 s of total silence is
            // overwhelmingly likely to be a wedged transport (silent
            // peer, proxy stall, half-open TCP) rather than legitimate
            // model behaviour.
            //
            // We still pair this with the HTTP layer's 90 s idle +
            // 15 s PING probe: the HTTP watchdog catches dead sockets
            // even when the reducer's Tick subscription is quiet (e.g.
            // an AwaitingPermission detour), the reducer watchdog
            // catches the case where PING ACKs keep the HTTP clock
            // happy but the application layer never advances.
            //
            // Clock-skew guard: Tick is scheduled every 33 ms, but on
            // long threads a render pass can block the UI thread for
            // hundreds of ms — sometimes multiple seconds on pathological
            // transcripts with many tool cards. When that happens the
            // stream looks "silent" from our clock's perspective even
            // though the worker thread has been pushing deltas into the
            // background queue the whole time (they just haven't been
            // drained yet). If we see a Tick gap well above the nominal
            // interval, rebase `last_event_at` forward by the observed
            // stall so one slow frame can't synthesize a spurious
            // stream-stalled error. The HTTP idle watchdog still catches
            // a genuinely dead wire — this is strictly about not blaming
            // the network for our own render backpressure.
            constexpr auto kTickRebaseThreshold = std::chrono::seconds(2);
            if (tick_gap >= kTickRebaseThreshold
                && m.s.last_event_at.time_since_epoch().count() != 0) {
                m.s.last_event_at += tick_gap;
            }
            constexpr auto kStallSecs = std::chrono::seconds(120);
            if (m.s.is_streaming() && m.s.active()
                && m.s.in_fresh()
                && m.s.last_event_at.time_since_epoch().count() != 0
                && now - m.s.last_event_at >= kStallSecs) {
                m.s.retry_state = retry::StallFired{};
                if (m.s.cancel) m.s.cancel->cancel();
                auto since = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - m.s.last_event_at).count();
                // User-facing message kept generic — they see a "retrying
                // in Ns…" toast seconds later, which tells the useful
                // story. Internal detail is in the StreamError payload
                // which the classifier reads.
                std::string msg = "stream stalled — no events for "
                                + std::to_string(since) + "s";
                // Schedule via Cmd::after(0) so the StreamError flows
                // through the same reducer arm as a real wire error,
                // including the typed retry path. Direct invocation of
                // the handler from here would skip the classifier.
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::milliseconds(0),
                    Msg{StreamError{std::move(msg)}})};
            }

            // Sample tok/s into the sparkline ring every ~500 ms while the
            // stream is actively producing bytes. Sampling slower than the
            // spinner tick keeps the bar reading as "trend" rather than
            // "noise"; sampling faster would show every transient
            // edge-batching artifact. Skip until the first delta arrives so
            // the leading bar isn't an artificial zero-stretch.
            if (m.s.is_streaming() && m.s.active()
                && m.s.first_delta_at.time_since_epoch().count() != 0) {
                constexpr auto kSampleInterval = std::chrono::milliseconds{500};
                if (m.s.rate_last_sample_at.time_since_epoch().count() == 0) {
                    m.s.rate_last_sample_at    = now;
                    m.s.rate_last_sample_bytes = m.s.live_delta_bytes;
                } else if (now - m.s.rate_last_sample_at >= kSampleInterval) {
                    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - m.s.rate_last_sample_at).count();
                    auto bytes_delta = (m.s.live_delta_bytes >= m.s.rate_last_sample_bytes)
                                       ? (m.s.live_delta_bytes - m.s.rate_last_sample_bytes)
                                       : 0;
                    // ~4 B/token (Claude tokenizer avg) and convert ms to s.
                    float rate = window_ms > 0
                               ? (static_cast<float>(bytes_delta) / 4.0f)
                                 * (1000.0f / static_cast<float>(window_ms))
                               : 0.0f;
                    m.s.rate_history[m.s.rate_history_pos] = rate;
                    m.s.rate_history_pos =
                        (m.s.rate_history_pos + 1) % StreamState::kRateSamples;
                    if (m.s.rate_history_pos == 0) m.s.rate_history_full = true;
                    m.s.rate_last_sample_at    = now;
                    m.s.rate_last_sample_bytes = m.s.live_delta_bytes;
                }
            }
            return done(std::move(m));
        },
        [&](Quit) -> Step {
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            return {std::move(m), Cmd<Msg>::quit()};
        },
        [&](NoOp) -> Step { return done(std::move(m)); },
        [&](ClearStatus& e) -> Step {
            // No-op if the user (or another handler) wrote a newer
            // status since this cleaner was scheduled — stamps won't
            // match, so the current banner stays.
            if (m.s.status_until == e.stamp) {
                m.s.status.clear();
                m.s.status_until = {};
            }
            return done(std::move(m));
        },
    }, msg);
}

} // namespace moha::app
