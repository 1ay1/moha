#include "moha/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <span>
#include <utility>

#include <maya/core/overload.hpp>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/io/http.hpp"
#include "moha/tool/util/partial_json.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app {

using maya::Cmd;
using maya::overload;
using json = nlohmann::json;

namespace {

using Step = std::pair<Model, Cmd<Msg>>;
inline Step done(Model m) { return {std::move(m), Cmd<Msg>::none()}; }

// Hard cap on per-message live buffers. A misbehaving server (or an
// adversarial proxy) emitting unbounded `text_delta`/`input_json_delta`
// would otherwise grow `streaming_text` / `args_streaming` until the
// process OOMs. 8 MiB is well above any realistic single-message body
// — a 16 K-token response is ~64 KB, and even multi-MB write tools
// stay under 1 MB per content block. Hitting this cap means something
// genuinely broken upstream, not a real workload.
constexpr std::size_t kMaxStreamingBytes = 8 * 1024 * 1024;

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

// Keys models sometimes emit in place of our canonical field name. Mirrors
// the ArgReader alias table — keep in sync.
constexpr std::string_view kPathAliases[]    = {"path", "file_path", "filepath", "filename"};
constexpr std::string_view kOldStrAliases[]  = {"old_string", "old_str", "oldStr"};
constexpr std::string_view kNewStrAliases[]  = {"new_string", "new_str", "newStr"};
constexpr std::string_view kContentAliases[] = {"content", "file_text", "text",
                                                 "file_content", "contents",
                                                 "body", "data"};
constexpr std::string_view kDisplayDescription = "display_description";

std::optional<std::string> sniff_any(const std::string& raw,
                                     std::span<const std::string_view> keys,
                                     bool partial) {
    for (auto k : keys) {
        auto v = partial ? sniff_string_progressive(raw, k)
                         : sniff_string(raw, k);
        if (v) return v;
    }
    return std::nullopt;
}

// Attempt to parse the streaming buffer via the partial-JSON closer. Returns
// an object when the result is a parseable object, otherwise nullopt. This is
// strictly more capable than the regex sniffer — it handles nested objects
// (edits[].old_text) and escaped quotes — but we still fall back to the
// sniffer for fields the partial closer can't yet expose (e.g. when the
// current field's value is itself a partial string that won't close until
// later).
std::optional<json> try_parse_partial(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    try {
        auto closed = moha::tools::util::close_partial_json(raw);
        auto parsed = json::parse(closed, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

// Pull a string out of a parsed partial object, trying canonical + aliases.
std::optional<std::string> get_string_any(const json& obj,
                                          std::span<const std::string_view> keys) {
    for (auto k : keys) {
        auto it = obj.find(std::string{k});
        if (it == obj.end()) continue;
        if (it->is_string()) return it->get<std::string>();
    }
    return std::nullopt;
}

// For a given tool, fill `tc.args` with whichever scalar field is most useful
// to display in the card header. We write into `tc.args` (rather than a
// separate preview field) so the existing view code — which reads path /
// command / pattern from args — picks it up unchanged.
// Hard cap on the live content preview shown during streaming.  The widget
// only renders the first ~6 lines of `content` while the model is mid-write,
// and re-extracting / re-comparing / re-laying-out a multi-KB body 8x/sec
// is what made big writes "feel" stuck even when the bytes were arriving.
// 4 KiB comfortably covers ~50 wide-screen lines — far more than the widget
// shows — and bounds every per-tick op to constant work.
constexpr size_t kStreamingPreviewCap = 4 * 1024;

void update_stream_preview(ToolUse& tc) {
    auto set_arg = [&](std::string_view key, std::string v) {
        if (!tc.args.is_object()) tc.args = json::object();
        auto& cur = tc.args[std::string{key}];
        // Cheap "did it change?" — full byte compare on a multi-KB content
        // string was ~half the per-tick cost. Same-size + same-bookend is a
        // very strong signal of "unchanged" in practice (the only way it
        // false-positives is if the model rewrote middle bytes without
        // changing length, which never happens during append-only streaming).
        if (cur.is_string()) {
            const auto& s = cur.get_ref<const std::string&>();
            if (s.size() == v.size()
                && (s.empty()
                    || (s.front() == v.front() && s.back() == v.back())))
                return;
        }
        cur = std::move(v);
        tc.mark_args_dirty();
    };
    auto try_set = [&](std::string_view canon,
                       std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/false)) {
            set_arg(canon, *v); return true;
        }
        return false;
    };
    auto try_set_partial = [&](std::string_view canon,
                               std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/true)) {
            set_arg(canon, *v); return true;
        }
        return false;
    };
    // Partial-JSON first pass: if we can close the buffer into a parseable
    // object, pull structured fields directly (including nested edits[0]).
    // Falls through to regex sniffing for anything the closer can't yield.
    auto parsed = try_parse_partial(tc.args_streaming);
    auto try_struct = [&](std::string_view canon,
                          std::span<const std::string_view> keys) -> bool {
        if (!parsed) return false;
        if (auto s = get_string_any(*parsed, keys)) {
            set_arg(canon, *s);
            return true;
        }
        return false;
    };
    // Edit tool: read the first edit's old_text / new_text so the UI shows a
    // live diff even while the model is mid-stream.
    auto try_struct_first_edit = [&](std::string_view canon,
                                     std::string_view field) -> bool {
        if (!parsed) return false;
        auto it = parsed->find("edits");
        if (it == parsed->end() || !it->is_array() || it->empty()) return false;
        const auto& first = (*it)[0];
        if (!first.is_object()) return false;
        auto f = first.find(std::string{field});
        if (f == first.end() || !f->is_string()) return false;
        set_arg(canon, f->get<std::string>());
        return true;
    };

