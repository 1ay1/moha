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

// Partial-JSON scalar sniffer. The Anthropic SSE stream delivers tool args as
// `input_json_delta` chunks that form incomplete JSON until the tool_use block
// closes. We want the widget to show the path/command/pattern live, not after
// the block closes. Full nlohmann::json parse can't handle partial input, so
// we walk the buffer by hand looking for a key we recognize, then collect its
// string value until the closing quote. Returns std::nullopt until a fully
// closed string is available. Zed uses `partial-json-fixer` for the same job.
std::optional<std::string> sniff_string(const std::string& raw,
                                        std::string_view key) {
    std::string needle = "\"" + std::string{key} + "\"";
    size_t p = raw.find(needle);
    if (p == std::string::npos) return std::nullopt;
    p += needle.size();
    while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\n')) p++;
    if (p >= raw.size() || raw[p] != ':') return std::nullopt;
    p++;
    while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\n')) p++;
    if (p >= raw.size() || raw[p] != '"') return std::nullopt;
    p++;
    std::string out;
    out.reserve(64);
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            if (p + 1 >= raw.size()) return std::nullopt;  // wait for next delta
            char n = raw[p + 1];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\':out += '\\'; break;
                case '/': out += '/';  break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                default:  out += n;    break;  // u-escapes degrade to literal
            }
            p += 2;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
            p++;
        }
    }
    return std::nullopt;  // string not closed yet
}

// Like `sniff_string` but returns whatever has accumulated so far, even when
// the closing quote hasn't arrived yet. Needed for fields whose value dwarfs
// every other arg — write's `content`, edit's `old_string`/`new_string` —
// because waiting for the close means the user sees an empty card for the
// entire duration of a 12 tok/s 800-line file write. We stop at a half-
// escape at buffer edge so we don't emit a dangling `\` into the widget.
std::optional<std::string> sniff_string_progressive(const std::string& raw,
                                                    std::string_view key) {
    std::string needle = "\"" + std::string{key} + "\"";
    size_t p = raw.find(needle);
    if (p == std::string::npos) return std::nullopt;
    p += needle.size();
    while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\n')) p++;
    if (p >= raw.size() || raw[p] != ':') return std::nullopt;
    p++;
    while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\n')) p++;
    if (p >= raw.size() || raw[p] != '"') return std::nullopt;
    p++;
    std::string out;
    out.reserve(256);
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            if (p + 1 >= raw.size()) break;  // half-escape at buffer edge — stop
            char n = raw[p + 1];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\':out += '\\'; break;
                case '/': out += '/';  break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                default:  out += n;    break;
            }
            p += 2;
        } else if (c == '"') {
            return out;  // closed — canonical
        } else {
            out.push_back(c);
            p++;
        }
    }
    return out;  // open string — return partial
}

