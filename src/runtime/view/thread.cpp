#include "moha/runtime/view/thread.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <maya/widget/markdown.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/timeline.hpp>

#include "moha/runtime/view/cache.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/permission.hpp"
#include "moha/runtime/view/tool_args.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// Cached markdown render. Finalized assistant messages have an immutable
// `text` — we build once and reuse forever. Streaming messages get a
// StreamingMarkdown with block-boundary caching (O(new_chars) per delta).
//
// Swapping from streaming to finalized happens automatically: when
// finalize_turn promotes streaming_text into text, the next render sees
// `text` non-empty and drops the StreamingMarkdown in favour of the
// finalized Element.
Element cached_markdown_for(const Message& msg, const ThreadId& tid,
                            std::size_t msg_idx) {
    auto& cache = message_md_cache(tid, msg_idx);
    if (msg.text.empty()) {
        if (!cache.streaming)
            cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_content(msg.streaming_text);
        return cache.streaming->build();
    }
    if (!cache.finalized) {
        cache.finalized = std::make_shared<Element>(maya::markdown(msg.text));
        cache.streaming.reset();
    }
    return *cache.finalized;
}

} // namespace

// render_tool_call — defined in tool_card.cpp.


// ════════════════════════════════════════════════════════════════════════

// Per-speaker visual identity: brand color + glyph + display name.
// Centralized so the rail color, the header glyph, and the bottom
// streaming indicator stay in lockstep.
struct SpeakerStyle {
    Color       color;
    std::string glyph;
    std::string label;
};

SpeakerStyle speaker_style_for(Role role, const Model& m) {
    if (role == Role::User) {
        // Cyan — distinct from every model brand color so user vs
        // assistant turns always read as different voices.
        return {highlight, "\xe2\x9d\xaf", "You"};                   // ❯
    }
    const auto& id = m.d.model_id.value;
    Color c;
    std::string label;
    if      (id.find("opus")   != std::string::npos) { c = accent;    label = "Opus";   }
    else if (id.find("sonnet") != std::string::npos) { c = info;      label = "Sonnet"; }
    else if (id.find("haiku")  != std::string::npos) { c = success;   label = "Haiku";  }
    else                                              { c = highlight; label = id;       }
    // Append a version like "4.7" if present in the id.
    for (std::size_t i = 0; i + 2 < id.size(); ++i) {
        char ch = id[i];
        if (ch >= '0' && ch <= '9') {
            char delim = id[i + 1];
            if ((delim == '-' || delim == '.') && id[i + 2] >= '0' && id[i + 2] <= '9') {
                std::size_t end = i + 3;
                while (end < id.size() && id[end] >= '0' && id[end] <= '9') ++end;
                auto ver = id.substr(i, end - i);
                for (auto& v : ver) if (v == '-') v = '.';
                label += " " + ver;
                break;
            }
        }
    }
    return {c, "\xe2\x9c\xa6", std::move(label)};                    // ✦
}

// Turn header: speaker glyph + name on the left (in the speaker color),
// dim metadata trailing right (timestamp · elapsed · turn N). The
// leading ▎ edge mark is gone — the continuous left rail (added at the
// turn-block level in render_message) already does that work and a
// double-bar would feel cluttered.
Element turn_header(Role role, int turn_num, const Message& msg,
                    const Model& m, std::optional<float> elapsed_secs) {
    auto style = speaker_style_for(role, m);

    // Trailing metadata: timestamp · elapsed · turn N — rendered with
    // generous spacing so the eye reads it as a quiet trailing strip,
    // not a comma-list.
    std::string meta = timestamp_hh_mm(msg.timestamp);
    if (elapsed_secs && *elapsed_secs > 0.0f) {
        char buf[24];
        if      (*elapsed_secs < 1.0f)  std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %.0fms", *elapsed_secs * 1000.0);
        else if (*elapsed_secs < 60.0f) std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %.1fs", static_cast<double>(*elapsed_secs));
        else {
            int mins = static_cast<int>(*elapsed_secs) / 60;
            float secs = *elapsed_secs - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %dm%.0fs", mins, static_cast<double>(secs));
        }
        meta += buf;
    }
    if (turn_num > 0) {
        meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    }

    // `grow(1.0f)` on the header row is load-bearing: without it the row
    // shrinks to content width when a sibling element (like the active
    // turn's Timeline card) has its own intrinsic width, and the
    // `spacer()` inside collapses to 0 so the timestamp snaps left
    // against the speaker label instead of pinning the right edge.
    return (h(
        text(style.glyph, fg_of(style.color)),
        text(" ", {}),
        text(std::move(style.label), Style{}.with_fg(style.color).with_bold()),
        spacer(),
        text(std::move(meta), fg_dim(muted)),
        text(" ", {})
    ) | grow(1.0f)).build();
}

// Wrap a turn's full content (header + body + tools) in a left-only
// border colored by the speaker. The border becomes a continuous
// vertical rail running the entire height of the turn — the visual
// signature of polished chat UIs (Claude Code, Zed, Cursor, Linear).
// Color groups everything under one speaker; padding pushes content
// off the rail with breathing room. Bold border style → ┃ heavier
// vertical so the rail reads as a real divider, not a thin line.
Element with_turn_rail(Element content, Color rail_color) {
    return maya::detail::box()
        .direction(FlexDirection::Row)
        .border(BorderStyle::Bold, rail_color)
        .border_sides({.top = false, .right = false,
                       .bottom = false, .left = true})
        .padding(0, 0, 0, 2)
        .grow(1.0f)
      (std::move(content));
}

// Inter-turn divider: a thin dim horizontal rule across the gap.
// Replaces a bare blank line — the rule gives the eye a real handhold
// for "new turn starts here" without being heavy. Skipped for the very
// first turn since there's nothing above to divide from.
Element inter_turn_divider() {
    return Element{ComponentElement{
        .render = [](int w, int /*h*/) -> Element {
            std::string line;
            // Faded thin rule: dim · runs across with some breathing
            // space at the indent column so it doesn't crash into the
            // turn rail above. Reads as a quiet timeline tick.
            int indent = 3;
            for (int i = 0; i < indent; ++i) line += ' ';
            for (int i = indent; i < w; ++i) line += "\xe2\x94\x80";  // ─
            return Element{TextElement{
                .content = std::move(line),
                .style = Style{}.with_fg(Color::bright_black()).with_dim(),
            }};
        },
        .layout = {},
    }};
}

