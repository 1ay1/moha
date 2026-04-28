#include "moha/runtime/view/thread.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <maya/widget/markdown.hpp>
#include <maya/widget/model_badge.hpp>

#include "moha/runtime/view/cache.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/permission.hpp"
#include "moha/runtime/view/tool_args.hpp"

namespace moha::ui {

namespace {

// ── Cached markdown render. Finalized assistant messages have an
//    immutable `text` — build once, reuse forever. Streaming messages
//    get a StreamingMarkdown with block-boundary caching (O(new_chars)
//    per delta). This is the ONE Element-returning helper kept in
//    moha — strictly because cross-frame cache state lives in the
//    StreamingMarkdown widget instance, which we keep alive across
//    frames so its block cache survives.
maya::Element cached_markdown_for(const Message& msg, const ThreadId& tid,
                                  std::size_t msg_idx) {
    auto& cache = message_md_cache(tid, msg_idx);
    if (msg.text.empty()) {
        if (!cache.streaming)
            cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_content(msg.streaming_text);
        return cache.streaming->build();
    }
    if (!cache.finalized) {
        cache.finalized = std::make_shared<maya::Element>(maya::markdown(msg.text));
        cache.streaming.reset();
    }
    return *cache.finalized;
}

// ── Per-speaker visual identity: rail color + glyph + display name.
//    Centralized so the rail color, the header glyph, and the bottom
//    streaming indicator stay in lockstep.
struct SpeakerStyle {
    maya::Color color;
    std::string glyph;
    std::string label;
};

SpeakerStyle speaker_style_for(Role role, const Model& m) {
    if (role == Role::User) {
        return {highlight, "\xe2\x9d\xaf", "You"};                   // ❯
    }
    const auto& id = m.d.model_id.value;
    maya::Color c;
    std::string label;
    if      (id.find("opus")   != std::string::npos) { c = accent;    label = "Opus";   }
    else if (id.find("sonnet") != std::string::npos) { c = info;      label = "Sonnet"; }
    else if (id.find("haiku")  != std::string::npos) { c = success;   label = "Haiku";  }
    else                                              { c = highlight; label = id;       }
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

// ── Trailing meta strip for a turn header — `12:34  ·  4.2s  ·  turn N`.
std::string format_turn_meta(const Message& msg, int turn_num,
                             std::optional<float> elapsed_secs) {
    std::string meta = timestamp_hh_mm(msg.timestamp);
    if (elapsed_secs && *elapsed_secs > 0.0f) {
        char buf[24];
        if      (*elapsed_secs < 1.0f)
            std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %.0fms",
                          static_cast<double>(*elapsed_secs) * 1000.0);
        else if (*elapsed_secs < 60.0f)
            std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %.1fs",
                          static_cast<double>(*elapsed_secs));
        else {
            int mins = static_cast<int>(*elapsed_secs) / 60;
            float secs = *elapsed_secs - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %dm%.0fs",
                          mins, static_cast<double>(secs));
        }
        meta += buf;
    }
    if (turn_num > 0) meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    return meta;
}

