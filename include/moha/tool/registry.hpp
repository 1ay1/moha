#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/runtime/model.hpp"

namespace moha::tools {

// ── Tool result types (std::expected-based) ──────────────────────────────

struct ToolOutput {
    std::string text;
    std::optional<FileChange> change;
};

// Typed error kind. Lets the UI color / retry / suggest based on category
// rather than string-matching `detail`. Add new variants rather than
// stuffing semantics into the detail message.
enum class ErrorKind : std::uint8_t {
    InvalidArgs,    // schema/validation failure (missing field, empty string, out of range)
    NotFound,       // file/dir/symbol doesn't exist
    NotAFile,       // exists but isn't a regular file
    NotADirectory,  // exists but isn't a directory
    TooLarge,       // input exceeded a size cap (read's 1 MiB, etc.)
    Binary,         // refused to treat a binary file as text
    Ambiguous,      // multiple matches where one was required (edit's old_string)
    NoMatch,        // pattern matched nothing (edit's old_string, grep)
    InvalidRegex,   // regex didn't compile
    Network,        // curl / HTTP transport failure
    Spawn,          // child process failed to start
    Subprocess,     // subprocess returned non-zero
    Io,             // generic I/O (write_file failed, etc.)
    Unknown,        // uncaught exception / unknown tool
};

[[nodiscard]] std::string_view to_string(ErrorKind k) noexcept;

struct ToolError {
    ErrorKind kind = ErrorKind::Unknown;
    std::string detail;

    // Factories read well at call sites:
    //     return std::unexpected(ToolError::not_found(path));
    [[nodiscard]] static ToolError invalid_args(std::string d)    noexcept { return {ErrorKind::InvalidArgs,   std::move(d)}; }
    [[nodiscard]] static ToolError not_found(std::string d)       noexcept { return {ErrorKind::NotFound,      std::move(d)}; }
    [[nodiscard]] static ToolError not_a_file(std::string d)      noexcept { return {ErrorKind::NotAFile,      std::move(d)}; }
    [[nodiscard]] static ToolError not_a_directory(std::string d) noexcept { return {ErrorKind::NotADirectory, std::move(d)}; }
    [[nodiscard]] static ToolError too_large(std::string d)       noexcept { return {ErrorKind::TooLarge,      std::move(d)}; }
    [[nodiscard]] static ToolError binary(std::string d)          noexcept { return {ErrorKind::Binary,        std::move(d)}; }
    [[nodiscard]] static ToolError ambiguous(std::string d)       noexcept { return {ErrorKind::Ambiguous,     std::move(d)}; }
    [[nodiscard]] static ToolError no_match(std::string d)        noexcept { return {ErrorKind::NoMatch,       std::move(d)}; }
    [[nodiscard]] static ToolError invalid_regex(std::string d)   noexcept { return {ErrorKind::InvalidRegex,  std::move(d)}; }
    [[nodiscard]] static ToolError network(std::string d)         noexcept { return {ErrorKind::Network,       std::move(d)}; }
    [[nodiscard]] static ToolError spawn(std::string d)           noexcept { return {ErrorKind::Spawn,         std::move(d)}; }
    [[nodiscard]] static ToolError subprocess(std::string d)      noexcept { return {ErrorKind::Subprocess,    std::move(d)}; }
    [[nodiscard]] static ToolError io(std::string d)              noexcept { return {ErrorKind::Io,            std::move(d)}; }
    [[nodiscard]] static ToolError unknown(std::string d)         noexcept { return {ErrorKind::Unknown,       std::move(d)}; }

    // "[not found] path/to/file" — the UI's default stringification when it
    // doesn't care to branch on kind. Tools that just want the raw message
    // read `.detail` directly.
    [[nodiscard]] std::string render() const;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

// ── Tool definition ──────────────────────────────────────────────────────

struct ToolDef {
    ToolName    name;
    std::string description;
    nlohmann::json input_schema;

    // Anthropic's fine-grained tool streaming flag (GA on Claude 4.6, gated by
    // beta `fine-grained-tool-streaming-2025-05-14` on older models). When set,
    // the API streams `input_json_delta` events token-by-token as the model
    // emits the tool input, instead of buffering and trickling the whole tool
    // input in larger chunks. Decisive for `write` (multi-KB content body):
    // without this, big writes drop from ~60 tok/s to ~1 tok/s as Anthropic's
    // edge holds bytes for batching. CC sets this when `tengu_fgts` statsig
    // is enabled or `CLAUDE_CODE_ENABLE_FINE_GRAINED_TOOL_STREAMING=1`; Zed
    // sets it per-tool that opts in via `supports_input_streaming()`.
    bool eager_input_streaming = false;

    std::function<bool(Profile)> needs_permission;
    std::function<ExecResult(const nlohmann::json& args)> execute;
};

[[nodiscard]] const std::vector<ToolDef>& registry();
[[nodiscard]] const ToolDef* find(std::string_view name);

// ── Live progress sink (thread-local) ────────────────────────────────────
//
// Set by the cmd runner (cmd_factory::run_tool) before dispatching a tool
// and cleared after — bracketed in RAII so exceptions can't leak state
// across tools. Subprocess runners (run_command_s / run_win32_cmdline_s)
// forward each read of the child's stdout+stderr to this sink, which
// ultimately materialises as a ToolExecProgress Msg on the UI thread.
//
// Why thread-local: keeps ToolDef::execute's signature (json -> ExecResult)
// stable; progress is an orthogonal concern of the *outer* cmd runner, not
// of individual tool implementations. A tool that never touches a
// subprocess (e.g. read_file) simply never emits anything.
namespace progress {
    using Sink = std::function<void(std::string_view snapshot)>;
    void set(Sink s);
    void clear();
    // No-op if no sink is installed — cheap enough to call per pipe read.
    void emit(std::string_view snapshot);

    // RAII guard. `set` on construction, `clear` on destruction.
    struct Scope {
        explicit Scope(Sink s) { set(std::move(s)); }
        ~Scope()                { clear(); }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    };
}

} // namespace moha::tools
