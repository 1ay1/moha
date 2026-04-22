#include "moha/provider/anthropic/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "moha/io/http.hpp"
#include "moha/tool/registry.hpp"

namespace moha::provider::anthropic {

namespace {

// Belt-and-suspenders UTF-8 scrubber. Registry already converts subprocess
// output at the capture boundary (GetConsoleOutputCP / CP_ACP pivot), but
// any string that reaches json::dump() must be valid UTF-8 or the API call
// dies with `type_error.316`. Cheap to run on already-valid strings, and
// guards future call sites that assemble tool output from multiple pieces
// (e.g. error suffix + partial output) where a byte boundary could split a
// UTF-8 sequence. Replaces invalid byte runs with U+FFFD.
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    auto repl = [&]{ out.append("\xEF\xBF\xBD"); };
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        int extra; unsigned char mask; uint32_t min_cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; min_cp = 0x10000; }
        else { repl(); ++i; continue; }
        if (i + (size_t)extra >= in.size()) { repl(); ++i; continue; }
        uint32_t cp = c & mask;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = (unsigned char)in[i + (size_t)k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            repl(); ++i; continue;
        }
        out.append(in.data() + i, (size_t)(extra + 1));
        i += (size_t)extra + 1;
    }
    return out;
}

// Env-var-gated request/SSE dump. Set MOHA_DEBUG_API=1 to write to
// $MOHA_DEBUG_FILE (or ./moha-api.log). Appends, never truncates.
FILE* debug_log() {
    static std::mutex m;
    static FILE* fp = nullptr;
    static bool tried = false;
    std::lock_guard<std::mutex> lk(m);
    if (tried) return fp;
    tried = true;
    const char* on = std::getenv("MOHA_DEBUG_API");
    if (!on || !*on || *on == '0') return nullptr;
    const char* path = std::getenv("MOHA_DEBUG_FILE");
    std::string p = (path && *path) ? std::string{path} : std::string{"moha-api.log"};
    fp = std::fopen(p.c_str(), "ab");
    return fp;
}
void dbg(const char* fmt, ...) {
    FILE* fp = debug_log();
    if (!fp) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fflush(fp);
}
} // namespace

using json = nlohmann::json;

// Wire-format identity. We send byte-for-byte the same headers the official
// Claude Code CLI sends so OAuth tokens minted for "Claude Code" are accepted
// — Anthropic's edge gates oauth-2025-04-20 traffic on the matching x-app /
// user-agent / anthropic-beta combination. Pinned to the CLI version we
// reverse-engineered against (cli.js BUILD_TIME 2026-02-16); refresh when a
// newer release adds beta flags we want to ride.
namespace headers {
    inline constexpr const char* anthropic_version = "2023-06-01";
    inline constexpr const char* claude_cli_version = "2.1.44";
    inline constexpr const char* anth_sdk_version  = "0.74.0";
    inline constexpr const char* node_runtime_ver  = "v22.11.0";
    inline constexpr const char* user_agent        =
        "claude-cli/2.1.44 (external, cli)";
    inline constexpr const char* x_app             = "cli";

    // Beta IDs (literal strings from cli.js line 1336 / line 20). Listed
    // individually so select_betas() can compose the call-site set.
    inline constexpr const char* beta_claude_code         = "claude-code-20250219";
    inline constexpr const char* beta_oauth               = "oauth-2025-04-20";
    inline constexpr const char* beta_interleaved_think   = "interleaved-thinking-2025-05-14";
    inline constexpr const char* beta_prompt_cache_scope  = "prompt-caching-scope-2026-01-05";
    inline constexpr const char* beta_context_1m          = "context-1m-2025-08-07";
} // namespace headers

