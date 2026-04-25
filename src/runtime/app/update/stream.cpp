// Stream-side helpers for the update reducer: live-preview salvage during
// input_json_delta, partial-JSON closing + truncation guards, and the
// finalize_turn state-machine handoff from Streaming → Idle / Permission /
// ExecutingTool. Kept out of update.cpp so the reducer orchestrator stays
// easy to read.

#include "moha/runtime/app/update/internal.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <span>
#include <utility>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/util/partial_json.hpp"

namespace moha::app::detail {

using json = nlohmann::json;
using moha::tools::util::sniff_string;
using moha::tools::util::sniff_string_progressive;

namespace {

// Keys models sometimes emit in place of our canonical field name. Mirrors
// the ArgReader alias table — keep in sync.
constexpr std::string_view kPathAliases[]    = {"path", "file_path", "filepath", "filename"};
constexpr std::string_view kOldStrAliases[]  = {"old_string", "old_str", "oldStr"};
constexpr std::string_view kNewStrAliases[]  = {"new_string", "new_str", "newStr"};
constexpr std::string_view kContentAliases[] = {"content", "file_text", "text",
                                                 "file_content", "contents",
                                                 "body", "data"};
constexpr std::string_view kDisplayDescription = "display_description";

// Cap on transparent retries per user turn before we give up and surface the
// truncation as a real Error to the model. Two attempts rides out intermittent
// edge idle-timeouts; more would loop on a genuinely broken upstream.
constexpr int kMaxTruncationRetries = 2;

// Hard cap on the live content preview shown during streaming. The widget
// only renders the first ~6 lines of `content` while the model is mid-write;
// re-extracting / re-laying-out a multi-KB body 8x/sec was what made big
// writes "feel" stuck even when bytes were arriving. 4 KiB covers ~50 wide
// lines — far more than the widget shows — and bounds per-tick work.
constexpr std::size_t kStreamingPreviewCap = 4 * 1024;

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
// an object when the result is a parseable object, otherwise nullopt. Strictly
// more capable than the regex sniffer — handles nested objects (edits[].old_text)
// and escaped quotes — but we still fall back to the sniffer for fields the
// partial closer can't yet expose (e.g. when the current field's value is a
// partial string that won't close until later).
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

std::optional<std::string> get_string_any(const json& obj,
                                          std::span<const std::string_view> keys) {
    for (auto k : keys) {
        auto it = obj.find(std::string{k});
        if (it == obj.end()) continue;
        if (it->is_string()) return it->get<std::string>();
    }
    return std::nullopt;
}

// Truncation guard: after the stream parses/salvages tool args, verify the
// minimum fields the target tool actually needs. A common failure mode is the
// wire dropping between `display_description`'s closing `"` and the
// `"content":` that should follow — close_partial_json then strips the
// dangling `,` and produces a well-formed but content-less object. Running
// the tool on that would silently produce an empty file and the model would
// retry on a cryptic "content required" loop.
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
    auto is_nonempty_string = [&](std::string_view key) {
        auto it = args.find(std::string{key});
        return it != args.end() && it->is_string()
            && !it->get_ref<const std::string&>().empty();
    };

    // Closed-set dispatch via spec::Kind. Tools not in the catalog
    // (kind_of returns nullopt) get treated as "no required fields" so
    // an unknown future tool isn't blocked by this guard — the runtime
    // dispatcher will reject the unknown name with a typed error first.
    auto kind = tools::spec::kind_of(tool_name);
    if (!kind) return {};