// User message body: plain text. Indent removed since the turn rail's
// padding already handles offset; this keeps the single-source-of-truth
// for "how far in" content sits.
Element user_message_body(const std::string& body) {
    return text(body, fg_of(fg));
}

// Brief "what this tool is doing" line for the Timeline view. Tool-
// specific so the user can read the sequence at a glance: paths for fs
// ops, the actual command for bash, the pattern for grep, etc. When
// the tool has settled (terminal status), the detail also folds in
// post-completion stats — line count for read/write, hunk + Δ for
// edit, match count for grep, exit code for bash, etc. — so the
// Timeline doubles as a compact result log without the user needing
// to expand individual cards.
std::string tool_timeline_detail(const ToolUse& tc) {
    auto safe = [&](const char* k) -> std::string { return safe_arg(tc.args, k); };
    auto path = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    const auto& n = tc.name.value;

    // Pretty-print "src/foo/bar.cpp" rather than the absolute path when
    // the path lives under cwd. Uses the same trick the existing tool
    // cards do — strip a known-prefix.
    auto pretty_path = [&](std::string p) -> std::string {
        if (p.empty()) return p;
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec).string();
        if (!ec && !cwd.empty()
            && p.size() > cwd.size()
            && p.compare(0, cwd.size(), cwd) == 0
            && p[cwd.size()] == '/') {
            return p.substr(cwd.size() + 1);
        }
        // Drop the user's home prefix as `~/…`.
        if (const char* home = std::getenv("HOME"); home && *home) {
            std::string h{home};
            if (p.size() > h.size() && p.compare(0, h.size(), h) == 0
                && p[h.size()] == '/')
                return std::string{"~/"} + p.substr(h.size() + 1);
        }
        return p;
    };

    auto path_pp = pretty_path(path);

    if (n == "read") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (auto off = safe_int_arg(tc.args, "offset", 0); off > 0)
            detail += " @" + std::to_string(off);
        if (tc.is_done()) {
            // Output starts with the directory listing or "Read N lines..."
            // header. Just count newlines for a rough size hint.
            int lines = count_lines(tc.output());
            if (lines > 1) detail += "  \xc2\xb7  " + std::to_string(lines) + " lines";
        }
        return detail;
    }
    if (n == "write") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (tc.is_done()) {
            // Output line "Wrote/Overwrote N (+X -Y)" — just include +/− if
            // we can find them; otherwise fall back to char count of args.
            const auto& out = tc.output();
            if (auto plus = out.find('+'); plus != std::string::npos) {
                if (auto end = out.find(')', plus); end != std::string::npos) {
                    auto from = out.rfind('(', plus);
                    if (from != std::string::npos)
                        detail += "  " + out.substr(from, end - from + 1);
                }
            }
        }
        return detail;
    }
    if (n == "edit") {
        if (path_pp.empty()) return "\xe2\x80\xa6";
        std::string detail = path_pp;
        // Surface hunk count if the args carry an edits[] array.
        if (tc.args.is_object()) {
            auto it = tc.args.find("edits");
            if (it != tc.args.end() && it->is_array() && !it->empty())
                detail += "  \xc2\xb7  " + std::to_string(it->size()) + " edits";
        }
        if (tc.is_done()) {
            // Pull "(+X -Y)" out of the tool output if present.
            const auto& out = tc.output();
            if (auto from = out.find('('); from != std::string::npos) {
                if (auto end = out.find(')', from); end != std::string::npos
                    && (out.find('+', from) < end || out.find('-', from) < end))
                    detail += "  " + out.substr(from, end - from + 1);
            }
        }
        return detail;
    }
    if (n == "bash" || n == "diagnostics") {
        auto cmd = safe("command");
        if (cmd.empty()) return "\xe2\x80\xa6";
        if (auto nl = cmd.find('\n'); nl != std::string::npos)
            cmd = cmd.substr(0, nl) + " \xe2\x80\xa6";
        if (tc.is_done()) {
            int rc = parse_exit_code(tc.output());
            if (rc != 0) cmd += "  \xc2\xb7  exit " + std::to_string(rc);
        }
        return cmd;
    }
    if (n == "grep") {
        auto pat = safe("pattern");
        if (pat.empty()) return "\xe2\x80\xa6";
        std::string detail = path_pp.empty() ? pat : pat + "  in  " + path_pp;
        if (tc.is_done()) {
            int matches = count_lines(tc.output());
            if (matches > 0) detail += "  \xc2\xb7  " + std::to_string(matches) + " matches";
        }
        return detail;
    }
    if (n == "glob") {
        auto pat = safe("pattern");
        if (pat.empty()) return "\xe2\x80\xa6";
        std::string detail = pat;
        if (tc.is_done()) {
            int hits = count_lines(tc.output());
            if (hits > 0) detail += "  \xc2\xb7  " + std::to_string(hits) + " hits";
        }
        return detail;
    }
    if (n == "list_dir") {
        std::string detail = path_pp.empty() ? std::string{"."} : path_pp;
        if (tc.is_done()) {
            int entries = count_lines(tc.output());
            if (entries > 0) detail += "  \xc2\xb7  " + std::to_string(entries) + " entries";
        }
        return detail;
    }
    if (n == "find_definition") {
        std::string detail = safe("symbol");
        if (tc.is_done()) {
            // Output uses the same `path:line:content` markdown grep
            // format, so "## Matches in" lines correlate to file hits
            // and the rest are individual matches. Cheap proxy.
            int hits = 0;
            const auto& out = tc.output();
            for (std::size_t p = 0; (p = out.find("## Matches in ", p)) != std::string::npos; p += 14)
                ++hits;
            if (hits > 0) detail += "  \xc2\xb7  " + std::to_string(hits)
                                 + (hits == 1 ? " file" : " files");
        }
        return detail;
    }
    if (n == "web_fetch") {
        std::string detail = safe("url");
        if (tc.is_done()) {
            // Output's first line is "HTTP <code> (<content-type>)"
            // — surface it inline so the user sees status without
            // expanding. Truncate URL middle if it crowds out the chip.
            const auto& out = tc.output();
            auto nl = out.find('\n');
            if (nl != std::string::npos && out.starts_with("HTTP ")) {
                auto sp = out.find(' ', 5);
                if (sp != std::string::npos)
                    detail += "  \xc2\xb7  " + out.substr(5, sp - 5);
            }
        }
        return detail;
    }
    if (n == "web_search") {
        std::string detail = safe("query");
        if (tc.is_done()) {
            // Each result starts with a numbered bullet ("1. " etc.).
            // Counting them is more reliable than parsing the body.
            int hits = 0;
            const auto& out = tc.output();
            for (std::size_t p = 0; p + 1 < out.size(); ++p) {
                if ((p == 0 || out[p - 1] == '\n')
                    && std::isdigit(static_cast<unsigned char>(out[p]))
                    && (out[p+1] == '.' || (p + 2 < out.size() && out[p+2] == '.')))
                    ++hits;
            }
            if (hits > 0) detail += "  \xc2\xb7  " + std::to_string(hits)
                                 + (hits == 1 ? " result" : " results");
        }
        return detail;
    }
    if (n == "git_commit") {
        auto msg = safe("message");
        if (auto nl = msg.find('\n'); nl != std::string::npos)
            msg = msg.substr(0, nl);
        if (tc.is_done()) {
            // Output line shape: "[main abc1234] commit subject" — pull
            // the short hash so the user can see *which* commit landed
            // without expanding.
            const auto& out = tc.output();
            auto open = out.find('[');
            if (open != std::string::npos) {
                auto close = out.find(']', open);
                auto sp = out.find(' ', open + 1);
                if (sp != std::string::npos && sp < close) {
                    auto hash = out.substr(sp + 1, close - sp - 1);
                    if (!hash.empty() && hash.size() <= 12)
                        msg += "  \xc2\xb7  " + hash;
                }
            }
        }
        return msg;
    }
    if (n == "git_status" && tc.is_done()) {
        // Parse the porcelain v2 output into a compact "branch · M+ S± U?".
        // Walks the output once; cheap.
        const auto& out = tc.output();
        std::string branch;
        int modified = 0, staged = 0, untracked = 0;
        std::size_t lo = 0;
        while (lo < out.size()) {
            auto eol = out.find('\n', lo);
            std::string_view line{out.data() + lo,
                (eol == std::string::npos ? out.size() : eol) - lo};
            if (line.starts_with("# branch.head ")) branch = std::string{line.substr(14)};
            else if (line.size() >= 4 && line[0] == '?')             ++untracked;
            else if (line.size() >= 4 && (line[0] == '1' || line[0] == '2')) {
                if (line[2] != '.') ++staged;
                if (line[3] == 'M' || line[3] == 'D') ++modified;
            }
            if (eol == std::string::npos) break;
            lo = eol + 1;
        }
        std::string detail = branch.empty() ? std::string{"(detached)"} : branch;
        if (modified || staged || untracked) {
            detail += "  \xc2\xb7  ";
            bool first = true;
            auto add = [&](int n_, const char* suffix) {
                if (n_ <= 0) return;
                if (!first) detail += " ";
                detail += std::to_string(n_) + suffix;
                first = false;
            };
            add(modified,  "M");
            add(staged,    "S");
            add(untracked, "?");
        } else {
            detail += "  \xc2\xb7  clean";
        }
        return detail;
    }
    if (n == "git_diff" || n == "git_log" || n == "git_status")
        return path_pp.empty() ? std::string{"."} : path_pp;
    if (n == "todo") {
        // "N items" loses the most useful signal (how much of the plan
        // is done). Show "done/total" so a glance tells the user the
        // model is making progress, not just bookkeeping.
        if (tc.args.is_object()) {
            auto it = tc.args.find("todos");
            if (it != tc.args.end() && it->is_array() && !it->empty()) {
                int total = 0, done = 0, in_progress = 0;
                for (const auto& td : *it) {
                    if (!td.is_object()) continue;
                    ++total;
                    auto st = td.value("status", std::string{"pending"});
                    if (st == "completed")        ++done;
                    else if (st == "in_progress") ++in_progress;
                }
                std::string detail = std::to_string(done) + "/" + std::to_string(total);
                if (in_progress > 0)
                    detail += "  \xc2\xb7  " + std::to_string(in_progress) + " in progress";
                return detail;
            }
        }
        return "\xe2\x80\xa6";
    }
    return safe_arg(tc.args, "display_description");
}