    // Every tool may carry a `display_description`; pull it up-front so the
    // card title can reflect it the moment the field closes in the stream.
    auto pull_desc = [&] {
        if (!try_struct("display_description", std::span{&kDisplayDescription, 1}))
            (void)try_set("display_description", std::span{&kDisplayDescription, 1});
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") {
        if (!try_struct("path", kPathAliases)) try_set("path", kPathAliases);
        pull_desc();
    }
    else if (n == "write") {
        // Write's dedicated fast path. The general try_struct call closes
        // and parses the entire growing args buffer into nlohmann::json on
        // every tick — fine for tiny tools, ~quadratic on a multi-KB write
        // body and the dominant cost behind big writes "hanging" the UI
        // even when bytes are flowing on the wire.
        //
        // Path + display_description are <100 bytes each, parsing is cheap,
        // and we *want* the structured close to handle weird value escapes
        // robustly. Keep the full try_struct path for those.
        if (!try_struct("path", kPathAliases)) try_set("path", kPathAliases);
        pull_desc();

        // Content goes through a stripped-down path: progressive sniff only
        // (no full close+parse), tail-clipped to kStreamingPreviewCap so the
        // preview-update cost is O(cap) regardless of how big the body gets.
        // Wrapped in try because sniff_string_progressive walks the buffer
        // by hand and any malformed escape at the buffer edge would, in
        // principle, still throw via std::string growth — we'd rather show
        // a stale preview than fail the reducer step.
        try {
            auto v = sniff_any(tc.args_streaming, kContentAliases,
                               /*partial=*/true);
            if (v && !v->empty()) {
                if (v->size() > kStreamingPreviewCap)
                    *v = std::string{"… (showing tail) …\n"}
                       + v->substr(v->size() - kStreamingPreviewCap);
                set_arg("content", std::move(*v));
            }
        } catch (...) { /* swallow — header fields already painted */ }
    }
    else if (n == "edit") {
        if (!try_struct("path", kPathAliases)) try_set("path", kPathAliases);
        pull_desc();
        // Mirror the FULL edits array into tc.args["edits"] as it grows so
        // the card can render every edit during streaming, not just the
        // first. Each entry is a partial object — old_text may be present
        // before new_text starts arriving — and we keep them in order so
        // the renderer's "edit N/M" labels stay stable as more edits land.
        bool wrote_edits_array = false;
        if (parsed) {
            if (auto it = parsed->find("edits");
                it != parsed->end() && it->is_array() && !it->empty())
            {
                json arr = json::array();
                for (const auto& e : *it) {
                    if (!e.is_object()) continue;
                    json out = json::object();
                    if (auto o = e.find("old_text"); o != e.end() && o->is_string())
                        out["old_text"] = o->get<std::string>();
                    else if (auto o2 = e.find("old_string"); o2 != e.end() && o2->is_string())
                        out["old_text"] = o2->get<std::string>();
                    if (auto nv = e.find("new_text"); nv != e.end() && nv->is_string())
                        out["new_text"] = nv->get<std::string>();
                    else if (auto nv2 = e.find("new_string"); nv2 != e.end() && nv2->is_string())
                        out["new_text"] = nv2->get<std::string>();
                    arr.push_back(std::move(out));
                }
                if (!arr.empty()) {
                    if (!tc.args.is_object()) tc.args = json::object();
                    auto& cur = tc.args["edits"];
                    // Cheap change check: same length + same first/last
                    // old_text size pair → assume unchanged. Bookend check
                    // the same way set_arg does for content.
                    bool changed = !cur.is_array() || cur.size() != arr.size();
                    if (!changed) {
                        for (std::size_t i = 0; i < arr.size(); ++i) {
                            const auto& a = arr[i];
                            const auto& b = cur[i];
                            auto a_old = a.value("old_text", std::string{});
                            auto b_old = b.value("old_text", std::string{});
                            auto a_new = a.value("new_text", std::string{});
                            auto b_new = b.value("new_text", std::string{});
                            if (a_old.size() != b_old.size() || a_new.size() != b_new.size()) {
                                changed = true; break;
                            }
                        }
                    }
                    if (changed) {
                        cur = std::move(arr);
                        tc.mark_args_dirty();
                    }
                    wrote_edits_array = true;
                }
            }
        }
        // Legacy single-edit shape (top-level old_string/new_string or
        // old_text/new_text). Only used when no edits array was visible —
        // these top-level fields and the array are mutually exclusive in
        // practice and showing both would double-render the diff.
        if (!wrote_edits_array) {
            if (!try_struct_first_edit("old_string", "old_text"))
                try_set_partial("old_string", kOldStrAliases);
            if (!try_struct_first_edit("new_string", "new_text"))
                try_set_partial("new_string", kNewStrAliases);
        }
    }
    else if (n == "bash")  { try_set("command"); pull_desc(); }
    else if (n == "grep")  { try_set("pattern"); try_set("path", kPathAliases); pull_desc(); }
    else if (n == "glob")  { try_set("pattern"); pull_desc(); }
    else if (n == "find_definition") { try_set("symbol"); pull_desc(); }
    else if (n == "web_fetch")       { try_set("url");    pull_desc(); }
    else if (n == "web_search")      { try_set("query");  pull_desc(); }
    else if (n == "diagnostics")     { try_set("command"); pull_desc(); }
    else if (n == "git_status" || n == "git_diff"
          || n == "git_log"    || n == "git_commit"
          || n == "todo")        { if (n == "git_commit") try_set("message"); pull_desc(); }
}

