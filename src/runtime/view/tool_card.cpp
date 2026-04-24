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

    return tool_card(tc.name.value, ToolCallKind::Other,
        tc.args.is_object() && !tc.args.empty() ? tc.args_dump() : "",
        tc.status, tc.expanded, tc.output(), elapsed);
}

} // namespace

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