// Map a ToolUse status to maya's TaskStatus. Failed/Rejected fold into
// Completed (it IS terminal); the failure is surfaced via the detail
// line so the timeline still reads as a clean sequence.
TaskStatus tool_task_status(const ToolUse& tc) {
    if (tc.is_pending() || tc.is_approved()) return TaskStatus::Pending;
    if (tc.is_running())                     return TaskStatus::InProgress;
    return TaskStatus::Completed; // Done / Failed / Rejected
}

// Pretty title-case for the tool name shown as the timeline event label.
// Maps moha's lowercase canonical names to the brand TitleCase forms
// (matches the names users see in CC / Zed agent panel).
std::string tool_display_name(const std::string& n) {
    if (n == "read")            return "Read";
    if (n == "write")           return "Write";
    if (n == "edit")            return "Edit";
    if (n == "bash")            return "Bash";
    if (n == "grep")            return "Grep";
    if (n == "glob")            return "Glob";
    if (n == "list_dir")        return "List";
    if (n == "todo")            return "Todo";
    if (n == "web_fetch")       return "Fetch";
    if (n == "web_search")      return "Search";
    if (n == "find_definition") return "Definition";
    if (n == "diagnostics")     return "Diag";
    if (n == "git_status")      return "Git Status";
    if (n == "git_diff")        return "Git Diff";
    if (n == "git_log")         return "Git Log";
    if (n == "git_commit")      return "Git Commit";
    if (n == "outline")         return "Outline";
    if (n == "repo_map")        return "Repo Map";
    if (n == "signatures")      return "Signatures";
    if (n == "investigate")     return "Investigate";
    return n;
}

// Format a duration as ms/s/m+s — short, glanceable, no surprising
// precision changes across magnitudes.
std::string format_duration(float secs) {
    char buf[24];
    if      (secs < 1.0f)  std::snprintf(buf, sizeof(buf), "%.0fms", secs * 1000.0);
    else if (secs < 60.0f) std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
    else {
        int mins = static_cast<int>(secs) / 60;
        float s = secs - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(s));
    }
    return buf;
}