// Truncation guard: after the stream parses/salvages tool args, verify the
// minimum fields the *target tool* actually needs. A common failure mode is
// the wire dropping between `display_description`'s closing `"` and the
// `"content":` that should follow — `close_partial_json` then strips the
// dangling `,` and produces a well-formed but content-less object. If we ran
// the tool on that, write would silently produce an empty file and the model
// would retry on a cryptic "content required" loop. Returns non-empty string
// ("content", "old_string", …) naming a missing required field; empty when
// the shape is complete enough to dispatch.
std::string_view missing_required_field(std::string_view tool_name,
                                        const json& args) {
    if (!args.is_object()) return "(args)";
    auto is_nonempty_string_any = [&](std::span<const std::string_view> keys) {
        for (auto k : keys) {
            auto it = args.find(std::string{k});
            if (it == args.end() || !it->is_string()) continue;
            if (!it->get_ref<const std::string&>().empty()) return true;
        }
        return false;
    };
    if (tool_name == "write") {
        if (!is_nonempty_string_any(kPathAliases))    return "path";
        if (!is_nonempty_string_any(kContentAliases)) return "content";
    } else if (tool_name == "edit") {
        if (!is_nonempty_string_any(kPathAliases))    return "path";
        auto it = args.find("edits");
        if (it != args.end() && it->is_array() && !it->empty()) return {};
        if (!is_nonempty_string_any(kOldStrAliases))  return "old_string";
        if (!is_nonempty_string_any(kNewStrAliases))  return "new_string";
    } else if (tool_name == "bash" || tool_name == "diagnostics") {
        auto it = args.find("command");
        if (it == args.end() || !it->is_string()
            || it->get_ref<const std::string&>().empty()) return "command";
    } else if (tool_name == "read" || tool_name == "list_dir"
            || tool_name == "glob"
            || tool_name == "git_diff"  || tool_name == "git_log") {
        // `path` is nice-to-have but not strictly required (list_dir/glob
        // default to cwd; read without path is already a tool error).
    } else if (tool_name == "grep") {
        auto it = args.find("pattern");
        if (it == args.end() || !it->is_string()
            || it->get_ref<const std::string&>().empty()) return "pattern";
    } else if (tool_name == "find_definition") {
        auto it = args.find("symbol");
        if (it == args.end() || !it->is_string()
            || it->get_ref<const std::string&>().empty()) return "symbol";
    } else if (tool_name == "web_fetch") {
        auto it = args.find("url");
        if (it == args.end() || !it->is_string()
            || it->get_ref<const std::string&>().empty()) return "url";
    } else if (tool_name == "git_commit") {
        auto it = args.find("message");
        if (it == args.end() || !it->is_string()
            || it->get_ref<const std::string&>().empty()) return "message";
    }
    return {};
}

