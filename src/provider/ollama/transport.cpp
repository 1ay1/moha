// agentty::provider::ollama — native /api/chat transport for a local Ollama
// server. See transport.hpp for the rationale. Modelled on zed-industries/zed
// crates/ollama/src/ollama.rs: NDJSON stream, structured message.tool_calls,
// object-form tool arguments, keep_alive + options.num_predict.
//
// We prefer Ollama's structured tool_calls channel, but weak local models
// (qwen2.5-coder, codellama, etc.) routinely leak tool calls as bare JSON in
// `content` instead. So content is held-then-classified at the START of a
// response: if it parses as a tool-call object/array (or ```json / <tool_call>
// wrapper) naming an advertised tool, we salvage it into a structured call;
// otherwise it flushes as ordinary prose. Once any structured tool_calls frame
// arrives, salvage is disabled (the model can drive the real channel).

#include "agentty/provider/ollama/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/stream_scaffold.hpp"
#include "agentty/provider/usage.hpp"
#include "agentty/provider/msg_shared.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/provider/debug.hpp"
#include "agentty/provider/wire_supersede.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // util::workspace_root/project_root — AGENTS.md anchor + walk start
#include "agentty/util/base64.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::ollama {

using json = nlohmann::json;
using auth::AuthHeader;

namespace {

// ── UTF-8 scrub ─────────────────────────────────────────────────────────────
// A tool output or pasted blob can carry invalid UTF-8; nlohmann's dump()
// throws on it. Replace every malformed byte with U+FFFD so the request
// builds instead of the turn dying with a json type_error. Shared strict
// implementation (rejects overlong encodings + surrogates) — see
// wire::scrub_utf8.
using wire::scrub_utf8;

http::Headers build_request_headers(const AuthHeader& auth) {
    http::Headers h;
    h.push_back({"accept", "application/json"});
    h.push_back({"content-type", "application/json"});
    h.push_back({"user-agent", "agentty/" AGENTTY_VERSION});
    // Ollama needs no auth by default, but a remote / proxied instance may
    // sit behind a bearer key — emit it the same way the OpenAI family does
    // (auth::bearer_token is the single source of truth for extraction).
    if (std::string tok = auth::bearer_token(auth); !tok.empty())
        h.push_back({"authorization", "Bearer " + tok});
    return h;
}

bool is_assistant_with_results(const Message& m) {
    return wire::is_assistant_with_results(m);
}

// ── Stream state ─────────────────────────────────────────────────────────────
struct StreamCtx {
    EventSink   sink;
    // Mirrors req.show_reasoning — gates whether swallowed <think>…</think>
    // text is surfaced as StreamThinkingDelta (visible reasoning) or reduced
    // to a StreamHeartbeat (liveness only; no hidden capture bloating the
    // message + session file). The native `thinking` field is already gated
    // request-side (`think:true` only sent when show_reasoning). Defaults
    // true so the test harnesses keep capture semantics.
    bool        show_reasoning = true;
    // NDJSON line framing (buffer + read cursor + compaction) lives in the
    // shared wire::LineFramer — see include/agentty/provider/wire.hpp. Ollama
    // uses a larger compaction threshold than the SSE default because its
    // native frames (whole message objects) run bigger than SSE deltas.
    wire::LineFramer framer{256 * 1024};
    int         tool_seq = 0; // uniquifies synthesised tool-call ids
    StopReason  stop_reason = StopReason::Unspecified;
    bool        terminated  = false;

    // ── Leaked-tool-call salvage (weak local models) ────────────────────
    // Content frames are held while the response COULD still be a bare
    // tool-call JSON. Once we decide it's prose we flush + stop holding;
    // once a structured tool_calls frame arrives salvage is disabled.
    std::string text_hold;                 // buffered content under suspicion
    bool        holding         = true;    // still buffering potential tool JSON
    bool        salvage_eligible = true;   // can still be a tool call (start)
    bool        any_text_flushed = false;
    bool        any_structured_tool = false;
    int         salvage_seq     = 0;       // uniquifies salvaged call ids
    std::vector<std::string> known_tools;  // tool names we may salvage to
    bool allow_remember_salvage = false;
    bool allow_forget_salvage = false;
    bool allow_wipe_salvage = false;
    std::string full_content;              // every content byte (rescue scan)

    // JSON-protocol mode (agent-zero style, very weak models). No native
    // tools array was sent; the WHOLE response is expected to be one
    // `{"thoughts":[...],"tool_name":"...","tool_args":{...}}` object (or
    // plain prose for a chat-only turn). Enables the forgiving first-`{`..
    // last-`}` extraction in the terminal salvage so leading/trailing
    // narration around the object doesn't defeat the tool call.
    bool        json_protocol   = false;

    // ── Reasoning-model <think> stripping ───────────────────────────────
    // Reasoning models (deepseek-r1, qwen3, …) that are NOT driven through
    // Ollama's native `thinking` field dump their chain-of-thought into
    // `content` wrapped in <think>…</think>. We must NOT emit that as user-
    // visible prose, and a leading <think> block would otherwise defeat the
    // could_be_tool_json hold (it doesn't start with `{`) and flush the
    // reasoning as text ahead of the real tool call. While `in_think` we
    // swallow content until the closing </think>, then resume normal
    // hold/classify on whatever follows.
    bool        in_think        = false;
    bool        think_seen      = false;  // ever entered a think block this turn
    // Partial-tag hold: a <think>/</think> tag can split across NDJSON
    // frames ("<thi" + "nk>"). filter_think holds back a chunk-final byte
    // run that is a proper prefix of a tag and re-prepends it to the next
    // chunk; flushed as literal content at `done` if the tag never completes.
    std::string think_carry;