// Status icon for a tool event in the rich timeline. Spinner advances
// in sync with the activity-bar spinner via the shared frame index.
//
// Pending (= model is still streaming this tool's args — "thinking")
// also gets a spinner, otherwise a long write/edit args stream looked
// frozen next to an identical static ○ for every not-yet-approved
// call. Pending uses the muted color so it still reads as a quieter
// stage than the bright-info running spinner.
Element rich_status_icon(const ToolUse& tc, int frame) {
    // Same braille spinner pattern as maya::Timeline / Spinner<Dots>.
    static constexpr const char* frames[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f",
    };
    if (tc.is_running() || tc.is_approved()) {
        // Bright cyan + bold. `info` alone reads as faint grey on a lot
        // of low-contrast palettes (Solarized light, anything where the
        // ANSI 4 maps to a desaturated blue) — bright_cyan is the most
        // universally vivid "work in progress" color across themes.
        return text(frames[frame % 10],
                    Style{}.with_fg(Color::bright_cyan()).with_bold());
    }
    if (tc.is_pending()) {
        // Pending = model is still streaming the tool's args. Used to
        // be muted+dim, which on most palettes rendered as a barely-
        // visible grey — a long edit args stream looked frozen. Now
        // bright_yellow + bold so it's unmistakably alive while still
        // being a different hue from the running cyan, so the user can
        // tell "args streaming" from "executing" at a glance.
        return text(frames[frame % 10],
                    Style{}.with_fg(Color::bright_yellow()).with_bold());
    }
    if (tc.is_done())     return text("\xe2\x9c\x93", Style{}.with_fg(Color::bright_green()).with_bold());   // ✓
    if (tc.is_failed())   return text("\xe2\x9c\x97", Style{}.with_fg(Color::bright_red()).with_bold());     // ✗
    if (tc.is_rejected()) return text("\xe2\x8a\x98", Style{}.with_fg(Color::bright_yellow()).with_bold());  // ⊘
    return text("\xe2\x97\x8b", fg_dim(muted));                            // ○
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

// First N lines of `s` joined back, with a `… N more lines` footer when
// there were more. Used for tool body previews so a 1000-line bash output
// doesn't blow up the timeline card.
std::pair<std::string,int> head_lines(const std::string& s, int max_lines) {
    int kept = 0;
    int total = 0;
    std::size_t cut = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            ++total;
            if (kept < max_lines) { ++kept; cut = i + 1; }
        }
    }
    if (!s.empty() && s.back() != '\n') {
        ++total;
        if (kept < max_lines) { ++kept; cut = s.size(); }
    }
    return {s.substr(0, cut), std::max(0, total - kept)};
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