    using K = tools::spec::Kind;
    switch (*kind) {
        case K::Write:
            if (!is_nonempty_string_any(kPathAliases))    return "path";
            if (!is_nonempty_string_any(kContentAliases)) return "content";
            return {};
        case K::Edit: {
            if (!is_nonempty_string_any(kPathAliases))    return "path";
            auto it = args.find("edits");
            if (it != args.end() && it->is_array() && !it->empty()) return {};
            if (!is_nonempty_string_any(kOldStrAliases))  return "old_string";
            if (!is_nonempty_string_any(kNewStrAliases))  return "new_string";
            return {};
        }
        case K::Bash:
        case K::Diagnostics:
            if (!is_nonempty_string("command")) return "command";
            return {};
        case K::Grep:
            if (!is_nonempty_string("pattern")) return "pattern";
            return {};
        case K::FindDefinition:
            if (!is_nonempty_string("symbol")) return "symbol";
            return {};
        case K::WebFetch:
            if (!is_nonempty_string("url")) return "url";
            return {};
        case K::GitCommit:
            if (!is_nonempty_string("message")) return "message";
            return {};
        // `path` is nice-to-have but not strictly required for these
        // (list_dir/glob default to cwd; read without path is already
        // a tool error — surfacing it from the tool itself preserves
        // the typed ToolError chain instead of converting to a stream-
        // level salvage failure here).
        case K::Read:
        case K::ListDir:
        case K::Glob:
        case K::GitDiff:
        case K::GitLog:
        case K::GitStatus:
        case K::WebSearch:
        case K::Todo:
            return {};
    }
    return {};   // unreachable: switch is exhaustive over Kind
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

void update_stream_preview(ToolUse& tc) {
    // Cheap early-out: if the streaming buffer hasn't grown since the
    // last preview, re-parsing gives identical output. The 120 ms
    // throttle in the caller limits rate; this limits *work* when the
    // model pauses mid-stream (empty deltas, heartbeat gaps) — zero-copy,
    // zero-alloc skip.
    if (tc.args_streaming.size() == tc.stream_sniff_size
        && tc.stream_sniff_size != 0) {
        return;
    }
    tc.stream_sniff_size = tc.args_streaming.size();

    auto set_arg = [&](std::string_view key, std::string v) {
        if (!tc.args.is_object()) tc.args = json::object();
        auto& cur = tc.args[std::string{key}];
        // Cheap "did it change?" — full byte compare on a multi-KB content
        // string was ~half the per-tick cost. Same-size + same-bookend is a
        // very strong signal of "unchanged" during append-only streaming.
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
    // Lazy: try_parse_partial is O(|args_streaming|) per call — on a
    // multi-KB write body, running it every 120 ms is quadratic over
    // the turn. Only the `edit` branch actually needs the structured
    // view (to walk `edits[]`). Every other branch works with sniffer
    // scalars + the cached-offset content decode, and skips this cost
    // entirely. The optional is populated on first access.
    std::optional<json> parsed_cache;
    bool                parsed_attempted = false;
    auto get_parsed = [&]() -> const std::optional<json>& {
        if (!parsed_attempted) {
            parsed_cache = try_parse_partial(tc.args_streaming);
            parsed_attempted = true;
        }
        return parsed_cache;
    };
    auto try_struct = [&](std::string_view canon,
                          std::span<const std::string_view> keys) -> bool {
        const auto& p = get_parsed();
        if (!p) return false;
        if (auto s = get_string_any(*p, keys)) {
            set_arg(canon, *s);
            return true;
        }
        return false;
    };
    auto try_struct_first_edit = [&](std::string_view canon,
                                     std::string_view field) -> bool {
        const auto& p = get_parsed();
        if (!p) return false;
        auto it = p->find("edits");
        if (it == p->end() || !it->is_array() || it->empty()) return false;
        const auto& first = (*it)[0];
        if (!first.is_object()) return false;
        auto f = first.find(std::string{field});
        if (f == first.end() || !f->is_string()) return false;
        set_arg(canon, f->get<std::string>());
        return true;
    };
    auto pull_desc = [&] {
        // display_description is a short scalar field — sniffer is
        // strictly faster than a full parse for it. Skip try_struct
        // so we don't force-materialize `parsed` just for a 40-char
        // description on the hot write/bash/grep paths.
        (void)try_set("display_description", std::span{&kDisplayDescription, 1});
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") {
        // path is a short scalar — sniffer is faster than a full parse.
        try_set("path", kPathAliases);
        pull_desc();
    }
    else if (n == "write") {
        // Write's fast path. General try_struct / try_parse_partial closes
        // and re-parses the *entire* growing args buffer on every tick —
        // fine for tiny tools, quadratic on a multi-KB write body (a 50 KB
        // content field at 8 ticks/sec is 400 KB of O(N) work per second,
        // rising to megabytes by the tail of the stream). Path + desc are
        // located near the head of the buffer and cheap; content uses a
        // *cached* start-of-value offset so each tick only decodes the
        // newly-appended bytes, not the cumulative buffer.
        try_set("path", kPathAliases);
        pull_desc();

        // Locate the opening `"` of content's value ONCE, cache the
        // offset, then on subsequent ticks resume decoding from there.
        // args_streaming grows append-only, so the offset stays valid
        // for the tool call's lifetime. We probe each alias until one
        // hits, then stop probing (stream_sniff_offset != 0).
        if (tc.stream_sniff_offset == 0) {
            for (auto k : kContentAliases) {
                if (auto p = moha::tools::util::locate_string_value(
                        tc.args_streaming, k)) {
                    tc.stream_sniff_offset = *p;
                    break;
                }
            }
        }
        if (tc.stream_sniff_offset != 0) {
            auto v = moha::tools::util::decode_string_from(
                tc.args_streaming, tc.stream_sniff_offset);
            if (!v.empty()) {
                if (v.size() > kStreamingPreviewCap) {
                    // Preview shows the tail — the newest bytes are the
                    // most useful confirmation that data is still flowing.
                    // Build the capped preview in one shot instead of
                    // concat + substr (which allocates the full N-byte
                    // intermediate only to throw it away).
                    std::string tail;
                    tail.reserve(kStreamingPreviewCap + 32);
                    tail.append("\xe2\x80\xa6 (showing tail) \xe2\x80\xa6\n");
                    tail.append(v, v.size() - kStreamingPreviewCap,
                                kStreamingPreviewCap);
                    set_arg("content", std::move(tail));
                } else {
                    set_arg("content", std::move(v));
                }
            }
        }
    }
    else if (n == "edit") {
        // Edit is the only branch that genuinely needs the structured
        // parse — `edits[]` is an array of objects whose fields we want
        // to mirror into tc.args in order. try_struct/try_struct_first_edit
        // will lazily materialize `parsed` on first call.
        if (!try_struct("path", kPathAliases)) try_set("path", kPathAliases);
        pull_desc();
        // Mirror the FULL edits array into tc.args["edits"] as it grows so
        // the card can render every edit during streaming, not just the
        // first. Each entry is a partial object — old_text may be present
        // before new_text starts — we keep them ordered so the renderer's
        // "edit N/M" labels stay stable as more edits land.
        bool wrote_edits_array = false;
        const auto& parsed = get_parsed();
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

json salvage_args(const ToolUse& tc) {
    if (auto parsed = try_parse_partial(tc.args_streaming)) {
        if (!parsed->empty()) return *parsed;
    }
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
    pick("display_description", std::span{&kDisplayDescription, 1});
    return out;
}

maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    using maya::Cmd;
    // Stream is over — drop the cancel handle so a stale Esc can't trip
    // the next turn's stream the moment it launches. Phase transitions
    // below (or in kick_pending_tools) drive whether active() flips off.
    m.s.cancel.reset();
    bool any_truncated = false;
    const bool max_tokens_hit = (stop_reason == StopReason::MaxTokens);
    if (!m.d.current.messages.empty()) {
        auto& last = m.d.current.messages.back();
        if (last.role == Role::Assistant && !last.streaming_text.empty()) {
            if (last.text.empty()) last.text = std::move(last.streaming_text);
            else                   last.text += std::move(last.streaming_text);
            std::string{}.swap(last.streaming_text);
        }
        // Flush any tool_calls whose StreamToolUseEnd never fired — Anthropic
        // normally sends content_block_stop per tool block, but proxies /
        // message_stop cut-offs can skip it.
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

    // Surface a clear error on max_tokens cutoff — retrying would just burn
    // the budget the same way again. Replace the generic guard message with
    // one that names the actual cause.
    if (any_truncated && max_tokens_hit
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        for (auto& tc : m.d.current.messages.back().tool_calls) {
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

    // Transparent retry on upstream truncation — libcurl's TCP keepalive
    // can't prevent an edge LB from closing an idle connection. When that
    // happens mid-tool-input we silently re-launch on the same context,
    // capped at kMaxTruncationRetries.
    if (any_truncated
        && !max_tokens_hit
        && m.s.truncation_retries < kMaxTruncationRetries
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        auto& last = m.d.current.messages.back();
        const bool has_committed_work =
            !last.text.empty() ||
            std::ranges::any_of(last.tool_calls, [](const auto& tc) {
                return tc.is_done() || tc.is_running();
            });
        if (!has_committed_work) {
            ++m.s.truncation_retries;
            m.d.current.messages.pop_back();
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            m.s.phase = phase::Streaming{};
            m.s.status = "retrying (upstream cut off)…";
            return cmd::launch_stream(m);
        }
    }

    deps().save_thread(m.d.current);
    auto kp = cmd::kick_pending_tools(m);

    if (m.s.is_idle() && !m.ui.composer.queued.empty()) {
        m.ui.composer.text = m.ui.composer.queued.front();
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
    }

    // Tier-4 token-saver: if input tokens crossed the auto-compact
    // threshold (default 85 % of context_max), schedule a Haiku
    // summarisation for the next tick. No-op when the threshold isn't
    // met, when a compaction is already in flight, or when the thread
    // is too short to be worth compacting. Returns Cmd::none() in all
    // those cases so the batch below stays cheap.
    if (auto compact_cmd = auto_compact_if_due(m); !compact_cmd.is_none()) {
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
            std::move(kp), std::move(compact_cmd)});
    }
    return kp;
}

} // namespace moha::app::detail
