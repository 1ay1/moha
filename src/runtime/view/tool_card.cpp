// Per-tool card rendering — the big dispatch that turns a ToolUse into the
// right maya::widget for its tool family (ReadTool, WriteTool, EditTool,
// BashTool, SearchResult, …). Split out from thread.cpp so the main thread
// renderer stays focused on turn/timeline composition.

#include "moha/runtime/view/thread.hpp"

#include <concepts>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <maya/widget/bash_tool.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/git_commit_tool.hpp>
#include <maya/widget/git_graph.hpp>
#include <maya/widget/git_status.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/todo_list.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/write_tool.hpp>

#include "moha/runtime/view/cache.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/tool_args.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// Map a ToolUse status onto whatever (Running, Failed, Done)-flavored enum
// the target widget exposes. Template-with-NTTPs keeps each call site
// one-liner while every widget gets its own triple of enum values.
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

// Prepend the model's `display_description` to a card title when set:
//   "Fix null-deref in auth.cpp  ·  src/auth.cpp"
// Empty desc returns the title unchanged so callers don't need to branch.
std::string with_desc(std::string_view title, const std::string& desc) {
    if (desc.empty()) return std::string{title};
    return desc + "  \u00B7  " + std::string{title};
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
    // Legacy format (find_definition still uses path:line:content):
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
    (void)range_end;
    return sr.build();
}

