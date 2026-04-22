#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/model.hpp"

namespace moha::tools {

// ── Tool result types (std::expected-based) ──────────────────────────────

struct ToolOutput {
    std::string text;
    std::optional<FileChange> change;
};

struct ToolError {
    std::string message;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

// ── Tool definition ──────────────────────────────────────────────────────

struct ToolDef {
    ToolName    name;
    std::string description;
    nlohmann::json input_schema;

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