namespace {

// --- SSE parser -------------------------------------------------------------
struct SseState {
    // Pre-reserve so typical chunk sizes don't force a cascade of reallocations
    // during a fast stream.
    SseState() { buf.reserve(32 * 1024); data_accum.reserve(8 * 1024); }
    std::string buf;
    std::string event_name;
    std::string data_accum;
};

struct StreamCtx {
    EventSink sink;
    SseState sse;
    // Tool-use tracking (current block index in-flight)
    std::string current_tool_id;
    std::string current_tool_name;
    bool in_tool_use = false;
    // Terminal-event tracking — exactly one of finished/errored must fire.
    bool terminated = false;
    // Stashed from message_delta so we can hand it to StreamFinished. Lets
    // the reducer tell "natural end" / "tool_use" apart from "max_tokens"
    // (which leaves the in-flight tool_use block truncated).
    std::string stop_reason;
    // simdjson parser is stateful and caches its scratch buffer across
    // iterate() calls — reusing one per stream avoids a malloc per SSE frame.
    simdjson::ondemand::parser simd_parser;
    simdjson::padded_string     simd_scratch;
};

// Fast path: content_block_delta dominates stream volume (one per output
// token).  simdjson's ondemand walks the bytes in-place, grabs the two
// strings we need, and returns without ever materialising a DOM.  Falls
// back to caller for anything unexpected (unknown delta.type).
// Returns true if the event was fully handled.
bool dispatch_content_block_delta_fast(StreamCtx& ctx, const std::string& data) {
    const std::size_t need = data.size() + simdjson::SIMDJSON_PADDING;
    if (ctx.simd_scratch.size() < need) {
        ctx.simd_scratch = simdjson::padded_string(need);
    }
    std::memcpy(ctx.simd_scratch.data(), data.data(), data.size());
    std::memset(ctx.simd_scratch.data() + data.size(), 0, simdjson::SIMDJSON_PADDING);

    simdjson::ondemand::document doc;
    if (ctx.simd_parser.iterate(ctx.simd_scratch.data(), data.size(), need).get(doc))
        return false;

    simdjson::ondemand::object root;
    if (doc.get_object().get(root)) return false;

    simdjson::ondemand::object delta;
    if (root["delta"].get_object().get(delta)) return false;

    std::string_view delta_type;
    if (delta["type"].get_string().get(delta_type)) return false;

    if (delta_type == "text_delta") {
        std::string_view text;
        if (delta["text"].get_string().get(text)) return false;
        ctx.sink(StreamTextDelta{std::string{text}});
        return true;
    }
    if (delta_type == "input_json_delta") {
        std::string_view partial;
        if (delta["partial_json"].get_string().get(partial)) return false;
        ctx.sink(StreamToolUseDelta{std::string{partial}});
        return true;
    }
    return false;
}

void dispatch_event(StreamCtx& ctx, const std::string& name, const std::string& data) {
    if (data.empty() || data == "[DONE]") return;
    dbg("<< event=%s data=%s\n", name.c_str(), data.c_str());

    // Hot path first — ~95% of events during a streaming turn.
    if (name == "content_block_delta"
        && dispatch_content_block_delta_fast(ctx, data)) {
        return;
    }

    json j;
    try { j = json::parse(data); } catch (...) { return; }
    if (name == "message_start") {
        ctx.sink(StreamStarted{});
        if (j.contains("message") && j["message"].contains("usage")) {
            int in = j["message"]["usage"].value("input_tokens", 0);
            ctx.sink(StreamUsage{in, 0});
        }
    } else if (name == "content_block_start") {
        auto block = j.value("content_block", json::object());
        auto type = block.value("type", "");
        if (type == "tool_use") {
            ctx.current_tool_id = block.value("id", "");
            ctx.current_tool_name = block.value("name", "");
            ctx.in_tool_use = true;
            ctx.sink(StreamToolUseStart{ToolCallId{ctx.current_tool_id}, ToolName{ctx.current_tool_name}});
        }
    } else if (name == "content_block_delta") {
        auto delta = j.value("delta", json::object());
        auto type = delta.value("type", "");
        if (type == "text_delta") {
            ctx.sink(StreamTextDelta{delta.value("text", "")});
        } else if (type == "input_json_delta") {
            ctx.sink(StreamToolUseDelta{delta.value("partial_json", "")});
        }
    } else if (name == "content_block_stop") {
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
    } else if (name == "message_delta") {
        if (j.contains("usage")) {
            int out = j["usage"].value("output_tokens", 0);
            ctx.sink(StreamUsage{0, out});
        }
        if (j.contains("delta") && j["delta"].contains("stop_reason")
            && j["delta"]["stop_reason"].is_string()) {
            ctx.stop_reason = j["delta"]["stop_reason"].get<std::string>();
        }
    } else if (name == "message_stop") {
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
        ctx.sink(StreamFinished{ctx.stop_reason});
        ctx.terminated = true;
    } else if (name == "error") {
        auto err = j.value("error", json::object());
        ctx.sink(StreamError{err.value("message", "unknown error")});
        ctx.terminated = true;
    }
}

void feed_sse(StreamCtx& ctx, const char* data, size_t len) {
    ctx.sse.buf.append(data, len);
    size_t pos = 0;
    while (true) {
        size_t nl = ctx.sse.buf.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = ctx.sse.buf.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (!ctx.sse.data_accum.empty() || !ctx.sse.event_name.empty())
                dispatch_event(ctx, ctx.sse.event_name, ctx.sse.data_accum);
            ctx.sse.event_name.clear();
            ctx.sse.data_accum.clear();
        } else if (line.rfind("event:", 0) == 0) {
            size_t s = 6; while (s < line.size() && line[s] == ' ') ++s;
            ctx.sse.event_name = line.substr(s);
        } else if (line.rfind("data:", 0) == 0) {
            size_t s = 5; while (s < line.size() && line[s] == ' ') ++s;
            if (!ctx.sse.data_accum.empty()) ctx.sse.data_accum += "\n";
            ctx.sse.data_accum += line.substr(s);
        }
    }
    ctx.sse.buf.erase(0, pos);
}

