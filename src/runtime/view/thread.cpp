#include "moha/runtime/view/thread.hpp"

#include <string>
#include <vector>

#include <maya/widget/bash_tool.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/git_commit_tool.hpp>
#include <maya/widget/git_graph.hpp>
#include <maya/widget/git_status.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/todo_list.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/turn_divider.hpp>
#include <maya/widget/write_tool.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/permission.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// Cached markdown render for an assistant message body.  Completed messages
// are immutable (mutators in update.cpp must reset cached_md_element), so
// once built the Element is reused across every frame.  The streaming tail
// uses StreamingMarkdown — block-boundary cache → O(new_chars) per delta.
Element cached_markdown_for(const Message& msg) {
    if (msg.text.empty()) {
        if (!msg.stream_md)
            msg.stream_md = std::make_shared<maya::StreamingMarkdown>();
        msg.stream_md->set_content(msg.streaming_text);
        return msg.stream_md->build();
    }
    if (!msg.cached_md_element) {
        msg.cached_md_element =
            std::make_shared<Element>(maya::markdown(msg.text));
        msg.stream_md.reset();
    }
    return *msg.cached_md_element;
}

// ── Helpers ─────────────────────────────────────────────────────────

template <class W, class StatusEnum>
StatusEnum map_status(const ToolUse::Status& s, StatusEnum running, StatusEnum failed,
                      StatusEnum done) {
    return std::visit([&](const auto& v) -> StatusEnum {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, ToolUse::Pending>
                   || std::same_as<T, ToolUse::Running>) return running;
        else if constexpr (std::same_as<T, ToolUse::Failed>
                        || std::same_as<T, ToolUse::Rejected>) return failed;
        else /* Done | Approved */ return done;
    }, s);
}

maya::ToolCallStatus tc_status(const ToolUse::Status& s) {
    return map_status<maya::ToolCall>(s,
        ToolCallStatus::Running, ToolCallStatus::Failed, ToolCallStatus::Completed);
}

std::string safe_arg(const nlohmann::json& args, const char* key) {
    if (!args.is_object()) return {};
    return args.value(key, "");
}

int safe_int_arg(const nlohmann::json& args, const char* key, int def) {
    if (!args.is_object() || !args.contains(key)) return def;
    return args.value(key, def);
}

int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') n++;
    return n + (!s.empty() && s.back() != '\n' ? 1 : 0);
}

// Prepend the model's `display_description` to a card title when set:
//   "Fix null-deref in auth.cpp  ·  src/auth.cpp"
// When desc is empty the title is returned unchanged, so callers don't need
// to branch at the call site.
std::string with_desc(std::string_view title, const std::string& desc) {
    if (desc.empty()) return std::string{title};
    return desc + "  \u00B7  " + std::string{title};
}

// Seconds spent on this tool call so far. For running tools, "now - started";
// for finished tools, "finished - started". Returns 0 if started_at is unset
// (tool still Pending). Called every Tick frame while a tool runs, so the
// elapsed counter on the card updates live (~30 fps).
float tool_elapsed(const ToolUse& tc) {
    auto zero = std::chrono::steady_clock::time_point{};
    auto started = tc.started_at();
    if (started == zero) return 0.0f;
    auto finished = tc.finished_at();
    auto end = finished == zero ? std::chrono::steady_clock::now() : finished;
    auto dt = end - started;
    return std::chrono::duration<float>(dt).count();
}