// Check missing-required against the parsed/salvaged args and, if something's
// absent, mark the tool as Error with a message framed for *the model* — so
// it can tell this apart from "your tool call was wrong" (which would prompt
// the same bad output again) and emit a fresh tool_use with the missing field.
// Returns true when the tool was marked Error (caller should not dispatch).
bool guard_truncated_tool_args(ToolUse& tc) {
    auto missing = missing_required_field(tc.name.value, tc.args);
    if (missing.empty()) return false;
    auto now = std::chrono::steady_clock::now();
    tc.status = ToolUse::Failed{
        tc.started_at(),
        now,
        std::string{"Tool call arguments look incomplete — `"}
            + std::string{missing}
            + "` is missing. This usually means the stream was truncated "
              "before the full tool input arrived. Please emit a fresh "
              "tool call with every required field populated (including `"
            + std::string{missing} + "`).",
    };
    return true;
}

// Rescue partial args when json::parse fails on the raw SSE buffer (truncated
// mid-content, malformed escape, etc). We best-effort sniff each expected
// scalar field and hand the result back as a json object — if at least one
// usable field came through, the tool gets to run instead of the whole turn
// dying on a cosmetic parse error. Returns {} when nothing salvageable.
json salvage_args(const ToolUse& tc) {
    // First try: close the partial JSON and parse it structurally. This
    // preserves nested shapes like edits[] and object fields that the regex
    // sniffer can't see. If parsing succeeds and gives us a usable object,
    // return it directly; the tool's own parser will handle tolerance.
    if (auto parsed = try_parse_partial(tc.args_streaming)) {
        if (!parsed->empty()) return *parsed;
    }
    // Fallback: walk known scalar keys with the regex sniffer. Only hit when
    // the partial closer couldn't yield any object (deeply malformed buffer).
    json out = json::object();
    auto pick = [&](std::string_view canon,
                    std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/true))
            out[std::string{canon}] = *v;
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") { pick("path", kPathAliases); }
    else if (n == "write") { pick("path", kPathAliases); pick("content", kContentAliases); }
    else if (n == "edit")  { pick("path", kPathAliases);
                             pick("old_string", kOldStrAliases);
                             pick("new_string", kNewStrAliases); }
    else if (n == "bash")  { pick("command"); }
    else if (n == "grep")  { pick("pattern"); pick("path", kPathAliases); }
    else if (n == "glob")  { pick("pattern"); }
    else if (n == "find_definition") { pick("symbol"); }
    else if (n == "web_fetch")       { pick("url"); }
    else if (n == "web_search")      { pick("query"); }
    else if (n == "diagnostics")     { pick("command"); }
    else if (n == "git_commit")      { pick("message"); }
    // display_description applies to every tool and often wins the race vs
    // the required field — include it in the salvaged fallback too.
    pick("display_description", std::span{&kDisplayDescription, 1});
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
        const auto& out = tc.output();
        if (!out.empty()) {
            int nl = 0;
            for (char c : out) if (c == '\n') ++nl;
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

    if (m.stream.is_streaming() || m.stream.is_executing_tool()) {
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
    m.stream.phase = phase::Streaming{};
    m.stream.active = true;
    m.stream.truncation_retries = 0;
    auto virt = maybe_virtualize(m);
    auto launch = cmd::launch_stream(m);
    auto cmd = virt.is_none()
        ? std::move(launch)
        : Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(virt), std::move(launch)});
    return {std::move(m), std::move(cmd)};
}