// For a given tool, fill `tc.args` with whichever scalar field is most useful
// to display in the card header. We write into `tc.args` (rather than a
// separate preview field) so the existing view code — which reads path /
// command / pattern from args — picks it up unchanged.
void update_stream_preview(ToolUse& tc) {
    auto set_arg = [&](std::string_view key, std::string v) {
        if (!tc.args.is_object()) tc.args = json::object();
        auto& cur = tc.args[std::string{key}];
        if (!cur.is_string() || cur.get<std::string>() != v) {
            cur = std::move(v);
            tc.mark_args_dirty();
        }
    };
    auto try_set = [&](std::string_view key) {
        if (auto v = sniff_string(tc.args_streaming, key)) { set_arg(key, *v); return true; }
        return false;
    };
    auto try_set_partial = [&](std::string_view key) {
        if (auto v = sniff_string_progressive(tc.args_streaming, key)) { set_arg(key, *v); return true; }
        return false;
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") try_set("path");
    else if (n == "write") { try_set("path"); try_set_partial("content"); }
    else if (n == "edit")  { try_set("path"); try_set_partial("old_string"); try_set_partial("new_string"); }
    else if (n == "bash")  { try_set("command"); }
    else if (n == "grep")  { try_set("pattern"); try_set("path"); }
    else if (n == "glob")  { try_set("pattern"); }
    else if (n == "find_definition") try_set("symbol");
    else if (n == "web_fetch")       try_set("url");
    else if (n == "web_search")      try_set("query");
    else if (n == "diagnostics")     try_set("command");
    else if (n == "git_commit")      try_set("message");
}

// Rescue partial args when json::parse fails on the raw SSE buffer (truncated
// mid-content, malformed escape, etc). We best-effort sniff each expected
// scalar field and hand the result back as a json object — if at least one
// usable field came through, the tool gets to run instead of the whole turn
// dying on a cosmetic parse error. Returns {} when nothing salvageable.
json salvage_args(const ToolUse& tc) {
    json out = json::object();
    auto pick = [&](std::string_view key) {
        if (auto v = sniff_string_progressive(tc.args_streaming, key))
            out[std::string{key}] = *v;
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") { pick("path"); }
    else if (n == "write") { pick("path"); pick("content"); }
    else if (n == "edit")  { pick("path"); pick("old_string"); pick("new_string"); }
    else if (n == "bash")  { pick("command"); }
    else if (n == "grep")  { pick("pattern"); pick("path"); }
    else if (n == "glob")  { pick("pattern"); }
    else if (n == "find_definition") { pick("symbol"); }
    else if (n == "web_fetch")       { pick("url"); }
    else if (n == "web_search")      { pick("query"); }
    else if (n == "diagnostics")     { pick("command"); }
    else if (n == "git_commit")      { pick("message"); }
    return out;
}

// ── View virtualization ───────────────────────────────────────────────────
// When the transcript grows past kWindow messages, slice kChunk of the oldest
// into terminal scrollback so maya's Yoga/paint cost stays O(window).  We
// estimate each sliced message's row height so Cmd::commit_scrollback can
// shift maya's prev-frame buffer by the right amount; an imperfect estimate
// costs at most one visible refresh of the window on the slice frame.

inline constexpr int kViewWindow = 60;
inline constexpr int kSliceChunk = 20;
inline constexpr int kEstWidth   = 100;  // no terminal width in Model yet

int estimate_message_rows(const Message& msg) {
    int rows = 3; // turn divider + spacing around body
    if (!msg.text.empty()) {
        const int w = std::max(1, kEstWidth - 4);
        rows += static_cast<int>(msg.text.size()) / w + 1;
        int nl = 0;
        for (char c : msg.text) if (c == '\n') ++nl;
        rows += nl;
    }
    for (const auto& tc : msg.tool_calls) {
        rows += 4;
        if (!tc.output.empty()) {
            int nl = 0;
            for (char c : tc.output) if (c == '\n') ++nl;
            rows += std::min(nl, 10);
        }
    }
    return rows;
}

Cmd<Msg> maybe_virtualize(Model& m) {
    const int total = static_cast<int>(m.current.messages.size());
    const int visible = total - m.thread_view_start;
    // Only slice in discrete chunks — a one-per-turn slice would refresh
    // the visible area every turn, whereas chunking it causes one refresh
    // every kSliceChunk turns.
    if (visible <= kViewWindow + kSliceChunk) return Cmd<Msg>::none();

    int committed_rows = 0;
    for (int i = m.thread_view_start; i < m.thread_view_start + kSliceChunk; ++i)
        committed_rows += estimate_message_rows(m.current.messages[i]);
    m.thread_view_start += kSliceChunk;
    return Cmd<Msg>::commit_scrollback(committed_rows);
}

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
    auto virt = maybe_virtualize(m);
    auto launch = cmd::launch_stream(m);
    auto cmd = virt.is_none()
        ? std::move(launch)
        : Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(virt), std::move(launch)});
    return {std::move(m), std::move(cmd)};
}