// ── Compute the assistant turn's wall-clock elapsed: from previous
//    user message timestamp to this one.
std::optional<float> assistant_elapsed(const Message& msg, const Model& m) {
    if (msg.role != Role::Assistant) return std::nullopt;
    for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
        if (&m.d.current.messages[i] == &msg) continue;
        if (m.d.current.messages[i].role == Role::User) {
            auto dt = std::chrono::duration<float>(
                msg.timestamp - m.d.current.messages[i].timestamp).count();
            if (dt > 0.0f && dt < 3600.0f) return dt;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// ── Tool category — semantic grouping for color + stats badge.
//    inspect (read/grep/glob/list/find/diag/web)  → info
//    mutate  (edit/write)                          → accent
//    execute (bash)                                → success
//    plan    (todo)                                → warn
//    vcs     (git_*)                               → highlight
maya::Color tool_category_color(const std::string& n) {
    if (n == "edit" || n == "write")  return accent;
    if (n == "bash")                  return success;
    if (n == "todo")                  return warn;
    if (n.rfind("git_", 0) == 0)      return highlight;
    return info;
}

std::string_view tool_category_label(const std::string& n) {
    if (n == "edit" || n == "write")  return "mutate";
    if (n == "bash")                  return "execute";
    if (n == "todo")                  return "plan";
    if (n.rfind("git_", 0) == 0)      return "vcs";
    return "inspect";
}

// ── ToolUse status → AgentEventStatus mapping. Approved folds into
//    Running because both render with the same in-flight spinner.
maya::AgentEventStatus tool_event_status(const ToolUse& tc) {
    if (tc.is_running() || tc.is_approved()) return maya::AgentEventStatus::Running;
    if (tc.is_pending())                     return maya::AgentEventStatus::Pending;
    if (tc.is_done())                        return maya::AgentEventStatus::Done;
    if (tc.is_failed())                      return maya::AgentEventStatus::Failed;
    return maya::AgentEventStatus::Rejected;
}

// ── Tool name → display label. Maps moha's lowercase canonical names
//    to brand TitleCase forms (matches Zed / Claude Code agent panel).
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
    return n;
}

// ── Brief "what this tool is doing" line for the timeline event.
//    Tool-specific so the user can read the sequence at a glance:
//    paths for fs ops, the actual command for bash, the pattern for
//    grep, etc. When settled, folds in post-completion stats.
std::string tool_timeline_detail(const ToolUse& tc) {
    auto safe = [&](const char* k) -> std::string { return safe_arg(tc.args, k); };
    auto path = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    const auto& n = tc.name.value;

    auto pretty_path = [&](std::string p) -> std::string {
        if (p.empty()) return p;
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec).string();
        if (!ec && !cwd.empty() && p.size() > cwd.size()
            && p.compare(0, cwd.size(), cwd) == 0 && p[cwd.size()] == '/')
            return p.substr(cwd.size() + 1);
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
            int lines = count_lines(tc.output());
            if (lines > 1) detail += "  \xc2\xb7  " + std::to_string(lines) + " lines";
        }
        return detail;
    }
    if (n == "write") {
        auto detail = path_pp.empty() ? std::string{"\xe2\x80\xa6"} : path_pp;
        if (tc.is_done()) {
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
        if (tc.args.is_object()) {
            auto it = tc.args.find("edits");
            if (it != tc.args.end() && it->is_array() && !it->empty())
                detail += "  \xc2\xb7  " + std::to_string(it->size()) + " edits";
        }
        if (tc.is_done()) {
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
        auto m = safe("message");
        if (auto nl = m.find('\n'); nl != std::string::npos) m = m.substr(0, nl);
        if (tc.is_done()) {
            const auto& out = tc.output();
            auto open = out.find('[');
            if (open != std::string::npos) {
                auto close = out.find(']', open);
                auto sp = out.find(' ', open + 1);
                if (sp != std::string::npos && sp < close) {
                    auto hash = out.substr(sp + 1, close - sp - 1);
                    if (!hash.empty() && hash.size() <= 12)
                        m += "  \xc2\xb7  " + hash;
                }
            }
        }
        return m;
    }
    if (n == "git_status" && tc.is_done()) {
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
            add(modified, "M"); add(staged, "S"); add(untracked, "?");
        } else {
            detail += "  \xc2\xb7  clean";
        }
        return detail;
    }
    if (n == "git_diff" || n == "git_log" || n == "git_status")
        return path_pp.empty() ? std::string{"."} : path_pp;
    if (n == "todo") {
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

// ── ToolUse → ToolBodyPreview::Config. Picks the right discriminated
//    body kind based on tool name + state, and extracts the relevant
//    data (output text, edit hunks, todo items). No element work.
maya::ToolBodyPreview::Config tool_body_config(const ToolUse& tc) {
    using Kind = maya::ToolBodyPreview::Kind;
    const auto& n = tc.name.value;
    maya::ToolBodyPreview::Config out;

    // ── Failure: surface stderr inline so it isn't hidden.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = tc.output();
        return out;
    }

    // ── Edit: parse hunks from args.
    if (n == "edit" && tc.args.is_object()) {
        if (auto it = tc.args.find("edits");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            out.kind = Kind::EditDiff;
            out.hunks.reserve(it->size());
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                auto ot = e.value("old_text", e.value("old_string", std::string{}));
                auto nt = e.value("new_text", e.value("new_string", std::string{}));
                out.hunks.push_back({std::move(ot), std::move(nt)});
            }
            return out;
        }
        // Top-level legacy single-edit shape.
        auto ot = safe_arg(tc.args, "old_text");
        if (ot.empty()) ot = safe_arg(tc.args, "old_string");
        auto nt = safe_arg(tc.args, "new_text");
        if (nt.empty()) nt = safe_arg(tc.args, "new_string");
        if (!ot.empty() || !nt.empty()) {
            out.kind = Kind::EditDiff;
            out.hunks.push_back({std::move(ot), std::move(nt)});
        }
        return out;
    }

    // ── Write: head+tail of the streaming/written content.
    if (n == "write") {
        auto content = safe_arg(tc.args, "content");
        if (!content.empty()) {
            out.kind = Kind::CodeBlock;
            out.text = std::move(content);
            out.text_color = fg;
        }
        return out;
    }

    // ── Bash / diagnostics: head+tail of output.
    if ((n == "bash" || n == "diagnostics") && tc.is_terminal()) {
        auto stripped = strip_bash_output_fence(tc.output());
        if (!stripped.empty()) {
            out.kind = Kind::CodeBlock;
            out.text = std::move(stripped);
            out.text_color = fg;
        }
        return out;
    }
    if (n == "bash" && tc.is_running() && !tc.progress_text().empty()) {
        out.kind = Kind::CodeBlock;
        out.text = tc.progress_text();
        out.text_color = fg;
        return out;
    }

    // ── git_diff: per-line diff coloring.
    if (n == "git_diff" && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty() && body != "no changes") {
            out.kind = Kind::GitDiff;
            out.text = body;
            out.text_color = fg;
        }
        return out;
    }

    // ── Generic line-oriented tools: head+tail preview.
    if ((n == "read" || n == "list_dir" || n == "grep" || n == "glob"
         || n == "find_definition"
         || n == "web_fetch" || n == "web_search"
         || n == "git_status" || n == "git_log" || n == "git_commit")
        && tc.is_done())
    {
        if (!tc.output().empty()) {
            out.kind = Kind::CodeBlock;
            out.text = tc.output();
            out.text_color = fg;
        }
        return out;
    }

    // ── Todo: parse items + statuses.
    if (n == "todo" && tc.args.is_object()) {
        if (auto it = tc.args.find("todos");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            using Status = maya::ToolBodyPreview::TodoItem::Status;
            out.kind = Kind::TodoList;
            out.todos.reserve(it->size());
            for (const auto& td : *it) {
                if (!td.is_object()) continue;
                Status s = Status::Pending;
                auto st = td.value("status", std::string{"pending"});
                if      (st == "completed")   s = Status::Completed;
                else if (st == "in_progress") s = Status::InProgress;
                out.todos.push_back({td.value("content", ""), s});
            }
            return out;
        }
    }

    return out;     // kind = None
}

