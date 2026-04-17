#include "moha/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>

#include "moha/app/cmd_factory.hpp"
#include "moha/app/deps.hpp"
#include "moha/view/helpers.hpp"

namespace moha::app {

using maya::Cmd;
using maya::overload;
using json = nlohmann::json;

namespace {

using Step = std::pair<Model, Cmd<Msg>>;
inline Step done(Model m) { return {std::move(m), Cmd<Msg>::none()}; }

// ── Composer ──────────────────────────────────────────────────────────────

Step submit_message(Model m) {
    if (m.composer.text.empty()) return done(std::move(m));

    if (m.stream.phase == Phase::Streaming || m.stream.phase == Phase::ExecutingTool) {
        m.composer.queued.push_back(std::exchange(m.composer.text, {}));
        m.composer.cursor = 0;
        return done(std::move(m));
    }

    Message user;
    user.role = Role::User;
    user.text = std::exchange(m.composer.text, {});
    m.composer.cursor = 0;
    if (m.current.title.empty())
        m.current.title = deps().title_from(user.text);
    m.current.messages.push_back(std::move(user));

    Message placeholder;
    placeholder.role = Role::Assistant;
    m.current.messages.push_back(std::move(placeholder));

    m.current.updated_at = std::chrono::system_clock::now();
    m.stream.phase = Phase::Streaming;
    m.stream.active = true;
    auto cmd = cmd::launch_stream(m);
    return {std::move(m), std::move(cmd)};
}

Cmd<Msg> finalize_turn(Model& m) {
    m.stream.active = false;
    if (!m.current.messages.empty()) {
        auto& last = m.current.messages.back();
        if (last.role == Role::Assistant && !last.streaming_text.empty()) {
            if (last.text.empty()) last.text = std::move(last.streaming_text);
            else                   last.text += std::move(last.streaming_text);
            last.streaming_text.clear();
        }
    }
    deps().save_thread(m.current);
    auto kp = cmd::kick_pending_tools(m);

    if (m.stream.phase == Phase::Idle && !m.composer.queued.empty()) {
        m.composer.text = m.composer.queued.front();
        m.composer.queued.erase(m.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
    }
    return kp;
}

void persist_settings(const Model& m) {
    persistence::Settings s;
    s.model_id = m.model_id;
    s.profile  = m.profile;
    for (const auto& mi : m.available_models)
        if (mi.favorite) s.favorite_models.push_back(mi.id);
    deps().save_settings(s);
}

// ── Tool exec result handling ─────────────────────────────────────────────

void apply_tool_output(Model& m, const ToolCallId& id, std::string&& output, bool error) {
    for (auto& msg : m.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                tc.output = std::move(output);
                tc.status = error ? ToolUse::Status::Error : ToolUse::Status::Done;
            }
}

void mark_tool_status_by_id(Model& m, const ToolCallId& id,
                            ToolUse::Status status,
                            std::string_view output_if_set) {
    for (auto& msg : m.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                tc.status = status;
                if (!output_if_set.empty()) tc.output = std::string{output_if_set};
            }
}

} // namespace

// ── The reducer ───────────────────────────────────────────────────────────

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    return std::visit(overload{
        // ── Composer ────────────────────────────────────────────────────
        [&](ComposerCharInput e) -> Step {
            auto utf8 = ui::utf8_encode(e.ch);
            m.composer.text.insert(m.composer.cursor, utf8);
            m.composer.cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](ComposerBackspace) -> Step {
            if (m.composer.cursor > 0 && !m.composer.text.empty()) {
                int p = ui::utf8_prev(m.composer.text, m.composer.cursor);
                m.composer.text.erase(p, m.composer.cursor - p);
                m.composer.cursor = p;
            }
            return done(std::move(m));
        },
        [&](ComposerEnter)  { return submit_message(std::move(m)); },
        [&](ComposerSubmit) { return submit_message(std::move(m)); },
        [&](ComposerNewline) -> Step {
            m.composer.text.insert(m.composer.cursor, "\n");
            m.composer.cursor += 1;
            m.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerToggleExpand) -> Step {
            m.composer.expanded = !m.composer.expanded;
            return done(std::move(m));
        },
        [&](ComposerCursorLeft) -> Step {
            m.composer.cursor = ui::utf8_prev(m.composer.text, m.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorRight) -> Step {
            m.composer.cursor = ui::utf8_next(m.composer.text, m.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorHome) -> Step {
            m.composer.cursor = 0;
            return done(std::move(m));
        },
        [&](ComposerCursorEnd) -> Step {
            m.composer.cursor = static_cast<int>(m.composer.text.size());
            return done(std::move(m));
        },
        [&](ComposerPaste& e) -> Step {
            m.composer.text.insert(m.composer.cursor, e.text);
            m.composer.cursor += static_cast<int>(e.text.size());
            if (e.text.find('\n') != std::string::npos) m.composer.expanded = true;
            return done(std::move(m));
        },

        // ── Stream events ───────────────────────────────────────────────
        [&](StreamStarted) -> Step {
            m.stream.active = true;
            m.stream.started = std::chrono::steady_clock::now();
            return done(std::move(m));
        },
        [&](StreamTextDelta& e) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant)
                m.current.messages.back().streaming_text += e.text;
            return done(std::move(m));
        },
        [&](StreamToolUseStart& e) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant) {
                ToolUse tc;
                tc.id   = e.id;
                tc.name = e.name;
                tc.status = ToolUse::Status::Pending;
                tc.args = json::object();
                m.current.messages.back().tool_calls.push_back(std::move(tc));
            }
            return done(std::move(m));
        },
        [&](StreamToolUseDelta& e) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant
                && !m.current.messages.back().tool_calls.empty()) {
                auto& tc = m.current.messages.back().tool_calls.back();
                tc.args_streaming += e.partial_json;
                try { tc.args = json::parse(tc.args_streaming); } catch (...) {}
            }
            return done(std::move(m));
        },
        [&](StreamToolUseEnd) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant
                && !m.current.messages.back().tool_calls.empty()) {
                auto& tc = m.current.messages.back().tool_calls.back();
                try { tc.args = json::parse(tc.args_streaming); } catch (...) {}
            }
            return done(std::move(m));
        },
        [&](StreamUsage& e) -> Step {
            if (e.input_tokens)  m.stream.tokens_in  += e.input_tokens;
            if (e.output_tokens) m.stream.tokens_out  = e.output_tokens;
            return done(std::move(m));
        },
        [&](StreamFinished) -> Step {
            auto cmd = finalize_turn(m);
            return {std::move(m), std::move(cmd)};
        },
        [&](StreamError& e) -> Step {
            m.stream.active = false;
            m.stream.phase = Phase::Idle;
            m.stream.status = "error: " + e.message;
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant
                && m.current.messages.back().text.empty()
                && m.current.messages.back().streaming_text.empty()
                && m.current.messages.back().tool_calls.empty()) {
                m.current.messages.back().text = "\u26A0 " + e.message;
            }
            return done(std::move(m));
        },

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Step {
            for (const auto& msg_ : m.current.messages)
                for (const auto& tc : msg_.tool_calls)
                    if (tc.id == e.id && tc.name == "todo" && !e.error) {
                        auto todos = tc.args.value("todos", json::array());
                        m.todo.items.clear();
                        for (const auto& td : todos) {
                            TodoItem item;
                            item.content = td.value("content", "");
                            auto st = td.value("status", "pending");
                            item.status = st == "completed"   ? TodoStatus::Completed
                                        : st == "in_progress" ? TodoStatus::InProgress
                                                              : TodoStatus::Pending;
                            m.todo.items.push_back(std::move(item));
                        }
                    }
            apply_tool_output(m, e.id, std::move(e.output), e.error);
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Permission ──────────────────────────────────────────────────
        [&](PermissionApprove) -> Step {
            if (!m.pending_permission) return done(std::move(m));
            auto id = m.pending_permission->id;
            std::vector<Cmd<Msg>> cmds;
            for (auto& msg_ : m.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == id) {
                        tc.status = ToolUse::Status::Running;
                        cmds.push_back(cmd::run_tool(tc.id, tc.name, tc.args));
                    }
            m.pending_permission.reset();
            m.stream.phase = Phase::ExecutingTool;
            return {std::move(m), Cmd<Msg>::batch(std::move(cmds))};
        },
        [&](PermissionReject) -> Step {
            if (!m.pending_permission) return done(std::move(m));
            auto id = m.pending_permission->id;
            mark_tool_status_by_id(m, id, ToolUse::Status::Rejected,
                                   "User rejected this tool call.");  // ToolCallId
            m.pending_permission.reset();
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },
        [&](PermissionApproveAlways) -> Step {
            // MVP: same as Approve.  Sticky grants TBD.
            return update(std::move(m), PermissionApprove{});
        },

        // ── Model picker ────────────────────────────────────────────────
        [&](OpenModelPicker) -> Step {
            m.model_picker.open = true;
            for (int i = 0; i < static_cast<int>(m.available_models.size()); ++i)
                if (m.available_models[i].id == m.model_id) m.model_picker.index = i;
            return {std::move(m), cmd::fetch_models()};
        },
        [&](ModelsLoaded& e) -> Step {
            if (e.models.empty()) return done(std::move(m));
            auto settings = deps().load_settings();
            m.available_models.clear();
            for (auto& mi : e.models) {
                for (const auto& fav : settings.favorite_models)
                    if (mi.id == fav) mi.favorite = true;
                m.available_models.push_back(std::move(mi));
            }
            m.model_picker.index = 0;
            for (int i = 0; i < static_cast<int>(m.available_models.size()); ++i)
                if (m.available_models[i].id == m.model_id) m.model_picker.index = i;
            return done(std::move(m));
        },
        [&](CloseModelPicker) -> Step {
            m.model_picker.open = false;
            return done(std::move(m));
        },
        [&](ModelPickerMove& e) -> Step {
            if (m.available_models.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.available_models.size());
            m.model_picker.index = (m.model_picker.index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ModelPickerSelect) -> Step {
            if (!m.available_models.empty()) {
                m.model_id = m.available_models[m.model_picker.index].id;
                persist_settings(m);
            }
            m.model_picker.open = false;
            return done(std::move(m));
        },
        [&](ModelPickerToggleFavorite) -> Step {
            if (!m.available_models.empty()) {
                auto& mi = m.available_models[m.model_picker.index];
                mi.favorite = !mi.favorite;
            }
            return done(std::move(m));
        },

        // ── Thread list ─────────────────────────────────────────────────
        [&](OpenThreadList) -> Step {
            m.thread_list.open = true;
            m.threads = deps().load_threads();
            m.thread_list.index = 0;
            return done(std::move(m));
        },
        [&](CloseThreadList) -> Step {
            m.thread_list.open = false;
            return done(std::move(m));
        },
        [&](ThreadListMove& e) -> Step {
            if (m.threads.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.threads.size());
            m.thread_list.index = (m.thread_list.index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListSelect) -> Step {
            if (!m.threads.empty()) m.current = m.threads[m.thread_list.index];
            m.thread_list.open = false;
            return done(std::move(m));
        },
        [&](NewThread) -> Step {
            if (!m.current.messages.empty()) deps().save_thread(m.current);
            m.current = Thread{};
            m.current.id = deps().new_thread_id();
            m.current.created_at = m.current.updated_at = std::chrono::system_clock::now();
            m.thread_list.open = false;
            m.command_palette.open = false;
            m.composer.text.clear();
            m.composer.cursor = 0;
            m.stream.phase = Phase::Idle;
            return done(std::move(m));
        },

        // ── Command palette ─────────────────────────────────────────────
        [&](OpenCommandPalette) -> Step {
            m.command_palette.open = true;
            m.command_palette.query.clear();
            m.command_palette.index = 0;
            return done(std::move(m));
        },
        [&](CloseCommandPalette) -> Step {
            m.command_palette.open = false;
            return done(std::move(m));
        },
        [&](CommandPaletteInput& e) -> Step {
            if (static_cast<uint32_t>(e.ch) < 0x80)
                m.command_palette.query.push_back(static_cast<char>(e.ch));
            return done(std::move(m));
        },
        [&](CommandPaletteBackspace) -> Step {
            if (!m.command_palette.query.empty()) m.command_palette.query.pop_back();
            return done(std::move(m));
        },
        [&](CommandPaletteMove& e) -> Step {
            m.command_palette.index = std::max(0, m.command_palette.index + e.delta);
            return done(std::move(m));
        },
        [&](CommandPaletteSelect) -> Step {
            m.command_palette.open = false;
            switch (m.command_palette.index) {
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
            m.todo.open = true;
            return done(std::move(m));
        },
        [&](CloseTodoModal) -> Step {
            m.todo.open = false;
            return done(std::move(m));
        },
        [&](UpdateTodos& e) -> Step {
            m.todo.items = std::move(e.items);
            return done(std::move(m));
        },

        // ── Profile ─────────────────────────────────────────────────────
        [&](CycleProfile) -> Step {
            m.profile = m.profile == Profile::Write   ? Profile::Ask
                      : m.profile == Profile::Ask     ? Profile::Minimal
                                                       : Profile::Write;
            persist_settings(m);
            return done(std::move(m));
        },

        // ── Diff review ─────────────────────────────────────────────────
        [&](OpenDiffReview) -> Step {
            m.diff_review.open = !m.pending_changes.empty();
            m.diff_review.file_index = 0;
            m.diff_review.hunk_index = 0;
            return done(std::move(m));
        },
        [&](CloseDiffReview) -> Step {
            m.diff_review.open = false;
            return done(std::move(m));
        },
        [&](DiffReviewMove& e) -> Step {
            if (m.pending_changes.empty()) return done(std::move(m));
            auto& fc = m.pending_changes[m.diff_review.file_index];
            int sz = static_cast<int>(fc.hunks.size());
            if (sz == 0) return done(std::move(m));
            m.diff_review.hunk_index = (m.diff_review.hunk_index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](DiffReviewNextFile) -> Step {
            if (m.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.pending_changes.size());
            m.diff_review.file_index = (m.diff_review.file_index + 1) % sz;
            m.diff_review.hunk_index = 0;
            return done(std::move(m));
        },
        [&](DiffReviewPrevFile) -> Step {
            if (m.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.pending_changes.size());
            m.diff_review.file_index = (m.diff_review.file_index - 1 + sz) % sz;
            m.diff_review.hunk_index = 0;
            return done(std::move(m));
        },
        [&](AcceptHunk) -> Step {
            if (!m.pending_changes.empty()) {
                auto& fc = m.pending_changes[m.diff_review.file_index];
                if (!fc.hunks.empty())
                    fc.hunks[m.diff_review.hunk_index].status = Hunk::Status::Accepted;
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            if (!m.pending_changes.empty()) {
                auto& fc = m.pending_changes[m.diff_review.file_index];
                if (!fc.hunks.empty())
                    fc.hunks[m.diff_review.hunk_index].status = Hunk::Status::Rejected;
            }
            return done(std::move(m));
        },
        [&](AcceptAllChanges) -> Step {
            for (auto& fc : m.pending_changes)
                for (auto& h : fc.hunks) h.status = Hunk::Status::Accepted;
            return done(std::move(m));
        },
        [&](RejectAllChanges) -> Step {
            for (auto& fc : m.pending_changes)
                for (auto& h : fc.hunks) h.status = Hunk::Status::Rejected;
            m.pending_changes.clear();
            m.diff_review.open = false;
            return done(std::move(m));
        },

        // ── Misc ────────────────────────────────────────────────────────
        [&](RestoreCheckpoint&) -> Step {
            m.stream.status = "checkpoint restore not implemented yet";
            return done(std::move(m));
        },
        [&](ScrollThread& e) -> Step {
            m.thread_scroll = std::max(0, m.thread_scroll + e.delta);
            return done(std::move(m));
        },
        [&](ToggleToolExpanded& e) -> Step {
            for (auto& msg_ : m.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == e.id) tc.expanded = !tc.expanded;
            return done(std::move(m));
        },
        [&](Tick) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (m.stream.last_tick.time_since_epoch().count() == 0) m.stream.last_tick = now;
            float dt = std::chrono::duration<float>(now - m.stream.last_tick).count();
            m.stream.last_tick = now;
            if (m.stream.active) m.stream.spinner.advance(dt);
            return done(std::move(m));
        },
        [&](Quit) -> Step {
            if (!m.current.messages.empty()) deps().save_thread(m.current);
            return {std::move(m), Cmd<Msg>::quit()};
        },
        [&](NoOp) -> Step { return done(std::move(m)); },
    }, msg);
}

} // namespace moha::app