    // ── Progressive `response` streaming (JSON-protocol chat replies) ───
    // In json_protocol mode a plain chat reply arrives as a grammar-forced
    // object {"tool_name":"response","tool_args":{"text":"…"}} streamed
    // char-by-char. The old path held the WHOLE object and emitted the text
    // in one StreamTextDelta at `done` — so the body appeared and settled in
    // a single frame, giving maya's reveal_fx nothing to animate (no
    // typewriter) and forcing a one-frame freeze handoff (the duplicated
    // body). Instead we detect tool_name=="response" mid-hold, find the
    // value string of the reply field, and stream its DECODED characters
    // incrementally as the wire delivers them — exactly like a native text
    // stream. resp_active gates the path; resp_scan_pos is how far into
    // text_hold we've scanned for the value-string opening; resp_in_value
    // means we're inside the string and resp_value_pos is the index of the
    // next undecoded byte; resp_esc tracks a pending backslash escape.
    bool        resp_active   = false;  // confirmed tool_name=="response"
    bool        resp_done     = false;  // streamed through the closing quote
    bool        resp_in_value = false;  // cursor is inside the value string
    bool        resp_esc      = false;  // last byte was an unconsumed '\\'
    std::size_t resp_scan_pos = 0;      // scan cursor while locating value
    std::size_t resp_value_pos = 0;     // next byte to decode inside value
    std::string resp_emitted;           // decoded text already streamed

};

// ── Salvage helpers (ported from openai/transport.cpp) ──────────────────

// Arg-key repair for weak local models. They routinely emit the RIGHT tool
// with the WRONG argument key — `bash` with {"cmd":"ls"} instead of
// {"command":"ls"}, `read` with {"file":"x"} instead of {"path":"x"},
// `grep` with {"query":"foo"} instead of {"pattern":"foo"}. The tool then
// sees no required arg, errors, and the weak model loops re-emitting the same
// broken call. We remap a small set of well-known aliases to their canonical
// key PER TOOL — but ONLY when the canonical key is absent and the alias is
// present, so a model that got it right is never touched. Conservative: a
// tool not in the table is left exactly as-is.
void repair_arg_keys(const std::string& tool, json& args) {
    if (!args.is_object()) return;
    // (canonical, {aliases...}) per tool. First alias found wins.
    struct Alias { const char* canon; std::vector<const char*> from; };
    auto remap = [&](const std::vector<Alias>& table) {
        for (const auto& a : table) {
            if (args.contains(a.canon)) continue;          // already correct
            for (const char* f : a.from) {
                if (args.contains(f)) {
                    args[a.canon] = args[f];
                    args.erase(f);
                    break;
                }
            }
        }
    };
    // `bash` is kept as a LEGACY ALIAS for `shell`: the tool is registered as
    // `shell`, but the name `bash` is so overwhelmingly common in model
    // training data that a weak local model will emit it — accept it here so
    // its args still get canonicalised (and dispatch resolves it downstream).
    if (tool == "shell" || tool == "bash" || tool == "diagnostics") {
        remap({{"command", {"cmd", "shell", "script", "run", "cmdline"}}});
    } else if (tool == "read" || tool == "list_dir" || tool == "find_definition") {
        remap({{"path", {"file", "filepath", "file_path", "filename", "dir",
                          "directory", "target"}}});
    } else if (tool == "write") {
        remap({{"file_path", {"path", "file", "filepath", "filename", "target"}},
               {"content",   {"text", "body", "data", "contents", "code"}}});
    } else if (tool == "edit") {
        remap({{"path",     {"file", "filepath", "file_path", "filename", "target"}},
               {"old_text", {"old", "old_string", "search", "find", "from"}},
               {"new_text", {"new", "new_string", "replace", "replacement", "to"}}});
    } else if (tool == "grep") {
        remap({{"pattern", {"query", "q", "regex", "search", "text", "term"}},
               {"path",    {"dir", "directory", "root", "file"}}});
    } else if (tool == "glob") {
        remap({{"pattern", {"query", "q", "glob", "pat", "match"}},
               {"path",    {"dir", "directory", "root"}}});
    } else if (tool == "web_fetch") {
        remap({{"url", {"uri", "link", "address", "href"}}});
    } else if (tool == "web_search") {
        remap({{"query", {"q", "search", "term", "text", "prompt"}}});
    } else if (tool == "search_docs") {
        remap({{"query", {"q", "search", "term", "text", "question"}}});
    }
}

// Could `s` be the START of a leaked tool-call? Only meaningful at the start
// of a response (salvage_eligible). Shared classifier — see
// wire::could_be_tool_json (single source of truth; was duplicated here and in
// the openai transport).
using wire::could_be_tool_json;

// Strip <tool_call>…</tool_call> and ```json…``` wrappers + surrounding ws.
[[nodiscard]] std::string_view strip_tool_call_wrappers(std::string_view sv) noexcept {
    auto ltrim = [](std::string_view& s) {
        while (!s.empty() && (s.front()==' '||s.front()=='\t'
                              ||s.front()=='\n'||s.front()=='\r')) s.remove_prefix(1);
    };
    auto rtrim = [](std::string_view& s) {
        while (!s.empty() && (s.back()==' '||s.back()=='\t'
                              ||s.back()=='\n'||s.back()=='\r')) s.remove_suffix(1);
    };
    ltrim(sv); rtrim(sv);
    if (sv.starts_with("<tool_call>")) {
        sv.remove_prefix(std::string_view{"<tool_call>"}.size());
        if (auto p = sv.rfind("</tool_call>"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        ltrim(sv); rtrim(sv);
    }
    if (sv.starts_with("```")) {
        sv.remove_prefix(3);
        // Skip an optional language tag on the fence line (json, sh, bash,
        // tool_code, …) up to the first newline. Weak models wrap a leaked
        // tool call in whatever fence they feel like.
        if (auto nl = sv.find('\n'); nl != std::string_view::npos) {
            std::string_view first = sv.substr(0, nl);
            bool word = !first.empty();
            for (char c : first)
                if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-')) { word=false; break; }
            if (word) sv.remove_prefix(nl + 1);   // drop the lang tag line
        } else if (sv.starts_with("json")) {
            sv.remove_prefix(4);
        }
        ltrim(sv);
        if (auto p = sv.rfind("```"); p != std::string_view::npos)
            sv = sv.substr(0, p);
        rtrim(sv);
    }
    return sv;
}

// Held buffer opens like a tool-call object but doesn't parse (wire cut off),
// or is a wrapper-only remnant / empty `{}` — drop it rather than show garbage.
[[nodiscard]] bool hold_is_truncated_tool_json(std::string_view sv) noexcept {
    std::string_view raw = sv;
    sv = strip_tool_call_wrappers(sv);
    if (sv.empty() && raw != sv) return true;
    if (sv == "json" || sv == "```" || sv == "`") return true;
    if (sv == "{}") return true;
    if (sv.empty() || sv.front() != '{') return false;
    try { auto _ = json::parse(sv); (void)_; return false; }
    catch (...) { return true; }
}

// Complete tool-call-shaped object naming an UNADVERTISED tool — drop it.
[[nodiscard]] bool hold_is_unknown_tool_call(
        std::string_view sv, const std::vector<std::string>& known) noexcept {
    if (known.empty()) return false;
    sv = strip_tool_call_wrappers(sv);
    if (sv.empty() || sv.front() != '{') return false;
    json j;
    try { j = json::parse(sv); } catch (...) { return false; }
    if (!j.is_object()) return false;
    std::string name;
    if (j.contains("name") && j["name"].is_string())
        name = j["name"].get<std::string>();
    else if (j.contains("function") && j["function"].is_string())
        name = j["function"].get<std::string>();
    else if (j.contains("tool_name") && j["tool_name"].is_string())
        name = j["tool_name"].get<std::string>();
    else if (j.contains("tool") && j["tool"].is_string())
        name = j["tool"].get<std::string>();
    else
        return false;
    if (!j.contains("arguments") && !j.contains("parameters")
            && !j.contains("tool_args") && !j.contains("args")) return false;
    // Strip any `tool:action` suffix before the known-tool check.
    if (auto colon = name.find(':'); colon != std::string::npos)
        name = name.substr(0, colon);
    // The `response` pseudo-tool is the JSON-protocol chat escape hatch — it
    // is NOT in known_tools (we never advertise it) but its payload is the
    // user-visible reply, so it must NEVER be dropped here. rescue_json_protocol
    // unwraps its text into prose; report it as "not an unknown tool call".
    if (name == "response") return false;
    for (const auto& t : known) if (t == name) return false;
    return true;
}

// Emit one salvaged tool call from a JSON object. Returns true if consumed
// (emitted OR deliberately swallowed as a never-salvage footgun tool).
//
// Accepts every shape weak models produce for a tool request:
//   • native-ish:   {"name":"bash","arguments":{...}}  (or "function")
//   • agent-zero:   {"tool_name":"bash","tool_args":{...}}
//   • loose:        {"tool":"bash","args":{...}}
//   • action suffix:{"tool_name":"x:run",...} → args.action="run"
// This mirrors agent-zero's normalize_tool_request: the model only has to
// emit SOME recognisable {name, args} object, not the exact native schema.
[[nodiscard]] bool emit_salvaged_tool(StreamCtx& ctx, std::string_view sv) {
    json j;
    try { j = json::parse(sv); } catch (...) { return false; }
    if (!j.is_object()) return false;
    std::string name;
    if (j.contains("name") && j["name"].is_string())
        name = j["name"].get<std::string>();
    else if (j.contains("function") && j["function"].is_string())
        name = j["function"].get<std::string>();
    else if (j.contains("tool_name") && j["tool_name"].is_string())
        name = j["tool_name"].get<std::string>();
    else if (j.contains("tool") && j["tool"].is_string())
        name = j["tool"].get<std::string>();
    else
        return false;

    // `tool_name` may carry a `tool:action` suffix (agent-zero method dispatch).
    // Split it off; the action becomes an arg the tool reads.
    std::string action_suffix;
    if (auto colon = name.find(':'); colon != std::string::npos
            && colon > 0 && colon + 1 < name.size()) {
        action_suffix = name.substr(colon + 1);
        name = name.substr(0, colon);
    }

    // Legacy alias: the tool is registered as `shell`, but `bash` is so
    // common in model training data that weak local models emit it. If the
    // model said `bash` and `shell` is advertised, canonicalise the name so
    // the salvaged call targets the real tool (and downstream sees `shell`).
    if (name == "bash") {
        for (const auto& t : ctx.known_tools)
            if (t == "shell") { name = "shell"; break; }
    }

    bool known = false;
    for (const auto& t : ctx.known_tools) if (t == name) { known = true; break; }
    if (!known) return false;
    // Memory JSON is safe to salvage only for the exact operation the latest
    // user explicitly requested. This preserves the unsolicited-mutation guard
    // while making remember/forget work on models without native tool calls.
    if (name == "remember" && !ctx.allow_remember_salvage) return true;
    if (name == "forget" && !ctx.allow_forget_salvage) return true;
    if (name == "wipe_memory" && !ctx.allow_wipe_salvage) return true;
    if (name == "skill") return true;

    // Arguments under any of the accepted keys; default to {}.
    json args_obj = json::object();
    bool have_args = false;
    auto take_args = [&](const char* key) {
        if (have_args || !j.contains(key)) return;
        const auto& a = j[key];
        if (a.is_string()) {
            try { args_obj = json::parse(a.get<std::string>()); have_args = true; }
            catch (const std::exception& e) { util::dbglog("ollama.salvage_args.parse", e.what()); }
            catch (...) { util::dbglog("ollama.salvage_args.parse", "non-std exception"); }
        } else if (a.is_object()) {
            args_obj = a; have_args = true;
        } else if (!a.is_null()) {
            args_obj = a; have_args = true;
        }
    };
    take_args("arguments");
    take_args("tool_args");
    take_args("args");
    take_args("parameters");
    if (!action_suffix.empty() && args_obj.is_object()
            && !args_obj.contains("action"))
        args_obj["action"] = action_suffix;
    // Repair common wrong arg keys (cmd→command, file→path, query→pattern, …)
    // before the tool sees them — the #1 weak-model loop cause after a leaked
    // call is finally salvaged but names its one required arg wrong.
    repair_arg_keys(name, args_obj);
    std::string args = args_obj.is_object() || args_obj.is_array()
        ? args_obj.dump() : "{}";

    std::string call_id = "call_salvaged_" + std::to_string(ctx.salvage_seq++);
    ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
    ctx.sink(StreamToolUseDelta{ToolCallId{call_id}, args});
    ctx.sink(StreamToolUseEnd{ToolCallId{call_id}});
    ctx.stop_reason = StopReason::ToolUse;
    return true;
}

// Emit salvaged calls from a JSON array [{...},{...}]. Returns true if any.
[[nodiscard]] bool emit_salvaged_tool_array(StreamCtx& ctx, std::string_view sv) {
    json arr;
    try { arr = json::parse(sv); } catch (...) { return false; }
    if (!arr.is_array()) return false;
    bool any = false;
    for (const auto& item : arr)
        if (item.is_object() && emit_salvaged_tool(ctx, item.dump())) any = true;
    return any;
}

// Try to salvage the held content as a tool call (object or array, after
// stripping wrappers). Returns true if a call was emitted/consumed.
[[nodiscard]] bool try_salvage_hold(StreamCtx& ctx) {
    std::string_view inner = strip_tool_call_wrappers(ctx.text_hold);
    if (inner.empty()) return false;
    if (inner.front() == '[') return emit_salvaged_tool_array(ctx, inner);
    if (inner.front() == '{') return emit_salvaged_tool(ctx, inner);
    return false;
}

// Flush held content as prose (dropping leaked/garbage tool JSON) and stop
// holding. Called once we decide the hold is not a salvageable tool call.
void flush_text_hold(StreamCtx& ctx) {
    ctx.holding = false;
    if (ctx.text_hold.empty()) return;
    if (hold_is_truncated_tool_json(ctx.text_hold)) {
        AGT_LOG(Model, Warn, "salvage.dropped_truncated",
                "dialect=ollama bytes={} head={}", ctx.text_hold.size(),
                std::string_view{ctx.text_hold}.substr(0, 200));
        ctx.text_hold.clear(); return;
    }
    if (hold_is_unknown_tool_call(ctx.text_hold, ctx.known_tools)) {
        AGT_LOG(Model, Warn, "salvage.dropped_unknown_tool",
                "dialect=ollama bytes={} head={}", ctx.text_hold.size(),
                std::string_view{ctx.text_hold}.substr(0, 200));
        ctx.text_hold.clear(); return;
    }
    // Whitespace-only hold is not a real reply — leave it for the
    // never-blank-turn net to render an explicit `(empty response)` rather
    // than emitting a blank-looking text delta.
    if (ctx.text_hold.find_first_not_of(" \t\r\n") == std::string::npos) {
        ctx.text_hold.clear(); return;
    }
    ctx.sink(StreamTextDelta{ctx.text_hold});
    ctx.any_text_flushed = true;
    ctx.text_hold.clear();
}

// Last-resort: a weak model narrated prose AND buried a leaked tool call in a
// fenced ```…``` block mid-response (so it was never held at the start). At
// terminal close, if NOTHING was salvaged or structured, scan full_content for
// the FIRST fenced block whose body is an advertised tool-call object and
// salvage it. Conservative: only fires when no other call happened, only on
// fenced blocks, only for advertised tools. Returns true if a call fired.
bool rescue_tool_from_prose(StreamCtx& ctx) {
    if (ctx.any_structured_tool) return false;
    if (ctx.stop_reason == StopReason::ToolUse) return false;  // already salvaged
    std::string_view sv{ctx.full_content};
    std::size_t pos = 0;
    while ((pos = sv.find("```", pos)) != std::string_view::npos) {
        std::size_t open_end = sv.find('\n', pos + 3);
        if (open_end == std::string_view::npos) break;
        std::size_t close = sv.find("```", open_end + 1);
        if (close == std::string_view::npos) break;
        std::string_view body = sv.substr(open_end + 1, close - open_end - 1);
        // trim
        while (!body.empty() && (body.front()==' '||body.front()=='\t'
               ||body.front()=='\n'||body.front()=='\r')) body.remove_prefix(1);
        while (!body.empty() && (body.back()==' '||body.back()=='\t'
               ||body.back()=='\n'||body.back()=='\r')) body.remove_suffix(1);
        if (!body.empty() && (body.front()=='{' || body.front()=='[')) {
            bool ok = body.front()=='['
                ? emit_salvaged_tool_array(ctx, body)
                : emit_salvaged_tool(ctx, body);
            if (ok) return true;
        }
        pos = close + 3;
    }
    return false;
}

// Find the first BALANCED top-level JSON object substring in `sv`, honouring
// string literals + escapes (so a `}` inside a quoted value doesn't close the
// object early). Returns the [first '{' .. matching '}'] span, or empty if no
// balanced object exists. This is agent-zero's extract_json_root_string: weak
// models in JSON-protocol mode wrap their `{tool_name,tool_args}` object in
// stray narration ("Sure! {"tool_name":...}  Let me know!"); pulling the
// balanced object out makes the call survive that noise.
[[nodiscard]] std::string_view extract_first_json_object(std::string_view sv) noexcept {
    std::size_t start = sv.find('{');
    if (start == std::string_view::npos) return {};
    int depth = 0;
    bool in_str = false, esc = false;
    for (std::size_t i = start; i < sv.size(); ++i) {
        char c = sv[i];
        if (in_str) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  in_str = false;
            continue;
        }
        if (c == '"')      in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return sv.substr(start, i - start + 1);
        }
    }
    return {};  // never balanced (wire cut mid-object)
}

// JSON-protocol terminal rescue (very weak models, agent-zero style). The
// whole response was meant to be ONE {thoughts, tool_name, tool_args} object.
// Pull the first balanced object out of full_content and try to emit it as a
// tool call. If it carries a `thoughts`/`headline` field but no usable tool,
// surface those as prose so the user still sees the model's reasoning. Returns
// true if a tool call fired.
bool rescue_json_protocol(StreamCtx& ctx) {
    if (!ctx.json_protocol) return false;
    if (ctx.any_structured_tool) return false;
    if (ctx.stop_reason == StopReason::ToolUse) return false;
    std::string_view obj = extract_first_json_object(ctx.full_content);
    if (obj.empty()) return false;
    json j;
    try { j = json::parse(obj); } catch (...) { return false; }
    if (!j.is_object()) return false;

    // Plain-chat escape hatch: tool_name=="response" means the model chose to
    // talk, not act. Unwrap its text out of tool_args and stream it as prose.
    // (Grammar-forced JSON would otherwise dump a raw object at the user.)
    auto name_of = [&](const char* k) -> std::string {
        return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>()
                                                   : std::string{};
    };
    std::string tname = name_of("tool_name");
    if (tname.empty()) tname = name_of("tool");
    if (tname.empty()) tname = name_of("name");
    if (tname == "response") {
        std::string text;
        const json* args = nullptr;
        if (j.contains("tool_args") && j["tool_args"].is_object()) args = &j["tool_args"];
        else if (j.contains("args") && j["args"].is_object())      args = &j["args"];
        if (args) {
            for (const char* k : {"text", "response", "content", "message", "answer"}) {
                if (args->contains(k) && (*args)[k].is_string()) {
                    text = (*args)[k].get<std::string>();
                    break;
                }
            }
        }
        // Fall back to top-level keys some models use instead of nesting.
        if (text.empty()) {
            for (const char* k : {"response", "content", "message", "answer"}) {
                if (j.contains(k) && j[k].is_string()) { text = j[k].get<std::string>(); break; }
            }
        }
        // Last resort: the model chose `response` but left text empty and put
        // its actual words in `thoughts` (common with grammar-forced output
        // on tiny models). Surface the thoughts as prose — the user must see
        // SOMETHING, never a blank turn (Aider's discipline: never drop a
        // non-empty model reply).
        if (text.empty() && j.contains("thoughts")) {
            const auto& th = j["thoughts"];
            if (th.is_array()) {
                for (const auto& t : th)
                    if (t.is_string()) {
                        if (!text.empty()) text += '\n';
                        text += t.get<std::string>();
                    }
            } else if (th.is_string()) {
                text = th.get<std::string>();
            }
        }
        if (!text.empty()) {
            ctx.sink(StreamTextDelta{text});
            ctx.any_text_flushed = true;
            ctx.stop_reason = StopReason::EndTurn;
        }
        // Consume the held buffer either way: the object was the whole reply,
        // so it must NOT also be flushed as raw-JSON prose by the caller.
        ctx.text_hold.clear();
        ctx.holding = false;
        return false;  // not a tool call; let terminal flush proceed
    }

    // A tool request? emit it.
    const bool has_tool = !tname.empty();
    if (has_tool && emit_salvaged_tool(ctx, obj)) return true;
    return false;
}

// Surface swallowed reasoning text: visible StreamThinkingDelta when the
// user shows reasoning, otherwise a liveness-only StreamHeartbeat (no hidden
// capture — see StreamCtx::show_reasoning).
void emit_reasoning(StreamCtx& ctx, const std::string& text) {
    if (text.empty()) return;
    if (ctx.show_reasoning) ctx.sink(StreamThinkingDelta{text, {}});
    else                    ctx.sink(StreamHeartbeat{});
}

// Final safety net — NEVER let an assistant turn render blank (Aider's
// `if not received_content` discipline). Called at every terminal path after
// salvage/flush. If the turn produced NO text and NO tool call, surface
// whatever the model actually sent so the user always sees a reply:
//   • a JSON-protocol object → its `response`/`text`/`thoughts` payload,
//     stripped of the protocol scaffolding (extract_first_json_object +
//     the same field walk rescue uses);
//   • otherwise the raw captured content, verbatim;
//   • if even that is empty, a clear `(empty response)` marker (Aider's
//     exact placeholder) so the turn isn't a silent void.
void flush_unhandled_content(StreamCtx& ctx) {
    // A partial <think>-tag hold that never completed is literal content —
    // fold it back before the salvage/flush decisions below. (The done-frame
    // path flushes it eagerly; this covers a wire cut before `done`.)
    if (!ctx.think_carry.empty()) {
        std::string tail = std::move(ctx.think_carry);
        ctx.think_carry.clear();
        if (ctx.in_think)      emit_reasoning(ctx, tail);
        else if (ctx.holding)  ctx.text_hold += tail;
        else                   ctx.full_content += tail;
    }
    if (ctx.any_text_flushed) return;
    if (ctx.any_structured_tool) return;
    if (ctx.stop_reason == StopReason::ToolUse) return;

    auto emit = [&](std::string s) {
        // Trim leading/trailing whitespace so a lone newline doesn't count.
        std::size_t a = 0, b = s.size();
        while (a < b && (s[a]==' '||s[a]=='\t'||s[a]=='\n'||s[a]=='\r')) ++a;
        while (b > a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'||s[b-1]=='\r')) --b;
        s = s.substr(a, b - a);
        if (s.empty()) return false;
        ctx.sink(StreamTextDelta{s});
        ctx.any_text_flushed = true;
        if (ctx.stop_reason != StopReason::ToolUse)
            ctx.stop_reason = StopReason::EndTurn;
        return true;
    };

    // Try to pull a human reply out of a JSON-protocol object first so the
    // user sees the words, not the {tool_name:response,...} scaffolding.
    std::string_view obj = extract_first_json_object(ctx.full_content);
    if (!obj.empty()) {
        json j;
        try { j = json::parse(obj); } catch (...) { j = json{}; }
        if (j.is_object()) {
            std::string text;
            const json* args = nullptr;
            if (j.contains("tool_args") && j["tool_args"].is_object()) args = &j["tool_args"];
            else if (j.contains("args") && j["args"].is_object())      args = &j["args"];
            if (args)
                for (const char* k : {"text","response","content","message","answer"})
                    if (args->contains(k) && (*args)[k].is_string())
                        { text = (*args)[k].get<std::string>(); break; }
            if (text.empty())
                for (const char* k : {"response","content","message","answer","text"})
                    if (j.contains(k) && j[k].is_string())
                        { text = j[k].get<std::string>(); break; }
            if (text.empty() && j.contains("thoughts")) {
                const auto& th = j["thoughts"];
                if (th.is_array()) {
                    for (const auto& t : th) if (t.is_string()) {
                        if (!text.empty()) text += '\n';
                        text += t.get<std::string>();
                    }
                } else if (th.is_string()) text = th.get<std::string>();
            }
            if (emit(std::move(text))) return;
        }
    }

    // No JSON payload (or it was empty) — show the raw content verbatim,
    // UNLESS it's a tool-call-shaped object that was deliberately swallowed
    // (a footgun tool like `remember`, an unadvertised tool, or truncated
    // tool JSON). Surfacing that raw would leak `{"name":"remember",…}` at
    // the user — exactly the garbage the salvage path exists to hide.
    std::string_view rc{ctx.full_content};
    const bool rc_blank = rc.find_first_not_of(" \t\r\n") == std::string_view::npos;
    const bool swallowed_tool_json =
        !rc_blank && (hold_is_truncated_tool_json(rc) ||
                      hold_is_unknown_tool_call(rc, ctx.known_tools) ||
                      could_be_tool_json(rc));
    if (!swallowed_tool_json && emit(ctx.full_content)) return;

    // Deliberately-swallowed tool JSON: the turn has no user-facing text by
    // design — stay silent, don't show a placeholder.
    if (swallowed_tool_json) return;

    // Genuinely nothing came back. Mirror Aider's placeholder so the turn is
    // never a silent blank.
    ctx.sink(StreamTextDelta{"(empty response)"});
    ctx.any_text_flushed = true;
    if (ctx.stop_reason != StopReason::ToolUse)
        ctx.stop_reason = StopReason::EndTurn;
}

// Filter reasoning-model <think>…</think> out of a streamed content chunk,
// tracking open/close state across frames in ctx. Returns only the visible
// (non-think) text. The native `thinking` field is the clean path; this is the
// fallback for models that inline their CoT into `content`. We also accept
// <thinking>…</thinking> (some templates use the long form).
[[nodiscard]] std::string filter_think(StreamCtx& ctx, std::string_view chunk) {
    // Re-prepend any held-back partial tag from the previous chunk — tags
    // split across token-sized NDJSON frames ("<thi" + "nk>") and a plain
    // per-chunk find() would leak the fragment as visible prose (or swallow
    // the answer, on a split close tag).
    std::string joined;
    std::string_view s = chunk;
    if (!ctx.think_carry.empty()) {
        joined = std::move(ctx.think_carry);
        ctx.think_carry.clear();
        joined.append(chunk);
        s = joined;
    }
    std::string out;
    out.reserve(s.size());
    // Reasoning swallowed from inside <think>…</think> this chunk — surfaced
    // via StreamThinkingDelta (the SAME unified event the native `thinking`
    // field and every other provider use) instead of being discarded, so
    // inline-CoT local models render reasoning in the shared block too.
    std::string reasoning;
    // Longest chunk-final suffix that is a PROPER prefix of an open/close
    // tag — held back into think_carry instead of emitted. npos = none.
    auto partial_tag_at = [](std::string_view text, bool closing) {
        const std::size_t max_tag = closing ? 11 : 10;  // </thinking> / <thinking>
        const std::size_t lo = text.size() > max_tag ? text.size() - max_tag + 1 : 0;
        for (std::size_t p = lo; p < text.size(); ++p) {
            if (text[p] != '<') continue;
            std::string_view suf = text.substr(p);
            auto is_pref = [&](std::string_view tag) {
                return suf.size() < tag.size()
                    && tag.substr(0, suf.size()) == suf;
            };
            if (closing ? (is_pref("</think>") || is_pref("</thinking>"))
                        : (is_pref("<think>")  || is_pref("<thinking>")))
                return p;
        }
        return std::string_view::npos;
    };
    std::size_t i = 0;
    while (i < s.size()) {
        if (ctx.in_think) {
            // Look for a closing tag.
            std::size_t close = s.find("</think>", i);
            std::size_t close2 = s.find("</thinking>", i);
            std::size_t c = std::min(close, close2);
            if (c == std::string_view::npos) {
                // Still inside think — the remainder is reasoning, minus a
                // possible partial close tag held for the next chunk.
                std::string_view rem = s.substr(i);
                if (auto p = partial_tag_at(rem, /*closing=*/true);
                    p != std::string_view::npos) {
                    ctx.think_carry.assign(rem.substr(p));
                    rem = rem.substr(0, p);
                }
                reasoning.append(rem);
                emit_reasoning(ctx, reasoning);
                return out;
            }
            reasoning.append(s.substr(i, c - i));
            ctx.in_think = false;
            i = c + (c == close ? 8 : 11);
        } else {
            std::size_t open = s.find("<think>", i);
            std::size_t open2 = s.find("<thinking>", i);
            std::size_t o = std::min(open, open2);
            if (o == std::string_view::npos) {
                // No open tag — the remainder is prose, minus a possible
                // partial open tag held for the next chunk.
                std::string_view rem = s.substr(i);
                if (auto p = partial_tag_at(rem, /*closing=*/false);
                    p != std::string_view::npos) {
                    ctx.think_carry.assign(rem.substr(p));
                    rem = rem.substr(0, p);
                }
                out.append(rem);
                break;
            }
            out.append(s.substr(i, o - i));
            ctx.in_think   = true;
            ctx.think_seen = true;
            i = o + (o == open ? 7 : 10);
        }
    }
    emit_reasoning(ctx, reasoning);
    return out;
}

// Progressive `response` streaming for JSON-protocol chat replies. While the
// hold buffer accumulates a grammar-forced {"tool_name":"response",
// "tool_args":{"text":"…"}} object char-by-char, this decodes and emits the
// reply text INCREMENTALLY so maya's reveal_fx animates it like a normal text
// stream — instead of the old behaviour (hold the whole object, emit one giant
// StreamTextDelta at `done`, which gave the typewriter nothing to animate and
// forced a one-frame freeze handoff that duplicated the body in scrollback).
//
// State machine over ctx.text_hold (which only grows):
//   1. Not yet active: scan for `"tool_name"` : `"response"`. Until we SEE the
//      tool_name value we can't commit — the model might be emitting a real
//      tool call. If we positively see a non-"response" tool_name, give up
//      (return false forever) and let the normal salvage path handle it.
//   2. Active: locate the reply field's opening quote (text|response|content|
//      message|answer), then stream decoded characters of that string value
//      until its closing (unescaped) quote, emitting StreamTextDelta per chunk.
//
// Returns true once it has taken ownership of the response (active), so the
// caller stops feeding text_hold to could_be_tool_json / flush. Conservative:
// emits nothing until the value string actually opens, and stops cleanly at
// the closing quote (trailing object bytes `"}}` are ignored).
bool try_progressive_response(StreamCtx& ctx) {
    if (ctx.resp_done) return true;
    std::string_view hold{ctx.text_hold};

    // ── Phase 1: confirm tool_name == "response" ───────────────────
    if (!ctx.resp_active) {
        // Find the tool_name key. Accept "tool_name" / "tool" / "name".
        // On success, resp_scan_pos is set just past the tool_name VALUE so
        // Phase 2's reply-key search never trips over the literal
        // "response" that appears as the tool_name value itself.
        auto find_key_val = [&](std::string_view key) -> int {
            // returns: 1 == response, 0 == other tool (abort), -1 == unknown yet
            std::size_t k = hold.find(key);
            if (k == std::string_view::npos) return -1;
            std::size_t p = k + key.size();
            // skip ws + ':' + ws
            while (p < hold.size() && (hold[p]==' '||hold[p]=='\t'
                   ||hold[p]=='\n'||hold[p]=='\r')) ++p;
            if (p >= hold.size()) return -1;
            if (hold[p] != ':') return -1;   // not the key:value we want
            ++p;
            while (p < hold.size() && (hold[p]==' '||hold[p]=='\t'
                   ||hold[p]=='\n'||hold[p]=='\r')) ++p;
            if (p >= hold.size()) return -1;
            if (hold[p] != '"') return -1;   // value string not open yet
            std::size_t vs = p + 1;
            std::size_t ve = hold.find('"', vs);
            if (ve == std::string_view::npos) return -1;  // value incomplete
            std::string_view val = hold.substr(vs, ve - vs);
            ctx.resp_scan_pos = ve + 1;  // just past the closing value quote
            return val == "response" ? 1 : 0;
        };
        int r = find_key_val("\"tool_name\"");
        if (r == -1) r = find_key_val("\"tool\"");
        if (r == -1) r = find_key_val("\"name\"");
        if (r == 0)  { ctx.resp_done = true; return false; }  // real tool call
        if (r != 1)  return false;                            // undecided
        ctx.resp_active = true;
    }

    // ── Phase 2: locate the reply value string's opening quote ──────────
    // Search only AFTER the tool_name value (resp_scan_pos) so the literal
    // "response" / "name" that is the tool_name value isn't mistaken for the
    // reply key.
    if (!ctx.resp_in_value) {
        std::string_view tail = ctx.resp_scan_pos < hold.size()
            ? hold.substr(ctx.resp_scan_pos) : std::string_view{};
        std::size_t best = std::string_view::npos;
        for (std::string_view key : {"\"text\"", "\"response\"", "\"content\"",
                                     "\"message\"", "\"answer\""}) {
            std::size_t k = tail.find(key);
            if (k == std::string_view::npos) continue;
            std::size_t p = k + key.size();
            while (p < tail.size() && (tail[p]==' '||tail[p]=='\t'
                   ||tail[p]=='\n'||tail[p]=='\r')) ++p;
            if (p >= tail.size() || tail[p] != ':') continue;
            ++p;
            while (p < tail.size() && (tail[p]==' '||tail[p]=='\t'
                   ||tail[p]=='\n'||tail[p]=='\r')) ++p;
            if (p >= tail.size() || tail[p] != '"') continue;  // not open yet
            if (k < best) {
                best = k;
                ctx.resp_value_pos = ctx.resp_scan_pos + p + 1;
            }
        }
        if (best == std::string_view::npos) return true;  // active, value pending
        ctx.resp_in_value = true;
    }

    // ── Phase 3: decode + emit newly-arrived value bytes ────────────────
    // Walk from resp_value_pos honouring JSON string escapes; stop at the
    // first unescaped closing quote. Buffer decoded output and emit it in
    // one StreamTextDelta per call (per wire frame) so the reveal cursor
    // advances smoothly.
    std::string out;
    std::size_t i = ctx.resp_value_pos;
    bool closed = false;
    while (i < hold.size()) {
        char c = hold[i];
        if (ctx.resp_esc) {
            ctx.resp_esc = false;
            switch (c) {
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case '/':  out += '/';  break;
                case '\\': out += '\\'; break;
                case '"':  out += '"';  break;
                case 'u': {
                    // \uXXXX — need 4 more hex digits available. If not all
                    // here yet, leave resp_esc set and stop; resume next frame
                    // once the digits arrive. Cursor stays just past the 'u'.
                    if (i + 4 >= hold.size()) {
                        ctx.resp_esc = true;
                        ctx.resp_value_pos = i;  // re-read 'u' (and digits) next frame
                        // We were mid-escape: i points AT 'u'. Persist and bail.
                        goto done_decode;
                    }
                    auto hexval = [](char h) -> int {
                        if (h>='0'&&h<='9') return h-'0';
                        if (h>='a'&&h<='f') return h-'a'+10;
                        if (h>='A'&&h<='F') return h-'A'+10;
                        return -1;
                    };
                    int h0=hexval(hold[i+1]),h1=hexval(hold[i+2]),
                        h2=hexval(hold[i+3]),h3=hexval(hold[i+4]);
                    if (h0<0||h1<0||h2<0||h3<0) { out += 'u'; break; }
                    unsigned cp = (h0<<12)|(h1<<8)|(h2<<4)|h3;
                    // Encode BMP code point as UTF-8 (surrogate pairs are
                    // rare in chat replies; lone surrogates pass through as
                    // replacement-ish bytes, harmless for display).
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    i += 4;
                    break;
                }
                default:   out += c;    break;
            }
            ++i;
            continue;
        }
        if (c == '\\') {
            // Escape lead. If it's the LAST byte we have, stop here keeping the
            // cursor AT the backslash so the escape completes next frame.
            if (i + 1 >= hold.size()) { ctx.resp_value_pos = i; goto done_decode; }
            ctx.resp_esc = true;
            ++i;
            continue;
        }
        if (c == '"') { closed = true; ++i; break; }
        out += c;
        ++i;
    }
    // Cursor sits past the last fully-decoded byte (or the closing quote).
    ctx.resp_value_pos = i;
done_decode:
    if (!out.empty()) {
        ctx.resp_emitted += out;
        ctx.sink(StreamTextDelta{out});
        ctx.any_text_flushed = true;
    }
    if (closed) {
        ctx.resp_done       = true;
        ctx.holding         = false;
        ctx.salvage_eligible = false;
        ctx.stop_reason     = StopReason::EndTurn;
        ctx.text_hold.clear();
    }
    return true;
}

// Handle one native `message` object from an NDJSON frame.
void handle_message(StreamCtx& ctx, const json& message) {
    // Native reasoning: Ollama routes the model's chain-of-thought into a
    // structured `thinking` field (streamed like content) when we send
    // think:true. Emit it as StreamThinkingDelta — the SAME unified event the
    // Anthropic (thinking_delta), OpenAI-compat (reasoning_content), and
    // ChatGPT/Codex (reasoning_summary) transports emit — so local reasoning
    // renders in the same block as every other provider. No signature on this
    // wire. Doubles as a liveness heartbeat during a long reasoning pause.
    if (auto it = message.find("thinking");
        it != message.end() && it->is_string()) {
        const auto& think = it->get_ref<const std::string&>();
        emit_reasoning(ctx, think);
    }
    // Structured tool calls. Ollama returns them fully-formed in one frame
    // (not streamed char-by-char), so emit Start+Delta+End back to back.
    if (message.contains("tool_calls") && message["tool_calls"].is_array()
        && !message["tool_calls"].empty()) {
        int idx = 0;
        for (const auto& tc : message["tool_calls"]) {
            std::string name, args = "{}";
            std::string call_id;
            if (tc.contains("id") && tc["id"].is_string())
                call_id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    name = fn["name"].get<std::string>();
                if (fn.contains("arguments")) {
                    const auto& a = fn["arguments"];
                    if (a.is_string())     args = a.get<std::string>();
                    else if (!a.is_null()) args = a.dump();
                }
            }
            if (name.empty()) continue;
            if (call_id.empty())
                call_id = "call_ollama_" + std::to_string(ctx.tool_seq++)
                        + "_" + std::to_string(idx);
            ++idx;
            // Repair common wrong arg keys even on the NATIVE channel — a weak
            // model that earned the structured path can still name `command`
            // as `cmd` etc. Only remaps when the canonical key is absent.
            if (!args.empty() && args != "{}") {
                try {
                    json aj = json::parse(args);
                    if (aj.is_object()) {
                        repair_arg_keys(name, aj);
                        args = aj.dump();
                    }
                } catch (...) { /* leave args as-is on parse failure */ }
            }
            ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
            ctx.sink(StreamToolUseDelta{ToolCallId{call_id}, args});
            ctx.sink(StreamToolUseEnd{ToolCallId{call_id}});
            ctx.stop_reason = StopReason::ToolUse;
            ctx.any_structured_tool = true;
            ctx.salvage_eligible    = false;  // model drives the real channel
        }
        // A real structured call arrived — any held content is genuine prose
        // that preceded it; flush it now.
        if (ctx.holding) flush_text_hold(ctx);
    }

    // Assistant prose. Held-and-classified while salvage is still eligible so
    // a leaked tool-call JSON can be recovered instead of dumped as text.
    if (message.contains("content") && message["content"].is_string()) {
        const auto& raw = message["content"].get_ref<const std::string&>();
        if (raw.empty()) return;
        // Strip any inline <think>…</think> reasoning (deepseek-r1, qwen3 when
        // not driven through the native `thinking` field) before it reaches
        // the hold/classify path or the user. Ollama's native `thinking` field
        // (message.thinking) is handled separately and never enters content.
        std::string s = filter_think(ctx, raw);
        if (s.empty()) return;  // entire chunk was inside a think block
        ctx.full_content += s;
        // Progressive `response` already streamed + closed this turn's reply:
        // any further content frames are just the object's trailing structure
        // (`"}}` / pretty-print whitespace). Swallow them — emitting them as
        // text would tack ` }` onto the reply (the trailing-brace leak).
        if (ctx.resp_active && ctx.resp_done) return;
        if (!ctx.holding) {
            ctx.sink(StreamTextDelta{s});
            ctx.any_text_flushed = true;
            return;
        }
        ctx.text_hold += s;
        // JSON-protocol chat reply? Stream the `response` text incrementally
        // as it arrives so reveal_fx animates it (and the freeze handoff is
        // gradual, not a single-frame dump that duplicates the body). Once
        // this takes ownership it owns the rest of the hold; bail out so the
        // bytes aren't ALSO classified/flushed as prose below.
        if (ctx.json_protocol && !ctx.any_structured_tool) {
            if (try_progressive_response(ctx)) return;
        }
        // Still could be a tool call? keep holding until `done` decides.
        if (ctx.salvage_eligible && could_be_tool_json(ctx.text_hold)) return;
        // Decided it's prose — release the buffer and stream the rest live.
        flush_text_hold(ctx);
    }
}

void dispatch_line(StreamCtx& ctx, std::string_view line) {
    if (line.empty()) return;
    json j;
    try { j = json::parse(line); } catch (...) { return; }

    if (j.contains("error")) {
        std::string msg = j["error"].is_string()
            ? j["error"].get<std::string>() : j["error"].dump();
        ctx.sink(StreamError{msg, std::nullopt});
        ctx.terminated = true;
        return;
    }
    if (j.contains("message") && j["message"].is_object())
        handle_message(ctx, j["message"]);

    if (j.value("done", false)) {
        // Flush any held-back partial <think>-tag bytes — the tag never
        // completed, so they are literal content: reasoning if we're still
        // inside a think block, otherwise prose.
        if (!ctx.think_carry.empty()) {
            std::string tail = std::move(ctx.think_carry);
            ctx.think_carry.clear();
            if (ctx.in_think) {
                emit_reasoning(ctx, tail);
            } else if (ctx.holding) {
                ctx.text_hold += tail;
            } else if (!(ctx.resp_active && ctx.resp_done)) {
                ctx.sink(StreamTextDelta{tail});
                ctx.any_text_flushed = true;
            }
        }
        // If the progressive-response streamer already took ownership of this
        // turn's reply (json_protocol chat reply streamed incrementally), the
        // text is already emitted. Don't let rescue_json_protocol re-emit it
        // (that double-streamed the body — the duplication bug). If the value
        // string never closed (wire cut mid-reply), flush whatever decoded
        // bytes are pending and finish.
        if (ctx.resp_active) {
            if (!ctx.resp_done) {
                // Drain any remaining decodable bytes (no closing quote seen).
                try_progressive_response(ctx);
                ctx.resp_done = true;
                ctx.holding   = false;
                ctx.text_hold.clear();
                if (ctx.stop_reason != StopReason::ToolUse)
                    ctx.stop_reason = StopReason::EndTurn;
            }
            if (auto su = usage::from_ollama(j)) ctx.sink(*su);
            flush_unhandled_content(ctx);
            return;
        }
        // JSON-protocol (very weak models): the ENTIRE reply is meant to be
        // one {thoughts, tool_name, tool_args} object. Try to pull a tool
        // call out of the full content FIRST (before flushing the hold as
        // prose) so leading narration like "Sure, I'll do that:" doesn't
        // get emitted ahead of — and visually compete with — the action.
        bool jp_fired = false;
        if (ctx.json_protocol) {
            jp_fired = rescue_json_protocol(ctx);
            if (jp_fired) { ctx.text_hold.clear(); ctx.holding = false; }
        }
        // Final decision on any still-held content: try to salvage it as a
        // leaked tool call, else flush as prose (dropping garbage JSON).
        if (!jp_fired && ctx.holding && !ctx.text_hold.empty()) {
            if (!try_salvage_hold(ctx)) flush_text_hold(ctx);
            else { ctx.text_hold.clear(); ctx.holding = false; }
        } else if (ctx.holding) {
            ctx.holding = false;
        }
        // Mid-prose buried tool call (narration + ```fenced``` JSON).
        rescue_tool_from_prose(ctx);
        // Never-blank-turn net: if nothing was emitted, surface the model's
        // content (or an explicit `(empty response)`).
        flush_unhandled_content(ctx);
        if (auto su = usage::from_ollama(j)) ctx.sink(*su);
        auto reason = j.value("done_reason", std::string{"stop"});
        if (ctx.stop_reason != StopReason::ToolUse)
            ctx.stop_reason = (reason == "length") ? StopReason::MaxTokens
                                                   : StopReason::EndTurn;
    }
}

void feed_ndjson(StreamCtx& ctx, const char* data, std::size_t len) {
    ctx.framer.feed(data, len, [&](std::string_view line) {
        dispatch_line(ctx, line);
    });
}

// ── CLAUDE.md memory tiers (user-authored, concise) ─────────────────────────
// AGENTS.md (AAIF standard, project-scoped) is prepended via
// wire::agents_md_block so the standardized public project guidance appears
// BEFORE the personal CLAUDE.md tiers — same ordering the Anthropic prompt
// uses. Empty/missing AGENTS.md elides the block.
std::string memory_blocks() {
    return wire::agents_md_block(
               "Project guidance following the open AGENTS.md standard "
               "(agents.md, stewarded by the Agentic AI Foundation under "
               "the Linux Foundation). Treat as authoritative public "
               "project conventions.",
               tools::util::workspace_root(),
               tools::util::project_root(),
               wire::resolve_global_agents_md())
         + wire::claude_md_blocks(
               "Project-specific guidance the user has authored. Treat these as "
               "persistent context for THIS workspace and user.");
}

} // namespace