// Render compact body content for a single tool event — placed under the
// timeline event's `│` connector. Tool-specific so each row carries
// real, glanceable information: a few lines of read content, the diff
// hunks for an edit, the head of bash output, etc. Empty Element when
// nothing useful exists yet (still streaming) — caller handles spacing.
Element compact_tool_body(const ToolUse& tc) {
    const auto& n = tc.name.value;
    constexpr int kMaxLines = 6;

    auto code_line = [](std::string_view ln, Style st) {
        return text(std::string{ln}, st);
    };

    // ── Edit: unified-diff-style preview, per-hunk head+tail ───
    // Replaces the previous "first line per side" rendering, which lost
    // every multi-line edit beyond row 1 — the user saw `- foo {` /
    // `+ bar {` without any clue what the actual change was. Now each
    // hunk gets up to ~4 lines per side with head+tail elision, plus a
    // −N/+M line-count tag so the user can see scope at a glance.
    if (n == "edit" && tc.args.is_object()) {
        std::vector<Element> rows;
        auto rem      = Style{}.with_fg(danger);
        auto add      = Style{}.with_fg(success);
        auto rem_pre  = Style{}.with_fg(danger).with_dim();
        auto add_pre  = Style{}.with_fg(success).with_dim();

        // Per-side cap. 3 head + 1 tail is enough to recognize "this is
        // the function I'm editing" without letting a 200-line replacement
        // dominate the timeline; head+tail keeps the closing brace / last
        // logical line visible so the diff frames the change.
        // Generous-but-bounded preview. Most real edits the model
        // makes are 5-15 lines; the previous 3+1 was so tight that
        // anything over 4 rows immediately collapsed into "… N hidden"
        // and the user couldn't see the actual change. 6+2 keeps short
        // edits fully visible (≤ 8 rows shown raw) while still
        // capping the worst case (200-line replacement) at ~10 rows.
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

    // ── Read / list_dir / grep / glob / find_definition / web /
    //    git_log / git_status / git_commit: head+tail preview of
    //    the textual output. Each tool produces structured-but-
    //    line-oriented text that the head+tail helper handles
    //    well; specialising each one to a custom widget here
    //    would be over-design for what the timeline is — a
    //    glanceable summary, not a viewer.
    if ((n == "read" || n == "list_dir" || n == "grep" || n == "glob"
         || n == "find_definition"
         || n == "web_fetch" || n == "web_search"
         || n == "git_status" || n == "git_log" || n == "git_commit"
         || n == "outline" || n == "repo_map" || n == "signatures")
        && tc.is_done())
    {
        const auto& out = tc.output();
        if (out.empty()) return text("");
        return preview_block(out, fg_dim(fg));
    }

    // ── investigate: live progress while running, synthesis when done ─
    // Sub-agent runs can take 10-30 s; without a body the timeline
    // event is just a header that says "investigating…" with nothing
    // visible. Surface the streamed progress (turn boundaries, fan-
    // out, synthesis text) so the user can follow along.
    if (n == "investigate") {
        if (tc.is_running() && !tc.progress_text().empty())
            return preview_block(tc.progress_text(), fg_dim(fg));
        if (tc.is_done() && !tc.output().empty()) {
            // Strip the framing line if present so the timeline body
            // shows the actual synthesis, not the metadata.
            std::string body = tc.output();
            if (body.starts_with("[investigate")) {
                if (auto end = body.find("]\n\n"); end != std::string::npos)
                    body = body.substr(end + 3);
            }
            return preview_block(body, fg_dim(fg));
        }
        return text("");
    }

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

// Color-code a tool's wall-clock duration so the eye finds slow steps
// without parsing numbers. Green = snappy (<250ms), dim = normal, warn
// = slow (>2s), danger = stalling (>15s).
Color duration_color(float secs) {
    if (secs < 0.25f) return success;
    if (secs < 2.0f)  return Color::bright_black();
    if (secs < 15.0f) return warn;
    return danger;
}

// Pick the body-connector color for an event based on its status. The
// connector is the visual "thread" running down each event's body —
// coloring it by status reinforces the icon at the head of the event
// without adding more chrome.
Color event_connector_color(const ToolUse& tc) {
    if (tc.is_failed())                                return danger;
    if (tc.is_rejected())                              return warn;
    if (tc.is_running() || tc.is_approved())           return info;
    if (tc.is_done())                                  return Color::bright_black();
    return Color::bright_black();   // pending
}

// Tool category color — semantic grouping so the eye can scan a
// timeline and instantly see "this turn was mostly inspect + one
// modify". Five buckets, each with a distinct hue:
//
//   inspect (read, grep, glob, list, find, diag, web)  → info (blue)
//   mutate  (edit, write)                              → accent (magenta)
//   execute (bash)                                     → success (green)
//   plan    (todo)                                     → warn (yellow)
//   vcs     (git_*)                                    → highlight (cyan)
//
// Used for the gutter number and the tool name so a glance at the
// timeline shows the *kind* of work happening, not just the order.
Color tool_category_color(const std::string& n) {
    if (n == "edit" || n == "write")        return accent;
    if (n == "bash")                        return success;
    if (n == "todo")                        return warn;
    if (n.rfind("git_", 0) == 0)            return highlight;
    return info;  // read, grep, glob, list_dir, find_definition,
                  // diagnostics, web_fetch, web_search
}

// Short category label for the stats header.
std::string_view tool_category_label(const std::string& n) {
    if (n == "edit" || n == "write")        return "mutate";
    if (n == "bash")                        return "execute";
    if (n == "todo")                        return "plan";
    if (n.rfind("git_", 0) == 0)            return "vcs";
    return "inspect";
}

// Build the assistant turn's "Actions" panel: one bordered card whose
// body is a continuous timeline of tool events. Each event has a
// gutter-numbered header (like code line numbers), a status icon, the
// tool name + brief detail + duration, and an indented body where the
// rich tool-specific content sits under a status-colored connector.
// Overview + detail in one cohesive, scannable view.
Element assistant_timeline(const Message& msg, int spinner_frame,
                           Color rail_color) {
    std::vector<Element> rows;
    int total = static_cast<int>(msg.tool_calls.size());
    int done  = 0;
    float total_elapsed = 0.0f;
    int running_idx = -1;
    // Per-category counts for the stats header. Order kept stable so
    // the badges always appear in the same relative position.
    std::vector<std::pair<std::string, int>> cat_counts;
    auto bump_cat = [&](const std::string& cat) {
        for (auto& [k, n] : cat_counts) if (k == cat) { ++n; return; }
        cat_counts.emplace_back(cat, 1);
    };
    for (std::size_t i = 0; i < msg.tool_calls.size(); ++i) {
        const auto& tc = msg.tool_calls[i];
        if (tc.is_terminal()) {
            ++done;
            total_elapsed += tool_elapsed(tc);
        }
        if (running_idx < 0 && (tc.is_running() || tc.is_approved()))
            running_idx = static_cast<int>(i);
        bump_cat(std::string{tool_category_label(tc.name.value)});
    }

    // ── Stats header ───────────────────────────────────────────────
    // Quick TL;DR of the turn: small-caps category badges showing the
    // mix (e.g. "I N S P E C T 3 · M U T A T E 2 · E X E C U T E 1").
    // Small-caps treatment marks these as section labels — typography
    // does the work that a chip background would, without the toy feel.
    if (total > 1) {
        std::vector<Element> stats;
        bool first = true;
        for (const auto& [cat, n] : cat_counts) {
            if (!first) stats.push_back(text("  \xc2\xb7  ", fg_dim(muted)));
            first = false;
            // Pick a color from a representative tool name in this
            // category — same map as tool_category_color so the badge
            // and the per-event gutter agree.
            Color cc = (cat == "mutate")  ? accent
                     : (cat == "execute") ? success
                     : (cat == "plan")    ? warn
                     : (cat == "vcs")     ? highlight
                                          : info;
            stats.push_back(text(small_caps(cat),
                                 Style{}.with_fg(cc).with_bold()));
            stats.push_back(text(" " + std::to_string(n), fg_dim(muted)));
        }
        rows.push_back((h(std::move(stats)) | grow(1.0f)).build());
        rows.push_back(text(""));
    }

    // Sequence-position glyph: rounded + light box-drawing characters,
    // softer than the previous bold ┏━┣━┗━ which read as too-loud
    // chrome. Modern agent UIs (Cursor, Cline, Linear's pipeline view)
    // use light strokes for structure and reserve weight for the
    // *signal* (icon, name, status) — that's the discipline here.
    //   ╭─ first event
    //   ├─ middle events
    //   ╰─ last event
    //   ── singleton
    // Drawn in the per-event category color so the leading edge of
    // each row still reads as a colored timeline at a glance.
    auto tree_glyph = [&](std::size_t idx) -> std::string {
        if (total == 1)              return "\xe2\x94\x80\xe2\x94\x80";  // ──
        if (idx == 0)                return "\xe2\x95\xad\xe2\x94\x80";  // ╭─
        if (idx + 1 == static_cast<std::size_t>(total))
                                     return "\xe2\x95\xb0\xe2\x94\x80";  // ╰─
        return                              "\xe2\x94\x9c\xe2\x94\x80";  // ├─
    };

    for (std::size_t i = 0; i < msg.tool_calls.size(); ++i) {
        const auto& tc = msg.tool_calls[i];
        bool is_last   = (i + 1 == msg.tool_calls.size());
        bool is_active = tc.is_running() || tc.is_approved();

        // ── Header row ──────────────────────────────────────────────
        // Layout: `╭─ ⠋ Bash    npm test                       1.2s`
        //          ^^ ^ ^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        //          tree icon name              detail   spacer dur
        //
        // The redundant active-marker (▸) is gone — the spinner icon
        // already signals "this is the running one." Consistent
        // 4-cell wide name column gives the detail a stable column
        // boundary for scanning.
        Element icon = rich_status_icon(tc, spinner_frame);
        std::string name = tool_display_name(tc.name.value);
        std::string detail = tool_timeline_detail(tc);
        if (detail.empty())
            detail = tc.is_running()  ? std::string{"running\xe2\x80\xa6"}
                   : tc.is_pending()  ? std::string{"queued\xe2\x80\xa6"}
                   : tc.is_approved() ? std::string{"approved\xe2\x80\xa6"}
                                      : std::string{"\xe2\x80\xa6"};

        Color cat = tool_category_color(tc.name.value);
        // Name styling: failed/rejected stay in their status colors so
        // the eye catches them. Otherwise color by tool *category* so
        // a glance at the timeline reads "inspect inspect mutate
        // execute" by hue. Active tools get bold + bright; settled
        // tools stay dim so the running step pops.
        Style name_style;
        if      (tc.is_failed())   name_style = Style{}.with_fg(danger).with_bold();
        else if (tc.is_rejected()) name_style = Style{}.with_fg(warn).with_bold();
        else if (is_active)        name_style = Style{}.with_fg(cat).with_bold();
        else                        name_style = Style{}.with_fg(cat).with_dim();

        // Italic for the detail column — visually separates "data the
        // tool is acting on" (paths, commands, patterns) from the
        // surrounding timeline chrome. The eye reads chrome → name →
        // italic-data without the columns blurring together.
        Style detail_style = is_active
            ? Style{}.with_fg(muted).with_italic()
            : Style{}.with_fg(muted).with_dim().with_italic();

        // Tree-glyph in category color: light strokes for the pipeline
        // structure, brighter on the active event so the eye lands
        // there. Settled events stay dim so the running step pops.
        Style tree_style = is_active
            ? Style{}.with_fg(cat)
            : Style{}.with_fg(cat).with_dim();

        // Build the left-side run as one h(), then compose with
        // spacer + elapsed at the outer level. This mirrors the
        // turn_header pattern — passing a vector of children to
        // h() in one shot doesn't propagate spacer-grow the same
        // way an explicit variadic does, so the elapsed used to
        // snap left against the detail instead of pinning the
        // right edge.
        std::vector<Element> left_parts;
        left_parts.push_back(text(tree_glyph(i), tree_style));
        left_parts.push_back(text(" ", {}));
        left_parts.push_back(icon);
        left_parts.push_back(text("  ", {}));
        left_parts.push_back(text(std::move(name), name_style));
        left_parts.push_back(text("  ", {}));
        left_parts.push_back(text(std::move(detail), detail_style));
        Element left = h(std::move(left_parts)).build();

        if (tc.is_terminal()) {
            float secs = tool_elapsed(tc);
            Element elapsed = text(format_duration(secs),
                                   Style{}.with_fg(duration_color(secs)));
            rows.push_back((h(left, spacer(), elapsed) | grow(1.0f)).build());
        } else {
            rows.push_back((h(left, spacer()) | grow(1.0f)).build());
        }

        // ── Body content under a light │ stripe ─────────────────────
        // Body content (file preview, command output, diff hunks) is
        // supplementary information — chrome should be quiet. Use a
        // light │ in dim status color so the body reads as "indented
        // detail under this event" without competing with the header
        // for visual weight. Active events get the stripe slightly
        // brighter to keep the running event grouped.
        Color cc = event_connector_color(tc);
        bool is_active_body = tc.is_running() || tc.is_approved();
        Style stripe_style = is_active_body
            ? Style{}.with_fg(cc)
            : Style{}.with_fg(cc).with_dim();
        auto body_rule = h(
            text("   ", {}),                                         // tree+space alignment (3 cols)
            text("\xe2\x94\x82  ", stripe_style)                     // │ (light)
        ).build();

        Element body_el = compact_tool_body(tc);
        bool body_has_content = false;
        // `grow(1.0f)` on each body row makes it expand to fill the
        // Actions panel's width. Without it the row was content-width
        // — long edit/diff/grep lines clipped against an invisible
        // boundary that matched the longest header above instead of
        // growing the card to accommodate them. The user reported the
        // edit "toolbox doesn't grow"; this is the cause.
        if (auto* bx = maya::as_box(body_el)) {
            for (const auto& child : bx->children) {
                rows.push_back((h(body_rule, child) | grow(1.0f)).build());
                body_has_content = true;
            }
        } else if (auto* t = maya::as_text(body_el)) {
            if (!t->content.empty()) {
                rows.push_back((h(body_rule, body_el) | grow(1.0f)).build());
                body_has_content = true;
            }
        }

        // ── Continuation between events ────────────────────────────
        // A short colored connector below the body keeps the visual
        // thread running into the next event. Light │ matches the new
        // body stripe — each event's lane reads as a continuous left
        // edge from header through body into the next event's header,
        // without overpowering the icon column.
        if (!is_last) {
            Color next_cc = event_connector_color(msg.tool_calls[i + 1]);
            rows.push_back(h(
                text("   ", {}),
                text("\xe2\x94\x82", Style{}.with_fg(next_cc).with_dim())  // │
            ).build());
        }
        (void)body_has_content;
    }

    // ── Footer summary when settled ────────────────────────────────
    // Once all tools are terminal, append a one-line footer with
    // aggregate stats: "D O N E · 5 actions · 1.8s elapsed". Small-caps
    // verb signals "this is the closing label" — typographic weight
    // pinning the panel from below.
    if (done == total && total > 0) {
        std::string verb_text = "done";
        const char* verb_glyph = "\xe2\x9c\x93";   // ✓
        // If anything failed, lead with that count instead.
        int failed = 0, rejected = 0;
        for (const auto& tc : msg.tool_calls) {
            if (tc.is_failed())   ++failed;
            if (tc.is_rejected()) ++rejected;
        }
        Color verb_color = success;
        if (failed > 0) {
            verb_text = std::to_string(failed) + " failed";
            verb_glyph = "\xe2\x9c\x97";           // ✗
            verb_color = danger;
        } else if (rejected > 0) {
            verb_text = std::to_string(rejected) + " rejected";
            verb_glyph = "\xe2\x8a\x98";           // ⊘
            verb_color = warn;
        }

        rows.push_back(text(""));
        rows.push_back(h(
            text("   ", {}),
            text(std::string{verb_glyph} + " ",
                 Style{}.with_fg(verb_color).with_bold()),
            text(small_caps(verb_text),
                 Style{}.with_fg(verb_color).with_bold()),
            text("   ", {}),
            text(std::to_string(total)
                 + (total == 1 ? " action" : " actions"),
                 fg_dim(muted)),
            text("   ", {}),
            text(format_duration(total_elapsed), fg_dim(muted))
        ).build());
    }

    // ── Card title: small-caps "ACTIONS" + progress + active step ─
    // The title sits inline on the top border. Small-caps treatment
    // signals "section header" — pairs the panel visually with the
    // small-caps category badges in the stats row above.
    std::string title = " " + small_caps("Actions") + "  \xc2\xb7  "
                      + std::to_string(done) + "/"
                      + std::to_string(total);
    if (running_idx >= 0) {
        title += "  \xc2\xb7  " + tool_display_name(
            msg.tool_calls[static_cast<std::size_t>(running_idx)].name.value);
    } else if (done == total && total > 0) {
        title += "  \xc2\xb7  " + format_duration(total_elapsed);
    }
    title += " ";

    // Quieter panel chrome: dashed border in muted color (not the
    // speaker rail color — that's loud and competes with the message
    // rail just to the left). The dashed style reads as "this is a
    // soft container holding related work" rather than "this is a
    // separate document." Settled panels dim further so they recede.
    bool all_done = (done == total && total > 0);
    Color border_c = all_done ? muted : rail_color;
    // `grow(1.0f)` on the whole panel, not just its rows. The per-row
    // grow (added earlier for edit diff/grep line widths) only expands
    // rows *within* the panel — yoga still shrinks the panel itself to
    // content width unless it's explicitly flexible in its parent. That
    // leaves the bordered card narrower than the available message
    // column, clipping long bash/grep/edit lines at an invisible
    // boundary matching the longest header. With the outer grow, the
    // panel stretches to fill the full message column width and the
    // inner rows (already grow=1) fill that in turn.
    return (v(std::move(rows))
            | border(BorderStyle::Round)
            | bcolor(border_c)
            | btext(std::move(title), BorderTextPos::Top, BorderTextAlign::Start)
            | padding(0, 1, 0, 1)
            | grow(1.0f)
           ).build();
}

Element render_message(const Message& msg, std::size_t msg_idx,
                       int turn_num, const Model& m) {
    std::vector<Element> rows;

    // Compute elapsed wall-clock for assistant turns: from the previous
    // user message's timestamp to this one. Skipped for the first turn
    // (nothing to compare against).
    std::optional<float> assistant_elapsed;
    if (msg.role == Role::Assistant) {
        // Walk back to the most recent user message timestamp.
        for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
            if (&m.d.current.messages[i] == &msg) continue;
            if (m.d.current.messages[i].role == Role::User) {
                auto dt = std::chrono::duration<float>(
                    msg.timestamp - m.d.current.messages[i].timestamp).count();
                if (dt > 0.0f && dt < 3600.0f) assistant_elapsed = dt;
                break;
            }
        }
    }

    // Build the turn body (header + content) without the rail; the
    // rail is applied as a left border to the entire stack so it runs
    // continuously top-to-bottom regardless of how many sub-rows the
    // turn produces.
    std::vector<Element> body;
    Color rail_color;

    if (msg.role == Role::User) {
        if (msg.checkpoint_id) rows.push_back(render_checkpoint_divider());
        rail_color = speaker_style_for(Role::User, m).color;
        body.push_back(turn_header(Role::User, turn_num, msg, m, std::nullopt));
        body.push_back(text(""));
        body.push_back(user_message_body(msg.text));
    } else if (msg.role == Role::Assistant) {
        rail_color = speaker_style_for(Role::Assistant, m).color;
        body.push_back(turn_header(Role::Assistant, turn_num, msg, m,
                                   assistant_elapsed));
        body.push_back(text(""));
        bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            body.push_back(cached_markdown_for(msg, m.d.current.id, msg_idx));
            if (!msg.tool_calls.empty()) body.push_back(text(""));
        }

        // Timeline view ALWAYS — both during the response and after it
        // settles. The Timeline is the higher-level "Actions" view: a
        // clean CI-pipeline-style log of what the assistant did, with
        // post-completion stats folded into each event's detail line
        // (line counts, hunk Δ, exit codes, match counts). Avoids the
        // wall of giant detailed cards that buried the conversation
        // every time the assistant ran a few tools.
        if (!msg.tool_calls.empty()) {
            int frame = m.s.spinner.frame_index();
            body.push_back(assistant_timeline(msg, frame, rail_color));
            // Render any in-flight permission inline so the user can
            // approve without losing the timeline context above.
            for (const auto& tc : msg.tool_calls) {
                if (m.d.pending_permission && m.d.pending_permission->id == tc.id) {
                    body.push_back(text(""));
                    body.push_back(render_inline_permission(*m.d.pending_permission, tc));
                }
            }
        }

        // Per-message error banner — set when the turn ended in a
        // stream-level error (overloaded, 5xx, network drop, etc.). Kept
        // SEPARATE from the message body so a partial assistant
        // response (preserved into `text` on error) and the failure
        // reason render distinctly. The status bar carries the live
        // signal; this is the historical marker so scrolling back shows
        // the user *which* turn died and why.
        if (msg.error) {
            body.push_back(text(""));
            body.push_back(h(
                text("\xe2\x9a\xa0  ", fg_bold(danger)),     // ⚠
                text(*msg.error, fg_dim(danger).with_italic())
            ).build());
        }
    }

    rows.push_back(with_turn_rail((v(std::move(body)) | grow(1.0f)).build(),
                                  rail_color));
    // Bottom breathing — a short blank then the next turn's divider.
    rows.push_back(text(""));
    return v(std::move(rows)).build();
}

