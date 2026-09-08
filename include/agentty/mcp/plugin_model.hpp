#pragma once
// agentty::mcp::PluginModel — the value snapshot of plugin/tool state.
//
// Split out of client.hpp so it can live IN the TEA Model (model.hpp) without
// dragging in the whole MCP client API (which depends on tools::ToolDef and
// would form an include cycle). This is a plain data type — <string>/<vector>
// only — precisely so the Model can own it and the view/visual_hash can treat
// the Plugins panel as a pure function of the Model. See docs/design/
// plugin-model.md and docs/design/plugin-model-in-model.md.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::mcp {

// One advertised tool of a connected server.
struct ToolState {
    std::string name;            // bare advertised name (no mcp__ prefix)
    std::string description;     // advertised description (for the UI)
    bool        enabled = true;  // NOT in config tools.exclude
    bool        over_budget = false; // enabled but trimmed from the wire
};

// Where a server's config entry was read from — enough for the picker to
// badge scope and for a reducer to route an edit back to the right file,
// without pulling the whole scope algebra into this value header.
enum class Origin : std::uint8_t { User, Project, Explicit };

[[nodiscard]] constexpr std::string_view to_string(Origin o) noexcept {
    switch (o) {
        case Origin::User:     return "user";
        case Origin::Project:  return "project";
        case Origin::Explicit: return "explicit";
    }
    return "user";
}

// One configured MCP server + its live connection state.
struct ServerState {
    std::string name;            // config key
    std::string command;         // config command (empty for HTTP/SSE)
    std::string url;             // config url (empty for stdio)
    bool        connected = false;   // handshake succeeded this session
    bool        disabled = false;    // config `disabled:true` — not connected on purpose
    std::string error;           // why not connected (empty if connected/ok)
    Origin      origin = Origin::User;   // which scope this entry came from
    std::string config_dir;      // the .agentty dir holding this entry's mcp.json
    bool        untrusted = false;   // project config not vouched for — won't connect
    bool        passthrough = false; // type:"passthrough" — dispatch-only foreign tools
    std::vector<ToolState> tools;

    [[nodiscard]] std::size_t enabled_count() const noexcept {
        std::size_t n = 0;
        for (const auto& t : tools) if (t.enabled) ++n;
        return n;
    }
};

// The single UI-facing truth: every configured server, its connection state,
// and its advertised tools, unified from config + live pool. A value snapshot
// safe to hold across a concurrent pool swap.
struct PluginModel {
    std::vector<ServerState> servers;
    std::size_t native_tool_count = 0; // agentty's own tools (always shipped)
    std::size_t wire_tool_count   = 0; // total tools actually on the wire
    std::size_t tool_budget       = 0; // soft cap (0 = unset)
    std::size_t trimmed_count     = 0; // enabled MCP tools dropped for budget

    [[nodiscard]] bool over_budget() const noexcept {
        return tool_budget > 0 && wire_tool_count > tool_budget;
    }
    [[nodiscard]] std::size_t trimmed() const noexcept { return trimmed_count; }
};

} // namespace agentty::mcp