// ── Messages array (native shape) ───────────────────────────────────────────
// Ollama wants tool arguments as a JSON OBJECT (not a serialized string) and
// tool results as role:"tool" with `tool_name`.
//
// `json_protocol`: when true the model was NEVER shown a native `tools` array
// — it was taught (json_protocol_addendum) to emit a `{tool_name,tool_args}`
// JSON object and "wait for its result in the next message." Feeding the
// history back in the NATIVE shape (assistant.tool_calls[] + role:"tool")
// breaks that contract: a tiny model that only knows the prose-JSON protocol
// has no idea what a role:"tool" message is, so on a multi-step task it loses
// the thread, re-issues the call it already ran, or hallucinates a result.
// In JSON-protocol mode we therefore render the round-trip in the model's OWN
// taught vocabulary: the assistant turn becomes the literal
// {thoughts?,tool_name,tool_args} object it emitted, and the result comes back
// as a plain USER message ("TOOL RESULT (name): …"). The loop now closes in
// exactly the shape the system prompt promised.
json build_messages(const std::vector<Message>& msgs, bool json_protocol) {
    json arr = json::array();
    // Recency ranks for the shared age-tiered wire budget (0 = newest tool
    // result). Stale successful dumps fade to a tight head+tail so a big read
    // from many calls ago stops replaying in full — doubly valuable here,
    // where local models have the SMALLEST context windows. Both the
    // JSON-protocol "TOOL RESULT (name):" user turn and the native role:"tool"
    // turn share the same counter. See wire::cap_tool_result_aged.
    int total_tool_results = 0;
    for (const auto& m : msgs)
        if (is_assistant_with_results(m))
            total_tool_results += static_cast<int>(m.tool_calls.size());
    int tool_results_emitted = 0;
    const auto superseded = wire::superseded_read_ids(msgs);
    // Per-side image ceiling for THIS request (see wire::wire_image_count):
    // the many-image rule counts blocks across the whole request, so the cap
    // is derived once here rather than per message.
    const unsigned max_side = util::wire_max_side(wire::wire_image_count(msgs));
    for (const auto& m : msgs) {
        const bool has_text  = !m.text.empty();
        const bool has_tools = is_assistant_with_results(m);
        const bool has_images = wire::has_wire_message_image(m, max_side);

        // ── JSON-protocol round-trip: assistant tool call rendered as the
        //    model's own {tool_name,tool_args} object, result as a user turn.
        if (json_protocol && has_tools) {
            // Prose the assistant emitted alongside the call (rare in JP mode,
            // but keep it so reasoning isn't lost).
            if (has_text) {
                arr.push_back({{"role", "assistant"},
                               {"content", scrub_utf8(m.text)}});
            }
            for (const auto& tc : m.tool_calls) {
                const int recency_rank = total_tool_results - 1 - tool_results_emitted;
                ++tool_results_emitted;
                json obj;
                obj["tool_name"] = tc.name.value;
                obj["tool_args"] = tc.args.is_null() ? json::object() : tc.args;
                // Echo the call back as the assistant's prose-JSON reply.
                arr.push_back({{"role", "assistant"},
                               {"content", scrub_utf8(obj.dump())}});
                // Result as a USER turn the model was taught to read next.
                std::string out = tc.output();
                bool is_error = !tc.is_terminal() || tc.is_failed() || tc.is_rejected();
                if (out.empty()) {
                    if (tc.is_rejected())       out = "(rejected by user)";
                    else if (!tc.is_terminal()) out = "(no output)";
                } else if (!is_error && superseded.count(tc.id.value)) {
                    out = std::string{wire::kSupersededReadPointer};
                } else {
                    out = wire::cap_tool_result_aged(out, recency_rank, is_error);
                }
                std::string body = "TOOL RESULT (" + tc.name.value + "):\n" + out;
                arr.push_back({{"role", "user"},
                               {"content", scrub_utf8(body)}});
            }
            continue;   // handled this message fully
        }

        if (has_text || has_images || has_tools) {
            json msg;
            msg["role"] = (m.role == Role::User) ? "user" : "assistant";
            std::string wire_text = m.attachments.empty()
                ? m.text : attachment::expand(m.text, m.attachments);
            msg["content"] = scrub_utf8(wire_text);
            if (has_images) {
                json imgs = json::array();
                for (const auto* imgp : wire::wire_message_images(m, max_side))
                    imgs.push_back(agentty::util::base64_encode(imgp->bytes));
                if (!imgs.empty()) msg["images"] = std::move(imgs);
            }
            if (has_tools) {
                json calls = json::array();
                for (const auto& tc : m.tool_calls) {
                    calls.push_back({
                        {"id", tc.id.value},
                        {"function", {
                            {"name", tc.name.value},
                            {"arguments", tc.args.is_null() ? json::object()
                                                            : tc.args},
                        }},
                    });
                }
                msg["tool_calls"] = std::move(calls);
            }
            arr.push_back(std::move(msg));
        }
        if (has_tools) {
            for (const auto& tc : m.tool_calls) {
                const int recency_rank = total_tool_results - 1 - tool_results_emitted;
                ++tool_results_emitted;
                std::string out = tc.output();
                bool is_error = !tc.is_terminal() || tc.is_failed() || tc.is_rejected();
                if (out.empty()) {
                    if (tc.is_rejected())       out = "(rejected by user)";
                    else if (!tc.is_terminal()) out = "(no output)";
                } else if (!is_error && superseded.count(tc.id.value)) {
                    out = std::string{wire::kSupersededReadPointer};
                } else {
                    out = wire::cap_tool_result_aged(out, recency_rank, is_error);
                }
                json tool_msg{
                    {"role", "tool"},
                    {"tool_name", tc.name.value},
                    {"content", scrub_utf8(out)},
                };
                // Vision tool_result: ollama's /api/chat accepts an `images`
                // array on ANY message, including a tool result. Same SSOT
                // selector as the other dialects; the base64 array shape is
                // ollama-specific.
                json tool_imgs = json::array();
                for (const auto* imgp : wire::wire_tool_result_images(tc, max_side))
                    tool_imgs.push_back(agentty::util::base64_encode(imgp->bytes));
                if (!tool_imgs.empty()) tool_msg["images"] = std::move(tool_imgs);
                arr.push_back(std::move(tool_msg));
            }
        }
    }
    return arr;
}