// Cap on transparent retries per user turn before we give up and surface the
// truncation as a real Error to the model. Two attempts is enough to ride
// out an intermittent edge-side idle-timeout cut, but small enough that a
// genuinely broken upstream surfaces quickly instead of looping.
constexpr int kMaxTruncationRetries = 2;

Cmd<Msg> finalize_turn(Model& m, std::string_view stop_reason = {}) {
    m.stream.active = false;
    // Stream is over — drop the cancel handle so a stale Esc can't trip the
    // next turn's stream the moment it launches.
    m.stream.cancel.reset();
    bool any_truncated = false;
    const bool max_tokens_hit = (stop_reason == "max_tokens");
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
        // Flush any tool_calls whose StreamToolUseEnd never fired — Anthropic
        // normally sends content_block_stop per tool block, but proxies /
        // message_stop cut-offs can skip it. Without this, write/edit tools
        // would run with only the progressive-sniffer partials that
        // update_stream_preview accumulated, which for long content is
        // typically empty or truncated. Parse if we can, salvage if we can't,
        // mark Error if neither works.
        for (auto& tc : last.tool_calls) {
            if (!tc.args_streaming.empty() && tc.is_pending()) {
                try {
                    tc.args = json::parse(tc.args_streaming);
                    tc.mark_args_dirty();
                } catch (const std::exception& ex) {
                    auto salvaged = salvage_args(tc);
                    if (!salvaged.empty()) {
                        tc.args = std::move(salvaged);
                        tc.mark_args_dirty();
                    } else {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            std::string{"tool args never closed: "} + ex.what()};
                    }
                }
            }
            std::string{}.swap(tc.args_streaming);
            if (tc.is_pending()) {
                if (guard_truncated_tool_args(tc)) any_truncated = true;
            }
        }
    }

    // ── Surface a clear error on max_tokens cutoff ─────────────────────
    // Retrying does nothing useful when stop_reason==max_tokens: the model
    // would just burn its budget the same way again. Replace the generic
    // "stream was truncated" guard message with one that names the actual
    // cause so the model can shrink its output (smaller diff, edit instead
    // of write, fewer files at once).
    if (any_truncated && max_tokens_hit
        && !m.current.messages.empty()
        && m.current.messages.back().role == Role::Assistant) {
        for (auto& tc : m.current.messages.back().tool_calls) {
            if (auto* f = std::get_if<ToolUse::Failed>(&tc.status);
                f && f->output.starts_with("Tool call arguments look incomplete")) {
                f->output = "Output token cap (max_tokens) was reached before "
                            "the tool input finished streaming, so the call "
                            "was cut off. Retry with a smaller payload — for "
                            "long files, prefer the `edit` tool over `write`, "
                            "or split the change across multiple calls.";
            }
        }
    }

    // ── Transparent retry on upstream truncation ───────────────────────
    // libcurl's TCP keepalive can't prevent an upstream LB (Anthropic edge,
    // Cloudflare, etc.) from closing an idle connection. When that happens
    // mid-tool-input, we get a truncated args buffer and the model sees a
    // confusing red "content required" card. Instead, drop the half-baked
    // assistant turn and silently re-launch — the user message before it is
    // intact, so the next attempt runs with the same context. We only retry
    // when nothing useful was already produced (no rendered text, no
    // completed tool calls), to avoid clobbering work the user can already
    // see. Capped at kMaxTruncationRetries. Skipped when the truncation was
    // caused by hitting max_tokens (retry would loop on the same cap).
    if (any_truncated
        && !max_tokens_hit
        && m.stream.truncation_retries < kMaxTruncationRetries
        && !m.current.messages.empty()
        && m.current.messages.back().role == Role::Assistant) {
        auto& last = m.current.messages.back();
        const bool has_committed_work =
            !last.text.empty() ||
            std::ranges::any_of(last.tool_calls, [](const auto& tc) {
                return tc.is_done() || tc.is_running();
            });
        if (!has_committed_work) {
            ++m.stream.truncation_retries;
            m.current.messages.pop_back();  // drop the truncated assistant turn
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.current.messages.push_back(std::move(placeholder));
            m.stream.phase = phase::Streaming{};
            m.stream.active = true;
            m.stream.status = "retrying (upstream cut off)…";
            return cmd::launch_stream(m);
        }
    }

    deps().save_thread(m.current);
    auto kp = cmd::kick_pending_tools(m);

    if (m.stream.is_idle() && !m.composer.queued.empty()) {
        m.composer.text = m.composer.queued.front();
        m.composer.queued.erase(m.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
    }
    return kp;
}

