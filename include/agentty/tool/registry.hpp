#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/tool/effects.hpp"

namespace agentty::tools {

// ── Tool result types (std::expected-based) ──────────────────────────────

struct ToolOutput {
    std::string text;
    std::optional<FileChange> change;
    // Additional file changes for MULTI-FILE tools (replace); `change` above is
    // the single-file case. Both feed the diff-review queue.
    std::vector<FileChange> changes;
    // Images a tool surfaced for a vision model (read on an image file). These
    // become image blocks in the turn's tool_result; empty for text tools.
    std::vector<ImageContent> images;
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
    OutOfWorkspace, // path is outside the configured workspace root
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
    [[nodiscard]] static ToolError out_of_workspace(std::string d) noexcept { return {ErrorKind::OutOfWorkspace, std::move(d)}; }
    [[nodiscard]] static ToolError unknown(std::string d)         noexcept { return {ErrorKind::Unknown,       std::move(d)}; }

    // "[not found] path/to/file" — the UI's default stringification when it
    // doesn't care to branch on kind. Tools that just want the raw message
    // read `.detail` directly.
    [[nodiscard]] std::string render() const;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

// ── Tool definition ──────────────────────────────────────────────────────

enum class ToolOrigin : std::uint8_t { Native, Mcp, Passthrough };
enum class OutputTruncation : std::uint8_t { Head, Tail, HeadTail };

struct ToolDef {
    ToolName    name;
    std::string description;
    nlohmann::json input_schema;

    // Stable provenance. Dynamic MCP tools are never allowed to masquerade as
    // native tools; the origin also drives catalog replacement and UX labels.
    ToolOrigin origin = ToolOrigin::Native;
    std::string origin_id; // empty for native; configured MCP server id otherwise

    // Runtime metadata lives on ToolDef so dynamic tools receive the same
    // safety/performance treatment as compile-time native tools.
    EffectSet scheduling_effects;
    int max_output_chars = 30'000;
    OutputTruncation output_truncation = OutputTruncation::HeadTail;
    std::chrono::milliseconds timeout{60'000};
    bool always_expose = false; // native, pinned, or MCP catalog discovery

    // Advertise vs dispatch — two different sets, officially. `advertise =
    // false` keeps the tool OUT of the wire request's tools array while
    // dispatch (find/execute) still resolves it. The consumer is
    // passthrough tools fulfilling a PROXY-advertised schema (LiteLLM +
    // headroom injects `headroom_retrieve` into our request in its
    // pre-call hook): the proxy owns advertisement, agentty owns
    // execution — advertising it ourselves would double the schema. Every
    // other tool keeps the default and nothing changes.
    bool advertise = true;

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

    // Capability tags describing the tool's observable impact on the
    // world. The permission policy reads these and these alone — there
    // is no per-tool override. Set this when constructing the ToolDef
    // (e.g. `t.effects = {Effect::ReadFs};`); leaving it default
    // (empty) means "this tool is pure and never needs permission".
    EffectSet effects;

    std::function<ExecResult(const nlohmann::json& args)> execute;
};

[[nodiscard]] const std::vector<ToolDef>& native_registry();
[[nodiscard]] const std::vector<ToolDef>& registry();

// Race-safe value copy of the current wire catalog. Unlike registry() /
// wire_tools() — which return a reference into cache-owned memory that a
// concurrent reload_mcp_plugins() (background thread) can swap out from
// under the caller — this returns an independent snapshot the caller owns.
// Use this from any path that might run while a plugin reload is in flight
// (e.g. the settings picker reading the catalog to list plugin tools).
[[nodiscard]] std::vector<ToolDef> wire_tools_snapshot();
[[nodiscard]] const ToolDef* find(std::string_view name);

// Actionable "unknown tool" diagnostic. A bare "unknown tool: X" strands
// the model — observed in the field when a LiteLLM-style proxy in front of
// the wire advertised ITS OWN tools (e.g. `headroom_retrieve`) alongside
// ours: the model called one, got an unhelpful error, and retried. Naming
// the real catalog turns the failure into a one-shot correction ("that
// tool isn't mine; here's what is"). Defined in registry.cpp beside the
// snapshot it enumerates.
[[nodiscard]] std::string unknown_tool_error(std::string_view name);

// The tool set to advertise on the wire for THIS turn. Equals registry()
// plus any MCP tools that appeared after startup via a `tools/list_changed`
// notification (and minus any that vanished). When MCP is unconfigured or no
// server has changed its list since startup, this returns registry() verbatim
// (no allocation, no copy). The per-turn provider walk should iterate this,
// not registry(), so a server adding a tool mid-session reaches the model on
// the next turn. Dispatch (`find`) already resolves these names live.
[[nodiscard]] const std::vector<ToolDef>& wire_tools();
// Build a bounded per-turn catalog: all native/pinned tools plus the external
// MCP tools most relevant to the current user request.
[[nodiscard]] std::vector<const ToolDef*> select_wire_tools(
    std::string_view query, std::size_t max_external = 16);

// The MCP tool-list generation counter, surfaced through the tools namespace
// so callers (ACP server, wire walks) don't need to link the mcp TU or know
// whether MCP is compiled in. Returns 0 when MCP is disabled or no server has
// re-listed. Bumps on every `tools/list_changed` (and resources/prompts).
[[nodiscard]] unsigned long mcp_generation() noexcept;

// Live-reload MCP plugins from mcp.json into the CURRENT session: rebuild
// the connection pool (spawn newly-added servers, drop removed ones) and
// re-project the wire catalog so the new tools are usable immediately —
// no restart. Returns the number of servers connected after the reload.
// Blocking (server handshakes); the TUI runs it off the UI thread.
[[nodiscard]] std::size_t reload_mcp_plugins();

// Force the wire catalog to re-project on next access WITHOUT re-spawning
// any server. Used by the per-tool enable/disable toggle: the server stays
// connected; only the projection filter (config tools.exclude, read live)
// changes, so a re-spawn would be wasteful and — under rapid toggles —
// race-prone. Cheap and synchronous; safe to call from the UI thread.
void invalidate_mcp_catalog();

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
    // Copy the sink installed on this worker so an async protocol reader can
    // forward progress back to the originating tool card.
    [[nodiscard]] Sink current();

    // RAII guard. `set` on construction, `clear` on destruction.
    struct Scope {
        explicit Scope(Sink s) { set(std::move(s)); }
        ~Scope()                { clear(); }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    };
}

namespace cancellation {
    using Probe = std::function<bool()>;
    void set(Probe probe);
    void clear();
    [[nodiscard]] Probe current();
    [[nodiscard]] bool requested();

    struct Scope {
        explicit Scope(Probe probe) { set(std::move(probe)); }
        ~Scope() { clear(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
}

} // namespace agentty::tools