namespace {
json build_tools(const std::vector<provider::ToolSpec>& tools) {
    // SSOT: shared with the OpenAI-compat transport — see wire::openai_chat_tools.
    return wire::openai_chat_tools(tools);
}

// JSON-protocol addendum (agent-zero style) for very weak models. Appended to
// the system prompt INSTEAD of sending a native `tools` array. Inlines the
// tool catalog (name + description + parameter names) and pins the exact
// single-object response format the extractor parses. Kept terse — tiny
// models drown in long schemas, so we give names + a one-line param hint, not
// the full JSON Schema.
std::string json_protocol_addendum(const std::vector<provider::ToolSpec>& tools) {
    std::string s;
    s += "\n\n## How to act (IMPORTANT — read carefully)\n";
    s += "You do NOT have a function-calling API. To use a tool you MUST reply "
         "with ONE single JSON object and NOTHING else — no prose before or "
         "after it, no markdown fences. The JSON object has exactly these "
         "fields:\n";
    s += "  - \"thoughts\": array of short strings, your reasoning\n";
    s += "  - \"tool_name\": the EXACT name of one tool from the list below\n";
    s += "  - \"tool_args\": an object of arguments for that tool\n\n";
    s += "Example (run a shell command):\n";
    s += "{\"thoughts\":[\"I need to list files\"],\"tool_name\":\"bash\","
         "\"tool_args\":{\"command\":\"ls -la\"}}\n\n";
    s += "Rules:\n";
    s += "- Output the JSON object ALONE, valid JSON, double quotes.\n";
    s += "- Use ONE tool per reply, then wait for its result in the next "
         "message before the next step.\n";
    s += "- The tool's result comes back as a user message beginning "
         "`TOOL RESULT (toolname):` — read it, then emit your NEXT JSON object "
         "(another tool call, or a \"response\" object when the task is done).\n";
    s += "- `tool_name` must be one of the listed names, never an action verb "
         "like read/write/run.\n";
    s += "- Tools act on LOCAL files and commands only. `read` opens a local "
         "file by path — it does NOT fetch a URL. To get a web page use "
         "`web_fetch` with a `url`; to search the web use `web_search`. Never "
         "pass an http(s):// address to `read`.\n";
    s += "- If a tool result is an ERROR, do NOT re-issue the same call — it "
         "will fail again. Fix the arguments (a path that doesn't exist, or "
         "the wrong tool), or answer the user with what you already have.\n";
    s += "- If you do NOT need a tool (a greeting, or a question you can answer "
         "from the conversation), set \"tool_name\" to \"response\" and put your "
         "reply text in \"tool_args\": {\"text\": \"...\"}.\n\n";
    s += "## Available tools\n";
    for (const auto& t : tools) {
        s += "- " + t.name;
        if (!t.description.empty()) {
            // First sentence / first line of the description keeps it short.
            std::string d = t.description;
            if (auto nl = d.find('\n'); nl != std::string::npos) d = d.substr(0, nl);
            if (d.size() > 160) d = d.substr(0, 160);
            s += ": " + d;
        }
        // Parameter name hints from the JSON Schema's `properties`.
        if (t.input_schema.is_object() && t.input_schema.contains("properties")
                && t.input_schema["properties"].is_object()) {
            std::string params;
            for (auto it = t.input_schema["properties"].begin();
                 it != t.input_schema["properties"].end(); ++it) {
                if (!params.empty()) params += ", ";
                params += it.key();
            }
            if (!params.empty()) s += "  (args: " + params + ")";
        }
        s += "\n";
    }
    return s;
}

// Grammar-constrained decoding schema (Ollama `format` field). Ollama compiles
// a JSON Schema into a GBNF grammar and masks every non-conforming next-token
// during sampling, forcing the model to emit a parseable object matching this
// shape regardless of size. tool_name is an ENUM of the advertised tools plus
// a "response" pseudo-tool (plain-chat escape hatch: the model puts its prose
// in tool_args.text, which rescue_json_protocol unwraps into a text delta).
json json_protocol_schema(const std::vector<provider::ToolSpec>& tools) {
    json tool_names = json::array();
    for (const auto& t : tools) tool_names.push_back(t.name);
    tool_names.push_back("response");

    json thoughts_prop;
    thoughts_prop["type"] = "array";
    thoughts_prop["items"]["type"] = "string";

    json name_prop;
    name_prop["type"] = "string";
    name_prop["enum"] = std::move(tool_names);

    json args_prop;
    args_prop["type"] = "object";

    json props;
    props["thoughts"]  = std::move(thoughts_prop);
    props["tool_name"] = std::move(name_prop);
    props["tool_args"] = std::move(args_prop);

    json schema;
    schema["type"]       = "object";
    schema["properties"] = std::move(props);
    schema["required"]   = json::array({"tool_name", "tool_args"});
    return schema;
}
} // namespace