void persist_settings(const Model& m) {
    store::Settings s;
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
                auto now = std::chrono::steady_clock::now();
                auto started = tc.started_at();
                if (error) tc.status = ToolUse::Failed{started, now, std::move(output)};
                else       tc.status = ToolUse::Done  {started, now, std::move(output)};
            }
}

// Mark a tool as Rejected (user denied permission). The output goes onto a
// synthetic Failed-shaped state? No — Rejected has no output channel. The
// caller passes a message that, for now, we route into a Failed transition
// so it surfaces in the card. Used only by the permission-denied path.
void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason) {
    for (auto& msg : m.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                auto now = std::chrono::steady_clock::now();
                if (reason.empty()) {
                    tc.status = ToolUse::Rejected{now};
                } else {
                    tc.status = ToolUse::Failed{tc.started_at(), now, std::string{reason}};
                }
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
            // Reset the live-rate accumulator so each sub-turn (post-tool)
            // measures its own generation speed instead of polluting the
            // average with the previous turn's bytes.
            m.stream.live_delta_bytes = 0;
            m.stream.first_delta_at = {};
            // Wipe the sparkline ring buffer so each fresh stream starts
            // with an empty bar instead of inheriting the previous turn's
            // tail. The Tick handler refills it as bytes arrive.
            m.stream.rate_history.fill(0.0f);
            m.stream.rate_history_pos = 0;
            m.stream.rate_history_full = false;
            m.stream.rate_last_sample_at = {};
            m.stream.rate_last_sample_bytes = 0;
            return done(std::move(m));
        },
        [&](StreamTextDelta& e) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant) {
                auto& st = m.current.messages.back().streaming_text;
                if (st.size() < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - st.size();
                    if (e.text.size() <= room) st += e.text;
                    else                       st.append(e.text, 0, room);
                }
                // Beyond the cap we silently drop further text — the visible
                // truncation message is appended once on the cap boundary.
            }
            if (!e.text.empty()) {
                if (m.stream.first_delta_at.time_since_epoch().count() == 0)
                    m.stream.first_delta_at = std::chrono::steady_clock::now();
                m.stream.live_delta_bytes += e.text.size();
            }
            return done(std::move(m));
        },
        [&](StreamToolUseStart& e) -> Step {
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant) {
                ToolUse tc;
                tc.id   = e.id;
                tc.name = e.name;
                tc.args = json::object();
                // Stamp start now so the card shows a live timer during the
                // args-streaming phase too, not just during execution —
                // lets the user tell "model hasn't started emitting" from
                // "execution is slow" at a glance.
                tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                m.current.messages.back().tool_calls.push_back(std::move(tc));
            }
            return done(std::move(m));
        },
        [&](StreamToolUseDelta& e) -> Step {
            if (!e.partial_json.empty()) {
                if (m.stream.first_delta_at.time_since_epoch().count() == 0)
                    m.stream.first_delta_at = std::chrono::steady_clock::now();
                m.stream.live_delta_bytes += e.partial_json.size();
            }
            if (!m.current.messages.empty()
                && m.current.messages.back().role == Role::Assistant
                && !m.current.messages.back().tool_calls.empty()) {
                auto& tc = m.current.messages.back().tool_calls.back();
                // Bounded append — see kMaxStreamingBytes. Going past the cap
                // would never produce a parseable JSON object anyway (the
                // unmatched braces grow without bound), so dropping further
                // bytes lets the salvage path at StreamToolUseEnd run on
                // whatever scalars sniffed cleanly.
                if (tc.args_streaming.size() < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - tc.args_streaming.size();
                    if (e.partial_json.size() <= room) tc.args_streaming += e.partial_json;
                    else tc.args_streaming.append(e.partial_json, 0, room);
                }
                // Throttle the live preview. update_stream_preview() closes
                // the partial JSON and parses the entire growing buffer on
                // every call — fine for a 200-byte read/grep, ~quadratic for
                // a multi-KB write `content` body and visible to the user as
                // the card "hanging" while bytes are arriving on the wire.
                // First delta runs unconditionally (so the path/header paints
                // immediately), then we space subsequent re-parses by ~120 ms.
                // StreamToolUseEnd below always runs the full parse, so the
                // final state is exact.
                using clock = std::chrono::steady_clock;
                constexpr auto kPreviewInterval = std::chrono::milliseconds{120};
                auto now = clock::now();
                if (tc.last_preview_at.time_since_epoch().count() == 0
                    || now - tc.last_preview_at >= kPreviewInterval) {
                    update_stream_preview(tc);
                    tc.last_preview_at = now;
                }
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
                // turn-level retry logic owns the single decision point —
                // running it here too would mark the tool Error before
                // finalize_turn gets a chance to silently re-launch the
                // stream on truncation.
            }
            return done(std::move(m));
        },
        [&](StreamUsage& e) -> Step {
            // `input_tokens` from Anthropic is the FULL prefix for this
            // request (system + history + tools + current user message),
            // NOT the delta. Accumulating across turns triple-counted by
            // turn 5 — the ctx bar would silently overshoot 100 % even on
            // a small conversation. Replace, don't add. Also fold in the
            // cache fields: cached read/creation tokens are excluded from
            // `input_tokens` per the API but still occupy the context
            // window, so the true "tokens in context" is the sum.
            if (e.input_tokens || e.cache_read_input_tokens
                || e.cache_creation_input_tokens) {
                m.stream.tokens_in = e.input_tokens
                                   + e.cache_read_input_tokens
                                   + e.cache_creation_input_tokens;
            }
            if (e.output_tokens) m.stream.tokens_out = e.output_tokens;
            return done(std::move(m));
        },
        [&](StreamFinished e) -> Step {
            auto cmd = finalize_turn(m, e.stop_reason);
            return {std::move(m), std::move(cmd)};
        },
        [&](StreamError& e) -> Step {
            m.stream.active = false;
            m.stream.phase = phase::Idle{};
            m.stream.status = "error: " + e.message;
            // Worker thread is unwinding (cleanly cancelled or transport
            // failure); drop the token so the next turn mints a fresh one.
            m.stream.cancel.reset();
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
                // Any tool_call still Pending at stream-error time will never
                // dispatch — no content_block_stop reached us and there's no
                // more stream. Mark them Error so the UI doesn't spin forever
                // on an orphaned "writing…" card. Running tools are in-flight
                // on a worker thread; leave them alone, their ToolExecOutput
                // will still arrive.
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
            if (m.stream.cancel) m.stream.cancel->cancel();
            m.stream.status = "cancelling…";
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
                    if (tc.id == e.id) {
                        if (auto* r = std::get_if<ToolUse::Running>(&tc.status))
                            r->progress_text = std::move(e.snapshot);
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
                        // started_at stays at StreamToolUseStart time — the
                        // card shows total lifetime including the permission
                        // wait, which is exactly what the user cares about.
                        tc.status = ToolUse::Running{tc.started_at(), {}};
                        cmds.push_back(cmd::run_tool(tc.id, tc.name, tc.args));
                    }
            m.pending_permission.reset();
            m.stream.phase = phase::ExecutingTool{};
            m.stream.active = true;  // keep Tick alive for live elapsed counter
            return {std::move(m), Cmd<Msg>::batch(std::move(cmds))};
        },
        [&](PermissionReject) -> Step {
            if (!m.pending_permission) return done(std::move(m));
            auto id = m.pending_permission->id;
            mark_tool_rejected(m, id, "User rejected this tool call.");
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
                // Update the per-model context cap so the status-bar ctx
                // % bar reflects the right denominator for the new model
                // (1 M for `[1m]` variants, 200 K otherwise). Without
                // this, switching to a 1 M model and sending 600 K of
                // input would read as 300 % full.
                m.stream.context_max = ui::context_max_for_model(m.model_id.value);
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
            m.stream.phase = phase::Idle{};
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

            // Sample tok/s into the sparkline ring buffer every ~500 ms
            // while the stream is actively producing bytes. Sampling slower
            // than the spinner tick keeps the bar reading as "trend" rather
            // than "noise"; sampling faster would show every transient
            // edge-batching artifact. Skip until the first delta arrives
            // so the leading bar isn't an artificial zero-stretch.
            if (m.stream.is_streaming() && m.stream.active
                && m.stream.first_delta_at.time_since_epoch().count() != 0) {
                using clock = std::chrono::steady_clock;
                constexpr auto kSampleInterval = std::chrono::milliseconds{500};
                if (m.stream.rate_last_sample_at.time_since_epoch().count() == 0) {
                    m.stream.rate_last_sample_at    = now;
                    m.stream.rate_last_sample_bytes = m.stream.live_delta_bytes;
                } else if (now - m.stream.rate_last_sample_at >= kSampleInterval) {
                    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - m.stream.rate_last_sample_at).count();
                    auto bytes_delta = (m.stream.live_delta_bytes >= m.stream.rate_last_sample_bytes)
                                       ? (m.stream.live_delta_bytes - m.stream.rate_last_sample_bytes)
                                       : 0;
                    // ~4 B/token (Claude tokenizer avg) and convert ms to s.
                    float rate = window_ms > 0
                               ? (static_cast<float>(bytes_delta) / 4.0f)
                                 * (1000.0f / static_cast<float>(window_ms))
                               : 0.0f;
                    m.stream.rate_history[m.stream.rate_history_pos] = rate;
                    m.stream.rate_history_pos =
                        (m.stream.rate_history_pos + 1) % StreamState::kRateSamples;
                    if (m.stream.rate_history_pos == 0) m.stream.rate_history_full = true;
                    m.stream.rate_last_sample_at    = now;
                    m.stream.rate_last_sample_bytes = m.stream.live_delta_bytes;
                }
            }
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
