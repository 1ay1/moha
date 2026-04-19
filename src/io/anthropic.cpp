#include "moha/io/anthropic.hpp"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "moha/tool/registry.hpp"

namespace moha::anthropic {

using json = nlohmann::json;

// Anthropic API version / beta headers. Rotate here — every caller picks them
// up. Keep groups small so a caller can opt in with less scope if needed.
namespace headers {
    inline constexpr const char* version           = "anthropic-version: 2023-06-01";
    inline constexpr const char* beta_oauth_full   =
        "anthropic-beta: oauth-2025-04-20,prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12";
    inline constexpr const char* beta_apikey_full  =
        "anthropic-beta: prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12";
    inline constexpr const char* beta_oauth_only   = "anthropic-beta: oauth-2025-04-20";
    inline constexpr const char* content_type_json = "content-type: application/json";
    inline constexpr const char* accept_sse        = "accept: text/event-stream";
} // namespace headers

namespace {

// --- SSE parser -------------------------------------------------------------
struct SseState {
    // Pre-reserve so typical chunk sizes (CURLOPT_BUFFERSIZE default 16 KB)
    // don't force a cascade of reallocations during a fast stream.
    SseState() { buf.reserve(32 * 1024); data_accum.reserve(8 * 1024); }
    std::string buf;
    std::string event_name;
    std::string data_accum;
};

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);

struct StreamCtx {
    EventSink sink;
    void* cancel; // unused — kept for future cancellation hook
    SseState sse;
    // Tool-use tracking (current block index in-flight)
    std::string current_tool_id;
    std::string current_tool_name;
    bool in_tool_use = false;
    // Terminal-event tracking — exactly one of finished/errored must fire.
    bool terminated = false;
};

void dispatch_event(StreamCtx& ctx, const std::string& name, const std::string& data) {
    if (data.empty() || data == "[DONE]") return;
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
    } else if (name == "message_stop") {
        ctx.sink(StreamFinished{});
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

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    (void)ctx->cancel;
    feed_sse(*ctx, ptr, size * nmemb);
    return size * nmemb;
}

json tool_spec_to_json(const ToolSpec& s) {
    json j;
    j["name"] = s.name;  // std::string, serializes directly
    j["description"] = s.description;
    j["input_schema"] = s.input_schema;
    return j;
}

} // namespace