Cmd<Msg> finalize_turn(Model& m) {
    m.stream.active = false;
    if (!m.current.messages.empty()) {
        auto& last = m.current.messages.back();
        if (last.role == Role::Assistant && !last.streaming_text.empty()) {
            if (last.text.empty()) last.text = std::move(last.streaming_text);
            else                   last.text += std::move(last.streaming_text);
            // Release the moved-from buffer's capacity and drop the
            // StreamingMarkdown + now-stale Element cache; next render
            // rebuilds from the finalized text.
            std::string{}.swap(last.streaming_text);
            last.cached_md_element.reset();
            last.stream_md.reset();
        }
        // Free per-tool-call streaming JSON buffers — args is already parsed
        // by this point, and the raw text is never read again.
        for (auto& tc : last.tool_calls)
            std::string{}.swap(tc.args_streaming);
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
                tc.finished_at = std::chrono::steady_clock::now();
                // Live buffer has been superseded by the formatted result.
                // Keep tc.progress_text empty so the view's fallback logic
                // ("show progress_text while Running, output when Done")
                // can't accidentally double-render stale bytes.
                std::string{}.swap(tc.progress_text);
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
                // Defer the full json::parse until StreamToolUseEnd (O(n^2) on
                // each delta otherwise), but do sniff the single "header" field
                // (path, command, pattern, ...) so the tool card stops showing
                // an empty title while the rest of the args stream in.
                update_stream_preview(tc);
            }
            return done(std::move(m));
        },
        [&](StreamToolUseEnd) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant
                && !m.current.messages.back().tool_calls.empty()) {
                auto& tc = m.current.messages.back().tool_calls.back();
                // Empty args_streaming is legitimate for argumentless tools;
                // args was seeded to {} at StreamToolUseStart.
                if (!tc.args_streaming.empty()) {
                    try {
                        tc.args = json::parse(tc.args_streaming);
                        tc.mark_args_dirty();
                        // Args is now the canonical form; drop the raw stream
                        // buffer so we don't hold two copies until finalize.
                        std::string{}.swap(tc.args_streaming);
                    } catch (const std::exception& ex) {
                        // Parse failed — typically an SSE cutoff mid-content.
                        // Salvage whatever scalar fields we can via the same
                        // progressive sniffs used for live preview, so the
                        // tool still has a shot at running with partial args
                        // instead of nuking the whole turn.
                        auto salvaged = salvage_args(tc);
                        if (!salvaged.empty()) {
                            tc.args = std::move(salvaged);
                            tc.mark_args_dirty();
                            std::string{}.swap(tc.args_streaming);
                        } else {
                            tc.status = ToolUse::Status::Error;
                            tc.output = std::string{"invalid tool arguments: "} + ex.what()
                                      + "\nraw: " + tc.args_streaming;
                            std::string{}.swap(tc.args_streaming);
                        }
                    }
                }
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
            // Always surface the error inline so it is visible regardless of
            // how much content the turn had produced before failing.
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant) {
                auto& last = m.current.messages.back();
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
                // text just got mutated — drop stale render caches.
                last.cached_md_element.reset();
                last.stream_md.reset();
                for (auto& tc : last.tool_calls)
                    std::string{}.swap(tc.args_streaming);
            }
            return done(std::move(m));
        },

        // ── Live tool progress (streaming subprocess output) ────────────
        // Arrives from the subprocess runner every ~80 ms with the full
        // accumulated output so far. We just set it — no Cmd to return —
        // and rely on the existing Tick subscription (active during
        // ExecutingTool) to re-render. Ignore if the tool has already
        // finalised (a late snapshot racing the terminal ToolExecOutput).
        [&](ToolExecProgress& e) -> Step {
            for (auto& msg_ : m.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == e.id && tc.status == ToolUse::Status::Running)
                        tc.progress_text = std::move(e.snapshot);
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
                        tc.started_at = std::chrono::steady_clock::now();
                        cmds.push_back(cmd::run_tool(tc.id, tc.name, tc.args));
                    }
            m.pending_permission.reset();
            m.stream.phase = Phase::ExecutingTool;
            m.stream.active = true;  // keep Tick alive for live elapsed counter
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
            // A freshly-loaded thread has no prev-frame correspondence in
            // the inline buffer — render everything that fits in the
            // window; older messages just won't appear in scrollback, which
            // is expected on a cold load.
            int total = static_cast<int>(m.current.messages.size());
            m.thread_view_start = std::max(0, total - kViewWindow);
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
            m.thread_view_start = 0;
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
