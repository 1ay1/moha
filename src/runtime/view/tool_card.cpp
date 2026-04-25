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

#include "moha/diff/diff.hpp"
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

// Split a string into lines (without owning them). Used by the head+
// tail truncator so we can pick lines from front and back of the body.
std::vector<std::string_view> split_lines_view(const std::string& s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.emplace_back(s.data() + start, i - start);
            start = i + 1;
        }
    }
    if (start < s.size()) out.emplace_back(s.data() + start, s.size() - start);
    return out;
}

// Smart head+tail elision: for content longer than `cap_lines`, show
// `head` lines from the start, an elision marker, and `tail` lines from
// the end. Reads like a `git diff` smart-context block — far more
// useful than just showing the first N and dropping the conclusion.
// Returns the stitched preview and the count of elided lines (0 when
// nothing was elided).
struct ElidedPreview {
    std::vector<std::string> lines;
    int elided = 0;
};

ElidedPreview head_tail_lines(const std::string& s, int head, int tail) {
    auto all = split_lines_view(s);
    int total = static_cast<int>(all.size());
    ElidedPreview out;
    int cap = head + tail;
    if (total <= cap) {
        out.lines.reserve(static_cast<std::size_t>(total));
        for (auto v : all) out.lines.emplace_back(v);
        return out;
    }
    out.lines.reserve(static_cast<std::size_t>(cap));
    for (int i = 0; i < head; ++i) out.lines.emplace_back(all[static_cast<std::size_t>(i)]);
    out.elided = total - head - tail;
    for (int i = total - tail; i < total; ++i)
        out.lines.emplace_back(all[static_cast<std::size_t>(i)]);
    return out;
}

// Format an elapsed-seconds value the way the maya widgets do — keeps the
// edit card visually consistent with the rest of the timeline. Sub-second
// reads as "230ms", under a minute as "0.4s", longer as "2m04s".
std::string format_elapsed_secs(float secs) {
    char buf[32];
    if (secs < 1.0f) {
        std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(secs * 1000.0f));
    } else if (secs < 60.0f) {
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
    } else {
        int mins = static_cast<int>(secs) / 60;
        float rem = secs - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%02ds", mins, static_cast<int>(rem));
    }
    return buf;
}