json build_messages(const Thread& t) {
    json msgs = json::array();
    for (const auto& m : t.messages) {
        json jm;
        jm["role"] = (m.role == Role::User) ? "user" : "assistant";
        json content = json::array();
        if (!m.text.empty()) {
            content.push_back({{"type", "text"}, {"text", m.text}});
        }
        for (const auto& tc : m.tool_calls) {
            if (m.role == Role::Assistant) {
                // Anthropic requires tool_use.input to be an object — coerce
                // null/array/scalar (e.g. from a tool with no args, where no
                // input_json_delta arrived) into an empty object.
                json input = tc.args.is_object() ? tc.args : json::object();
                content.push_back({
                    {"type", "tool_use"},
                    {"id", tc.id.value},
                    {"name", tc.name.value},
                    {"input", std::move(input)},
                });
            }
        }
        // Append tool_result blocks as user messages (Anthropic convention).
        if (!content.empty()) { jm["content"] = content; msgs.push_back(jm); }

        if (m.role == Role::Assistant && !m.tool_calls.empty()) {
            json user_msg;
            user_msg["role"] = "user";
            json results = json::array();
            for (const auto& tc : m.tool_calls) {
                if (tc.status == ToolUse::Status::Done ||
                    tc.status == ToolUse::Status::Error ||
                    tc.status == ToolUse::Status::Rejected) {
                    results.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", tc.id.value},
                        {"content", tc.output.empty() ? "(no output)" : tc.output},
                        {"is_error", tc.status == ToolUse::Status::Error ||
                                     tc.status == ToolUse::Status::Rejected},
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
    // Platform / shell hints so the model generates the right command syntax.
    // Without these, the bash tool gets POSIX-only commands (uname, sw_vers,
    // cat /etc/os-release) on Windows, which fail under cmd.exe.
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
    oss << "You are Moha, a terminal coding assistant based on Claude. "
        << "You work in the user's current directory. "
        << "Use the provided tools to read, edit, and run shell commands when needed. "
        << "Be concise. When proposing file edits, prefer the edit tool over write. "
        << "Use the write tool to create files and edit to modify them — never shell out "
        << "(cat/echo/sed/heredoc) for file IO; one tool call per file change. "
        << "Before running destructive shell commands, explain what you're doing.\n\n"
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

void run_stream_sync(Request req, EventSink sink) {
    if (req.auth_header.empty()) {
        sink(StreamError{"not authenticated — run 'moha login' or set ANTHROPIC_API_KEY"});
        return;
    }
    CURL* curl = curl_easy_init();
    if (!curl) { sink(StreamError{"curl_easy_init failed"}); return; }

    // Emit StreamFinished as a fallback if the SSE stream never produced one
    // (e.g. proxy buffering, server cutoff). UI must not be left spinning.
    auto emit_terminal = [&](StreamCtx& ctx, std::optional<std::string> err) {
        if (ctx.terminated) return;
        if (err) sink(StreamError{*err}); else sink(StreamFinished{});
        ctx.terminated = true;
    };

    const bool is_oauth = (req.auth_style == auth::Style::Bearer);

    json body;
    body["model"] = req.model;  // std::string from Request
    body["max_tokens"] = req.max_tokens;
    body["stream"] = true;

    // For OAuth, prepend a billing-header text block so subscription billing
    // is attributed correctly; the real system prompt follows.
    if (is_oauth) {
        json sys = json::array();
        sys.push_back({
            {"type", "text"},
            {"text", "x-anthropic-billing-header: cc_version=0.1.0;"
                     " cc_entrypoint=cli; cch=00000;"}
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
    std::string body_str = body.dump();

    StreamCtx ctx{sink, nullptr, {}, {}, {}, false, false};

    struct curl_slist* headers = nullptr;
    std::string auth_hdr = is_oauth
        ? (std::string("Authorization: ") + req.auth_header)
        : (std::string("x-api-key: ") + req.auth_header);
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, anthropic::headers::version);
    headers = curl_slist_append(headers,
        is_oauth ? anthropic::headers::beta_oauth_full
                 : anthropic::headers::beta_apikey_full);
    headers = curl_slist_append(headers, anthropic::headers::content_type_json);
    headers = curl_slist_append(headers, anthropic::headers::accept_sse);

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_str.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    // Stall guard: if the stream delivers fewer than 1 byte/sec for 90 s,
    // curl aborts with CURLE_OPERATION_TIMEDOUT instead of hanging forever.
    // Without this a stalled TLS connection (Schannel/Windows has occasional
    // buffering quirks) leaves the UI stuck on "Streaming…".
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);
    // HTTP/1.1 + identity encoding so each SSE event flushes to our callback
    // the moment it arrives, instead of being buffered inside a gzip stream.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    // Write to our callback directly; 0 or low buffer keeps latency down.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);
    auth::apply_tls_options(curl);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        emit_terminal(ctx, std::string("http: ") + curl_easy_strerror(rc));
    } else if (http >= 400) {
        std::string body_err = ctx.sse.buf;
        std::string msg = std::string("HTTP ") + std::to_string(http);
        try {
            auto j = json::parse(body_err);
            if (j.contains("error") && j["error"].contains("message"))
                msg += ": " + j["error"]["message"].get<std::string>();
            else if (j.contains("message"))
                msg += ": " + j["message"].get<std::string>();
            else
                msg += ": " + body_err.substr(0, 300);
        } catch (...) {
            if (!body_err.empty()) msg += ": " + body_err.substr(0, 300);
        }
        if (http == 401 || http == 403)
            msg += "  (run 'moha login' to re-authenticate)";
        emit_terminal(ctx, std::move(msg));
    } else {
        // 2xx and the SSE parser may or may not have produced message_stop.
        // Guarantee one terminal event so the UI can finalize the turn.
        emit_terminal(ctx, std::nullopt);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

namespace {
size_t write_string_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

std::vector<ModelInfo> list_models(const std::string& auth_header,
                                   auth::Style auth_style) {
    std::vector<ModelInfo> result;
    if (auth_header.empty()) return result;

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    const bool is_oauth = (auth_style == auth::Style::Bearer);
    struct curl_slist* headers = nullptr;
    std::string auth_hdr = is_oauth
        ? (std::string("Authorization: ") + auth_header)
        : (std::string("x-api-key: ") + auth_header);
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, anthropic::headers::version);
    if (is_oauth)
        headers = curl_slist_append(headers, anthropic::headers::beta_oauth_only);
    headers = curl_slist_append(headers, anthropic::headers::content_type_json);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/models?limit=100");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    auth::apply_tls_options(curl);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http != 200) return result;

    try {
        auto j = json::parse(response);
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

} // namespace moha::anthropic