// ── Build the assistant turn's "Actions" panel config.
maya::AgentTimeline::Config agent_timeline_config(const Message& msg,
                                                  int spinner_frame,
                                                  maya::Color rail_color) {
    int total = static_cast<int>(msg.tool_calls.size());
    int done  = 0;
    float total_elapsed = 0.0f;
    int running_idx = -1;

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

    maya::AgentTimeline::Config cfg;
    cfg.frame = spinner_frame;

    // ── Stats. Pick a representative color per category so the badge
    //    matches the per-event tree glyph color downstream.
    for (const auto& [cat, n] : cat_counts) {
        maya::Color cc = (cat == "mutate")  ? accent
                       : (cat == "execute") ? success
                       : (cat == "plan")    ? warn
                       : (cat == "vcs")     ? highlight
                                            : info;
        cfg.stats.push_back({cat, n, cc});
    }

    // ── Events.
    cfg.events.reserve(msg.tool_calls.size());
    for (const auto& tc : msg.tool_calls) {
        std::string detail = tool_timeline_detail(tc);
        if (detail.empty()) {
            detail = tc.is_running()  ? std::string{"running\xe2\x80\xa6"}
                   : tc.is_pending()  ? std::string{"queued\xe2\x80\xa6"}
                   : tc.is_approved() ? std::string{"approved\xe2\x80\xa6"}
                                      : std::string{"\xe2\x80\xa6"};
        }
        cfg.events.push_back({
            .name            = tool_display_name(tc.name.value),
            .detail          = std::move(detail),
            .elapsed_seconds = tc.is_terminal() ? tool_elapsed(tc) : 0.0f,
            .category_color  = tool_category_color(tc.name.value),
            .status          = tool_event_status(tc),
            .body            = tool_body_config(tc),
        });
    }

    // ── Footer: ✓ DONE / ✗ N FAILED / ⊘ N REJECTED, only when settled.
    if (done == total && total > 0) {
        int failed = 0, rejected = 0;
        for (const auto& tc : msg.tool_calls) {
            if (tc.is_failed())   ++failed;
            if (tc.is_rejected()) ++rejected;
        }
        maya::AgentTimelineFooter f;
        f.glyph = "\xe2\x9c\x93";   // ✓
        f.text  = "done";
        f.color = success;
        if (failed > 0) {
            f.glyph = "\xe2\x9c\x97";           // ✗
            f.text  = std::to_string(failed) + " failed";
            f.color = danger;
        } else if (rejected > 0) {
            f.glyph = "\xe2\x8a\x98";           // ⊘
            f.text  = std::to_string(rejected) + " rejected";
            f.color = warn;
        }
        char dur_buf[24];
        if (total_elapsed < 1.0f)
            std::snprintf(dur_buf, sizeof(dur_buf), "%.0fms",
                          static_cast<double>(total_elapsed) * 1000.0);
        else if (total_elapsed < 60.0f)
            std::snprintf(dur_buf, sizeof(dur_buf), "%.1fs",
                          static_cast<double>(total_elapsed));
        else {
            int mins   = static_cast<int>(total_elapsed) / 60;
            float rest = total_elapsed - static_cast<float>(mins * 60);
            std::snprintf(dur_buf, sizeof(dur_buf), "%dm%.0fs",
                          mins, static_cast<double>(rest));
        }
        f.summary = std::to_string(total)
                  + (total == 1 ? " action   " : " actions   ")
                  + dur_buf;
        cfg.footer = std::move(f);
    }

    // ── Title and border.
    std::string title = " " + small_caps("Actions") + "  \xc2\xb7  "
                      + std::to_string(done) + "/" + std::to_string(total);
    if (running_idx >= 0) {
        title += "  \xc2\xb7  " + tool_display_name(
            msg.tool_calls[static_cast<std::size_t>(running_idx)].name.value);
    } else if (done == total && total > 0) {
        char buf[24];
        if (total_elapsed < 1.0f)
            std::snprintf(buf, sizeof(buf), "%.0fms",
                          static_cast<double>(total_elapsed) * 1000.0);
        else if (total_elapsed < 60.0f)
            std::snprintf(buf, sizeof(buf), "%.1fs",
                          static_cast<double>(total_elapsed));
        else {
            int mins   = static_cast<int>(total_elapsed) / 60;
            float rest = total_elapsed - static_cast<float>(mins * 60);
            std::snprintf(buf, sizeof(buf), "%dm%.0fs",
                          mins, static_cast<double>(rest));
        }
        title += "  \xc2\xb7  ";
        title += buf;
    }
    title += " ";

    bool all_done = (done == total && total > 0);
    cfg.title        = std::move(title);
    cfg.border_color = all_done ? muted : rail_color;
    return cfg;
}