// Build the Ollama `options` object: num_ctx, num_predict, and sampling. This
// is the single highest-leverage robustness lever for local models. Exposed
// (non-anonymous) so a unit test can assert the chosen values without a
// network round-trip. Reads AGENTTY_OLLAMA_NUM_CTX / _NUM_PREDICT overrides.
json build_options(const Request& req) {
    auto env_int = [](const char* name) -> int {
        if (const char* v = std::getenv(name)) {
            int n = 0;
            for (const char* p = v; *p >= '0' && *p <= '9'; ++p) n = n * 10 + (*p - '0');
            return n;
        }
        return 0;
    };

    // ── num_ctx (context window) ── THE critical option for agents ───────────
    //
    // Ollama's default num_ctx is a tiny 2048 (4096 on newer builds),
    // INDEPENDENT of the model's real window. Since agentty never set it, every
    // request silently truncated to that floor — after a few tool round-trips
    // the system prompt + tool catalog + early conversation scroll off the
    // front and the model "forgets" its tools/instructions mid-session. This is
    // the #1 cause of weak-model agent failure on Ollama.
    //
    // Precedence:
    //   1. AGENTTY_OLLAMA_NUM_CTX env override (power-user escape hatch).
    //   2. The model's real context window probed from /api/show model_info
    //      (req.context_window), clamped to a sane agent ceiling so we don't
    //      try to allocate a 128k-token KV cache on a laptop and OOM the
    //      Ollama server (which would fail the whole request).
    //   3. A safe agent-sized default (8192) when the window is unknown — 4x
    //      Ollama's floor, enough to hold the prompt + a few tool turns,
    //      small enough to load on modest hardware.
    int num_ctx = env_int("AGENTTY_OLLAMA_NUM_CTX");
    if (num_ctx <= 0) {
        constexpr int kAgentCtxFloor   = 8192;    // unknown-window default
        constexpr int kAgentCtxCeiling = 32768;   // don't OOM a local KV cache
        if (req.context_window > 0) {
            num_ctx = req.context_window;
            if (num_ctx < kAgentCtxFloor)   num_ctx = kAgentCtxFloor;
            if (num_ctx > kAgentCtxCeiling) num_ctx = kAgentCtxCeiling;
        } else {
            num_ctx = kAgentCtxFloor;
        }
    }

    // ── num_predict (max output tokens) ────────────────────────────────
    //
    // Ollama's default is a low ~128 which truncates real answers. But blindly
    // sending the hosted-model cap (16384) is also wrong locally: num_predict
    // tokens are reserved OUT OF num_ctx, so a 16k num_predict on an 8k window
    // leaves almost no room for the prompt. Cap num_predict at half the context
    // window so the prompt always has room, with a floor of 2048 (enough for a
    // substantial answer or a tool call's args) and honour an explicit
    // override / the request's own max_tokens as the upper bound.
    int num_predict = env_int("AGENTTY_OLLAMA_NUM_PREDICT");
    if (num_predict <= 0) {
        num_predict = req.max_tokens > 0 ? req.max_tokens : 4096;
        const int half_ctx = num_ctx / 2;
        if (num_predict > half_ctx) num_predict = half_ctx;
        if (num_predict < 2048)     num_predict = std::min(2048, num_ctx);
    }

    json opts;
    opts["num_ctx"]     = num_ctx;
    opts["num_predict"] = num_predict;

    // ── Sampling ── deterministic-leaning for tool-calling reliability ───────
    //
    // Weak models emit far more parseable tool calls at low temperature — high
    // temperature is what makes a 7B model improvise a malformed `{"tool":...}`
    // or wander into prose mid-call. A low (not zero) temperature keeps a
    // little diversity for chat while sharply reducing tool-JSON corruption.
    // Only set when the model is on the weak/json-protocol path; capable models
    // keep their Modelfile defaults. Override via AGENTTY_OLLAMA_TEMPERATURE.
    if (req.json_protocol) {
        opts["temperature"] = 0.2;
        opts["top_p"]       = 0.9;
    }
    if (const char* t = std::getenv("AGENTTY_OLLAMA_TEMPERATURE")) {
        try { opts["temperature"] = std::stod(t); }
        catch (const std::exception& e) { util::dbglog("ollama.env_temperature.parse", e.what()); }
        catch (...) { util::dbglog("ollama.env_temperature.parse", "non-std exception"); }
    }

    return opts;
}

