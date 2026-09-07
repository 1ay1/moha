#pragma once
// Settings pickers — the Ctrl+K rows for the config concerns that used to
// be CLI-/file-only: Plugins, Commands, Agents, Hooks. Each is a palette
// entry that opens THIS one picker, parameterised by `concern`. A single
// list modal (matching every other picker's shape) rather than a bespoke
// pane: pick the concern from Ctrl+K, get a focused list, act on a row.
//
// The rows are built live from settings::items_for(concern) — the same
// loaders the runtime uses — so the picker always mirrors disk. Actionable
// rows (remove a plugin, approve hooks, open RAG/Smart) act on Enter; the
// rest are informational. Keys (subscribe.cpp::on_settings_list): ↑↓/jk
// move, Enter acts, Esc/q closes.
//
// UI-state only; reducer in update/settings_list.cpp, view in
// view/settings_list_view.cpp.

#include <cstdint>
#include <variant>

#include "agentty/runtime/panel/settings/categories.hpp"   // settings::Category

namespace agentty {

namespace settings {

struct ListClosed {};
struct ListOpen {
    Category concern = Category::Plugins;
    int      index   = 0;
    // Inline add-mode: pressing `a` on a concern opens a one-line text
    // prompt right in the picker. Empty `input_active` == browsing the
    // list; true == typing a new entry. The prompt's meaning depends on
    // the concern (see settings_list.cpp): for Plugins it's
    // "name command args…"; for Commands/Agents it's the new file's name
    // (a starter file is created + the path surfaced to edit).
    bool        input_active = false;
    std::string input;          // typed buffer
    int         cursor = 0;     // byte cursor into `input`
    // Destructive `d` (remove a plugin) is two-step, like account removal:
    // the first press ARMS (stores the server name here + the view paints the
    // row "press d again to remove"); the second press on the SAME row
    // commits. Any move/other action disarms it. A stray keystroke can never
    // delete a hand-tuned mcp.json entry with no undo.
    std::string confirm_remove;
};

} // namespace settings

using SettingsListState =
    std::variant<settings::ListClosed, settings::ListOpen>;

[[nodiscard]] inline bool
settings_list_is_open(const SettingsListState& s) noexcept {
    return std::holds_alternative<settings::ListOpen>(s);
}
[[nodiscard]] inline settings::ListOpen*
settings_list_opened(SettingsListState& s) noexcept {
    return std::get_if<settings::ListOpen>(&s);
}
[[nodiscard]] inline const settings::ListOpen*
settings_list_opened(const SettingsListState& s) noexcept {
    return std::get_if<settings::ListOpen>(&s);
}

} // namespace agentty