Element thread_panel(const Model& m) {
    std::vector<Element> rows;
    // Virtualize: older messages live in the terminal's native scrollback
    // (their rows were committed via maya::Cmd::commit_scrollback).  We
    // preserve absolute turn numbering by counting finalized assistant
    // messages *before* the view window too, so a user seeing "Turn 42"
    // after scrolling back stays consistent.
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1;
    for (std::size_t i = 0; i < start; ++i)
        if (m.d.current.messages[i].role == Role::Assistant) ++turn;
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];
        // Inter-turn divider: thin dim rule between consecutive
        // user/assistant messages. Skip before the first message in
        // the visible window (no prior turn to divide from).
        if (i > start) rows.push_back(inter_turn_divider());
        rows.push_back(render_message(msg, i, turn, m));
        if (msg.role == Role::Assistant) ++turn;
    }
    if (m.s.active() && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        // Suppress this bottom indicator when the active assistant turn
        // is already showing its Timeline card — the timeline's own
        // in-progress spinner + status bar's spinner together carry the
        // "still working" signal; an extra spinner here was duplicate
        // chrome stacked under the card.
        const auto& last = m.d.current.messages.back();
        bool tl_visible_above =
            !last.tool_calls.empty()
            && std::any_of(last.tool_calls.begin(), last.tool_calls.end(),
                           [](const auto& tc){ return !tc.is_terminal(); });
        if (!tl_visible_above) {
            // Match the assistant turn header's left-edge bar so the
            // spinner reads as "still typing" inline with the message
            // above, not as a detached notification floating at the bottom.
            const auto& mid = m.d.model_id.value;
            Color edge_color = (mid.find("opus")   != std::string::npos) ? accent
                             : (mid.find("sonnet") != std::string::npos) ? info
                             : (mid.find("haiku")  != std::string::npos) ? success
                                                                         : highlight;
            auto spin = m.s.spinner;
            spin.set_style(Style{}.with_fg(edge_color).with_bold());
            std::string verb{phase_verb(m.s.phase)};
            rows.push_back((h(
                text("\xe2\x96\x8e", fg_of(edge_color)),                // ▎
                text(" ", {}),
                spin.build(),
                text(" " + verb + "\u2026", fg_italic(muted))
            ) | padding(0, 0, 0, 1)).build());
        }
    }
    if (rows.empty()) {
        // Wordmark-style welcome — quiet brand presence + the details that
        // orient the user (which model, which profile, what to do next).
        // A blank thread is the loneliest screen in the app; give it a
        // focal point with real visual weight.
        //
        // The wordmark is built from box-drawing characters so it scales
        // with the user's font and renders in any UTF-8 terminal — no
        // image bitmap, no font dependency, no broken glyph fallbacks.

        auto centered_text = [](std::string s, Style st) {
            return h(spacer(), text(std::move(s), st), spacer()).build();
        };

        // ── Wordmark: m o h a ────────────────────────────────────────
        // 3 rows × ~26 cells. Spacing between letters chosen so each glyph
        // reads as a discrete shape; using outline (┌┐└┘) instead of solid
        // blocks keeps it elegant rather than chunky.
        auto mark_style = fg_bold(accent);
        std::vector<Element> mark_rows;
        mark_rows.push_back(centered_text(
            "\u250c\u252c\u2510\u250c\u2500\u2510\u252c\u0020\u252c\u250c\u2500\u2510",
            mark_style));  // ┌┬┐┌─┐┬ ┬┌─┐
        mark_rows.push_back(centered_text(
            "\u2502\u2502\u2502\u2502\u0020\u2502\u251c\u2500\u2524\u251c\u2500\u2524",
            mark_style));  // │││││ │├─┤├─┤
        mark_rows.push_back(centered_text(
            "\u2534\u0020\u2534\u2514\u2500\u2518\u2534\u0020\u2534\u2534\u0020\u2534",
            fg_dim(accent)));  // ┴ ┴└─┘┴ ┴┴ ┴

        // ── Tagline ──────────────────────────────────────────────────
        auto tagline = centered_text(
            "a calm middleware between you and the model",
            fg_italic(muted));

        // ── Model + profile chip row ─────────────────────────────────
        ModelBadge mb;
        mb.set_model(m.d.model_id.value);
        mb.set_compact(true);
        auto profile_color_v = profile_color(m.d.profile);
        auto profile_chip = h(
            text("\u258c", fg_of(profile_color_v)),                  // ▌
            text(" " + std::string{profile_label(m.d.profile)} + " ",
                 Style{}.with_fg(profile_color_v).with_inverse().with_bold()),
            text("\u2590", fg_of(profile_color_v))                   // ▐
        ).build();
        auto chips_row = h(
            spacer(),
            mb.build(),
            text("    ", {}),
            std::move(profile_chip),
            spacer()
        ).build();

        // ── Starter prompts ──────────────────────────────────────────
        // Three example asks framed as a quiet bordered card so the user
        // sees concrete affordances, not "type something". Each is dim so
        // the eye doesn't read them as already-typed input.
        auto starter = [](std::string text_) {
            return h(
                text("\u2022 ", fg_dim(accent)),                      // •
                text(std::move(text_), fg_dim(fg))
            ).build();
        };
        auto starters_card = (v(
            text(" " + small_caps("Try") + " ", fg_bold(muted)),
            text("", {}),
            starter("Implement a small feature"),
            starter("Refactor or clean up this file"),
            starter("Explain what this code does"),
            starter("Write tests for ...")
        ) | padding(0, 2, 0, 2)
          | border(BorderStyle::Round)
          | bcolor(muted)
        ).build();

        auto starters_row = h(spacer(), starters_card, spacer()).build();

        // ── Bottom hint row ──────────────────────────────────────────
        auto hint = h(spacer(),
            text("type to begin  \u00B7  ", fg_dim(muted)),
            text("^K", fg_bold(highlight)),
            text(" palette  \u00B7  ", fg_dim(muted)),
            text("^J", fg_bold(highlight)),
            text(" threads  \u00B7  ", fg_dim(muted)),
            text("^N", fg_bold(success)),
            text(" new", fg_dim(muted)),
            spacer()).build();

        rows.push_back((v(
            text(""), text(""),
            mark_rows[0], mark_rows[1], mark_rows[2],
            text(""),
            tagline,
            text(""), text(""),
            chips_row,
            text(""), text(""),
            starters_row,
            text(""), text(""),
            hint
        )).build());
    }
    return (v(std::move(rows)) | padding(0, 1) | grow(1.0f)).build();
}

} // namespace moha::ui