std::string system_prompt() {
    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); }
    catch (const std::exception& e) { util::dbglog("ollama.system_prompt.cwd", e.what()); }
    catch (...) { util::dbglog("ollama.system_prompt.cwd", "non-std exception"); }

#if defined(_WIN32)
    const char* os_name = "Windows";
    const char* shell   = "cmd.exe";
#elif defined(__APPLE__)
    const char* os_name = "macOS";
    const char* shell   = "sh";
#else
    const char* os_name = "Linux";
    const char* shell   = "sh";
#endif

    std::string out;
    out += "You are agentty, a terminal coding assistant. You are helpful, "
           "direct, and act on requests instead of asking which option to "
           "pick. Keep replies concise.\n\n";

    out += "CONVERSATION MEMORY\n"
           "- The full conversation so far is in the messages above. ALWAYS "
           "use earlier messages to answer follow-up questions (names, files, "
           "decisions the user already gave you).\n"
           "- If the user told you a fact earlier (e.g. their name), recall it "
           "from the conversation; never say you don't have it.\n\n";

    out += "TOOLS\n"
           "- Tools let you read/edit files and run commands. Call a tool ONLY "
           "when the task needs it. For greetings, chit-chat, or questions you "
           "can answer from the conversation, reply in plain text \xE2\x80\x94 "
           "do NOT call a tool.\n"
           "- When a task DOES need an action (rename/move/delete a file, run a "
           "shell command, read or edit code), you MUST actually call the tool. "
           "NEVER describe the command in prose or a code block and claim it "
           "ran \xE2\x80\x94 that does nothing. NEVER say a file was created, "
           "renamed, or deleted unless a tool you called returned that result.\n"
           "- To run a shell command (mv, rm, mkdir, git, etc.) call the `bash` "
           "tool with a `command` argument. There is NO `git` or `mv` tool \xE2\x80\x94 "
           "use `bash`. To edit an existing file use `edit`; use `write` only to "
           "create a new file.\n"
           "- Emit tool calls through the tool-call channel, NOT as JSON or a "
           "```code block``` in your reply.\n"
           "- Make ONE tool call at a time and wait for its result. Never "
           "invent a tool result.\n"
           "- Never call remember/forget/wipe_memory unless the user asks you "
           "to remember or forget something.\n"
           "- For questions about the user's OWN docs, manuals, specs, or "
           "notes (anything you can't reliably answer from general knowledge), "
           "call `search_docs` FIRST to retrieve the relevant passages, then "
           "answer from what it returns. Do NOT guess from memory when the "
           "answer should come from their documents.\n\n";

    out += "OUTPUT\n"
           "- Output is rendered as GitHub-flavoured markdown in a terminal. "
           "Use fenced code blocks for code. Keep tables small. LaTeX math "
           "renders too: `$…$` inline, `$$…$$` or a ```math fence for "
           "display (fractions, roots, sums, matrices).\n\n";

    out += "ENVIRONMENT\n";
    out += "- os: ";    out += os_name; out += "\n";
    out += "- shell: "; out += shell;   out += "\n";
    if (!cwd.empty()) { out += "- cwd: "; out += cwd; out += "\n"; }

    out += memory_blocks();
    return out;
}