json tool_spec_to_json(const ToolSpec& s) {
    json j;
    j["name"] = s.name;
    j["description"] = s.description;
    j["input_schema"] = s.input_schema;
    return j;
}

// x-stainless-os literal. Mirrors the SDK's normalize-platform table in
// anth-sdk/internal/detect-platform.mjs lines 50-58.
constexpr const char* stainless_os() {
#if defined(__APPLE__)
    return "MacOS";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#elif defined(__OpenBSD__)
    return "OpenBSD";
#else
    return "Other";
#endif
}

constexpr const char* stainless_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x32";
#else
    return "unknown";
#endif
}

// Pick the anthropic-beta value list for /v1/messages exactly the way the
// cli.js gate `y1A(model)` does (cli.js line 1504): drop claude-code on
// haiku models, gate context-1m on `[1m]` model suffix, always send the
// oauth beta on Bearer-auth, and ride prompt-caching-scope by default since
// the latest CLI does. We deliberately omit the statsig-gated / interleaved-
// thinking flags — they're harmless to add but only switch on for first-
// party REPL contexts and we don't have the statsig signal here.
std::string select_betas(std::string_view model, bool is_oauth) {
    std::vector<std::string_view> b;
    const bool is_haiku = model.find("haiku") != std::string_view::npos;
    if (!is_haiku) b.emplace_back(headers::beta_claude_code);
    if (is_oauth)  b.emplace_back(headers::beta_oauth);
    if (model.find("[1m]") != std::string_view::npos)
        b.emplace_back(headers::beta_context_1m);
    b.emplace_back(headers::beta_prompt_cache_scope);
    std::string out;
    for (size_t i = 0; i < b.size(); ++i) {
        if (i) out.push_back(',');
        out.append(b[i]);
    }
    return out;
}

// Build the lowercase HTTP/2 header set in the same order Claude Code lays
// them out. Order isn't semantically required (HTTP doesn't care) but
// matching it makes wireshark dumps line up cleanly during debugging.
http::Headers build_request_headers(bool is_oauth,
                                    const std::string& auth_header,
                                    std::string_view beta_value,
                                    int timeout_seconds) {
    http::Headers h;
    h.push_back({"accept",         "application/json"});
    h.push_back({"content-type",   "application/json"});
    h.push_back({"user-agent",     headers::user_agent});
    h.push_back({"x-app",          headers::x_app});
    h.push_back({"anthropic-version", headers::anthropic_version});
    h.push_back({"anthropic-dangerous-direct-browser-access", "true"});
    if (!beta_value.empty())
        h.push_back({"anthropic-beta", std::string{beta_value}});
    h.push_back({"x-stainless-lang",            "js"});
    h.push_back({"x-stainless-package-version", headers::anth_sdk_version});
    h.push_back({"x-stainless-os",              stainless_os()});
    h.push_back({"x-stainless-arch",            stainless_arch()});
    h.push_back({"x-stainless-runtime",         "node"});
    h.push_back({"x-stainless-runtime-version", headers::node_runtime_ver});
    h.push_back({"x-stainless-retry-count",     "0"});
    h.push_back({"x-stainless-timeout",         std::to_string(timeout_seconds)});
    if (is_oauth) h.push_back({"authorization", auth_header});
    else          h.push_back({"x-api-key",     auth_header});
    return h;
}

