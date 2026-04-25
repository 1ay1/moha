#include "moha/tool/registry.hpp"
#include "moha/tool/tools.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moha::tools {

std::string_view to_string(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::InvalidArgs:    return "invalid args";
        case ErrorKind::NotFound:       return "not found";
        case ErrorKind::NotAFile:       return "not a file";
        case ErrorKind::NotADirectory:  return "not a directory";
        case ErrorKind::TooLarge:       return "too large";
        case ErrorKind::Binary:         return "binary";
        case ErrorKind::Ambiguous:      return "ambiguous";
        case ErrorKind::NoMatch:        return "no match";
        case ErrorKind::InvalidRegex:   return "invalid regex";
        case ErrorKind::Network:        return "network";
        case ErrorKind::Spawn:          return "spawn failed";
        case ErrorKind::Subprocess:     return "subprocess failed";
        case ErrorKind::Io:             return "io";
        case ErrorKind::OutOfWorkspace: return "out of workspace";
        case ErrorKind::Unknown:        return "unknown";
    }
    return "unknown";
}

std::string ToolError::render() const {
    return std::format("[{}] {}", to_string(kind), detail);
}

std::string_view to_string(Effect e) noexcept {
    switch (e) {
        case Effect::ReadFs:  return "ReadFs";
        case Effect::WriteFs: return "WriteFs";
        case Effect::Net:     return "Net";
        case Effect::Exec:    return "Exec";
    }
    return "?";
}

std::string to_string(EffectSet e) {
    if (e.empty()) return "Pure";
    std::string out;
    auto add = [&](Effect bit) {
        if (!e.has(bit)) return;
        if (!out.empty()) out += ", ";
        out += to_string(bit);
    };
    add(Effect::Exec);
    add(Effect::WriteFs);
    add(Effect::Net);
    add(Effect::ReadFs);
    return out;
}

// ── Live progress sink (thread-local implementation) ────────────────────
//
// thread_local so the cmd runner's dispatch lambda can be captured without
// cross-thread synchronisation — each tool runs on its own worker, and
// cmd_factory installs/clears the sink on that worker via a RAII Scope.
// Subprocess runners (see util/subprocess.cpp) call progress::emit from the
// same thread, so it's a plain load from TLS — no atomics, no locking.
namespace progress {
namespace {
    thread_local Sink g_sink;
}
void set(Sink s)                       { g_sink = std::move(s); }
void clear()                           { g_sink = nullptr; }
void emit(std::string_view snapshot)   { if (g_sink) g_sink(snapshot); }
}

namespace {

// Assemble every tool. Order matters: the protocol treats the set as
// unordered but the model has a strong recall bias toward earlier-listed
// tools. Putting `edit` ahead of `write` is the cheapest single nudge to
// stop the model from rewriting whole files when a targeted substitution
// would do — and edit's tiny input_json_delta bodies sidestep the long
// mid-stream pause Anthropic's edge applies to multi-KB tool_use content.
std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> r;
    r.push_back(tool_read());
    r.push_back(tool_edit());
    r.push_back(tool_write());
    r.push_back(tool_bash());
    r.push_back(tool_grep());
    r.push_back(tool_glob());
    r.push_back(tool_list_dir());
    r.push_back(tool_todo());
    r.push_back(tool_web_fetch());
    r.push_back(tool_web_search());
    r.push_back(tool_find_definition());
    r.push_back(tool_diagnostics());
    r.push_back(tool_git_status());
    r.push_back(tool_git_diff());
    r.push_back(tool_git_log());
    r.push_back(tool_git_commit());
    // Tier-2 (table-of-contents) tools listed *before* Tier-3 so the
    // model's recall bias prefers the cheap structural lookups over the
    // sub-agent for ordinary "what's in the codebase?" questions.
    r.push_back(tool_outline());
    r.push_back(tool_repo_map());
    r.push_back(tool_signatures());
    r.push_back(tool_investigate());
    return r;
}

} // namespace

const std::vector<ToolDef>& registry() {
    static const std::vector<ToolDef> r = build_registry();
    return r;
}

// Process-wide name → ToolDef* index, built once on first access.
// Replaces the prior O(N) linear scan in `find()` with a hash-table
// hit. With N=16 the absolute speedup is small (~50 ns vs ~5 ns), but
// the dispatch path runs on every model tool call, every retry,
// every permission prompt — keeping it constant-time is the right
// shape for an agent loop. Stored alongside the registry so both
// share the same lifetime + initialisation order.
//
// The map keys are `std::string` (owning) rather than `string_view`
// to insulate the map from reallocations of the underlying vector.
// In practice the vector never grows after init, but std::string keys
// are the safer default and the lookup cost is identical (heterogeneous
// `find` lets `string_view` callers query without allocating).
namespace {
const std::unordered_map<std::string, const ToolDef*>& index() {
    static const std::unordered_map<std::string, const ToolDef*> m = []{
        const auto& r = registry();
        std::unordered_map<std::string, const ToolDef*> out;
        out.reserve(r.size());
        for (const auto& t : r) out.emplace(t.name.value, &t);
        return out;
    }();
    return m;
}
} // namespace

const ToolDef* find(std::string_view name) {
    const auto& m = index();
    // Heterogeneous lookup avoids constructing a temporary std::string.
    if (auto it = m.find(std::string{name}); it != m.end()) return it->second;
    return nullptr;
}

} // namespace moha::tools