void set_memory_salvage_intent(StreamCtx& ctx, const Request& req) {
    std::string text;
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
        if (it->role == Role::User && !it->is_proactive_context()) {
            text = it->text;
            break;
        }
    }
    std::ranges::transform(text, text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](std::string_view s) { return text.find(s) != std::string::npos; };
    ctx.allow_remember_salvage = has("remember") || has("don't forget")
        || has("do not forget") || has("keep in mind") || has("from now on");
    ctx.allow_forget_salvage = has("forget") || has("remove the memory")
        || has("remove memory") || has("drop the memory");
    ctx.allow_wipe_salvage = has("wipe memory") || has("wipe your memory")
        || has("forget everything") || has("clean slate");
}

// ── Streaming entry point ────────────────────────────────────────────────────
provider::StreamResult run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    StreamCtx ctx;
    ctx.sink = std::move(sink);
    ctx.show_reasoning = req.show_reasoning;
    set_memory_salvage_intent(ctx, req);
    // Salvage may only synthesise calls to tools we actually advertised.
    ctx.known_tools.reserve(req.tools.size());
    for (const auto& t : req.tools) ctx.known_tools.push_back(t.name);
    ctx.json_protocol = req.json_protocol && !req.tools.empty();
    // No tools advertised → nothing to salvage to; treat all content as prose.
    if (req.tools.empty()) { ctx.holding = false; ctx.salvage_eligible = false; }
    // JSON-protocol: the whole reply is one tool-request object, so always
    // hold + stay salvage-eligible (no native channel will arrive).
    if (ctx.json_protocol) { ctx.holding = true; ctx.salvage_eligible = true; }

    // ── Build /api/chat body ────────────────────────────────────────────────
    json body;
    body["model"]  = req.model;
    body["stream"] = true;
    // Keep the model resident between turns so the next prompt is instant.
    body["keep_alive"] = "10m";
    body["options"] = build_options(req);

    // Native reasoning: ask Ollama to route the model's chain-of-thought into
    // the structured `message.thinking` field (instead of inline
    // <think>…</think> in content) when the user opted into visible reasoning
    // (^R) AND we're not in JSON-protocol mode (weak models can't reason and
    // the grammar would fight the think field). Ollama ignores `think` on
    // models that don't support it, so this is safe to send broadly. The
    // stream handler emits message.thinking as StreamThinkingDelta — the SAME
    // unified event every other provider uses — so local reasoning renders in
    // the same block. Off by default keeps the dead-air-free wire unchanged.
    // Native thinking. Ollama's `think` accepts a BOOLEAN or a graded level
    // ("low"|"medium"|"high", newer builds also "max"); GPT-OSS in
    // particular REQUIRES a level to tune trace length. Map the model-
    // agnostic effort tier onto it so local reasoning models honour the
    // picker's ladder exactly like hosted ones:
    //   effort set  → think:"<level>"  (minimal folds to low — ollama
    //                 rejects "minimal"; xhigh/max fold to "high" since
    //                 "max" is not yet universal — see ollama#15831)
    //   effort off  → think:true only when the user shows reasoning
    //                 (visible thinking without a tier), else omitted.
    // Ollama ignores `think` on models that can't reason, so sending it
    // broadly is safe. Gated off in JSON-protocol mode (weak models; the
    // grammar would fight the think field). The stream handler emits
    // message.thinking as StreamThinkingDelta — the SAME unified event
    // every other provider uses.
    if (!ctx.json_protocol) {
        if (!req.effort.empty()) {
            std::string level = req.effort;
            if (level == "minimal")                    level = "low";
            else if (level == "xhigh" || level == "max") level = "high";
            body["think"] = level;
        } else if (req.show_reasoning) {
            body["think"] = true;
        }
    }

    json messages = json::array();
    std::string sys = req.system_prompt;
    // JSON-protocol: append the tool catalog + single-object response spec to
    // the system prompt, and do NOT send a native `tools` array below.
    if (ctx.json_protocol)
        sys += json_protocol_addendum(req.tools);
    if (!sys.empty())
        messages.push_back({{"role", "system"},
                            {"content", scrub_utf8(sys)}});
    for (auto& m : build_messages(req.messages, ctx.json_protocol))
        messages.push_back(std::move(m));
    body["messages"] = std::move(messages);

    // Native tools array ONLY when not in JSON-protocol mode. Weak models get
    // the inline catalog instead (the native schema confuses them).
    if (!req.tools.empty() && !ctx.json_protocol)
        body["tools"] = build_tools(req.tools);

    // Grammar-constrained decoding for weak models: a JSON Schema in `format`
    // makes Ollama compile a GBNF grammar and mask every non-conforming
    // next-token while sampling — the reply is FORCED into a parseable
    // {thoughts, tool_name, tool_args} object regardless of model size. The
    // salvage/rescue path below stays as a backstop for a truncated stream
    // (Ollama constrains per-token but does not validate the full response).
    if (ctx.json_protocol)
        body["format"] = json_protocol_schema(req.tools);

    std::string body_str;
    try {
        body_str = body.dump();
    } catch (const nlohmann::json::exception& e) {
        ctx.sink(StreamError{std::string{"request build failed (invalid UTF-8): "}
                             + e.what()});
        ctx.sink(StreamFinished{StopReason::Unspecified});
        return provider::StreamResult::failed("request build failed: invalid UTF-8");
    }

    // ── HTTP request ─────────────────────────────────────────────────────────
    http::Request hreq;
    hreq.method    = http::HttpMethod::Post;
    hreq.host      = req.endpoint.host;
    hreq.port      = req.endpoint.port;
    hreq.path      = "/api/chat";
    hreq.plaintext = !req.endpoint.use_tls;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.headers = build_request_headers(req.auth);
    hreq.body    = std::move(body_str);

    // Shared scaffold: status/Retry-After capture, heartbeats, buffered-wait
    // (previously MISSING here — buffered sends showed as dead air), wire
    // dump, capped error body. Only the NDJSON feed is ours.
    provider::StreamScaffold sc;
    sc.dialect = "ollama-native";
    sc.sink    = ctx.sink;
    sc.feed    = [&](std::string_view chunk) {
        feed_ndjson(ctx, chunk.data(), chunk.size());
        return true;
    };
    http::StreamHandler handler = sc.handler();

    // Standard ladder with the two local-server deviations: idle=120 s (local
    // generation is slow between tokens) and total=0/unbounded (a big model
    // can legitimately stream for longer than any hosted cap).
    http::Timeouts tos = provider::stream_timeouts(
        std::chrono::milliseconds(120'000));
    tos.total = std::chrono::milliseconds(0);

    // Keep a copy of the cancel token: moved into the stream call, but
    // finish_stream needs it to distinguish a user cancel from a transport
    // error at the post-loop.
    http::CancelTokenPtr cancel_for_end = cancel;
    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    // Uniform end-of-turn pair via the scaffold — this transport previously
    // logged NOTHING at stream end (drift vs every sibling).
    sc.log_result(bool(result),
                  result ? std::string_view{} : result.error().render());

    // Whole post-loop through the SHARED epilogue: one terminal event, correct
    // precedence, identical to every other provider. before_finish (success
    // only) does Ollama's terminal salvage/flush — the stream may end without a
    // `done` frame (wire cut) while content is still held, so drain/rescue it.
    return provider::finish_stream({
        .terminated  = ctx.terminated,
        .sink        = ctx.sink,
        .result_ok   = bool(result),
        .http_status = sc.http_status,
        .non_replayable = !result && result.error().non_replayable,
        .cancel      = cancel_for_end,
        .stop        = ctx.stop_reason,
        .http_error_message = [&]() -> std::string {
            std::string msg = "HTTP " + std::to_string(sc.http_status);
            try {
                auto j = json::parse(sc.error_body);
                if (j.contains("error") && j["error"].is_string())
                    msg += ": " + j["error"].get<std::string>();
                else if (!sc.error_body.empty())
                    msg += ": " + sc.error_body.substr(0, 300);
            } catch (...) {
                if (!sc.error_body.empty()) msg += ": " + sc.error_body.substr(0, 300);
            }
            if (sc.http_status == 404)
                msg += "  (model not loaded — run 'ollama pull " + req.model + "')";
            return msg;
        },
        .retry_after = sc.retry_after_hint,
        .transport_error_message = [&]() -> std::string {
            std::string msg = std::string{"http: "} + result.error().render();
            msg += "  (is Ollama running? start it with 'ollama serve', or check "
                   "--provider host:port)";
            return msg;
        },
        .before_finish = [&ctx]() {
            // Progressive-response already owned the turn — just drain any
            // pending decoded bytes; never re-run rescue (would duplicate).
            if (ctx.resp_active) {
                if (!ctx.resp_done) {
                    try_progressive_response(ctx);
                    ctx.resp_done = true; ctx.holding = false; ctx.text_hold.clear();
                    if (ctx.stop_reason != StopReason::ToolUse)
                        ctx.stop_reason = StopReason::EndTurn;
                }
            } else {
                bool jp_fired = false;
                if (ctx.json_protocol) {
                    jp_fired = rescue_json_protocol(ctx);
                    if (jp_fired) { ctx.text_hold.clear(); ctx.holding = false; }
                }
                if (!jp_fired && ctx.holding && !ctx.text_hold.empty()) {
                    if (!try_salvage_hold(ctx)) flush_text_hold(ctx);
                    else { ctx.text_hold.clear(); ctx.holding = false; }
                }
                rescue_tool_from_prose(ctx);
            }
            flush_unhandled_content(ctx);
        },
    });
}

std::vector<Msg> parse_ndjson_for_test(std::string_view ndjson_bytes,
                                       std::vector<std::string> known_tools,
                                       bool json_protocol,
                                       bool allow_memory_salvage) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.sink = [&out](Msg m) { out.push_back(std::move(m)); };
    ctx.known_tools = std::move(known_tools);
    ctx.allow_remember_salvage = allow_memory_salvage;
    ctx.allow_forget_salvage = allow_memory_salvage;
    ctx.allow_wipe_salvage = allow_memory_salvage;
    ctx.json_protocol = json_protocol && !ctx.known_tools.empty();
    if (ctx.known_tools.empty()) { ctx.holding = false; ctx.salvage_eligible = false; }
    if (ctx.json_protocol) { ctx.holding = true; ctx.salvage_eligible = true; }
    feed_ndjson(ctx, ndjson_bytes.data(), ndjson_bytes.size());
    // Mirror run_stream_sync's terminal flush so tests see the live sequence.
    if (!ctx.terminated) {
        if (ctx.resp_active) {
            if (!ctx.resp_done) {
                try_progressive_response(ctx);
                ctx.resp_done = true; ctx.holding = false; ctx.text_hold.clear();
                if (ctx.stop_reason != StopReason::ToolUse)
                    ctx.stop_reason = StopReason::EndTurn;
            }
        } else {
            bool jp_fired = false;
            if (ctx.json_protocol) {
                jp_fired = rescue_json_protocol(ctx);
                if (jp_fired) { ctx.text_hold.clear(); ctx.holding = false; }
            }
            if (!jp_fired && ctx.holding && !ctx.text_hold.empty()) {
                if (!try_salvage_hold(ctx)) flush_text_hold(ctx);
                else { ctx.text_hold.clear(); ctx.holding = false; }
            }
            rescue_tool_from_prose(ctx);
        }
        flush_unhandled_content(ctx);
        ctx.sink(StreamFinished{ctx.stop_reason});
    }
    return out;
}

} // namespace agentty::provider::ollama