// ── Build the WelcomeScreen config from Model.
maya::WelcomeScreen::Config welcome_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);
    maya::WelcomeScreen::Config cfg;
    cfg.wordmark       = {"\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\xe2\x94\xac \xe2\x94\xac\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",
                          "\xe2\x94\x82\xe2\x94\x82\xe2\x94\x82\xe2\x94\x82 \xe2\x94\x82\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",
                          "\xe2\x94\xb4 \xe2\x94\xb4\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98\xe2\x94\xb4 \xe2\x94\xb4\xe2\x94\xb4 \xe2\x94\xb4"};
    cfg.wordmark_color = accent;
    cfg.tagline        = "a calm middleware between you and the model";
    cfg.model_badge    = mb.build();
    cfg.profile_label  = std::string{profile_label(m.d.profile)};
    cfg.profile_color  = profile_color(m.d.profile);
    cfg.starters_title = "Try";
    cfg.starters       = {"Implement a small feature",
                          "Refactor or clean up this file",
                          "Explain what this code does",
                          "Write tests for ..."};
    cfg.hint_intro     = "type to begin";
    cfg.hints          = {{"^K", " palette", highlight},
                          {"^J", " threads", highlight},
                          {"^N", " new",     success}};
    cfg.accent_color   = accent;
    cfg.text_color     = fg;
    return cfg;
}