// Tool output from `bash` wraps the captured stdout+stderr in a ```…``` fence
// (so the raw bytes come back to the model inside a markdown code block, and
// the model sees an unambiguous "this is the literal output" boundary). The
// BashTool widget has its own monospace frame, so the fence chars would be
// rendered verbatim ("```" floating at the top of every card). Strip them —
// along with the elapsed/truncation trailers that follow — so the widget
// shows just the inner payload. Keep the raw, fenced string in tc.output
// for the model; only the view gets the stripped version.
std::string strip_bash_output_fence(const std::string& s) {
    std::string_view sv{s};
    // Trailing metadata lines we emit after the closing fence — drop them
    // first so the "```" we look for below actually ends the block.
    auto drop_trailer = [&](std::string_view marker) {
        auto pos = sv.rfind(marker);
        if (pos != std::string_view::npos) sv = sv.substr(0, pos);
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'
                               || sv.back() == ' '  || sv.back() == '\t'))
            sv.remove_suffix(1);
    };
    drop_trailer("\n\n[elapsed:");
    drop_trailer("\n\n[output truncated");

    auto fence = sv.find("```");
    if (fence == std::string_view::npos) return std::string{sv};
    // Allow a leading "Command …\n\n" header before the fence — the failure
    // and timeout branches put one there.
    auto body_start = fence + 3;
    // Skip a language tag (we don't emit one, but be forgiving) and the
    // newline after the opening fence.
    while (body_start < sv.size() && sv[body_start] != '\n') ++body_start;
    if (body_start < sv.size() && sv[body_start] == '\n') ++body_start;

    auto close = sv.rfind("```");
    if (close == std::string_view::npos || close <= body_start)
        return std::string{sv.substr(body_start)};

    auto body_end = close;
    while (body_end > body_start
           && (sv[body_end - 1] == '\n' || sv[body_end - 1] == '\r'))
        --body_end;

    std::string header{sv.substr(0, fence)};
    while (!header.empty() && (header.back() == '\n' || header.back() == '\r'
                               || header.back() == ' '))
        header.pop_back();
    std::string body{sv.substr(body_start, body_end - body_start)};
    if (header.empty()) return body;
    if (body.empty()) return header;
    return header + "\n\n" + body;
}

int parse_exit_code(const std::string& output) {
    // Recognize both formats: the "[exit code N]" suffix from legacy_format
    // (used by diagnostics/git) and the "failed with exit code N" clause from
    // the Zed-style bash formatter. Whichever appears, pull the integer.
    struct Marker { const char* text; size_t skip; };
    static constexpr Marker markers[] = {
        {"failed with exit code ", 22},
        {"[exit code ",            11},
    };
    for (const auto& m : markers) {
        auto pos = output.rfind(m.text);
        if (pos == std::string::npos) continue;
        try { return std::stoi(output.substr(pos + m.skip)); }
        catch (...) { return 1; }
    }
    if (output.find("timed out") != std::string::npos) return 124;
    return 0;
}

Element tool_card(const std::string& name, ToolCallKind kind,
                  const std::string& desc, const ToolUse::Status& status,
                  bool expanded, const std::string& output,
                  float elapsed = 0.0f) {
    maya::ToolCall::Config cfg;
    cfg.tool_name = name;
    cfg.kind = kind;
    cfg.description = desc;
    maya::ToolCall card(cfg);
    card.set_expanded(expanded);
    card.set_status(tc_status(status));
    card.set_elapsed(elapsed);
    if (!output.empty())
        card.set_content(text(output, fg_of(muted)));
    return card.build();
}

