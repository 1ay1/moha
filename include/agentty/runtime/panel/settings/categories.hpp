#pragma once
// settings_categories — the config concerns surfaced as Ctrl+K rows, each
// opening the shared settings-list picker. Just the enum + labels; the row
// data lives in settings_items.hpp, the UI state in settings_list.hpp.

#include <cstdint>

namespace agentty::settings {

enum class Category : std::uint8_t {
    General,   // profile, RAG mode, Smart Mode — the live toggles
    Plugins,   // MCP servers (mcp.json): list + remove
    Commands,  // slash commands: discovered list (read-only)
    Agents,    // user subagents: discovered list (read-only)
    Hooks,     // lifecycle hooks: file + approval state + approve action
};

[[nodiscard]] constexpr const char* label(Category c) noexcept {
    switch (c) {
        case Category::General:  return "General";
        case Category::Plugins:  return "Plugins";
        case Category::Commands: return "Commands";
        // "Subagents", not "Agents", to disambiguate from the AGENTS.md project-
        // guidance standard (agents.md). These are the task tool's delegate
        // PERSONAS (.agentty/agents/*.md); AGENTS.md is a project doc.
        case Category::Agents:   return "Subagents";
        case Category::Hooks:    return "Hooks";
    }
    return "?";
}

// A one-line subtitle for each picker (shown under the title).
[[nodiscard]] constexpr const char* subtitle(Category c) noexcept {
    switch (c) {
        case Category::General:  return "profile, Smart Mode, retrieval";
        case Category::Plugins:  return "MCP servers \xc2\xb7 agentty plugin add \xe2\x80\xa6";
        case Category::Commands: return "slash commands \xc2\xb7 .agentty/commands/*.md";
        case Category::Agents:   return "subagents \xc2\xb7 .agentty/agents/*.md";
        case Category::Hooks:    return "lifecycle hooks \xc2\xb7 .agentty/hooks.json";
    }
    return "";
}

} // namespace agentty::settings