// ── Build the Turn config for one message.
maya::Turn::Config turn_config(const Message& msg, std::size_t msg_idx,
                               int turn_num, const Model& m) {
    auto style = speaker_style_for(msg.role, m);

    maya::Turn::Config cfg;
    cfg.glyph      = style.glyph;
    cfg.label      = style.label;
    cfg.rail_color = style.color;
    cfg.meta       = format_turn_meta(msg, turn_num,
                         msg.role == Role::Assistant
                             ? assistant_elapsed(msg, m)
                             : std::nullopt);
    cfg.checkpoint_above = (msg.role == Role::User && msg.checkpoint_id.has_value());
    cfg.checkpoint_color = warn;

    if (msg.role == Role::User) {
        cfg.body.emplace_back(maya::Turn::PlainText{.content = msg.text, .color = fg});
    } else if (msg.role == Role::Assistant) {
        const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            // StreamingMarkdown caches block boundaries across frames —
            // use the Element escape hatch so the cached widget
            // instance survives between frames.
            cfg.body.emplace_back(cached_markdown_for(msg, m.d.current.id, msg_idx));
        }
        if (!msg.tool_calls.empty()) {
            cfg.body.emplace_back(
                agent_timeline_config(msg, m.s.spinner.frame_index(), style.color));
            // In-flight permission card under the timeline so the
            // user can approve without losing context.
            for (const auto& tc : msg.tool_calls) {
                if (m.d.pending_permission && m.d.pending_permission->id == tc.id) {
                    cfg.body.emplace_back(inline_permission_config(
                        *m.d.pending_permission, tc));
                }
            }
        }
        if (msg.error) cfg.error = *msg.error;
    }

    return cfg;
}

// ── Pick the bottom-of-thread in-flight indicator config, if needed.
//    Suppressed when the active assistant turn already shows a Timeline
//    spinner — its in-progress card + status bar already carry the
//    "still working" signal; a second one stacked under it was just
//    duplicate chrome.
std::optional<maya::ActivityIndicator::Config>
in_flight_indicator(const Model& m) {
    if (!m.s.active()) return std::nullopt;
    if (m.d.current.messages.empty()) return std::nullopt;
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return std::nullopt;
    bool tl_visible =
        !last.tool_calls.empty()
        && std::any_of(last.tool_calls.begin(), last.tool_calls.end(),
                       [](const auto& tc){ return !tc.is_terminal(); });
    if (tl_visible) return std::nullopt;

    const auto& mid = m.d.model_id.value;
    maya::Color edge = (mid.find("opus")   != std::string::npos) ? accent
                     : (mid.find("sonnet") != std::string::npos) ? info
                     : (mid.find("haiku")  != std::string::npos) ? success
                                                                 : highlight;
    maya::ActivityIndicator::Config cfg;
    cfg.edge_color    = edge;
    cfg.spinner_glyph = std::string{m.s.spinner.current_frame()};
    cfg.label         = std::string{phase_verb(m.s.phase)};
    return cfg;
}

} // namespace

maya::Thread::Config thread_config(const Model& m) {
    maya::Thread::Config cfg;

    if (m.d.current.messages.empty()) {
        cfg.is_empty = true;
        cfg.welcome  = welcome_config(m);
        return cfg;
    }

    // Virtualize: older messages live in the terminal's native scrollback
    // (committed via maya::Cmd::commit_scrollback). Preserve absolute
    // turn numbering by counting finalized assistant messages BEFORE the
    // view window too — "turn 42" stays consistent after scrolling back.
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1;
    for (std::size_t i = 0; i < start; ++i)
        if (m.d.current.messages[i].role == Role::Assistant) ++turn;

    cfg.turns.reserve(total - start);
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];
        cfg.turns.push_back(turn_config(msg, i, turn, m));
        if (msg.role == Role::Assistant) ++turn;
    }

    cfg.in_flight = in_flight_indicator(m);
    return cfg;
}

} // namespace moha::ui
