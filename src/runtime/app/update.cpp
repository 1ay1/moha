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

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app {

using maya::Cmd;
using maya::overload;
using json = nlohmann::json;

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
        [&](StreamStarted) -> Step {
            m.s.active = true;
            m.s.started = std::chrono::steady_clock::now();
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
                    m.s.first_delta_at = std::chrono::steady_clock::now();
                m.s.live_delta_bytes += e.text.size();
            }
            return done(std::move(m));
        },
        [&](StreamToolUseStart& e) -> Step {
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                ToolUse tc;
                tc.id   = e.id;
                tc.name = e.name;
                tc.args = json::object();
                // Stamp start now so the card shows a live timer during the
                // args-streaming phase too — lets the user tell "model hasn't
                // started emitting" from "execution is slow" at a glance.
                tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                m.d.current.messages.back().tool_calls.push_back(std::move(tc));
            }
            return done(std::move(m));
        },
        [&](StreamToolUseDelta& e) -> Step {
            if (!e.partial_json.empty()) {
                if (m.s.first_delta_at.time_since_epoch().count() == 0)
                    m.s.first_delta_at = std::chrono::steady_clock::now();
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
        [&](StreamFinished e) -> Step {
            auto cmd = detail::finalize_turn(m, e.stop_reason);
            return {std::move(m), std::move(cmd)};
        },
        [&](StreamError& e) -> Step {
            m.s.active = false;
            m.s.phase = phase::Idle{};
            m.s.status = "error: " + e.message;
            // Worker thread is unwinding; drop the token so the next turn
            // mints a fresh one.
            m.s.cancel.reset();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& last = m.d.current.messages.back();
                if (!last.streaming_text.empty()) {
                    if (last.text.empty()) last.text = std::move(last.streaming_text);
                    else                   last.text += std::move(last.streaming_text);
                    std::string{}.swap(last.streaming_text);
                }
                if (last.text.empty() && last.tool_calls.empty()) {
                    last.text = "\u26A0 " + e.message;
                } else {
                    if (!last.text.empty() && last.text.back() != '\n') last.text += '\n';
                    last.text += "\u26A0 " + e.message;
                }
                // Any tool_call still Pending at stream-error time will never
                // dispatch — mark them Failed so the UI doesn't spin forever.
                // Running tools are in-flight on a worker thread; leave them.
                for (auto& tc : last.tool_calls) {
                    if (tc.is_pending()) {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            "stream ended before tool args closed"};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
            }
            return done(std::move(m));
        },
        [&](CancelStream) -> Step {
            // Trip the token; the http worker notices within ~200 ms and
            // unwinds, eventually dispatching StreamError("cancelled") which
            // does the actual phase/state cleanup. Don't touch phase here —
            // doing so would race the in-flight stream's last few events.
            if (m.s.cancel) m.s.cancel->cancel();
            m.s.status = "cancelling…";
            return done(std::move(m));
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

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Step {
            for (const auto& msg_ : m.d.current.messages)
                for (const auto& tc : msg_.tool_calls)
                    if (tc.id == e.id && tc.name == "todo" && !e.error) {
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
            detail::apply_tool_output(m, e.id, std::move(e.output), e.error);
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Permission ──────────────────────────────────────────────────
        [&](PermissionApprove) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            std::vector<Cmd<Msg>> cmds;
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == id) {
                        // started_at stays at StreamToolUseStart time — the
                        // card shows total lifetime including the permission
                        // wait, which is exactly what the user cares about.
                        tc.status = ToolUse::Running{tc.started_at(), {}};
                        cmds.push_back(cmd::run_tool(tc.id, tc.name, tc.args));
                    }
            m.d.pending_permission.reset();
            m.s.phase = phase::ExecutingTool{};
            m.s.active = true;  // keep Tick alive for live elapsed counter
            return {std::move(m), Cmd<Msg>::batch(std::move(cmds))};
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
            m.ui.model_picker.open = true;
            for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                if (m.d.available_models[i].id == m.d.model_id) m.ui.model_picker.index = i;
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
            m.ui.model_picker.index = 0;
            for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                if (m.d.available_models[i].id == m.d.model_id) m.ui.model_picker.index = i;
            return done(std::move(m));
        },
        [&](CloseModelPicker) -> Step {
            m.ui.model_picker.open = false;
            return done(std::move(m));
        },
        [&](ModelPickerMove& e) -> Step {
            if (m.d.available_models.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.available_models.size());
            m.ui.model_picker.index = (m.ui.model_picker.index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ModelPickerSelect) -> Step {
            if (!m.d.available_models.empty()) {
                m.d.model_id = m.d.available_models[m.ui.model_picker.index].id;
                // Update the per-model context cap so the status-bar ctx
                // % bar reflects the right denominator for the new model
                // (1 M for `[1m]` variants, 200 K otherwise).
                m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
                detail::persist_settings(m);
            }
            m.ui.model_picker.open = false;
            return done(std::move(m));
        },
        [&](ModelPickerToggleFavorite) -> Step {
            if (!m.d.available_models.empty()) {
                auto& mi = m.d.available_models[m.ui.model_picker.index];
                mi.favorite = !mi.favorite;
            }
            return done(std::move(m));
        },

        // ── Thread list ─────────────────────────────────────────────────
        [&](OpenThreadList) -> Step {
            m.ui.thread_list.open = true;
            m.d.threads = deps().load_threads();
            m.ui.thread_list.index = 0;
            return done(std::move(m));
        },
        [&](CloseThreadList) -> Step {
            m.ui.thread_list.open = false;
            return done(std::move(m));
        },
        [&](ThreadListMove& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.threads.size());
            m.ui.thread_list.index = (m.ui.thread_list.index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListSelect) -> Step {
            if (!m.d.threads.empty()) m.d.current = m.d.threads[m.ui.thread_list.index];
            m.ui.thread_list.open = false;
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
            m.ui.thread_list.open = false;
            m.ui.command_palette.open = false;
            m.ui.composer.text.clear();
            m.ui.composer.cursor = 0;
            m.s.phase = phase::Idle{};
            m.ui.thread_view_start = 0;
            return done(std::move(m));
        },

        // ── Command palette ─────────────────────────────────────────────
        [&](OpenCommandPalette) -> Step {
            m.ui.command_palette.open = true;
            m.ui.command_palette.query.clear();
            m.ui.command_palette.index = 0;
            return done(std::move(m));
        },
        [&](CloseCommandPalette) -> Step {
            m.ui.command_palette.open = false;
            return done(std::move(m));
        },
        [&](CommandPaletteInput& e) -> Step {
            if (static_cast<uint32_t>(e.ch) < 0x80)
                m.ui.command_palette.query.push_back(static_cast<char>(e.ch));
            return done(std::move(m));
        },
        [&](CommandPaletteBackspace) -> Step {
            if (!m.ui.command_palette.query.empty()) m.ui.command_palette.query.pop_back();
            return done(std::move(m));
        },
        [&](CommandPaletteMove& e) -> Step {
            m.ui.command_palette.index = std::max(0, m.ui.command_palette.index + e.delta);
            return done(std::move(m));
        },
        [&](CommandPaletteSelect) -> Step {
            m.ui.command_palette.open = false;
            switch (m.ui.command_palette.index) {
                case 0: return update(std::move(m), NewThread{});
                case 1: return update(std::move(m), OpenDiffReview{});
                case 2: return update(std::move(m), AcceptAllChanges{});
                case 3: return update(std::move(m), RejectAllChanges{});
                case 4: return update(std::move(m), CycleProfile{});
                case 5: return update(std::move(m), OpenModelPicker{});
                case 6: return update(std::move(m), OpenThreadList{});
                case 7: return update(std::move(m), OpenTodoModal{});
                case 8: return update(std::move(m), Quit{});
            }
            return done(std::move(m));
        },

        // ── Todo modal ──────────────────────────────────────────────────
        [&](OpenTodoModal) -> Step {
            m.ui.todo.open = true;
            return done(std::move(m));
        },
        [&](CloseTodoModal) -> Step {
            m.ui.todo.open = false;
            return done(std::move(m));
        },
        [&](UpdateTodos& e) -> Step {
            m.ui.todo.items = std::move(e.items);
            return done(std::move(m));
        },

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
            m.ui.diff_review.open = !m.d.pending_changes.empty();
            m.ui.diff_review.file_index = 0;
            m.ui.diff_review.hunk_index = 0;
            return done(std::move(m));
        },
        [&](CloseDiffReview) -> Step {
            m.ui.diff_review.open = false;
            return done(std::move(m));
        },
        [&](DiffReviewMove& e) -> Step {
            if (m.d.pending_changes.empty()) return done(std::move(m));
            auto& fc = m.d.pending_changes[m.ui.diff_review.file_index];
            int sz = static_cast<int>(fc.hunks.size());
            if (sz == 0) return done(std::move(m));
            m.ui.diff_review.hunk_index = (m.ui.diff_review.hunk_index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](DiffReviewNextFile) -> Step {
            if (m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            m.ui.diff_review.file_index = (m.ui.diff_review.file_index + 1) % sz;
            m.ui.diff_review.hunk_index = 0;
            return done(std::move(m));
        },
        [&](DiffReviewPrevFile) -> Step {
            if (m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            m.ui.diff_review.file_index = (m.ui.diff_review.file_index - 1 + sz) % sz;
            m.ui.diff_review.hunk_index = 0;
            return done(std::move(m));
        },
        [&](AcceptHunk) -> Step {
            if (!m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[m.ui.diff_review.file_index];
                if (!fc.hunks.empty())
                    fc.hunks[m.ui.diff_review.hunk_index].status = Hunk::Status::Accepted;
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            if (!m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[m.ui.diff_review.file_index];
                if (!fc.hunks.empty())
                    fc.hunks[m.ui.diff_review.hunk_index].status = Hunk::Status::Rejected;
            }
            return done(std::move(m));
        },
        [&](AcceptAllChanges) -> Step {
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) h.status = Hunk::Status::Accepted;
            return done(std::move(m));
        },
        [&](RejectAllChanges) -> Step {
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) h.status = Hunk::Status::Rejected;
            m.d.pending_changes.clear();
            m.ui.diff_review.open = false;
            return done(std::move(m));
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
            float dt = std::chrono::duration<float>(now - m.s.last_tick).count();
            m.s.last_tick = now;
            if (m.s.active) m.s.spinner.advance(dt);

            // Sample tok/s into the sparkline ring every ~500 ms while the
            // stream is actively producing bytes. Sampling slower than the
            // spinner tick keeps the bar reading as "trend" rather than
            // "noise"; sampling faster would show every transient
            // edge-batching artifact. Skip until the first delta arrives so
            // the leading bar isn't an artificial zero-stretch.
            if (m.s.is_streaming() && m.s.active
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
    }, msg);
}

} // namespace moha::app