Element render_tool_call_uncached(const ToolUse& tc) {
    auto path = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    auto cmd  = safe_arg(tc.args, "command");
    auto desc = pick_arg(tc.args, {"display_description", "description"});

    bool done = tc.is_terminal();

    // Live elapsed — grows each frame while Running, freezes on terminal status.
    float elapsed = tool_elapsed(tc);

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

    if (tc.name == "write") {
        // While streaming, fall back from path → description → "(streaming…)"
        // so the card never sits on a static "(no path)" — that reads as
        // "stuck" even when the model is actively generating.
        std::string file_path;
        if (!path.empty())      file_path = path;
        else if (!desc.empty()) file_path = desc;
        else                    file_path = "(streaming\xe2\x80\xa6)";
        if (tc.is_failed() || tc.is_rejected()) {
            return tool_card("write", ToolCallKind::Edit,
                with_desc(path.empty() ? std::string{"write"} : path, desc),
                tc.status, tc.expanded, tc.output(), elapsed);
        }
        WriteTool wt(file_path);
        if (!desc.empty() && !path.empty()) wt.set_description(desc);
        wt.set_expanded(!done || tc.expanded);
        // Mirror the alias chain in src/tool/tools/write.cpp so the preview
        // shows the body whichever key the model picked.
        wt.set_content(pick_arg(tc.args, {"content", "file_text", "text",
                                          "body", "data", "contents",
                                          "file_content"}));
        wt.set_max_preview_lines(6);
        wt.set_status(map_status<WriteTool>(tc.status,
            WriteStatus::Writing, WriteStatus::Failed, WriteStatus::Written));
        wt.set_elapsed(elapsed);
        return wt.build();
    }

    if (tc.name == "edit") {
        std::string base;
        if (!path.empty())      base = path;
        else if (!desc.empty()) base = desc;
        else                    base = "(streaming\xe2\x80\xa6)";
        auto header = (!path.empty() && !desc.empty())
                          ? with_desc(base, desc) : base;
        if (tc.is_rejected()) {
            return tool_card("edit", ToolCallKind::Edit,
                header, tc.status, tc.expanded, tc.output(), elapsed);
        }
        EditTool et(header);
        // Edit cards stay expanded permanently — the diff is the whole point
        // and is usually small enough to leave visible. (Write collapses on
        // done because it's the whole file body.)
        et.set_expanded(true);
        // The tool accepts three input shapes (see src/tool/tools/edit.cpp):
        //   1. canonical:  edits: [{old_text, new_text, ...}, ...]
        //   2. Zed-legacy: old_text / new_text at top level
        //   3. moha-orig:  old_string / new_string at top level
        // For (1) surface EVERY edit — the streaming preview mirrors the full
        // array into tc.args["edits"] so the user sees all hunks land live.
        bool rendered_array = false;
        if (tc.args.is_object()) {
            if (auto it = tc.args.find("edits");
                it != tc.args.end() && it->is_array() && !it->empty())
            {
                std::vector<EditTool::EditPair> pairs;
                pairs.reserve(it->size());
                for (const auto& e : *it) {
                    if (!e.is_object()) continue;
                    std::string ot, nt;
                    if (auto v = e.find("old_text"); v != e.end() && v->is_string())
                        ot = v->get<std::string>();
                    else if (auto v2 = e.find("old_string"); v2 != e.end() && v2->is_string())
                        ot = v2->get<std::string>();
                    if (auto v = e.find("new_text"); v != e.end() && v->is_string())
                        nt = v->get<std::string>();
                    else if (auto v2 = e.find("new_string"); v2 != e.end() && v2->is_string())
                        nt = v2->get<std::string>();
                    pairs.push_back({std::move(ot), std::move(nt)});
                }
                if (!pairs.empty()) {
                    et.set_edits(std::move(pairs));
                    rendered_array = true;
                }
            }
        }
        if (!rendered_array) {
            auto pick = [&](const char* legacy_key, const char* orig_key) -> std::string {
                auto v = safe_arg(tc.args, legacy_key);
                if (!v.empty()) return v;
                return safe_arg(tc.args, orig_key);
            };
            et.set_old_text(pick("old_text", "old_string"));
            et.set_new_text(pick("new_text", "new_string"));
        }
        et.set_status(map_status<EditTool>(tc.status,
            EditStatus::Applying, EditStatus::Failed, EditStatus::Applied));
        et.set_elapsed(elapsed);
        // On failure, stack the error text *below* the attempted edits so
        // the user sees which edit missed AND why in the same card. The
        // previous behavior (fall through to generic tool_card) hid the
        // edit content entirely, leaving only a red error block — which
        // made it hard to tell whether the model's `old_text` was close
        // enough to retry or genuinely wrong.
        if (tc.is_failed() && !tc.output().empty()) {
            auto err_row = text("✗ " + tc.output(),
                                Style{}.with_fg(Color::red()));
            return v(et.build(), err_row).build();
        }
        return et.build();
    }

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
            // — the fence is added only by the final formatter once the
            // process exits.
            bt.set_output(tc.progress_text());
        }
        return bt.build();
    }

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

    if (tc.name == "grep" || tc.name == "find_definition") {
        auto pattern = tc.name == "grep"
            ? safe_arg(tc.args, "pattern")
            : safe_arg(tc.args, "symbol");
        bool collapsed = tc.is_done();
        return parse_grep_result(tc, pattern, collapsed);
    }

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

    // ── outline(path) ───────────────────────────────────────────────
    // Shows the file's symbol map: per-kind groups, line numbers, names.
    // Cheap to render — the tool's output is already structured, we
    // just colour the kind tags + line numbers and let names be plain.
    if (tc.name == "outline") {
        auto p = safe_arg(tc.args, "path");
        maya::ToolCall::Config cfg;
        cfg.tool_name = "outline";
        cfg.kind = ToolCallKind::Read;            // it's a read-shaped op
        cfg.description = with_desc(p.empty() ? "outline" : p, desc);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (tc.is_failed() && !tc.output().empty()) {
            card.set_content(text(tc.output(), Style{}.with_fg(danger)));
            return card.build();
        }
        if (tc.is_done() && !tc.output().empty()) {
            std::vector<Element> rows;
            std::istringstream iss(tc.output());
            std::string line;
            int row_count = 0;
            constexpr int kMaxRows = 24;
            while (std::getline(iss, line)) {
                if (row_count >= kMaxRows) {
                    rows.push_back(text("\xe2\x80\xa6 more (call `read` "
                                        "for full file)", fg_dim(muted)));
                    break;
                }
                if (line.empty()) continue;
                // First line is "<path>  (N symbols)" — render small/dim.
                if (rows.empty() && line.find("symbols)") != std::string::npos) {
                    rows.push_back(text(line, fg_dim(muted)));
                    continue;
                }
                // "[kind]" group header.
                if (line.starts_with("[") && line.find(']') != std::string::npos) {
                    rows.push_back(text(line, Style{}.with_fg(highlight).with_bold()));
                    ++row_count;
                    continue;
                }
                // "  L<line>  <name>    <signature>" rows.
                rows.push_back(text(line, fg_of(fg)));
                ++row_count;
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── repo_map(path?, max_kb?) ──────────────────────────────────
    // Two-section output (Most-referenced + Workspace tree) — render
    // each line and colour-tag the section headers + the bracketed
    // score prefixes so the eye finds the structure quickly.
    if (tc.name == "repo_map") {
        auto p = safe_arg(tc.args, "path");
        maya::ToolCall::Config cfg;
        cfg.tool_name = "repo_map";
        cfg.kind = ToolCallKind::Read;
        cfg.description = with_desc(p.empty() ? "workspace" : p, desc);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (tc.is_failed() && !tc.output().empty()) {
            card.set_content(text(tc.output(), Style{}.with_fg(danger)));
            return card.build();
        }
        if (tc.is_done() && !tc.output().empty()) {
            std::vector<Element> rows;
            std::istringstream iss(tc.output());
            std::string line;
            int row_count = 0;
            // Tighter cap when collapsed; generous when expanded since
            // repo_map is the table-of-contents the user explicitly asked
            // to see in full.
            const int kMaxRows = tc.expanded ? 60 : 14;
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.empty()) {
                    rows.push_back(text(""));
                    continue;
                }
                if (line.starts_with("# ")) {
                    // Header comment line (preamble explaining the map).
                    rows.push_back(text(line, fg_dim(muted)));
                } else if (line.starts_with("## ")) {
                    rows.push_back(text(line, Style{}.with_fg(accent).with_bold()));
                } else if (line.starts_with("[") && line.find(']') != std::string::npos
                           && line.find('/') != std::string::npos) {
                    // "[score] path/to/file" — colour the score bracket.
                    auto rb = line.find(']');
                    rows.push_back(h(
                        text(line.substr(0, rb + 1),
                             Style{}.with_fg(highlight).with_bold()),
                        text(line.substr(rb + 1), fg_of(fg))
                    ).build());
                } else if (line.ends_with("/")) {
                    // Directory header in the tree.
                    rows.push_back(text(line, Style{}.with_fg(info).with_bold()));
                } else if (line.starts_with("    //")) {
                    // Per-file description continuation.
                    rows.push_back(text(line, fg_dim(muted).with_italic()));
                } else {
                    rows.push_back(text(line, fg_of(fg)));
                }
                ++row_count;
            }
            // Footer if we truncated.
            std::string remainder;
            int remaining_lines = 0;
            while (std::getline(iss, line)) ++remaining_lines;
            if (remaining_lines > 0) {
                rows.push_back(text("\xe2\x80\xa6 +"
                                    + std::to_string(remaining_lines)
                                    + " more lines", fg_dim(muted)));
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── signatures(pattern, limit?) ─────────────────────────────────
    // Cross-file symbol grep. SearchResult would technically work, but
    // the result rows are kind-tagged ("[fn] name", "[class] Bar")
    // rather than literal-line matches — giving each hit the kind
    // colour reads better than the generic search-result widget.
    if (tc.name == "signatures") {
        auto pat = safe_arg(tc.args, "pattern");
        maya::ToolCall::Config cfg;
        cfg.tool_name = "signatures";
        cfg.kind = ToolCallKind::Search;
        cfg.description = with_desc(pat.empty() ? "signatures" : pat, desc);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (tc.is_failed() && !tc.output().empty()) {
            card.set_content(text(tc.output(), Style{}.with_fg(danger)));
            return card.build();
        }
        if (tc.is_done() && !tc.output().empty()) {
            std::vector<Element> rows;
            std::istringstream iss(tc.output());
            std::string line;
            int row_count = 0;
            const int kMaxRows = tc.expanded ? 40 : 12;
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.empty()) { rows.push_back(text("")); continue; }
                if (line.starts_with("Symbols matching")) {
                    rows.push_back(text(line, Style{}.with_fg(accent).with_bold()));
                } else if (line.starts_with("## ")) {
                    rows.push_back(text(line.substr(3),
                                        Style{}.with_fg(info).with_bold()));
                } else if (line.starts_with("  L")) {
                    // "  L42  [fn] name    optional signature"
                    rows.push_back(text(line, fg_of(fg)));
                } else {
                    rows.push_back(text(line, fg_dim(muted)));
                }
                ++row_count;
            }
            int remaining = 0;
            while (std::getline(iss, line)) ++remaining;
            if (remaining > 0)
                rows.push_back(text("\xe2\x80\xa6 +" + std::to_string(remaining)
                                    + " more rows", fg_dim(muted)));
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── investigate(query, model?) — distinctive sub-agent UI ───────
    // Layout:
    //   ┌─────────────────────────────────────────────────────────┐
    //   │ ❝ what does the auth flow do? ❞              haiku      │  ← query strip
    //   │ ◍ 3 turns · 6 tools · 12.4s              ▰▰▰▱▱  60%     │  ← stats
    //   ├─────────────────────────────────────────────────────────┤
    //   │ ▎ T1  ✓ 354ms                                            │
    //   │ ▎   ✓ outline   src/auth.cpp                       120ms │
    //   │ ▎   ✓ grep      "session"                           89ms │
    //   │ ▎   ✓ read      src/oauth.hpp                      145ms │
    //   │ ▎                                                         │
    //   │ ▎ T2  ✓ 170ms                                            │
    //   │ ▎   ✓ outline   src/session.hpp                     78ms │
    //   │ ▎                                                         │
    //   │ ▎ T3  ◍ thinking…                                         │
    //   ├─ ▶ SYNTHESIS ──────────────────────────────────────────── │
    //   │ The auth flow lives in three layers...                    │
    //   │ Bottom line: src/auth.cpp:142 is the entry.               │
    //   └─────────────────────────────────────────────────────────┘
    //
    // The vertical rail (▎) ties all the turn rows visually; the
    // synthesis section gets its own banner ruler so the answer reads
    // as a result, not just one more event in the stream.
    if (tc.name == "investigate") {
        auto query = safe_arg(tc.args, "query");
        auto model_short = safe_arg(tc.args, "model");
        std::string head = query.size() > 70
            ? query.substr(0, 67) + "..." : query;
        maya::ToolCall::Config cfg;
        cfg.tool_name = "investigate";
        cfg.kind = ToolCallKind::Other;
        std::string descr = head;
        if (!model_short.empty()) descr += "  \xc2\xb7  " + model_short;
        cfg.description = with_desc(std::move(descr), desc);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        card.set_content(investigate_body(tc, elapsed));
        return card.build();
    }

#if 0   // moved into investigate_body() below — kept here as a guarded
        // doc reference for the parser/renderer that lived inline. New
        // behavior: the standalone tool card and the in-Actions-panel
        // body BOTH go through investigate_body(), so the rich layout
        // shows up wherever an investigate ToolUse renders.
        // ── Parser for the structured transcript ──────────────────
        // Vocabulary (mirrors investigate.cpp:Progress comment):
        //   Q: <text>           one query line
        //   M: <model id>       model id
        //   T<n> thinking       turn N is awaiting model
        //   T<n> dispatch <c>: a, b, c    fan-out begins
        //   T<n> ok <name> <ms>ms         per-tool success
        //   T<n> err <name> <ms>ms <msg>  per-tool failure
        //   T<n> done <ok>/<n>            turn complete
        //   T<n> synthesis                synthesis section opens
        //   <free text>                   synthesis body (continues)
        struct ToolRow {
            std::string name;
            bool        ok = false;
            bool        running = true;   // true until ok/err arrives
            int         ms = 0;
            std::string err;              // first line of error, when failed
        };
        struct TurnView {
            int                    n = 0;
            bool                   thinking = true;   // before dispatch
            bool                   done = false;      // T<n> done line seen
            int                    ok_count = 0;
            int                    expected = 0;      // dispatch <c>
            std::vector<ToolRow>   tools;
            bool                   synthesis_open = false;
            std::string            synthesis;         // accumulated body
        };
        std::string parsed_query, parsed_model;
        std::vector<TurnView> turns;
        bool synthesis_in_progress = false;
        std::string trailing_text;     // catch any free text outside a turn

        const std::string& src = (tc.is_running() ? tc.progress_text()
                                                  : tc.progress_text());
        // For terminal cards we ALSO want to merge in the final output
        // (which has the framed answer). Use progress_text while running
        // so the user sees live updates; once done, the parent's
        // ToolExecOutput supersedes progress_text and we should fall
        // back to tc.output()'s synthesis body.

        auto find_or_make_turn = [&](int n) -> TurnView& {
            for (auto& t : turns) if (t.n == n) return t;
            turns.push_back(TurnView{n});
            return turns.back();
        };
        auto starts_with = [](const std::string& s, std::string_view p) {
            return s.size() >= p.size()
                && std::equal(p.begin(), p.end(), s.begin());
        };
        auto try_parse_turn_prefix = [](const std::string& s) -> int {
            // "T<n> ..." → returns n, or 0 if not matched.
            if (s.size() < 2 || s[0] != 'T') return 0;
            int i = 1, n = 0;
            while (i < (int)s.size() && s[i] >= '0' && s[i] <= '9') {
                n = n * 10 + (s[i] - '0');
                ++i;
            }
            if (n == 0 || i >= (int)s.size() || s[i] != ' ') return 0;
            return n;
        };

        std::istringstream iss(src);
        std::string line;
        while (std::getline(iss, line)) {
            if (synthesis_in_progress) {
                // Everything after a "T<n> synthesis" line and before
                // the next T<n>-prefixed line is synthesis body.
                int next_turn = try_parse_turn_prefix(line);
                if (next_turn != 0) {
                    synthesis_in_progress = false;
                    // fall through to handle this line
                } else {
                    if (!turns.empty()) {
                        if (!turns.back().synthesis.empty())
                            turns.back().synthesis += '\n';
                        turns.back().synthesis += line;
                    } else {
                        if (!trailing_text.empty()) trailing_text += '\n';
                        trailing_text += line;
                    }
                    continue;
                }
            }
            if (line.empty()) continue;
            if (starts_with(line, "Q: ")) {
                parsed_query = line.substr(3);
                continue;
            }
            if (starts_with(line, "M: ")) {
                parsed_model = line.substr(3);
                continue;
            }
            int tn = try_parse_turn_prefix(line);
            if (tn == 0) {
                // Stray line — keep as trailing text.
                if (!trailing_text.empty()) trailing_text += '\n';
                trailing_text += line;
                continue;
            }
            std::string rest = line.substr(2 + std::to_string(tn).size());
            // rest starts with " <verb> ..."
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            auto& t = find_or_make_turn(tn);
            if (rest == "thinking") {
                t.thinking = true;
            } else if (starts_with(rest, "dispatch ")) {
                t.thinking = false;
                // "dispatch 3: outline, grep, read"
                auto colon = rest.find(':');
                if (colon != std::string::npos) {
                    try {
                        t.expected = std::stoi(rest.substr(9, colon - 9));
                    } catch (...) {}
                    std::string list = rest.substr(colon + 1);
                    while (!list.empty() && list.front() == ' ') list.erase(0, 1);
                    std::size_t pos = 0;
                    while (pos < list.size()) {
                        auto comma = list.find(',', pos);
                        std::string nm = list.substr(pos,
                            comma == std::string::npos ? std::string::npos
                                                       : comma - pos);
                        while (!nm.empty() && nm.front() == ' ') nm.erase(0, 1);
                        while (!nm.empty() && nm.back() == ' ')  nm.pop_back();
                        if (!nm.empty()) t.tools.push_back({nm, false, true, 0, ""});
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                }
            } else if (starts_with(rest, "ok ") || starts_with(rest, "err ")) {
                bool ok = starts_with(rest, "ok ");
                std::string body = rest.substr(ok ? 3 : 4);
                // "<name> <ms>ms[ <err first line>]"
                auto sp = body.find(' ');
                std::string nm = body.substr(0, sp);
                int ms = 0;
                std::string err;
                if (sp != std::string::npos) {
                    auto rest2 = body.substr(sp + 1);
                    auto ms_end = rest2.find("ms");
                    if (ms_end != std::string::npos) {
                        try { ms = std::stoi(rest2.substr(0, ms_end)); }
                        catch (...) {}
                        if (ms_end + 3 <= rest2.size())
                            err = rest2.substr(ms_end + 3);
                    }
                }
                bool matched = false;
                for (auto& tr : t.tools) {
                    if (tr.running && tr.name == nm) {
                        tr.running = false; tr.ok = ok; tr.ms = ms; tr.err = err;
                        matched = true; break;
                    }
                }
                if (!matched) {
                    t.tools.push_back({nm, ok, false, ms, err});
                }
                if (ok) ++t.ok_count;
            } else if (starts_with(rest, "done ")) {
                t.done = true;
            } else if (rest == "synthesis") {
                t.synthesis_open = true;
                synthesis_in_progress = true;
            }
        }

        // ── Visual primitives ─────────────────────────────────────
        // The vertical rail glyph that ties all turn rows together.
        // U+258E (▎) — quarter-block — gives a slim continuous bar
        // without competing with text. Active runs get the accent
        // colour, settled runs go muted.
        const bool is_active_run = tc.is_running();
        const Style rail_active   = Style{}.with_fg(accent).with_bold();
        const Style rail_settled  = fg_dim(muted);
        const Style rail_st       = is_active_run ? rail_active : rail_settled;
        const std::string rail    = "\xe2\x96\x8e ";   // ▎ + space

        // Animated dot spinner — frame derived from card elapsed so it
        // advances visibly as Tick re-renders the card. Used for
        // in-flight tools to make the card feel "alive" rather than
        // static text.
        constexpr std::array<const char*, 10> kSpinFrames = {
            "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
            "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
            "\xe2\xa0\x87","\xe2\xa0\x8f",
        };
        const auto spin_frame = static_cast<std::size_t>(
            std::max(0.0f, elapsed * 12.0f))
            % kSpinFrames.size();
        const std::string spin_glyph = kSpinFrames[spin_frame];

        auto kind_color = [](std::string_view name) -> Color {
            if (name == "edit" || name == "write")            return accent;
            if (name == "bash")                                return success;
            if (name == "todo")                                return warn;
            if (name.size() >= 4 && name.substr(0, 4) == "git_") return highlight;
            return info;
        };

        auto pad_right = [](std::string s, std::size_t w) {
            if (s.size() < w) s.append(w - s.size(), ' ');
            return s;
        };

        std::vector<Element> rows;

        // ── Strip 1: the question, big and quoted ─────────────────
        // Treats the user's question as the protagonist of the card.
        // Italic + bold-white inside ❝ ❞ guillemets reads as a
        // pull-quote rather than a snake-cased arg dump.
        if (!parsed_query.empty()) {
            std::string q = parsed_query;
            if (q.size() > 110) { q.resize(107); q += "..."; }
            rows.push_back(h(
                text("\xe2\x9d\x9d ",                                 // ❝
                     Style{}.with_fg(accent).with_bold()),
                text(q, Style{}.with_fg(fg).with_italic().with_bold()),
                text(" \xe2\x9d\x9e",                                 // ❞
                     Style{}.with_fg(accent).with_bold())
            ).build());
        }

        // ── Strip 2: live stats ───────────────────────────────────
        //   ◍/✓ <N> turns · <K>/<T> tools · <elapsed>
        if (!turns.empty() || !parsed_model.empty()) {
            int total_turns = static_cast<int>(turns.size());
            int total_tools = 0;
            int total_ok    = 0;
            for (const auto& t : turns) {
                total_tools += static_cast<int>(t.tools.size());
                for (const auto& tr : t.tools) if (tr.ok) ++total_ok;
            }
            std::vector<Element> cells;
            // Lead glyph: spinner if active, ✓ if done, ✗ if failed.
            if (tc.is_failed()) {
                cells.push_back(text("\xe2\x9c\x97 ",
                    Style{}.with_fg(danger).with_bold()));
            } else if (tc.is_done()) {
                cells.push_back(text("\xe2\x9c\x93 ",
                    Style{}.with_fg(success).with_bold()));
            } else {
                cells.push_back(text(spin_glyph + " ",
                    Style{}.with_fg(accent).with_bold()));
            }
            std::ostringstream stat;
            stat << total_turns << " turn" << (total_turns == 1 ? "" : "s");
            if (total_tools > 0) {
                stat << "  \xc2\xb7  " << total_ok << "/" << total_tools
                     << " tool" << (total_tools == 1 ? "" : "s");
            }
            cells.push_back(text(stat.str(), Style{}.with_fg(fg).with_bold()));
            // Right-align the elapsed via spacer().
            cells.push_back(spacer());
            std::ostringstream rhs;
            if (!parsed_model.empty()) {
                std::string m = parsed_model;
                // Trim long "claude-haiku-4-5-20251001" → "haiku 4.5"
                if (m.find("haiku") != std::string::npos)      m = "haiku";
                else if (m.find("sonnet") != std::string::npos) m = "sonnet";
                else if (m.find("opus") != std::string::npos)   m = "opus";
                rhs << m << "  \xc2\xb7  ";
            }
            int sec_int = static_cast<int>(elapsed);
            int dec = static_cast<int>((elapsed - sec_int) * 10);
            rhs << sec_int << "." << dec << "s";
            cells.push_back(text(rhs.str(), fg_dim(muted)));
            rows.push_back((h(std::move(cells)) | grow(1.0f)).build());
        }

        // Slim divider before the timeline.
        if (!turns.empty()) {
            rows.push_back(text(""));
        }

        // ── Strip 3: per-turn timeline along the rail ─────────────
        const std::size_t kMaxTurnsShown = tc.expanded ? 12 : 4;
        std::size_t skip_turns = turns.size() > kMaxTurnsShown
            ? turns.size() - kMaxTurnsShown : 0;
        if (skip_turns > 0) {
            rows.push_back(h(
                text(rail, rail_settled),
                text("\xe2\x80\xa6 " + std::to_string(skip_turns)
                     + " earlier turn"
                     + (skip_turns == 1 ? "" : "s") + " collapsed",
                     fg_dim(muted))
            ).build());
        }

        // Compute name-width for clean alignment within turn cells.
        std::size_t name_w = 8;
        for (const auto& t : turns) {
            for (const auto& tr : t.tools)
                name_w = std::max(name_w, tr.name.size());
        }
        name_w = std::min<std::size_t>(name_w, 14);

        for (std::size_t i = skip_turns; i < turns.size(); ++i) {
            const auto& t = turns[i];
            const bool is_current = !t.done && (i + 1 == turns.size());

            if (i > skip_turns) {
                // Spacer-on-rail row for breathing room between turns.
                rows.push_back(h(text(rail, rail_st), text("")).build());
            }

            // Turn header: rail + status icon + "T<n>" + summary.
            std::string th_glyph;
            Style       th_style;
            if (t.done) {
                bool any_err = false;
                for (const auto& tr : t.tools) if (!tr.ok && !tr.running) any_err = true;
                if (any_err) {
                    th_glyph = "\xe2\x9c\x97";   // ✗
                    th_style = Style{}.with_fg(danger).with_bold();
                } else {
                    th_glyph = "\xe2\x9c\x93";   // ✓
                    th_style = Style{}.with_fg(success).with_bold();
                }
            } else if (is_current && t.thinking) {
                th_glyph = spin_glyph;
                th_style = Style{}.with_fg(accent).with_bold();
            } else if (is_current) {
                th_glyph = spin_glyph;
                th_style = Style{}.with_fg(info).with_bold();
            } else {
                th_glyph = "\xe2\x97\x8b";        // ○ (in transition)
                th_style = fg_dim(muted);
            }

            // "T1  3/3"   "T3  thinking…"   "T2  2 in-flight"
            std::ostringstream hdr;
            hdr << "T" << t.n;
            if (t.done && !t.tools.empty()) {
                hdr << "  " << t.ok_count << "/" << t.tools.size();
            } else if (is_current && t.thinking) {
                hdr << "  thinking\xe2\x80\xa6";   // …
            } else if (is_current && !t.tools.empty()) {
                int pending = 0;
                for (const auto& tr : t.tools) if (tr.running) ++pending;
                if (pending > 0) hdr << "  " << pending << " in-flight";
                else             hdr << "  " << t.ok_count
                                     << "/" << t.tools.size();
            }
            // Right-side: total turn elapsed (sum of tool ms so far).
            int turn_ms = 0;
            for (const auto& tr : t.tools) turn_ms += tr.ms;
            std::vector<Element> hdr_cells;
            hdr_cells.push_back(text(rail, rail_st));
            hdr_cells.push_back(text(th_glyph + " ", th_style));
            hdr_cells.push_back(text(hdr.str(),
                is_current ? Style{}.with_fg(fg).with_bold()
                           : Style{}.with_fg(fg)));
            hdr_cells.push_back(spacer());
            if (turn_ms > 0) {
                hdr_cells.push_back(text(std::to_string(turn_ms) + "ms",
                                         fg_dim(muted)));
            }
            rows.push_back((h(std::move(hdr_cells)) | grow(1.0f)).build());

            // Tool rows.
            for (const auto& tr : t.tools) {
                Style icon_st;
                std::string icon;
                if (tr.running) {
                    icon = spin_glyph + " ";
                    icon_st = Style{}.with_fg(info);
                } else if (tr.ok) {
                    icon = "\xe2\x9c\x93 ";        // ✓
                    icon_st = Style{}.with_fg(success).with_bold();
                } else {
                    icon = "\xe2\x9c\x97 ";        // ✗
                    icon_st = Style{}.with_fg(danger).with_bold();
                }
                std::vector<Element> tcells;
                tcells.push_back(text(rail, rail_st));
                tcells.push_back(text("    " + icon, icon_st));
                tcells.push_back(text(pad_right(tr.name, name_w),
                    Style{}.with_fg(kind_color(tr.name))));
                if (!tr.ok && !tr.running && !tr.err.empty()) {
                    std::string e = tr.err;
                    if (e.size() > 50) { e.resize(47); e += "..."; }
                    tcells.push_back(text("  " + e,
                        Style{}.with_fg(danger).with_dim()));
                }
                tcells.push_back(spacer());
                if (!tr.running) {
                    tcells.push_back(text(std::to_string(tr.ms) + "ms",
                                          fg_dim(muted)));
                }
                rows.push_back((h(std::move(tcells)) | grow(1.0f)).build());
            }
        }

        // ── Strip 4: synthesis banner + body ──────────────────────
        // Find which turn carries the synthesis (latest one with
        // synthesis_open == true). If a fall-back from tc.output()
        // is needed, build it after.
        const TurnView* synth = nullptr;
        for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
            if (it->synthesis_open && !it->synthesis.empty()) {
                synth = &*it; break;
            }
        }
        std::string fallback_body;
        if (!synth && (tc.is_done() || tc.is_failed())
            && !tc.output().empty()) {
            fallback_body = tc.output();
            if (fallback_body.starts_with("[investigate")) {
                if (auto end = fallback_body.find("]\n\n");
                    end != std::string::npos)
                    fallback_body = fallback_body.substr(end + 3);
            }
        }
        if (synth || !fallback_body.empty()) {
            // Banner: "▶ SYNTHESIS ━━━━━━━━━━━━━━…"
            // The horizontal rule visually separates the answer from the
            // process timeline above it.
            const std::string label = (tc.is_failed() && !synth)
                ? "RESULT" : "SYNTHESIS";
            const Color label_color = tc.is_failed() ? danger : accent;
            rows.push_back(text(""));
            rows.push_back(h(
                text("\xe2\x96\xb6 ",
                     Style{}.with_fg(label_color).with_bold()),
                text(label,
                     Style{}.with_fg(label_color).with_bold()),
                text("  ", {}),
                // Heavy horizontal ruler that fills the rest of the row.
                (text(std::string{}, {}) | grow(1.0f)).build(),
                text("\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",   // ───
                     fg_dim(muted))
            ).build());

            const std::string& body = synth ? synth->synthesis : fallback_body;
            std::istringstream bit(body);
            std::string bline;
            int bcount = 0;
            const int kBodyMax = tc.expanded ? 60 : 16;
            while (std::getline(bit, bline) && bcount < kBodyMax) {
                Style st = tc.is_failed() && !synth
                    ? Style{}.with_fg(danger) : fg_of(fg);
                // Markdown-ish highlights:
                //   **bold** lines → magenta bold
                //   # heading lines → cyan bold
                //   - / * bullet lines → leave fg
                if (bline.starts_with("**") && bline.ends_with("**")
                    && bline.size() >= 4) {
                    st = Style{}.with_fg(accent).with_bold();
                } else if (bline.starts_with("# ")) {
                    st = Style{}.with_fg(highlight).with_bold();
                }
                rows.push_back(text(bline, st));
                ++bcount;
            }
            int rem = 0;
            while (std::getline(bit, bline)) ++rem;
            if (rem > 0) {
                rows.push_back(text("\xe2\x80\xa6 +" + std::to_string(rem)
                                    + " more lines", fg_dim(muted)));
            }
        }

        if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        return card.build();
    }
#endif // moved-into-investigate_body

    return tool_card(tc.name.value, ToolCallKind::Other,
        tc.args.is_object() && !tc.args.empty() ? tc.args_dump() : "",
        tc.status, tc.expanded, tc.output(), elapsed);
}

} // namespace

// ── investigate_body — public, shared between standalone card &
//    in-Actions-panel inline body ────────────────────────────────────────
// Parses the structured Q/M/T<n> transcript emitted by
// src/tool/tools/investigate.cpp and renders the rich per-turn timeline
// + synthesis section. Returns a v-stack Element with no chrome — the
// caller wraps it in whatever container makes sense (ToolCall border for
// the standalone card, raw inline rows for the Actions panel).
Element investigate_body(const ToolUse& tc, float elapsed) {
    using namespace maya;
    using namespace maya::dsl;

    // Short-circuit on empty input — caller can decide whether to show
    // a minimal placeholder.
    if (tc.progress_text().empty() && tc.output().empty())
        return text("");

    // ── Parser ───────────────────────────────────────────────────────
    struct ToolRow {
        std::string name;
        bool        ok = false;
        bool        running = true;
        int         ms = 0;
        std::string err;
        std::string arg_summary;     // one-line `arg` line content
        std::string res_summary;     // one-line `res` line content
    };
    struct TurnView {
        int                    n = 0;
        bool                   thinking = true;
        bool                   done = false;
        int                    ok_count = 0;
        int                    expected = 0;
        std::vector<ToolRow>   tools;
        bool                   synthesis_open = false;
        std::string            synthesis;
    };
    std::string parsed_query, parsed_model;
    std::vector<TurnView> turns;
    bool synthesis_in_progress = false;
    std::string trailing_text;

    auto find_or_make_turn = [&](int n) -> TurnView& {
        for (auto& t : turns) if (t.n == n) return t;
        turns.push_back(TurnView{n});
        return turns.back();
    };
    auto starts_with = [](const std::string& s, std::string_view p) {
        return s.size() >= p.size()
            && std::equal(p.begin(), p.end(), s.begin());
    };
    auto try_parse_turn_prefix = [](const std::string& s) -> int {
        if (s.size() < 2 || s[0] != 'T') return 0;
        int i = 1, n = 0;
        while (i < (int)s.size() && s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + (s[i] - '0'); ++i;
        }
        if (n == 0 || i >= (int)s.size() || s[i] != ' ') return 0;
        return n;
    };

    const std::string& src = tc.progress_text();
    std::istringstream iss(src);
    std::string line;
    while (std::getline(iss, line)) {
        if (synthesis_in_progress) {
            int next_turn = try_parse_turn_prefix(line);
            if (next_turn != 0) {
                synthesis_in_progress = false;
            } else {
                if (!turns.empty()) {
                    if (!turns.back().synthesis.empty())
                        turns.back().synthesis += '\n';
                    turns.back().synthesis += line;
                } else {
                    if (!trailing_text.empty()) trailing_text += '\n';
                    trailing_text += line;
                }
                continue;
            }
        }
        if (line.empty()) continue;
        if (starts_with(line, "Q: ")) { parsed_query = line.substr(3); continue; }
        if (starts_with(line, "M: ")) { parsed_model = line.substr(3); continue; }
        int tn = try_parse_turn_prefix(line);
        if (tn == 0) {
            if (!trailing_text.empty()) trailing_text += '\n';
            trailing_text += line;
            continue;
        }
        std::string rest = line.substr(2 + std::to_string(tn).size());
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        auto& t = find_or_make_turn(tn);
        if (rest == "thinking") { t.thinking = true; }
        else if (starts_with(rest, "dispatch ")) {
            t.thinking = false;
            auto colon = rest.find(':');
            if (colon != std::string::npos) {
                try { t.expected = std::stoi(rest.substr(9, colon - 9)); }
                catch (...) {}
                std::string list = rest.substr(colon + 1);
                while (!list.empty() && list.front() == ' ') list.erase(0, 1);
                std::size_t pos = 0;
                while (pos < list.size()) {
                    auto comma = list.find(',', pos);
                    std::string nm = list.substr(pos,
                        comma == std::string::npos ? std::string::npos
                                                   : comma - pos);
                    while (!nm.empty() && nm.front() == ' ') nm.erase(0, 1);
                    while (!nm.empty() && nm.back() == ' ')  nm.pop_back();
                    if (!nm.empty()) t.tools.push_back({nm, false, true, 0, ""});
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
        } else if (starts_with(rest, "ok ") || starts_with(rest, "err ")) {
            bool ok = starts_with(rest, "ok ");
            std::string body = rest.substr(ok ? 3 : 4);
            auto sp = body.find(' ');
            std::string nm = body.substr(0, sp);
            int ms = 0;
            std::string err;
            if (sp != std::string::npos) {
                auto rest2 = body.substr(sp + 1);
                auto ms_end = rest2.find("ms");
                if (ms_end != std::string::npos) {
                    try { ms = std::stoi(rest2.substr(0, ms_end)); }
                    catch (...) {}
                    if (ms_end + 3 <= rest2.size())
                        err = rest2.substr(ms_end + 3);
                }
            }
            bool matched = false;
            for (auto& tr : t.tools) {
                if (tr.running && tr.name == nm) {
                    tr.running = false; tr.ok = ok; tr.ms = ms; tr.err = err;
                    matched = true; break;
                }
            }
            if (!matched) t.tools.push_back({nm, ok, false, ms, err, "", ""});
            if (ok) ++t.ok_count;
        } else if (starts_with(rest, "arg ") || starts_with(rest, "res ")) {
            bool is_arg = starts_with(rest, "arg ");
            std::string body = rest.substr(4);
            auto sp = body.find(' ');
            if (sp == std::string::npos) continue;
            std::string nm  = body.substr(0, sp);
            std::string val = body.substr(sp + 1);
            // Attach to the first tool of this name that doesn't already
            // have the corresponding summary populated. Order-aligned with
            // the dispatch list, so multiple `read` calls map correctly to
            // their respective paths.
            for (auto& tr : t.tools) {
                if (tr.name != nm) continue;
                std::string& slot = is_arg ? tr.arg_summary : tr.res_summary;
                if (slot.empty()) { slot = val; break; }
            }
        } else if (starts_with(rest, "done ")) {
            t.done = true;
        } else if (rest == "synthesis") {
            t.synthesis_open = true;
            synthesis_in_progress = true;
        }
    }

    // ── Visual primitives ──────────────────────────────────────────
    const bool is_active_run = tc.is_running();
    const Style rail_active   = Style{}.with_fg(accent).with_bold();
    const Style rail_settled  = fg_dim(muted);
    const Style rail_st       = is_active_run ? rail_active : rail_settled;
    const std::string rail    = "\xe2\x96\x8e ";   // ▎ + space

    constexpr std::array<const char*, 10> kSpinFrames = {
        "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
        "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
        "\xe2\xa0\x87","\xe2\xa0\x8f",
    };
    const auto spin_frame = static_cast<std::size_t>(
        std::max(0.0f, elapsed * 12.0f))
        % kSpinFrames.size();
    const std::string spin_glyph = kSpinFrames[spin_frame];

    auto kind_color = [](std::string_view name) -> Color {
        if (name == "edit" || name == "write")            return accent;
        if (name == "bash")                                return success;
        if (name == "todo")                                return warn;
        if (name.size() >= 4 && name.substr(0, 4) == "git_") return highlight;
        return info;
    };
    auto pad_right = [](std::string s, std::size_t w) {
        if (s.size() < w) s.append(w - s.size(), ' ');
        return s;
    };

    std::vector<Element> rows;

    // Strip 1: pull-quote question.
    if (!parsed_query.empty()) {
        std::string q = parsed_query;
        if (q.size() > 110) { q.resize(107); q += "..."; }
        rows.push_back(h(
            text("\xe2\x9d\x9d ", Style{}.with_fg(accent).with_bold()),
            text(q, Style{}.with_fg(fg).with_italic().with_bold()),
            text(" \xe2\x9d\x9e", Style{}.with_fg(accent).with_bold())
        ).build());
    }

    // Strip 2: live stats row.
    if (!turns.empty() || !parsed_model.empty()) {
        int total_turns = static_cast<int>(turns.size());
        int total_tools = 0;
        int total_ok    = 0;
        for (const auto& t : turns) {
            total_tools += static_cast<int>(t.tools.size());
            for (const auto& tr : t.tools) if (tr.ok) ++total_ok;
        }
        std::vector<Element> cells;
        if (tc.is_failed()) {
            cells.push_back(text("\xe2\x9c\x97 ",
                Style{}.with_fg(danger).with_bold()));
        } else if (tc.is_done()) {
            cells.push_back(text("\xe2\x9c\x93 ",
                Style{}.with_fg(success).with_bold()));
        } else {
            cells.push_back(text(spin_glyph + " ",
                Style{}.with_fg(accent).with_bold()));
        }
        std::ostringstream stat;
        stat << total_turns << " turn" << (total_turns == 1 ? "" : "s");
        if (total_tools > 0) {
            stat << "  \xc2\xb7  " << total_ok << "/" << total_tools
                 << " tool" << (total_tools == 1 ? "" : "s");
        }
        cells.push_back(text(stat.str(), Style{}.with_fg(fg).with_bold()));
        cells.push_back(spacer());
        std::ostringstream rhs;
        if (!parsed_model.empty()) {
            std::string m = parsed_model;
            if (m.find("haiku") != std::string::npos)      m = "haiku";
            else if (m.find("sonnet") != std::string::npos) m = "sonnet";
            else if (m.find("opus") != std::string::npos)   m = "opus";
            rhs << m << "  \xc2\xb7  ";
        }
        int sec_int = static_cast<int>(elapsed);
        int dec = static_cast<int>((elapsed - sec_int) * 10);
        rhs << sec_int << "." << dec << "s";
        cells.push_back(text(rhs.str(), fg_dim(muted)));
        rows.push_back((h(std::move(cells)) | grow(1.0f)).build());
    }

    if (!turns.empty()) rows.push_back(text(""));

    // Strip 3: per-turn timeline along the rail.
    const std::size_t kMaxTurnsShown = tc.expanded ? 12 : 4;
    std::size_t skip_turns = turns.size() > kMaxTurnsShown
        ? turns.size() - kMaxTurnsShown : 0;
    if (skip_turns > 0) {
        rows.push_back(h(
            text(rail, rail_settled),
            text("\xe2\x80\xa6 " + std::to_string(skip_turns)
                 + " earlier turn"
                 + (skip_turns == 1 ? "" : "s") + " collapsed",
                 fg_dim(muted))
        ).build());
    }

    std::size_t name_w = 8;
    for (const auto& t : turns)
        for (const auto& tr : t.tools)
            name_w = std::max(name_w, tr.name.size());
    name_w = std::min<std::size_t>(name_w, 14);

    for (std::size_t i = skip_turns; i < turns.size(); ++i) {
        const auto& t = turns[i];
        const bool is_current = !t.done && (i + 1 == turns.size());

        if (i > skip_turns)
            rows.push_back(h(text(rail, rail_st), text("")).build());

        std::string th_glyph;
        Style       th_style;
        if (t.done) {
            bool any_err = false;
            for (const auto& tr : t.tools) if (!tr.ok && !tr.running) any_err = true;
            if (any_err) {
                th_glyph = "\xe2\x9c\x97";
                th_style = Style{}.with_fg(danger).with_bold();
            } else {
                th_glyph = "\xe2\x9c\x93";
                th_style = Style{}.with_fg(success).with_bold();
            }
        } else if (is_current && t.thinking) {
            th_glyph = spin_glyph;
            th_style = Style{}.with_fg(accent).with_bold();
        } else if (is_current) {
            th_glyph = spin_glyph;
            th_style = Style{}.with_fg(info).with_bold();
        } else {
            th_glyph = "\xe2\x97\x8b";
            th_style = fg_dim(muted);
        }

        std::ostringstream hdr;
        hdr << "T" << t.n;
        if (t.done && !t.tools.empty()) {
            hdr << "  " << t.ok_count << "/" << t.tools.size();
        } else if (is_current && t.thinking) {
            hdr << "  thinking\xe2\x80\xa6";
        } else if (is_current && !t.tools.empty()) {
            int pending = 0;
            for (const auto& tr : t.tools) if (tr.running) ++pending;
            if (pending > 0) hdr << "  " << pending << " in-flight";
            else             hdr << "  " << t.ok_count << "/" << t.tools.size();
        }
        int turn_ms = 0;
        for (const auto& tr : t.tools) turn_ms += tr.ms;
        std::vector<Element> hdr_cells;
        hdr_cells.push_back(text(rail, rail_st));
        hdr_cells.push_back(text(th_glyph + " ", th_style));
        hdr_cells.push_back(text(hdr.str(),
            is_current ? Style{}.with_fg(fg).with_bold()
                       : Style{}.with_fg(fg)));
        hdr_cells.push_back(spacer());
        if (turn_ms > 0)
            hdr_cells.push_back(text(std::to_string(turn_ms) + "ms",
                                     fg_dim(muted)));
        rows.push_back((h(std::move(hdr_cells)) | grow(1.0f)).build());

        for (const auto& tr : t.tools) {
            Style icon_st;
            std::string icon;
            if (tr.running) {
                icon = spin_glyph + " ";
                icon_st = Style{}.with_fg(info);
            } else if (tr.ok) {
                icon = "\xe2\x9c\x93 ";
                icon_st = Style{}.with_fg(success).with_bold();
            } else {
                icon = "\xe2\x9c\x97 ";
                icon_st = Style{}.with_fg(danger).with_bold();
            }

            // Hard cap on the combined arg + " · " + res text so a
            // long path + verbose result hint can't blow the row
            // width and force yoga to wrap mid-word. The producer
            // (investigate.cpp:tool_arg_summary / _result_summary)
            // already truncates each piece to ~50 chars, but this
            // belt-and-suspenders cap protects against future tools
            // emitting long summaries we forgot about.
            std::string arg_disp = tr.arg_summary;
            std::string res_disp = tr.res_summary;
            constexpr std::size_t kCombinedCap = 90;
            std::size_t want = arg_disp.size() + res_disp.size()
                             + (res_disp.empty() ? 0 : 4);   // "  · "
            if (want > kCombinedCap) {
                // Drop res first (it's secondary); then truncate arg.
                std::size_t over = want - kCombinedCap;
                if (!res_disp.empty()) {
                    if (over >= res_disp.size() + 4) {
                        over -= res_disp.size() + 4;
                        res_disp.clear();
                    } else {
                        res_disp.resize(res_disp.size() - over - 1);
                        res_disp += "\xe2\x80\xa6";          // …
                        over = 0;
                    }
                }
                if (over > 0 && over < arg_disp.size()) {
                    arg_disp.resize(arg_disp.size() - over - 1);
                    arg_disp += "\xe2\x80\xa6";
                }
            }

            std::vector<Element> tcells;
            tcells.push_back(text(rail, rail_st));
            tcells.push_back(text("    " + icon, icon_st));
            tcells.push_back(text(pad_right(tr.name, name_w),
                Style{}.with_fg(kind_color(tr.name))));
            // Arg summary (path / pattern / query / etc.) — the *what*
            // of this call. Bright fg so it reads as the primary
            // content of the row, not metadata.
            if (!arg_disp.empty()) {
                tcells.push_back(text("  " + arg_disp,
                                      Style{}.with_fg(fg)));
            }
            // Result summary — what came back (match count, line count,
            // branch name, etc.). Dim cyan so it's distinguishable from
            // the arg without competing with it.
            if (!res_disp.empty()) {
                tcells.push_back(text("  \xc2\xb7  " + res_disp,
                                      Style{}.with_fg(highlight).with_dim()));
            }
            if (!tr.ok && !tr.running && !tr.err.empty()) {
                std::string e = tr.err;
                if (e.size() > 40) { e.resize(37); e += "..."; }
                tcells.push_back(text("  " + e,
                    Style{}.with_fg(danger).with_dim()));
            }
            tcells.push_back(spacer());
            if (!tr.running)
                tcells.push_back(text(std::to_string(tr.ms) + "ms",
                                      fg_dim(muted)));
            rows.push_back((h(std::move(tcells)) | grow(1.0f)).build());
        }
    }

    // Strip 4: synthesis banner + body.
    const TurnView* synth = nullptr;
    for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
        if (it->synthesis_open && !it->synthesis.empty()) {
            synth = &*it; break;
        }
    }
    std::string fallback_body;
    if (!synth && (tc.is_done() || tc.is_failed()) && !tc.output().empty()) {
        fallback_body = tc.output();
        if (fallback_body.starts_with("[investigate")) {
            if (auto end = fallback_body.find("]\n\n");
                end != std::string::npos)
                fallback_body = fallback_body.substr(end + 3);
        }
    }
    if (synth || !fallback_body.empty()) {
        const std::string label = (tc.is_failed() && !synth)
            ? "RESULT" : "SYNTHESIS";
        const Color label_color = tc.is_failed() ? danger : accent;
        rows.push_back(text(""));

        const std::string& body = synth ? synth->synthesis : fallback_body;

        // Word count for the live indicator. Approximate: split by
        // whitespace runs. Cheap; runs once per render.
        int word_count = 0;
        bool in_word = false;
        for (char c : body) {
            const bool ws = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
            if (!ws && !in_word) ++word_count;
            in_word = !ws;
        }

        // ── Banner row ─────────────────────────────────────────────
        // Streaming: "▶ SYNTHESIS  ·  writing… 312 words ───"
        // Done:      "▶ SYNTHESIS  ·  234 words           ───"
        std::string status_seg;
        if (tc.is_running()) {
            status_seg = "  \xc2\xb7  writing\xe2\x80\xa6 "
                       + std::to_string(word_count) + " words";
        } else if (word_count > 0) {
            status_seg = "  \xc2\xb7  " + std::to_string(word_count) + " words";
        }
        rows.push_back((h(
            text("\xe2\x96\xb6 ",
                 Style{}.with_fg(label_color).with_bold()),
            text(label, Style{}.with_fg(label_color).with_bold()),
            text(status_seg, fg_dim(muted)),
            spacer(),
            text("\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",
                 fg_dim(muted))
        ) | grow(1.0f)).build());

        // ── Body rendering policy ─────────────────────────────────
        // Layout-stability rule: during streaming we render NO body
        // text at all — only the banner above with its live word
        // counter. This is the user's complaint fix: the previous
        // "live tail" would re-flow on every delta as new lines
        // arrived and old lines fell off the tail window, causing
        // the card height to shrink/grow many times per second and
        // pushing everything below it around the screen.
        //
        // The cost: while streaming you don't see the synthesis text
        // accumulating — but the banner ticks "writing… 312 words" so
        // there's clear progress feedback, and the FULL answer lands
        // in the parent's tool_result the instant the sub-agent
        // finishes. The body renders once, statically, when done.
        //
        // When complete, render markdown HEAD preview (~800 chars
        // collapsed, ~4000 expanded) snapped to a line boundary so we
        // never slice a fenced code block or mid-list. `maya::markdown`
        // gives us proper headings / bold / inline code / fences.
        if (!tc.is_running()) {
            const std::size_t kHeadChars = tc.expanded ? 4000 : 800;
            auto snap_before_newline = [](std::string_view s,
                                          std::size_t pos) -> std::size_t {
                if (pos == 0) return 0;
                auto nl = s.rfind('\n', pos - 1);
                if (nl == std::string_view::npos) return pos;
                return nl + 1;
            };
            std::string snippet;
            std::size_t hidden_chars = 0;
            if (body.size() <= kHeadChars) {
                snippet = body;
            } else {
                std::size_t cut = snap_before_newline(body, kHeadChars);
                if (cut == 0) cut = kHeadChars;
                snippet = body.substr(0, cut);
                hidden_chars = body.size() - cut;
            }
            if (!snippet.empty()) {
                rows.push_back(maya::markdown(snippet));
            }
            if (hidden_chars > 0) {
                int approx_words = static_cast<int>(hidden_chars / 5);
                rows.push_back(text(
                    "\xe2\x80\xa6 +" + std::to_string(approx_words)
                    + " more words (full text returned to parent)",
                    fg_dim(muted)));
            }
        }
    }

    if (rows.empty()) return text("");
    return v(std::move(rows)).build();
}

// Terminal-state card cache. A chat with 40 tool calls rebuilds 40 borders
// + 40 Yoga layouts + 40 text runs every frame otherwise — even when
// nothing about those cards has changed in minutes. We only cache when the
// tool has reached a terminal status; running/pending tools rebuild so the
// live elapsed counter keeps ticking.
Element render_tool_call(const ToolUse& tc) {
    if (!tc.is_terminal())
        return render_tool_call_uncached(tc);
    auto& slot = tool_card_cache(tc.id);
    auto key = tc.compute_render_key();
    if (slot.element && slot.key == key)
        return *slot.element;
    auto built = render_tool_call_uncached(tc);
    slot.element = std::make_shared<Element>(built);
    slot.key     = key;
    return built;
}

} // namespace moha::ui