// Done edit card with a structured FileChange — renders the actual unified
// diff (line numbers, hunk headers, context) via maya's DiffView, wrapped
// in chrome that mirrors EditTool's look (✓ Edit border, green on success
// / red on failure) plus a styled "+N -M" line-delta in the header.
//
// Falls back to the streaming-style EditTool widget at the call site when
// the change isn't available yet (still streaming, applying, or the rare
// no-op edit that produced zero hunks).
Element render_edit_diff_card(const FileChange& ch,
                              const std::string& header_text,
                              float elapsed,
                              bool failed,
                              std::string_view error_text) {
    // Header row: "<path>   +12 -3   0.4s"
    //   path : default fg
    //   +N   : green
    //   -M   : red
    //   ts   : dim
    std::string line = header_text;
    std::vector<StyledRun> runs;

    {
        std::string plus = "  +" + std::to_string(ch.added);
        auto plus_off = line.size();
        line += plus;
        // Skip the two leading spaces in the styled run so only "+N" is colored.
        runs.push_back(StyledRun{plus_off + 2, plus.size() - 2,
                                  Style{}.with_fg(Color::green())});
    }
    {
        std::string minus = " -" + std::to_string(ch.removed);
        auto minus_off = line.size();
        line += minus;
        runs.push_back(StyledRun{minus_off + 1, minus.size() - 1,
                                  Style{}.with_fg(Color::red())});
    }
    if (elapsed > 0.0f) {
        std::string ts = "  " + format_elapsed_secs(elapsed);
        auto ts_off = line.size();
        line += ts;
        runs.push_back(StyledRun{ts_off, ts.size(), Style{}.with_dim()});
    }

    Element header_elem{TextElement{
        .content = std::move(line),
        .style = {},
        .wrap = TextWrap::NoWrap,
        .runs = std::move(runs),
    }};

    // Body — the unified diff. show_border=false so our outer chrome owns
    // the border; show_line_numbers=true is the whole point of this card.
    // We render hunks directly rather than calling diff::render_unified()
    // because the latter prefixes `--- a/path` / `+++ b/path` lines that
    // DiffView would re-color as red/green rows — duplicating the path
    // info already in the border label and header.
    std::string body_diff;
    body_diff.reserve(256);
    for (const auto& h : ch.hunks) {
        body_diff += "@@ -" + std::to_string(h.old_start) + ","
                   + std::to_string(h.old_len) + " +"
                   + std::to_string(h.new_start) + ","
                   + std::to_string(h.new_len) + " @@\n";
        body_diff += h.patch;
    }
    DiffView::Config dvc;
    dvc.show_border = false;
    dvc.show_line_numbers = true;
    DiffView dv(ch.path, std::move(body_diff), std::move(dvc));

    // Choose chrome by terminal status.
    auto border_color = failed ? Color::red() : Color::green();
    auto border_style = failed ? BorderStyle::Dashed : BorderStyle::Round;
    std::string border_label = failed ? " \xe2\x9c\x97 Edit "      // ✗
                                       : " \xe2\x9c\x93 Edit ";    // ✓

    std::vector<Element> rows;
    rows.push_back(std::move(header_elem));
    rows.push_back(dv.build());
    if (failed && !error_text.empty()) {
        rows.push_back(Element{TextElement{
            .content = "\xe2\x9c\x97 " + std::string{error_text},
            .style = Style{}.with_fg(Color::red()),
            .wrap = TextWrap::Wrap,
        }});
    }

    return (dsl::v(std::move(rows))
        | dsl::border(border_style)
        | dsl::bcolor(border_color)
        | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
        | dsl::padding(0, 1, 0, 1)).build();
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
        // Rich-diff card: terminal Done/Failed with a structured FileChange.
        // The tool already computed line-numbered hunks (src/diff/diff.cpp);
        // we render via maya's DiffView for the line-numbered, hunk-headed,
        // context-aware view. Skips the "no actual hunks" edge case (an edit
        // whose old==new produced a zero-hunk change) and falls through to
        // the streaming widget so the user still sees what the model tried.
        if (tc.is_terminal()) {
            if (auto* ch = tc.change(); ch && !ch->hunks.empty()) {
                return render_edit_diff_card(*ch, header, elapsed,
                                             tc.is_failed(), tc.output());
            }
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

    // ── recall(topic|id) ────────────────────────────────────────────
    // One memo's full content. Header: the topic the model asked for.
    // Body: the memo's `# title` + body, rendered as styled rows so the
    // title pops and the body reads like prose.
    if (tc.name == "recall") {
        auto topic = safe_arg(tc.args, "topic");
        if (topic.empty()) topic = safe_arg(tc.args, "id");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "recall";
        cfg.kind        = ToolCallKind::Read;
        cfg.description = with_desc(topic.empty() ? "recall" : topic, desc);
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
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.starts_with("# ")) {
                    rows.push_back(text(line.substr(2),
                        Style{}.with_fg(highlight).with_bold()));
                } else if (line.starts_with("[")) {
                    // [meta: model, freshness, ...]
                    rows.push_back(text(line, fg_dim(muted)));
                } else {
                    rows.push_back(text(line, fg_of(fg)));
                }
                ++row_count;
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── memos(filter?) ─────────────────────────────────────────────
    // List of stored memos with status icons (✓ fresh, ⚠ stale), topic,
    // and the per-memo metadata line. Each memo's body excerpt indents
    // under the header so the eye can group rows by memo.
    if (tc.name == "memos") {
        auto filter = safe_arg(tc.args, "filter");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "memos";
        cfg.kind        = ToolCallKind::Read;
        std::string title = filter.empty() ? std::string{"memos"} : "memos · " + filter;
        cfg.description = with_desc(title, desc);
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
            constexpr int kMaxRows = 30;
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.starts_with("Workspace memory")) {
                    rows.push_back(text(line, fg_dim(muted)));
                } else if (line.starts_with("\xe2\x9c\x93 ")) {       // ✓ fresh memo header
                    rows.push_back(text(line,
                        Style{}.with_fg(success).with_bold()));
                } else if (line.starts_with("\xe2\x9a\xa0 ")) {       // ⚠ stale memo header
                    rows.push_back(text(line,
                        Style{}.with_fg(warn).with_bold()));
                } else if (line.starts_with("   Q:") || line.starts_with("   files:")) {
                    rows.push_back(text(line, fg_dim(fg)));
                } else if (line.empty()) {
                    rows.push_back(text("", {}));
                } else {
                    rows.push_back(text(line, fg_of(fg)));
                }
                ++row_count;
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── remember(topic, content) ──────────────────────────────────
    // Confirmation card: shows the topic that was banked + the
    // workspace memo count. Card style is "edit-like" (it mutates
    // .moha/memos.json) so the green border on success matches the
    // edit/write semantics.
    if (tc.name == "remember") {
        auto topic = safe_arg(tc.args, "topic");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "remember";
        cfg.kind        = ToolCallKind::Edit;
        cfg.description = with_desc(topic.empty() ? "remember" : topic, desc);
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
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                if (line.starts_with("\xe2\x9c\x93 ")) {
                    rows.push_back(text(line,
                        Style{}.with_fg(success).with_bold()));
                } else if (line.starts_with("  \xc2\xb7  ")) {        // "  ·  ..." metadata line
                    rows.push_back(text(line, fg_dim(muted)));
                } else {
                    rows.push_back(text(line, fg_dim(fg)));
                }
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── forget(target) ────────────────────────────────────────────
    if (tc.name == "forget") {
        auto target = safe_arg(tc.args, "target");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "forget";
        cfg.kind        = ToolCallKind::Delete;
        cfg.description = with_desc(target.empty() ? "forget" : target, desc);
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        card.set_status(tc_status(tc.status));
        card.set_elapsed(elapsed);
        if (tc.is_failed() && !tc.output().empty()) {
            card.set_content(text(tc.output(), Style{}.with_fg(danger)));
            return card.build();
        }
        if (tc.is_done() && !tc.output().empty()) {
            // Output is a one-liner: "✗ forgot N memos matching 'x'" or
            // "no memo matched 'x'". Color the leading glyph by outcome.
            const auto& out = tc.output();
            Style st = out.starts_with("\xe2\x9c\x97 ")
                ? Style{}.with_fg(danger).with_bold()
                : fg_dim(muted);
            card.set_content(text(out, st));
        }
        return card.build();
    }

    // ── find_usages(symbol) ───────────────────────────────────────
    // Header: the symbol. Body: the file list with reference counts.
    // Useful enough on its own that we want it to stand out — the
    // model often calls it before a refactor and the user wants to
    // glance at the blast radius.
    if (tc.name == "find_usages") {
        auto sym = safe_arg(tc.args, "symbol");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "find_usages";
        cfg.kind        = ToolCallKind::Search;
        cfg.description = with_desc(sym.empty() ? "find_usages" : sym, desc);
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
            constexpr int kMaxRows = 20;
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.starts_with("Symbol ")) {
                    rows.push_back(text(line,
                        Style{}.with_fg(highlight).with_bold()));
                } else if (line.starts_with("(Use ")) {
                    rows.push_back(text(line, fg_dim(muted)));
                } else if (line.empty()) {
                    continue;   // collapse blank rows
                } else {
                    // "  path  (N refs)" rows.
                    rows.push_back(text(line, fg_of(fg)));
                }
                ++row_count;
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── mine_adrs(since?) ─────────────────────────────────────────
    if (tc.name == "mine_adrs") {
        auto since = safe_arg(tc.args, "since");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "mine_adrs";
        cfg.kind        = ToolCallKind::Agent;
        std::string title = since.empty() ? std::string{"mine_adrs"}
                                          : "mine_adrs · since " + since;
        cfg.description = with_desc(title, desc);
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
            constexpr int kMaxRows = 30;
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.starts_with("# ")) {
                    rows.push_back(text(line.substr(2),
                        Style{}.with_fg(highlight).with_bold()));
                } else if (line.starts_with("\xe2\x9c\x93 ")) {
                    rows.push_back(text(line,
                        Style{}.with_fg(success).with_bold()));
                } else if (line.empty()) {
                    rows.push_back(text("", {}));
                } else {
                    rows.push_back(text(line, fg_dim(fg)));
                }
                ++row_count;
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
        return card.build();
    }

    // ── navigate(question) ────────────────────────────────────────
    // Ranked semantic finder. Output has "## Likely modules" / "## Top
    // files" / "## Top symbols" sections with scored rows. Render the
    // section headers in accent color and rows in default fg with
    // [score] in highlight so the scores read as the salient signal.
    if (tc.name == "navigate") {
        auto q = safe_arg(tc.args, "question");
        maya::ToolCall::Config cfg;
        cfg.tool_name   = "navigate";
        cfg.kind        = ToolCallKind::Search;
        std::string head = q.size() > 70 ? q.substr(0, 67) + "..." : q;
        cfg.description = with_desc(head.empty() ? "navigate" : head, desc);
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
            constexpr int kMaxRows = 30;
            while (std::getline(iss, line) && row_count < kMaxRows) {
                if (line.starts_with("Navigation results")) {
                    rows.push_back(text(line, fg_dim(muted)));
                } else if (line.starts_with("## ")) {
                    rows.push_back(text(line.substr(3),
                        Style{}.with_fg(accent).with_bold()));
                } else if (line.starts_with("  [")) {
                    // "  [score] body" — surface the score.
                    auto rb = line.find(']');
                    if (rb != std::string::npos) {
                        rows.push_back(h(
                            text(line.substr(0, rb + 1),
                                 Style{}.with_fg(highlight).with_bold()),
                            text(line.substr(rb + 1), fg_of(fg))
                        ).build());
                    } else {
                        rows.push_back(text(line, fg_of(fg)));
                    }
                } else if (line.empty()) {
                    continue;
                } else {
                    rows.push_back(text(line, fg_dim(fg)));
                }
                ++row_count;
            }
            if (!rows.empty()) card.set_content(v(std::move(rows)).build());
        }
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

            // Per-row layout:
            //   rail · icon · name · arg · [sec] · spacer · ms
            //
            // Width discipline (more aggressive than before — was
            // breaking the layout on failed rows where err is often
            // longer than the path):
            //   * Drop the "·" separator when `arg` is empty —
            //     leaving "name  ·  err" with a phantom column gap
            //     looks broken (it's what you saw in the screenshot).
            //   * Strip the leading "[<kind>] " framing from err —
            //     the red ✗ icon already conveys "this failed"; the
            //     framing tag is redundant noise that eats width.
            //   * Combined cap dropped from 90 → 80 chars so the row
            //     stays inside an 80-column terminal cleanly.
            //   * Failed-row secondary (err) gets the tighter cap so
            //     the error message stays scannable.
            std::string arg_disp = tr.arg_summary;
            std::string sec_disp;
            bool        sec_is_err = false;
            if (!tr.ok && !tr.running) {
                sec_disp   = tr.err;
                sec_is_err = true;
                // Strip the "[<kind>] " framing prefix that
                // ToolError::render() adds — the icon column already
                // says "this failed", we don't need to repeat the
                // category in text. Keeps the visible message focused
                // on what actually went wrong.
                if (sec_disp.starts_with("[")) {
                    if (auto rb = sec_disp.find("] ");
                        rb != std::string::npos) {
                        sec_disp.erase(0, rb + 2);
                    }
                }
            } else if (tr.ok) {
                sec_disp = tr.res_summary;
            }
            const std::size_t kSecCap = sec_is_err ? 36u : 48u;
            if (sec_disp.size() > kSecCap) {
                sec_disp.resize(kSecCap - 1);
                sec_disp += "\xe2\x80\xa6";
            }
            constexpr std::size_t kCombinedCap = 80;
            const std::size_t sep_width = (arg_disp.empty()
                                         || sec_disp.empty()) ? 0u : 4u;
            std::size_t want = arg_disp.size() + sec_disp.size() + sep_width;
            if (want > kCombinedCap) {
                std::size_t over = want - kCombinedCap;
                if (!sec_disp.empty()) {
                    if (over >= sec_disp.size() + sep_width) {
                        over -= sec_disp.size() + sep_width;
                        sec_disp.clear();
                    } else {
                        sec_disp.resize(sec_disp.size() - over - 1);
                        sec_disp += "\xe2\x80\xa6";
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
            if (!arg_disp.empty()) {
                tcells.push_back(text("  " + arg_disp,
                                      Style{}.with_fg(fg)));
            }
            if (!sec_disp.empty()) {
                Style sec_st = sec_is_err
                    ? Style{}.with_fg(danger).with_dim()
                    : Style{}.with_fg(highlight).with_dim();
                // Use the dot separator only when there's a left
                // neighbour (arg) to separate FROM. Otherwise the
                // err sits flush after the name pad — no phantom
                // column gap.
                std::string lead = arg_disp.empty()
                    ? std::string{"  "}
                    : std::string{"  \xc2\xb7  "};
                tcells.push_back(text(lead + sec_disp, sec_st));
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

// Render compact body content for a single tool event — placed under the
// timeline event's `│` connector. Tool-specific so each row carries
// real, glanceable information: a few lines of read content, the diff
// hunks for an edit, the head of bash output, etc. Empty Element when
// nothing useful exists yet (still streaming) — caller handles spacing.
//
// Lives in the same TU as render_tool_call so per-tool branches for the
// compact and rich-card views are co-located. When you change a tool's
// rendering, this is the only file you should need to touch.
Element render_tool_compact(const ToolUse& tc) {
    const auto& n = tc.name.value;
    constexpr int kMaxLines = 6;

    auto code_line = [](std::string_view ln, Style st) {
        return text(std::string{ln}, st);
    };

    // ── Edit: unified-diff-style preview, per-hunk head+tail ───
    // The inline timeline body trades exact reviewability for visual
    // density — a 200-line replacement would dominate the panel and
    // push later events off-screen. Each side gets a 6-line head + 2-
    // line tail so a short edit shows in full while a long one
    // collapses to "head … N hidden … tail". The full diff is always
    // available in the rounded rich card path (render_edit_diff_card
    // → DiffView) when terminal+done with a structured FileChange.
    if (n == "edit" && tc.args.is_object()) {
        std::vector<Element> rows;
        auto rem      = Style{}.with_fg(danger);
        auto add      = Style{}.with_fg(success);
        auto rem_pre  = Style{}.with_fg(danger).with_dim();
        auto add_pre  = Style{}.with_fg(success).with_dim();

        // Per-side cap. 6+2 keeps short edits fully visible (≤ 8 rows)
        // while still capping the worst case (200-line replacement) at
        // ~10 rows — enough to recognize the function being edited
        // (head) and how it ends (tail) without overrunning the panel.
        constexpr int kHeadPerSide = 6;
        constexpr int kTailPerSide = 2;

        auto count_lines_in = [](std::string_view s) {
            if (s.empty()) return 0;
            int n_ = 1;
            for (char c : s) if (c == '\n') ++n_;
            // Trailing newline shouldn't count as an extra empty line.
            if (s.back() == '\n') --n_;
            return std::max(0, n_);
        };

        auto push_side = [&](std::string_view body, char marker,
                             Style mark_style, Style line_style) {
            if (body.empty()) return;
            auto p = head_tail_lines(std::string{body},
                                     kHeadPerSide, kTailPerSide);
            for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
                if (p.elided > 0 && i == kHeadPerSide) {
                    rows.push_back(h(
                        text(std::string{marker} + " ", mark_style),
                        text("\xe2\x80\xa6 " + std::to_string(p.elided)
                             + " hidden", fg_dim(muted))
                    ).build());
                }
                rows.push_back(h(
                    text(std::string{marker} + " ", mark_style),
                    code_line(p.lines[static_cast<std::size_t>(i)], line_style)
                ).build());
            }
        };

        auto push_hunk = [&](int hunk_idx, int hunk_total,
                             std::string_view old_text,
                             std::string_view new_text) {
            int minus = count_lines_in(old_text);
            int plus  = count_lines_in(new_text);
            // Per-hunk header: `edit i/N  ·  −k / +m`. Skipped on
            // single-edit calls where the decoration would be noise.
            if (hunk_total > 1) {
                std::string tag = "edit " + std::to_string(hunk_idx + 1)
                                + "/" + std::to_string(hunk_total)
                                + "  \xc2\xb7  ";
                std::string stat = "\xe2\x88\x92" + std::to_string(minus)
                                 + " / +" + std::to_string(plus);
                rows.push_back(h(
                    text(std::move(tag),  fg_dim(muted)),
                    text(std::move(stat), fg_dim(muted))
                ).build());
            }
            push_side(old_text, '-', rem_pre, rem);
            push_side(new_text, '+', add_pre, add);
        };

        if (auto it = tc.args.find("edits");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            constexpr int kMaxHunksShown = 4;
            int total = static_cast<int>(it->size());
            int shown = 0;
            for (const auto& e : *it) {
                if (shown >= kMaxHunksShown) {
                    rows.push_back(text(
                        "\xe2\x80\xa6 "
                            + std::to_string(total - shown) + " more edits",
                        fg_dim(muted)));
                    break;
                }
                if (!e.is_object()) continue;
                auto ot = e.value("old_text", e.value("old_string", std::string{}));
                auto nt = e.value("new_text", e.value("new_string", std::string{}));
                push_hunk(shown, total, ot, nt);
                ++shown;
            }
        } else {
            // Top-level legacy single-edit shape.
            auto ot = safe_arg(tc.args, "old_text"); if (ot.empty()) ot = safe_arg(tc.args, "old_string");
            auto nt = safe_arg(tc.args, "new_text"); if (nt.empty()) nt = safe_arg(tc.args, "new_string");
            if (!ot.empty() || !nt.empty()) push_hunk(0, 1, ot, nt);
        }
        if (rows.empty()) return text("");
        return v(std::move(rows)).build();
    }

    // Render an elided head+tail preview as a vertical stack with a
    // dim "··· N hidden ···" centered marker. Reads like `git diff`'s
    // smart context: top of file, gap, bottom of file — far more
    // informative than only the first N lines.
    auto preview_block = [&](const std::string& body, Style line_style) -> Element {
        constexpr int kHead = 4;
        constexpr int kTail = 3;
        auto p = head_tail_lines(body, kHead, kTail);
        std::vector<Element> rows;
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (p.elided > 0 && i == kHead) {
                rows.push_back(text("\xc2\xb7 \xc2\xb7 \xc2\xb7  "
                                    + std::to_string(p.elided) + " hidden  \xc2\xb7 \xc2\xb7 \xc2\xb7",
                                    fg_dim(muted)));
            }
            rows.push_back(text(p.lines[static_cast<std::size_t>(i)], line_style));
        }
        return v(std::move(rows)).build();
    };

    // ── Write: head+tail of the streaming/written content ──────────
    if (n == "write") {
        std::string content = safe_arg(tc.args, "content");
        if (content.empty()) return text("");
        return preview_block(content, fg_dim(fg));
    }

    // ── Bash / diagnostics: head+tail of output ────────────────────
    if ((n == "bash" || n == "diagnostics") && tc.is_terminal()) {
        auto out = strip_bash_output_fence(tc.output());
        if (out.empty()) return text("");
        return preview_block(out, fg_dim(fg));
    }
    // Live bash progress (running stdout snapshot).
    if (n == "bash" && tc.is_running() && !tc.progress_text().empty()) {
        return preview_block(tc.progress_text(), fg_dim(fg));
    }

    // ── Per-tool structured bodies (Actions-panel inline) ─────────
    // Each branch produces a header row (primary noun + counts) plus a
    // body matched to the tool's output shape — coloured kind tags,
    // file-grouped matches, iconified directory entries, etc. The
    // Actions panel is moha's primary tool surface; treating each tool
    // as a glance-card here is much higher leverage than the standalone
    // tool-card path (which fires only when the assistant's turn has a
    // single tool call).
    //
    // Local helpers — keep them inside the function so each tool branch
    // can compose its rows without per-tool boilerplate.
    auto meta_row = [&](std::string_view glyph, Color glyph_c,
                        std::string_view primary,
                        std::string_view stats) {
        std::vector<Element> cells;
        cells.push_back(text(std::string{glyph} + " ",
                             Style{}.with_fg(glyph_c).with_bold()));
        if (!primary.empty()) {
            cells.push_back(text(std::string{primary},
                                 Style{}.with_fg(fg).with_bold()));
        }
        if (!stats.empty()) {
            cells.push_back(text("  \xc2\xb7  " + std::string{stats},
                                 fg_dim(muted)));
        }
        return (h(std::move(cells)) | grow(1.0f)).build();
    };

    // ── read ─────────────────────────────────────────────────────
    // Header: "▸ <path> · L<a>-<b> of <total>" + a tighter preview
    // of the file body with subtle line numbering coloured cyan so
    // the eye can scan to the relevant line at a glance.
    if (n == "read" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto path = pick_arg(tc.args, {"path","file_path","filepath","filename"});
        // Detect the "[showing lines A-B of T]" footer the read tool
        // emits when paged.
        std::string range;
        if (auto p = out.find("[showing lines "); p != std::string::npos) {
            auto end = out.find(']', p);
            if (end != std::string::npos)
                range = out.substr(p + 15, end - (p + 15));
        }
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info,
                                path, range.empty() ? "" : "L" + range));
        // Body: head+tail. Existing preview_block is fine here — the
        // file body's own `cat -n` numbering is already styled by the
        // read tool itself.
        rows.push_back(preview_block(out, fg_dim(fg)));
        return v(std::move(rows)).build();
    }

    // ── grep / find_definition ──────────────────────────────────
    // Header: "▸ <pattern> · N matches in K files".
    // Body:   per-file groups, file path in info-bold, then up to 2
    //         match rows per group with `Lnnn` line numbers in cyan.
    if ((n == "grep" || n == "find_definition") && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        std::string_view kind = (n == "grep" ? "pattern" : "symbol");
        auto needle = safe_arg(tc.args, kind.data());
        // Pull "Found N matches across K files" if present (grep emits it).
        std::string stats;
        if (auto p = out.find("Found "); p != std::string::npos) {
            auto end = out.find('.', p);
            if (end != std::string::npos)
                stats = out.substr(p + 6, end - (p + 6));
        }
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, needle, stats));

        // Walk the markdown-ish output and emit colour-tagged rows.
        // Format produced by grep.cpp:
        //   ## Matches in <path>
        //   ### L<s>-<e>
        //   ```
        //   <line>
        //   <line>
        //   ```
        std::istringstream iss(out);
        std::string line;
        int row_count = 0;
        const int kCap = 14;
        bool in_code = false;
        int code_no = 0;
        while (std::getline(iss, line) && row_count < kCap) {
            if (line.starts_with("Found ") || line.starts_with("Showing ")
             || line.empty()) continue;
            if (line.starts_with("## Matches in ")) {
                rows.push_back(text("  " + line.substr(14),
                    Style{}.with_fg(info).with_bold()));
                ++row_count;
                in_code = false;
                continue;
            }
            if (line.starts_with("### L")) {
                try { code_no = std::stoi(line.substr(5)); } catch (...) { code_no = 0; }
                continue;
            }
            if (line == "```") { in_code = !in_code; continue; }
            if (in_code && code_no > 0) {
                std::ostringstream lr;
                lr << "    L" << code_no << "  " << line;
                rows.push_back(text(lr.str(), fg_dim(fg)));
                ++code_no;
                ++row_count;
                continue;
            }
            // Legacy `path:line:content` (find_definition).
            if (auto c1 = line.find(':'); c1 != std::string::npos) {
                if (auto c2 = line.find(':', c1 + 1); c2 != std::string::npos) {
                    rows.push_back(h(
                        text("  " + line.substr(0, c1),
                             Style{}.with_fg(info).with_bold()),
                        text(":" + line.substr(c1 + 1, c2 - c1 - 1),
                             Style{}.with_fg(highlight)),
                        text("  " + line.substr(c2 + 1), fg_dim(fg))
                    ).build());
                    ++row_count;
                    continue;
                }
            }
            // Anything else.
            rows.push_back(text(line, fg_dim(muted)));
            ++row_count;
        }
        if (row_count >= kCap)
            rows.push_back(text("  \xe2\x80\xa6 more (use offset)",
                                fg_dim(muted)));
        return v(std::move(rows)).build();
    }

    // ── glob ────────────────────────────────────────────────────
    // Header: "▸ <pattern> · N files".
    // Body:   numbered file rows.
    if (n == "glob" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto pat = safe_arg(tc.args, "pattern");
        // Count non-empty lines as a rough match count (the glob tool
        // doesn't emit a Found-N preamble).
        int total = 0;
        for (char c : out) if (c == '\n') ++total;
        if (!out.empty() && out.back() != '\n') ++total;
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, pat,
            std::to_string(total) + (total == 1 ? " file" : " files")));
        std::istringstream iss(out);
        std::string line;
        int row_count = 0;
        const int kCap = 8;
        while (std::getline(iss, line) && row_count < kCap) {
            if (line.empty()) continue;
            if (line.starts_with("./")) line = line.substr(2);
            rows.push_back(h(
                text("  \xe2\x97\x8b ", fg_dim(muted)),     // ○
                text(line, fg_of(fg))
            ).build());
            ++row_count;
        }
        if (total > row_count)
            rows.push_back(text("  \xe2\x80\xa6 +"
                                + std::to_string(total - row_count)
                                + " more", fg_dim(muted)));
        return v(std::move(rows)).build();
    }

    // ── list_dir ────────────────────────────────────────────────
    // Header: "▸ <path> · M dirs / N files".
    // Body:   iconified rows — folders in info-bold, files in fg.
    if (n == "list_dir" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto path = safe_arg(tc.args, "path");
        if (path.empty()) path = ".";
        int dirs = 0, files = 0;
        std::istringstream count_iss(out);
        std::string s;
        while (std::getline(count_iss, s)) {
            // Trim leading spaces so nested entries are counted too.
            std::size_t pos = 0;
            while (pos < s.size() && s[pos] == ' ') ++pos;
            if (pos >= s.size()) continue;
            auto trimmed = std::string_view{s}.substr(pos);
            // Heuristic: line ending in "/" is a directory; line with
            // "  <size>" suffix is a file.
            if (!trimmed.empty() && trimmed.back() == '/') ++dirs;
            else if (trimmed.find("  ") != std::string_view::npos) ++files;
        }
        std::ostringstream stat;
        stat << dirs << (dirs == 1 ? " dir" : " dirs")
             << " / " << files << (files == 1 ? " file" : " files");
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, path, stat.str()));
        std::istringstream iss(out);
        std::string line;
        int row_count = 0;
        const int kCap = tc.expanded ? 20 : 10;
        while (std::getline(iss, line) && row_count < kCap) {
            if (line.empty()) continue;
            // Preserve indent.
            std::size_t pos = 0;
            while (pos < line.size() && line[pos] == ' ') ++pos;
            std::string indent = line.substr(0, pos);
            std::string body = line.substr(pos);
            if (!body.empty() && body.back() == '/') {
                rows.push_back(h(
                    text("  " + indent + "\xe2\x96\xb6 ",  // ▶
                         Style{}.with_fg(info)),
                    text(body, Style{}.with_fg(info).with_bold())
                ).build());
            } else {
                // Split off the trailing size for dim styling.
                auto sz = body.rfind("  ");
                if (sz != std::string::npos) {
                    rows.push_back(h(
                        text("  " + indent + "  ", {}),
                        text(body.substr(0, sz), fg_of(fg)),
                        text(body.substr(sz), fg_dim(muted))
                    ).build());
                } else {
                    rows.push_back(text("  " + indent + "  " + body,
                                        fg_of(fg)));
                }
            }
            ++row_count;
        }
        return v(std::move(rows)).build();
    }

    // ── outline ─────────────────────────────────────────────────
    // Header: "▸ <path> · N symbols".
    // Body:   kind-grouped rows with "L<n>" cyan.
    if (n == "outline" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto path = safe_arg(tc.args, "path");
        // First line is "<path>  (N symbols)" — pull the count.
        std::string stats;
        if (auto p = out.find("("); p != std::string::npos) {
            auto end = out.find(')', p);
            if (end != std::string::npos) stats = out.substr(p + 1, end - p - 1);
        }
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, path, stats));
        std::istringstream iss(out);
        std::string line;
        int row_count = 0;
        const int kCap = tc.expanded ? 20 : 10;
        bool past_header = false;
        while (std::getline(iss, line) && row_count < kCap) {
            if (!past_header) {
                if (line.find("symbols)") != std::string::npos) {
                    past_header = true; continue;
                }
                continue;
            }
            if (line.empty()) continue;
            if (line.starts_with("[") && line.find(']') != std::string::npos) {
                rows.push_back(text("  " + line,
                    Style{}.with_fg(highlight).with_bold()));
            } else {
                rows.push_back(text("  " + line, fg_dim(fg)));
            }
            ++row_count;
        }
        return v(std::move(rows)).build();
    }

    // ── signatures ──────────────────────────────────────────────
    if (n == "signatures" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto pat = safe_arg(tc.args, "pattern");
        // "Symbols matching '<pat>' (N hits, ...)"
        std::string stats;
        if (auto p = out.find("("); p != std::string::npos) {
            auto end = out.find(',', p);
            if (end != std::string::npos) stats = out.substr(p + 1, end - p - 1);
        }
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, pat, stats));
        std::istringstream iss(out);
        std::string line;
        int row_count = 0;
        const int kCap = tc.expanded ? 20 : 10;
        while (std::getline(iss, line) && row_count < kCap) {
            if (line.empty() || line.starts_with("Symbols matching")) continue;
            if (line.starts_with("## ")) {
                rows.push_back(text("  " + line.substr(3),
                    Style{}.with_fg(info).with_bold()));
                ++row_count;
            } else if (line.starts_with("  L")) {
                rows.push_back(text("  " + line, fg_dim(fg)));
                ++row_count;
            }
        }
        return v(std::move(rows)).build();
    }

    // ── repo_map ────────────────────────────────────────────────
    if (n == "repo_map" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto path = safe_arg(tc.args, "path");
        if (path.empty()) path = "workspace";
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, path, ""));
        std::istringstream iss(out);
        std::string line;
        int row_count = 0;
        const int kCap = tc.expanded ? 30 : 12;
        while (std::getline(iss, line) && row_count < kCap) {
            if (line.empty()) continue;
            if (line.starts_with("# Repo map")) continue;
            if (line.starts_with("# ")) {
                rows.push_back(text("  " + line, fg_dim(muted)));
            } else if (line.starts_with("## ")) {
                rows.push_back(text("  " + line,
                    Style{}.with_fg(accent).with_bold()));
            } else if (line.starts_with("[") && line.find(']') != std::string::npos
                    && line.find('/') != std::string::npos) {
                auto rb = line.find(']');
                rows.push_back(h(
                    text("  " + line.substr(0, rb + 1),
                         Style{}.with_fg(highlight).with_bold()),
                    text(line.substr(rb + 1), fg_of(fg))
                ).build());
            } else if (line.ends_with("/")) {
                rows.push_back(text("  " + line,
                    Style{}.with_fg(info).with_bold()));
            } else {
                rows.push_back(text("  " + line, fg_dim(fg)));
            }
            ++row_count;
        }
        return v(std::move(rows)).build();
    }

    // ── web_fetch ───────────────────────────────────────────────
    // Header: "▸ <url> · status · type".
    // Body:   first lines of the body block (after the HTTP status line).
    if (n == "web_fetch" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto url = safe_arg(tc.args, "url");
        // First line is "HTTP <status> (content-type)".
        std::string status_line;
        std::string body;
        if (auto nl = out.find('\n'); nl != std::string::npos) {
            status_line = out.substr(0, nl);
            auto bs = out.find("\n\n");
            body = bs != std::string::npos ? out.substr(bs + 2) : out.substr(nl + 1);
        } else {
            body = out;
        }
        // Trim the URL display (full URL too long for the row).
        std::string short_url = url;
        if (short_url.size() > 60) short_url = short_url.substr(0, 57) + "...";
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, short_url, status_line));
        if (!body.empty()) rows.push_back(preview_block(body, fg_dim(fg)));
        return v(std::move(rows)).build();
    }

    // ── web_search ──────────────────────────────────────────────
    if (n == "web_search" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        auto q = safe_arg(tc.args, "query");
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", info, q, ""));
        rows.push_back(preview_block(out, fg_dim(fg)));
        return v(std::move(rows)).build();
    }

    // ── git_status / git_log / git_commit ───────────────────────
    if ((n == "git_status" || n == "git_log" || n == "git_commit")
        && tc.is_done())
    {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        std::string title;
        if (n == "git_status")  title = "git status";
        else if (n == "git_log") {
            title = "git log";
            if (auto r = safe_arg(tc.args, "ref"); !r.empty())
                title += "  \xc2\xb7  " + r;
        } else {
            title = "git commit";
            if (auto m = safe_arg(tc.args, "message"); !m.empty()) {
                if (m.size() > 60) m = m.substr(0, 57) + "...";
                title += "  \xc2\xb7  " + m;
            }
        }
        std::vector<Element> rows;
        rows.push_back(meta_row("\xe2\x96\xb8", highlight, title, ""));
        rows.push_back(preview_block(out, fg_dim(fg)));
        return v(std::move(rows)).build();
    }

    // ── investigate: rich per-turn timeline + synthesis ──────────────
    // Delegates to the same renderer the standalone tool card uses, so
    // the in-Actions-panel body and the dedicated card show identical
    // structure — pull-quote question, animated stats row, vertical
    // rail with per-turn cells, and the synthesis banner. Without this
    // delegation the panel body fell back to a head/tail preview of
    // the raw transcript text and looked like a debug log dump.
    if (n == "investigate")
        return investigate_body(tc, tool_elapsed(tc));

    // ── git_diff: per-line diff coloring (+ / - / @@) ──────────────
    // preview_block uses a single style; a real diff wants green
    // additions, red removals, dim hunk headers. Same head+tail
    // elision shape so a 500-line diff doesn't take over.
    if (n == "git_diff" && tc.is_done()) {
        const auto& out = tc.output();
        if (out.empty() || out == "no changes") return text("");
        constexpr int kHead = 4;
        constexpr int kTail = 3;
        auto p = head_tail_lines(out, kHead, kTail);
        std::vector<Element> rows;
        auto add_st = Style{}.with_fg(success);
        auto rem_st = Style{}.with_fg(danger);
        auto hdr_st = fg_dim(muted);
        auto ctx_st = fg_dim(fg);
        for (int i = 0; i < static_cast<int>(p.lines.size()); ++i) {
            if (p.elided > 0 && i == kHead) {
                rows.push_back(text("\xc2\xb7 \xc2\xb7 \xc2\xb7  "
                                    + std::to_string(p.elided)
                                    + " hidden  \xc2\xb7 \xc2\xb7 \xc2\xb7",
                                    fg_dim(muted)));
            }
            std::string_view ln = p.lines[static_cast<std::size_t>(i)];
            // Pick per-line style by the diff line marker. Skip the
            // hunk index headers (`@@ -X,Y +A,B @@`) into a dim color
            // so they read as structural metadata.
            Style st = ctx_st;
            if      (ln.starts_with("+++") || ln.starts_with("---")
                  || ln.starts_with("diff "))                 st = hdr_st;
            else if (ln.starts_with("@@"))                    st = hdr_st;
            else if (!ln.empty() && ln[0] == '+')             st = add_st;
            else if (!ln.empty() && ln[0] == '-')             st = rem_st;
            rows.push_back(text(std::string{ln}, st));
        }
        return v(std::move(rows)).build();
    }

    // ── Todo: list each item with its status icon ─────────────────
    // The Timeline header already shows "N items"; this body lists
    // them so the user can read the actual plan inline without
    // popping the dedicated todo modal. Cap the visible list so a
    // 30-item plan doesn't blow out the panel; surplus collapses to
    // a "… N more" footer.
    if (n == "todo" && tc.args.is_object()) {
        auto it = tc.args.find("todos");
        if (it == tc.args.end() || !it->is_array() || it->empty())
            return text("");
        constexpr int kMaxRows = 8;
        std::vector<Element> rows;
        int total = static_cast<int>(it->size());
        int shown = 0;
        for (const auto& td : *it) {
            if (shown >= kMaxRows) break;
            if (!td.is_object()) continue;
            std::string body = td.value("content", "");
            std::string st   = td.value("status", std::string{"pending"});
            const char* glyph;
            Style icon_st, body_st;
            if (st == "completed") {
                glyph   = "\xe2\x9c\x93";   // ✓
                icon_st = Style{}.with_fg(success).with_bold();
                body_st = fg_dim(muted);    // crossed-out feel via dim
            } else if (st == "in_progress") {
                glyph   = "\xe2\x97\x8d";   // ◍
                icon_st = Style{}.with_fg(info).with_bold();
                body_st = Style{}.with_fg(fg);
            } else {
                glyph   = "\xe2\x97\x8b";   // ○
                icon_st = fg_dim(muted);
                body_st = fg_dim(fg);
            }
            rows.push_back(h(
                text(std::string{glyph} + " ", icon_st),
                text(std::move(body), body_st)
            ).build());
            ++shown;
        }
        if (total > shown) {
            rows.push_back(text("\xe2\x80\xa6 " + std::to_string(total - shown)
                                + " more", fg_dim(muted)));
        }
        return v(std::move(rows)).build();
    }

    // ── Failure: surface the error message inline so it isn't hidden
    if (tc.is_failed() && !tc.output().empty()) {
        // Failures use the danger color so the error stands out, but
        // still through the elided preview path so a 200-line stderr
        // dump doesn't take over the panel.
        return preview_block(tc.output(),
                             Style{}.with_fg(danger));
    }

    (void)kMaxLines;
    return text("");
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
