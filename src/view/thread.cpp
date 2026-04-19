#include "moha/view/thread.hpp"

#include <string>
#include <vector>

#include <maya/widget/bash_tool.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/git_graph.hpp>
#include <maya/widget/git_status.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/turn_divider.hpp>
#include <maya/widget/write_tool.hpp>

#include "moha/view/helpers.hpp"
#include "moha/view/palette.hpp"
#include "moha/view/permission.hpp"

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
StatusEnum map_status(ToolUse::Status s, StatusEnum running, StatusEnum failed,
                      StatusEnum done) {
    switch (s) {
        case ToolUse::Status::Pending:
        case ToolUse::Status::Running:  return running;
        case ToolUse::Status::Error:
        case ToolUse::Status::Rejected: return failed;
        case ToolUse::Status::Done:
        case ToolUse::Status::Approved: return done;
    }
    return done;
}

maya::ToolCallStatus tc_status(ToolUse::Status s) {
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

int parse_exit_code(const std::string& output) {
    auto pos = output.rfind("[exit code ");
    if (pos == std::string::npos) return 0;
    try { return std::stoi(output.substr(pos + 11)); } catch(...) { return 1; }
}

Element tool_card(const std::string& name, ToolCallKind kind,
                  const std::string& desc, ToolUse::Status status,
                  bool expanded, const std::string& output) {
    maya::ToolCall::Config cfg;
    cfg.tool_name = name;
    cfg.kind = kind;
    cfg.description = desc;
    maya::ToolCall card(cfg);
    card.set_expanded(expanded);
    card.set_status(tc_status(status));
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
    if (!tc.output.empty() && tc.status == ToolUse::Status::Done
        && tc.output != "no matches") {
        SearchFileGroup current_group;
        std::istringstream iss(tc.output);
        std::string line;
        int total_groups = 0;
        while (std::getline(iss, line)) {
            if (line.starts_with("[>")) break;
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
                if (!current_group.file_path.empty()) {
                    sr.add_group(std::move(current_group));
                    if (++total_groups >= 10) break;
                }
                current_group = SearchFileGroup{file, {}};
            }
            current_group.matches.push_back({lineno, content});
        }
        if (!current_group.file_path.empty())
            sr.add_group(std::move(current_group));
    }
    return sr.build();
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// render_tool_call — every tool gets a bordered card with status icon
// ════════════════════════════════════════════════════════════════════════

Element render_tool_call(const ToolUse& tc) {
    auto path = safe_arg(tc.args, "path");
    auto cmd  = safe_arg(tc.args, "command");

    bool done = tc.status == ToolUse::Status::Done
             || tc.status == ToolUse::Status::Error
             || tc.status == ToolUse::Status::Rejected;

    // ── read ────────────────────────────────────────────────────────
    if (tc.name == "read") {
        ReadTool rt(path.empty() ? "read" : path);
        rt.set_expanded(!done);
        rt.set_start_line(safe_int_arg(tc.args, "offset", 1));
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        if (done) {
            rt.set_content(tc.output);
            rt.set_total_lines(count_lines(tc.output));
            rt.set_max_lines(6);
        }
        return rt.build();
    }

    // ── list_dir (same style as read) ───────────────────────────────
    if (tc.name == "list_dir") {
        auto dir = path.empty() ? safe_arg(tc.args, "path") : path;
        if (dir.empty()) dir = ".";
        ReadTool rt(dir);
        rt.set_expanded(tc.expanded);
        rt.set_start_line(0);
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        if (!tc.output.empty()) {
            rt.set_content(tc.output);
            rt.set_total_lines(count_lines(tc.output));
            rt.set_max_lines(8);
        }
        return rt.build();
    }

    // ── write ───────────────────────────────────────────────────────
    if (tc.name == "write") {
        WriteTool wt(path.empty() ? "write" : path);
        wt.set_expanded(tc.expanded);
        wt.set_content(safe_arg(tc.args, "content"));
        wt.set_max_preview_lines(4);
        wt.set_status(map_status<WriteTool>(tc.status,
            WriteStatus::Writing, WriteStatus::Failed, WriteStatus::Written));
        return wt.build();
    }

    // ── edit ────────────────────────────────────────────────────────
    if (tc.name == "edit") {
        EditTool et(path.empty() ? "edit" : path);
        et.set_expanded(tc.expanded);
        et.set_old_text(safe_arg(tc.args, "old_string"));
        et.set_new_text(safe_arg(tc.args, "new_string"));
        et.set_status(map_status<EditTool>(tc.status,
            EditStatus::Applying, EditStatus::Failed, EditStatus::Applied));
        return et.build();
    }

    // ── bash ────────────────────────────────────────────────────────
    if (tc.name == "bash") {
        BashTool bt(cmd.empty() ? "bash" : cmd);
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(5);
        bt.set_status(map_status<BashTool>(tc.status,
            BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        if (done) {
            int rc = parse_exit_code(tc.output);
            bt.set_exit_code(rc);
            if (rc != 0) bt.set_status(BashStatus::Failed);
            bt.set_output(tc.output);
        }
        return bt.build();
    }

    // ── diagnostics (same style as bash) ────────────────────────────
    if (tc.name == "diagnostics") {
        auto diag_cmd = safe_arg(tc.args, "command");
        BashTool bt(diag_cmd.empty() ? "diagnostics" : diag_cmd);
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(8);
        if (done) {
            int rc = parse_exit_code(tc.output);
            bt.set_exit_code(rc);
            bt.set_status(rc == 0 ? BashStatus::Success : BashStatus::Failed);
            bt.set_output(tc.output);
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
        bool collapsed = tc.status == ToolUse::Status::Done;
        return parse_grep_result(tc, pattern, collapsed);
    }

    // ── glob (SearchResult widget) ──────────────────────────────────
    if (tc.name == "glob") {
        auto pattern = safe_arg(tc.args, "pattern");
        SearchResult sr(SearchKind::Glob, pattern);
        sr.set_expanded(tc.expanded);
        sr.set_status(map_status<SearchResult>(tc.status,
            SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done
            && tc.output != "no matches") {
            SearchFileGroup group{"", {}};
            std::istringstream iss(tc.output);
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
        FetchTool ft(url);
        ft.set_expanded(tc.expanded);
        ft.set_max_body_lines(6);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            auto first_nl = tc.output.find('\n');
            if (first_nl != std::string::npos) {
                auto header = tc.output.substr(0, first_nl);
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
                auto body_start = tc.output.find("\n\n");
                if (body_start != std::string::npos)
                    ft.set_body(tc.output.substr(body_start + 2));
            }
        } else if (tc.status == ToolUse::Status::Error) {
            ft.set_body(tc.output);
        }
        return ft.build();
    }

    // ── web_search (FetchTool widget, same bordered style) ──────────
    if (tc.name == "web_search") {
        auto query = safe_arg(tc.args, "query");
        FetchTool ft("search: " + query);
        ft.set_expanded(tc.expanded);
        ft.set_max_body_lines(8);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        if (!tc.output.empty()) {
            ft.set_status_code(200);
            ft.set_body(tc.output);
        }
        return ft.build();
    }

    // ── git_status (GitStatusWidget inside a ToolCall card) ─────────
    if (tc.name == "git_status") {
        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_status";
        cfg.kind = ToolCallKind::Other;
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            GitStatusWidget gs;
            gs.set_compact(false);
            int modified = 0, staged = 0, untracked = 0, deleted = 0;
            std::istringstream iss(tc.output);
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
        cfg.description = safe_arg(tc.args, "ref");
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            GitGraph gg;
            gg.set_show_hash(true);
            gg.set_show_author(true);
            gg.set_show_time(true);
            std::istringstream iss(tc.output);
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
        std::string desc;
        if (!ref.empty()) desc += ref;
        if (!diff_path.empty()) { if (!desc.empty()) desc += " "; desc += diff_path; }

        maya::ToolCall::Config cfg;
        cfg.tool_name = "git_diff";
        cfg.kind = ToolCallKind::Other;
        cfg.description = desc.empty() ? "working tree" : desc;
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done
            && tc.output != "no changes") {
            DiffView dv("", tc.output);
            card.set_content(dv.build());
        }
        return card.build();
    }

    // ── git_commit (ToolCall card) ──────────────────────────────────
    if (tc.name == "git_commit") {
        return tool_card("git_commit", ToolCallKind::Execute,
            safe_arg(tc.args, "message"), tc.status, tc.expanded, tc.output);
    }

    // ── todo (ToolCall card) ────────────────────────────────────────
    if (tc.name == "todo") {
        return tool_card("todo", ToolCallKind::Other,
            "", tc.status, tc.expanded, tc.output);
    }

    return tool_card(tc.name.value, ToolCallKind::Other,
        tc.args.is_object() && !tc.args.empty() ? tc.args_dump() : "",
        tc.status, tc.expanded, tc.output);
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