// Synthesize a Claude-Code-shaped metadata.user_id. The CLI uses
// `user_<emailHash>_account_<accountUuid>_session_<sessionUuid>`. We don't
// own an Anthropic account UUID, so we derive a stable per-machine hex
// triplet. Anthropic uses this for abuse signals — keeping it stable per
// machine makes our traffic look like a single legit user instead of a
// thundering herd of fresh sessions every turn.
std::string machine_id_hex(int nibbles) {
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string seed;
        for (auto path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
            std::ifstream f(path);
            if (f) { std::getline(f, seed); if (!seed.empty()) break; }
        }
        if (seed.empty()) {
            if (const char* h = std::getenv("HOSTNAME")) seed = h;
        }
        if (seed.empty()) seed = "moha-anonymous";
        // FNV-1a 64-bit, twice with different offsets to pad to 128 bits.
        auto fnv = [](std::string_view s, uint64_t off) {
            uint64_t h = off;
            for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
            return h;
        };
        uint64_t a = fnv(seed, 0xcbf29ce484222325ull);
        uint64_t b = fnv(seed, 0x84222325cbf29ce4ull);
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)a, (unsigned long long)b);
        cached.assign(buf, 32);
    });
    return cached.substr(0, std::min<size_t>(nibbles, cached.size()));
}

std::string make_user_id() {
    // `user_<email-hash:8>_account_<uuid:32>_session_<uuid:32>` — fixed
    // widths so the regex Anthropic's edge applies (if any) accepts it.
    auto email = machine_id_hex(8);
    auto acct  = machine_id_hex(32);
    // Per-process session id: fold high-resolution clock into another FNV.
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t s1 = 0xcbf29ce484222325ull;
    for (int i = 0; i < 8; ++i) { s1 ^= (now >> (i*8)) & 0xff; s1 *= 0x100000001b3ull; }
    uint64_t s2 = s1 ^ 0x9e3779b97f4a7c15ull;
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long)s1, (unsigned long long)s2);
    return "user_" + email + "_account_" + acct + "_session_" + std::string(buf, 32);
}

} // namespace

json build_messages(const Thread& t) {
    json msgs = json::array();
    for (const auto& m : t.messages) {
        json jm;
        jm["role"] = (m.role == Role::User) ? "user" : "assistant";
        json content = json::array();
        if (!m.text.empty()) {
            content.push_back({{"type", "text"}, {"text", scrub_utf8(m.text)}});
        }
        for (const auto& tc : m.tool_calls) {
            if (m.role == Role::Assistant) {
                json input = tc.args.is_object() ? tc.args : json::object();
                content.push_back({
                    {"type", "tool_use"},
                    {"id", tc.id.value},
                    {"name", tc.name.value},
                    {"input", std::move(input)},
                });
            }
        }
        if (!content.empty()) { jm["content"] = content; msgs.push_back(jm); }

        if (m.role == Role::Assistant && !m.tool_calls.empty()) {
            json user_msg;
            user_msg["role"] = "user";
            json results = json::array();
            for (const auto& tc : m.tool_calls) {
                if (tc.is_terminal()) {
                    results.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", tc.id.value},
                        {"content", tc.output().empty()
                            ? std::string{"(no output)"}
                            : scrub_utf8(tc.output())},
                        {"is_error", tc.is_failed() || tc.is_rejected()},
                    });
                }
            }
            if (!results.empty()) {
                user_msg["content"] = results;
                msgs.push_back(user_msg);
            }
        }
    }
    return msgs;
}

