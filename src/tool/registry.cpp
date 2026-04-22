#include "moha/tool/registry.hpp"
#include "moha/tool/tools.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace moha::tools {

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

// Assemble every tool. Order matters only for display in the UI's tool
// picker — the protocol itself treats the set as unordered.
std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> r;
    r.push_back(tool_read());
    r.push_back(tool_write());
    r.push_back(tool_edit());
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
    return r;
}

} // namespace

const std::vector<ToolDef>& registry() {
    static const std::vector<ToolDef> r = build_registry();
    return r;
}

const ToolDef* find(std::string_view name) {
    for (const auto& t : registry()) if (t.name == name) return &t;
    return nullptr;
}

} // namespace moha::tools