Element parse_grep_result(const ToolUse& tc, const std::string& pattern, bool collapsed) {
    SearchResult sr(SearchKind::Grep, pattern);
    sr.set_expanded(!collapsed);
    sr.set_max_matches_per_file(2);
    sr.set_status(map_status<SearchResult>(tc.status,
        SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
    sr.set_elapsed(tool_elapsed(tc));
    if (tc.output().empty() || !tc.is_done()
        || tc.output().starts_with("No matches")) {
        return sr.build();
    }

    SearchFileGroup current_group;
    int total_groups = 0;
    auto flush = [&](SearchFileGroup& g) {
        if (!g.file_path.empty()) {
            sr.add_group(std::move(g));
            total_groups++;
            g = SearchFileGroup{};
        }
    };

    // Markdown format (new):
    //   ## Matches in {path}
    //   ### L{s}-{e}
    //   ```
    //   {context lines}
    //   ```
    // Legacy format (from find_definition which still uses path:line:content):
    //   {path}:{line}:{content}
    std::istringstream iss(tc.output());
    std::string line;
    int range_start = 0, range_end = 0;
    bool in_code = false;
    int code_line_no = 0;
    while (std::getline(iss, line)) {
        if (line.starts_with("## Matches in ")) {
            flush(current_group);
            if (total_groups >= 10) break;
            auto path = line.substr(14);
            if (path.starts_with("./")) path = path.substr(2);
            current_group = SearchFileGroup{std::move(path), {}};
            in_code = false;
            continue;
        }
        if (line.starts_with("### L")) {
            auto dash = line.find('-', 5);
            try {
                range_start = std::stoi(line.substr(5, dash - 5));
                range_end = dash != std::string::npos
                    ? std::stoi(line.substr(dash + 1)) : range_start;
            } catch (...) { range_start = range_end = 0; }
            continue;
        }
        if (line == "```") {
            if (!in_code) { in_code = true; code_line_no = range_start; }
            else          { in_code = false; }
            continue;
        }
        if (in_code && !current_group.file_path.empty()) {
            current_group.matches.push_back({code_line_no++, line});
            continue;
        }
        // Legacy fallback: path:line:content (find_definition still uses this).
        if (!in_code) {
            auto c1 = line.find(':');
            if (c1 == std::string::npos) continue;
            auto c2 = line.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            std::string file = line.substr(0, c1);
            if (file.starts_with("./")) file = file.substr(2);
            int lineno = 0;
            try { lineno = std::stoi(line.substr(c1+1, c2-c1-1)); } catch(...) {}
            std::string content = line.substr(c2 + 1);
            while (!content.empty() && (content.front() == ' ' || content.front() == '\t'))
                content.erase(content.begin());
            if (current_group.file_path != file) {
                flush(current_group);
                if (total_groups >= 10) break;
                current_group = SearchFileGroup{file, {}};
            }
            current_group.matches.push_back({lineno, content});
        }
    }
    flush(current_group);
    return sr.build();
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// render_tool_call — every tool gets a bordered card with status icon
// ════════════════════════════════════════════════════════════════════════

Element render_tool_call_uncached(const ToolUse& tc);

// Terminal-state card cache. A chat with 40 tool calls rebuilds 40 borders
// + 40 Yoga layouts + 40 text runs every frame otherwise — even when
// nothing about those cards has changed in minutes. We only cache when the
// tool has reached a terminal status; running/pending tools rebuild so the
// live elapsed counter keeps ticking.
Element render_tool_call(const ToolUse& tc) {
    const bool terminal = tc.is_terminal();
    if (terminal) {
        auto key = tc.compute_render_key();
        if (tc.render_cache && tc.render_cache_key == key)
            return *tc.render_cache;
        auto built = render_tool_call_uncached(tc);
        tc.render_cache     = std::make_shared<Element>(built);
        tc.render_cache_key = key;
        return built;
    }
    return render_tool_call_uncached(tc);
}

Element render_tool_call_uncached(const ToolUse& tc) {
    auto path = safe_arg(tc.args, "path");
    auto cmd  = safe_arg(tc.args, "command");
    auto desc = safe_arg(tc.args, "display_description");

    bool done = tc.is_terminal();

    // Live elapsed — grows each frame while Running, freezes on terminal status.
    float elapsed = tool_elapsed(tc);

    // ── read ────────────────────────────────────────────────────────
    if (tc.name == "read") {
        ReadTool rt(with_desc(path.empty() ? "read" : path, desc));
        rt.set_expanded(!done);
        rt.set_start_line(safe_int_arg(tc.args, "offset", 1));
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        rt.set_elapsed(elapsed);
        if (done) {
            rt.set_content(tc.output());
            rt.set_total_lines(count_lines(tc.output()));
            rt.set_max_lines(6);
        }
        return rt.build();
    }

    // ── list_dir (same style as read) ───────────────────────────────
    if (tc.name == "list_dir") {
        auto dir = path.empty() ? safe_arg(tc.args, "path") : path;
        if (dir.empty()) dir = ".";
        ReadTool rt(with_desc(dir, desc));
        rt.set_expanded(tc.expanded);
        rt.set_start_line(0);
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        rt.set_elapsed(elapsed);
        if (!tc.output().empty()) {
            rt.set_content(tc.output());
            rt.set_total_lines(count_lines(tc.output()));
            rt.set_max_lines(8);
        }
        return rt.build();
    }

    // ── write ───────────────────────────────────────────────────────
    if (tc.name == "write") {
        // While streaming, fall back from path → description → "(streaming…)"
        // so the card never sits on a static "(no path)" — that read as
        // "stuck" even when the model was actively generating.
        std::string file_path;
        if (!path.empty())      file_path = path;
        else if (!desc.empty()) file_path = desc;
        else                    file_path = "(streaming\xe2\x80\xa6)";
        // On error, fall back to the generic card so the failure reason
        // from the tool (permission denied, disk full, etc.) is visible.
        // WriteTool has no error-text surface.
        if (tc.is_failed() || tc.is_rejected()) {
            return tool_card("write", ToolCallKind::Edit,
                with_desc(path.empty() ? std::string{"write"} : path, desc),
                tc.status, tc.expanded, tc.output(), elapsed);
        }
        WriteTool wt(file_path);
        // Only set description as a separate field when path is real;
        // otherwise we already promoted it into the title above.
        if (!desc.empty() && !path.empty()) wt.set_description(desc);
        // Auto-expand while the model is still streaming `content` (Pending)
        // or the tool is writing to disk (Running) so the user sees a live
        // preview of the file being produced. Collapses on Done; user can
        // still toggle open via tc.expanded.
        wt.set_expanded(!done || tc.expanded);
        wt.set_content(safe_arg(tc.args, "content"));
        wt.set_max_preview_lines(6);
        wt.set_status(map_status<WriteTool>(tc.status,
            WriteStatus::Writing, WriteStatus::Failed, WriteStatus::Written));
        wt.set_elapsed(elapsed);
        return wt.build();
    }

    // ── edit ────────────────────────────────────────────────────────
    if (tc.name == "edit") {
        // Same path → desc → "(streaming…)" fallback as write so the card
        // never reads as stuck while the model is mid-stream.
        std::string base;
        if (!path.empty())      base = path;
        else if (!desc.empty()) base = desc;
        else                    base = "(streaming\xe2\x80\xa6)";
        auto header = (!path.empty() && !desc.empty())
                          ? with_desc(base, desc) : base;
        if (tc.is_failed() || tc.is_rejected()) {
            return tool_card("edit", ToolCallKind::Edit,
                header, tc.status, tc.expanded, tc.output(), elapsed);
        }
        EditTool et(header);
        // Auto-expand while streaming old/new strings so a big refactor's
        // progress is visible — same rationale as write.
        et.set_expanded(!done || tc.expanded);
        et.set_old_text(safe_arg(tc.args, "old_string"));
        et.set_new_text(safe_arg(tc.args, "new_string"));
        et.set_status(map_status<EditTool>(tc.status,
            EditStatus::Applying, EditStatus::Failed, EditStatus::Applied));
        et.set_elapsed(elapsed);
        return et.build();
    }

    // ── bash ────────────────────────────────────────────────────────
    if (tc.name == "bash") {
        BashTool bt(with_desc(cmd.empty() ? "bash" : cmd, desc));
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(5);
        bt.set_status(map_status<BashTool>(tc.status,
            BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        bt.set_elapsed(elapsed);
        if (done) {
            int rc = parse_exit_code(tc.output());
            bt.set_exit_code(rc);
            if (rc != 0) bt.set_status(BashStatus::Failed);
            bt.set_output(strip_bash_output_fence(tc.output()));
        } else if (!tc.progress_text().empty()) {
            // Live stream: stdout+stderr captured so far. Shown verbatim
            // (no fence stripping — the fence is added only by the final
            // formatter once the process exits).
            bt.set_output(tc.progress_text());
        }
        return bt.build();
    }

    // ── diagnostics (same style as bash) ────────────────────────────
    if (tc.name == "diagnostics") {
        auto diag_cmd = safe_arg(tc.args, "command");
        BashTool bt(with_desc(diag_cmd.empty() ? "diagnostics" : diag_cmd, desc));
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(8);
        bt.set_elapsed(elapsed);
        if (done) {
            int rc = parse_exit_code(tc.output());
            bt.set_exit_code(rc);
            bt.set_status(rc == 0 ? BashStatus::Success : BashStatus::Failed);
            bt.set_output(strip_bash_output_fence(tc.output()));
        } else if (!tc.progress_text().empty()) {
            bt.set_output(tc.progress_text());
            bt.set_status(BashStatus::Running);
        } else {
            bt.set_status(map_status<BashTool>(tc.status,
                BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        }
        return bt.build();
    }

    // ── grep / find_definition (SearchResult widget) ────────────────
    if (tc.name == "grep" || tc.name == "find_definition") {
        auto pattern = tc.name == "grep"
            ? safe_arg(tc.args, "pattern")
            : safe_arg(tc.args, "symbol");
        bool collapsed = tc.is_done();
        return parse_grep_result(tc, pattern, collapsed);
    }

    // ── glob (SearchResult widget) ──────────────────────────────────
    if (tc.name == "glob") {
        auto pattern = safe_arg(tc.args, "pattern");
        SearchResult sr(SearchKind::Glob, with_desc(pattern, desc));
        sr.set_expanded(tc.expanded);
        sr.set_status(map_status<SearchResult>(tc.status,
            SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
        sr.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()
            && tc.output() != "no matches") {
            SearchFileGroup group{"", {}};
            std::istringstream iss(tc.output());
            std::string line;
            while (std::getline(iss, line)) {
                if (line.starts_with("./")) line = line.substr(2);
                if (!line.empty()) group.matches.push_back({0, line});
            }
            if (!group.matches.empty()) sr.add_group(std::move(group));
        }
        return sr.build();
    }

    // ── web_fetch (FetchTool widget) ────────────────────────────────
    if (tc.name == "web_fetch") {
        auto url = safe_arg(tc.args, "url");
        FetchTool ft(with_desc(url, desc));
        ft.set_expanded(tc.expanded);
        ft.set_max_body_lines(6);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        ft.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()) {
            const auto& out = tc.output();
            auto first_nl = out.find('\n');
            if (first_nl != std::string::npos) {
                auto header = out.substr(0, first_nl);
                auto sp = header.find(' ');
                if (sp != std::string::npos) {
                    try { ft.set_status_code(std::stoi(header.substr(sp+1))); } catch(...) {}
                }
                auto paren = header.find('(');
                if (paren != std::string::npos) {
                    auto close = header.find(')', paren);
                    if (close != std::string::npos)
                        ft.set_content_type(header.substr(paren+1, close-paren-1));
                }
                auto body_start = out.find("\n\n");
                if (body_start != std::string::npos)
                    ft.set_body(out.substr(body_start + 2));
            }
        } else if (tc.is_failed()) {
            ft.set_body(tc.output());
        }
        return ft.build();
    }

    // ── web_search (FetchTool widget, same bordered style) ──────────
    if (tc.name == "web_search") {
        auto query = safe_arg(tc.args, "query");
        FetchTool ft(with_desc("search: " + query, desc));
        ft.set_expanded(tc.expanded);
        ft.set_max_body_lines(8);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        ft.set_elapsed(elapsed);
        if (!tc.output().empty()) {
            ft.set_status_code(200);
            ft.set_body(tc.output());
        }
        return ft.build();
    }

    // ── git_status (GitStatusWidget inside a ToolCall card) ─────────
    if (tc.name == "git_status") {
        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_status";
        cfg.kind = ToolCallKind::Other;
        if (!desc.empty()) cfg.description = desc;
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()) {
            GitStatusWidget gs;
            gs.set_compact(false);
            int modified = 0, staged = 0, untracked = 0, deleted = 0;
            std::istringstream iss(tc.output());
            std::string line;
            while (std::getline(iss, line)) {
                if (line.starts_with("# branch.head "))
                    gs.set_branch(line.substr(14));
                else if (line.starts_with("# branch.ab ")) {
                    auto ab = line.substr(12);
                    auto sp = ab.find(' ');
                    if (sp != std::string::npos) {
                        try { gs.set_ahead(std::stoi(ab.substr(0, sp))); } catch(...) {}
                        try { gs.set_behind(-std::stoi(ab.substr(sp+1))); } catch(...) {}
                    }
                } else if (line.size() >= 2) {
                    if (line[0] == '?') { untracked++; continue; }
                    if (line[0] != '1' && line[0] != '2') continue;
                    if (line.size() < 4) continue;
                    char x = line[2], y = line[3];
                    if (x != '.') staged++;
                    if (y == 'M') modified++;
                    else if (y == 'D') deleted++;
                }
            }
            gs.set_dirty(modified, staged, untracked);
            gs.set_deleted(deleted);
            card.set_content(gs.build());
        }
        return card.build();
    }

    // ── git_log (GitGraph inside a ToolCall card) ───────────────────
    if (tc.name == "git_log") {
        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_log";
        cfg.kind = ToolCallKind::Other;
        auto ref = safe_arg(tc.args, "ref");
        cfg.description = desc.empty() ? ref
                                       : (ref.empty() ? desc : desc + "  \u00B7  " + ref);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()) {
            GitGraph gg;
            gg.set_show_hash(true);
            gg.set_show_author(true);
            gg.set_show_time(true);
            std::istringstream iss(tc.output());
            std::string line;
            bool first = true;
            while (std::getline(iss, line)) {
                if (line.empty() || line[0] == ' ') continue;
                GitCommit gc;
                auto sp1 = line.find(' ');
                if (sp1 == std::string::npos) continue;
                gc.hash = line.substr(0, sp1);
                auto sp2 = line.find(' ', sp1 + 1);
                if (sp2 != std::string::npos) {
                    gc.time = line.substr(sp1 + 1, sp2 - sp1 - 1);
                    gc.author = line.substr(sp2 + 1);
                }
                std::string msg_line;
                if (std::getline(iss, msg_line)) {
                    while (!msg_line.empty() && msg_line.front() == ' ')
                        msg_line.erase(msg_line.begin());
                    gc.message = msg_line;
                }
                gc.is_head = first;
                first = false;
                gg.add_commit(std::move(gc));
            }
            card.set_content(gg.build());
        }
        return card.build();
    }

    // ── git_diff (DiffView inside a ToolCall card) ──────────────────
    if (tc.name == "git_diff") {
        auto ref = safe_arg(tc.args, "ref");
        auto diff_path = safe_arg(tc.args, "path");
        std::string body;
        if (!ref.empty()) body += ref;
        if (!diff_path.empty()) { if (!body.empty()) body += " "; body += diff_path; }

        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_diff";
        cfg.kind = ToolCallKind::Other;
        if (!desc.empty())
            cfg.description = body.empty() ? desc : desc + "  \u00B7  " + body;
        else
            cfg.description = body.empty() ? std::string{"working tree"} : body;
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (!tc.output().empty() && tc.is_done()
            && tc.output() != "no changes") {
            DiffView dv("", tc.output());
            card.set_content(dv.build());
        }
        return card.build();
    }

    // ── git_commit (GitCommitTool widget) ───────────────────────────
    if (tc.name == "git_commit") {
        auto msg = safe_arg(tc.args, "message");
        GitCommitTool gc(msg.empty() ? desc : msg);
        gc.set_expanded(tc.expanded);
        gc.set_status(map_status<GitCommitTool>(tc.status,
            GitCommitStatus::Running, GitCommitStatus::Failed, GitCommitStatus::Done));
        gc.set_elapsed(elapsed);
        if (!tc.output().empty()) gc.set_output(tc.output());
        return gc.build();
    }

    // ── todo (TodoListTool widget) ──────────────────────────────────
    if (tc.name == "todo") {
        TodoListTool tl;
        tl.set_description(desc);
        tl.set_elapsed(elapsed);
        tl.set_expanded(true);
        tl.set_status(map_status<TodoListTool>(tc.status,
            TodoListStatus::Running, TodoListStatus::Failed, TodoListStatus::Done));
        // Pull items straight from the model-supplied args so the card
        // reflects the intended state even while `run_todo` is still in-flight
        // (and so failure cards still show what was attempted).
        if (tc.args.is_object()) {
            if (auto it = tc.args.find("todos"); it != tc.args.end() && it->is_array()) {
                for (const auto& td : *it) {
                    if (!td.is_object()) continue;
                    TodoListItem item;
                    item.content = td.value("content", "");
                    auto s = td.value("status", std::string{"pending"});
                    item.status = s == "completed"   ? TodoItemStatus::Completed
                                : s == "in_progress" ? TodoItemStatus::InProgress
                                                     : TodoItemStatus::Pending;
                    tl.add(std::move(item));
                }
            }
        }
        return tl.build();
    }

    return tool_card(tc.name.value, ToolCallKind::Other,
        tc.args.is_object() && !tc.args.empty() ? tc.args_dump() : "",
        tc.status, tc.expanded, tc.output(), elapsed);
}

// ════════════════════════════════════════════════════════════════════════

Element render_message(const Message& msg, int turn_num, const Model& m) {
    std::vector<Element> rows;
    if (msg.role == Role::User) {
        if (msg.checkpoint_id) rows.push_back(render_checkpoint_divider());
        rows.push_back(TurnDivider(TurnRole::User, turn_num).build());
        rows.push_back(text(""));
        rows.push_back((v(UserMessage::build(msg.text)) | grow(1.0f)).build());
        rows.push_back(text(""));
    } else if (msg.role == Role::Assistant) {
        rows.push_back(TurnDivider(TurnRole::Assistant, turn_num).build());
        rows.push_back(text(""));
        bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            rows.push_back((v(cached_markdown_for(msg)) | padding(0, 0, 0, 2)).build());
            rows.push_back(text(""));
        }
        for (const auto& tc : msg.tool_calls) {
            rows.push_back((v(render_tool_call(tc)) | grow(1.0f)).build());
            if (m.pending_permission && m.pending_permission->id == tc.id)
                rows.push_back(render_inline_permission(*m.pending_permission, tc));
            rows.push_back(text(""));
        }
    }
    return v(std::move(rows)).build();
}

Element thread_panel(const Model& m) {
    std::vector<Element> rows;
    // Virtualize: older messages live in the terminal's native scrollback
    // (their rows were committed via maya::Cmd::commit_scrollback).  We
    // preserve absolute turn numbering by counting finalized assistant
    // messages *before* the view window too, so a user seeing "Turn 42"
    // after scrolling back stays consistent.
    const std::size_t total = m.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1;
    for (std::size_t i = 0; i < start; ++i)
        if (m.current.messages[i].role == Role::Assistant) ++turn;
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.current.messages[i];
        rows.push_back(render_message(msg, turn, m));
        if (msg.role == Role::Assistant) ++turn;
    }
    if (m.stream.active && !m.current.messages.empty()
        && m.current.messages.back().role == Role::Assistant) {
        auto spin = m.stream.spinner;
        spin.set_style(fg_bold(phase_color(m.stream.phase)));
        std::string verb{phase_verb(m.stream.phase)};
        rows.push_back((h(
            spin.build(),
            text(" " + verb + "\u2026", fg_italic(muted))
        ) | padding(0, 0, 0, 2)).build());
    }
    if (rows.empty()) {
        // Wordmark-style welcome — quiet brand presence + the one detail
        // that orients the user (which model they're talking to). A blank
        // thread is the loneliest screen in the app; give it a focal point.
        auto brand = h(spacer(),
            text("\u2726  ", fg_bold(accent)),
            text("moha", fg_bold(fg)),
            text("  \u2726", fg_dim(accent)),
            spacer()).build();

        auto subtitle = h(spacer(),
            text("a calm middleware between you and the model",
                 fg_italic(muted)),
            spacer()).build();

        auto model_line = h(spacer(),
            text("model  ", fg_dim(muted)),
            text(m.model_id.value, fg_of(fg)),
            spacer()).build();

        auto prompt_hint = h(spacer(),
            text("press  ", fg_dim(muted)),
            text("Enter", fg_bold(fg)),
            text("  to send  \u00B7  ", fg_dim(muted)),
            text("^K", fg_bold(fg)),
            text("  for the palette", fg_dim(muted)),
            spacer()).build();

        rows.push_back((v(
            text(""), text(""), text(""),
            brand,
            text(""),
            subtitle,
            text(""), text(""),
            model_line,
            text(""), text(""),
            prompt_hint
        )).build());
    }
    return (v(std::move(rows)) | padding(0, 1) | grow(1.0f)).build();
}

} // namespace moha::ui