std::string default_system_prompt() {
#if defined(_WIN32)
    constexpr const char* os_name  = "Windows";
    constexpr const char* shell    = "cmd.exe (Windows Command Prompt)";
    constexpr const char* shell_hint =
        "Prefer native Windows equivalents: `dir` / `where` / `systeminfo` / "
        "`type` / `findstr` / `powershell -c`. Do NOT use POSIX-only tools "
        "like `uname`, `cat /etc/os-release`, `sw_vers`, `ls`, `grep`, `sed`, "
        "`awk`, or shell heredocs (`<<EOF`) — they will fail. "
        "Commands chain with `&&` and `||` under cmd.exe, but path separators "
        "are backslashes and paths with spaces must be quoted.";
#elif defined(__APPLE__)
    constexpr const char* os_name  = "macOS (Darwin)";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `sw_vers` gives macOS version, `uname -a` gives kernel.";
#else
    constexpr const char* os_name  = "Linux";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `/etc/os-release` gives distro info, `uname -a` gives kernel.";
#endif

    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch (...) {}

    std::ostringstream oss;
    oss << "You are Moha, a terminal coding assistant based on Claude, "
        << "working in the user's current directory like Zed's agent or "
        << "Claude Code. Be concise; let tool cards speak for themselves "
        << "rather than narrating every step.\n\n"
        << "<file-editing>\n"
        << "  - Modify existing files with `edit` (one or more "
        << "old_text→new_text substitutions). It produces a clean diff and "
        << "streams less data than rewriting the whole file.\n"
        << "  - Use `write` ONLY when (a) creating a brand-new file, or "
        << "(b) regenerating an entire file from scratch (format conversion, "
        << "full code generation). Never use `write` to change a few lines "
        << "of an existing file.\n"
        << "  - When editing, `read` the file first if you don't already "
        << "have its current contents — `edit.old_text` must match exactly.\n"
        << "  - NEVER shell out (cat/echo/sed/heredoc/printf) for file IO. "
        << "One `write` or `edit` call per file change.\n"
        << "  - ALWAYS include a brief `display_description` on `write` "
        << "and `edit` calls (e.g. 'Add retry on 429'). It shows in the "
        << "tool card while the file streams, so the user sees what you "
        << "are doing before the bytes finish arriving.\n"
        << "  - Tool inputs stream as JSON. The schemas list `path` first "
        << "for a reason: emit it (and `display_description`) before the "
        << "long fields (`content`, `edits`) so the UI paints meaningful "
        << "context immediately. Don't reorder.\n"
        << "</file-editing>\n\n"
        << "<shell>\n"
        << "  - Use `bash` for commands. Explain destructive ones before "
        << "running.\n"
        << "  - For listing/searching files, prefer the dedicated tools "
        << "(`list_dir`, `glob`, `grep`, `find_definition`) over shelling "
        << "out — they give the UI structured cards.\n"
        << "</shell>\n\n"
        << "<environment>\n"
        << "  os: " << os_name << "\n"
        << "  shell: " << shell << "\n";
    if (!cwd.empty()) oss << "  cwd: " << cwd << "\n";
    oss << "</environment>\n\n"
        << "<shell-notes>\n"
        << shell_hint << "\n"
        << "</shell-notes>\n";
    return oss.str();
}

std::vector<ToolSpec> default_tools() {
    std::vector<ToolSpec> out;
    for (const auto& td : tools::registry()) {
        out.push_back({td.name.value, td.description, td.input_schema});
    }
    return out;
}

// ----------------------------------------------------------------------------

