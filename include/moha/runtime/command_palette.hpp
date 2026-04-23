#pragma once
// Command palette — the enum, the label/description table, and the open
// modal's UI state, kept in a single header so adding a new command is a
// one-file change (extend the enum, append a row to `kCommands`, then wire
// the selection in update.cpp's CommandPaletteSelect handler).

#include <array>
#include <cstdint>
#include <string>

namespace moha {

enum class Command : std::uint8_t {
    NewThread,
    ReviewChanges,
    AcceptAll,
    RejectAll,
    CycleProfile,
    OpenModels,
    OpenThreads,
    OpenPlan,
    Quit,
};

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
};

inline constexpr std::array kCommands = std::array{
    CommandDef{Command::NewThread,     "New thread",         "Start a fresh conversation"},
    CommandDef{Command::ReviewChanges, "Review changes",     "Open diff review pane"},
    CommandDef{Command::AcceptAll,     "Accept all changes", "Apply every pending hunk"},
    CommandDef{Command::RejectAll,     "Reject all changes", "Discard every pending hunk"},
    CommandDef{Command::CycleProfile,  "Cycle profile",      "Write \u2192 Ask \u2192 Minimal"},
    CommandDef{Command::OpenModels,    "Open model picker",  ""},
    CommandDef{Command::OpenThreads,   "Open threads",       ""},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress"},
    CommandDef{Command::Quit,          "Quit",               "Exit moha"},
};

struct CommandPaletteState {
    bool        open  = false;
    std::string query;
    int         index = 0;
};

} // namespace moha