void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    if (req.auth_header.empty()) {
        sink(StreamError{"not authenticated — run 'moha login' or set ANTHROPIC_API_KEY"});
        return;
    }

    auto emit_terminal = [&](StreamCtx& ctx, std::optional<std::string> err) {
        if (ctx.terminated) return;
        if (err) sink(StreamError{*err});
        else     sink(StreamFinished{ctx.stop_reason});
        ctx.terminated = true;
    };

    const bool is_oauth = (req.auth_style == auth::Style::Bearer);

    json body;
    body["model"]      = req.model;
    body["max_tokens"] = req.max_tokens;
    body["stream"]     = true;

    if (is_oauth) {
        // Two text blocks, the second cache-pinned, mirrors how cli.js
        // (line ~5641) lays out the system prompt — first block is the
        // immutable Claude Code preamble; second is the runtime-derived
        // environment text. Cached prefixes survive across turns.
        json sys = json::array();
        sys.push_back({
            {"type", "text"},
            {"text", "You are Claude Code, Anthropic's official CLI for Claude."}
        });
        sys.push_back({
            {"type", "text"},
            {"text", req.system_prompt},
            {"cache_control", {{"type", "ephemeral"}}}
        });
        body["system"] = std::move(sys);
    } else {
        body["system"] = req.system_prompt;
    }

    body["messages"] = build_messages(Thread{ThreadId{""}, "", req.messages, {}, {}});
    if (!req.tools.empty()) {
        json tools_j = json::array();
        for (const auto& t : req.tools) tools_j.push_back(tool_spec_to_json(t));
        body["tools"] = std::move(tools_j);
    }
    body["metadata"] = json{{"user_id", make_user_id()}};
    std::string body_str = body.dump();

    dbg("==== request ====\n%s\n==== /request ====\n", body_str.c_str());

    StreamCtx ctx;
    ctx.sink = std::move(sink);

    http::Request hreq;
    hreq.method  = "POST";
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    // `?beta=true` matches `beta.messages.create` in the SDK (cli.js line 393)
    // — the same path Anthropic's edge gates the beta header set against.
    hreq.path    = "/v1/messages?beta=true";
    hreq.headers = build_request_headers(is_oauth, req.auth_header,
                                         select_betas(req.model, is_oauth),
                                         /*timeout_seconds=*/600);
    hreq.body    = std::move(body_str);

    // We split on HTTP status: 2xx → feed SSE chunks straight to the parser;
    // anything else → buffer the whole body and surface a structured error.
    int  http_status = 0;
    bool is_success  = false;
    std::string error_body;

    http::StreamHandler handler;
    handler.on_headers = [&](int status, const http::Headers& /*hh*/) {
        http_status = status;
        is_success  = (status >= 200 && status < 300);
    };
    handler.on_chunk = [&](std::string_view chunk) -> bool {
        if (is_success) {
            feed_sse(ctx, chunk.data(), chunk.size());
        } else {
            // Cap the buffered error body so a misbehaving edge can't OOM us.
            if (error_body.size() < 64 * 1024)
                error_body.append(chunk.data(),
                                  std::min(chunk.size(), 64 * 1024 - error_body.size()));
        }
        return true;
    };

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(0);  // streaming phase unbounded

    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    dbg("==== http status=%d transport=%s ====\n", http_status,
        result ? "ok" : result.error().c_str());

    if (!result) {
        // Network / TLS / nghttp2-level error — never produced a complete SSE
        // stream. Surface verbatim.
        emit_terminal(ctx, std::string{"http: "} + result.error());
        return;
    }

    if (!is_success) {
        dbg("error body: %s\n", error_body.c_str());
        std::string msg = "HTTP " + std::to_string(http_status);
        try {
            auto j = json::parse(error_body);
            if (j.contains("error") && j["error"].contains("message"))
                msg += ": " + j["error"]["message"].get<std::string>();
            else if (j.contains("message"))
                msg += ": " + j["message"].get<std::string>();
            else
                msg += ": " + error_body.substr(0, 300);
        } catch (...) {
            if (!error_body.empty()) msg += ": " + error_body.substr(0, 300);
        }
        if (http_status == 401 || http_status == 403)
            msg += "  (run 'moha login' to re-authenticate)";
        emit_terminal(ctx, std::move(msg));
        return;
    }

    // 2xx — the SSE parser may or may not have produced message_stop.
    // Guarantee one terminal event so the UI can finalize the turn.
    emit_terminal(ctx, std::nullopt);
}

std::vector<ModelInfo> list_models(const std::string& auth_header,
                                   auth::Style auth_style) {
    std::vector<ModelInfo> result;
    if (auth_header.empty()) return result;

    const bool is_oauth = (auth_style == auth::Style::Bearer);

    http::Request hreq;
    hreq.method  = "GET";
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    hreq.path    = "/v1/models?limit=100";
    // /v1/models doesn't need the streaming beta cocktail — just the oauth
    // gate when applicable, matching how cli.js calls model-listing endpoints.
    hreq.headers = build_request_headers(is_oauth, auth_header,
                                         is_oauth ? headers::beta_oauth : "",
                                         /*timeout_seconds=*/10);

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(5'000);
    tos.total   = std::chrono::milliseconds(10'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp || resp->status != 200) return result;

    try {
        auto j = json::parse(resp->body);
        for (const auto& m : j.value("data", json::array())) {
            auto id = m.value("id", "");
            auto name = m.value("display_name", id);
            if (id.empty()) continue;
            result.push_back(ModelInfo{
                .id = ModelId{id},
                .display_name = name,
                .provider = "anthropic",
            });
        }
    } catch (...) {}

    return result;
}

} // namespace moha::provider::anthropic
