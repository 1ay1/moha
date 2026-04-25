#pragma once
// Command palette — the enum, the label/description table, and the open
// modal's UI state, kept in a single header so adding a new command is a
// one-file change (extend the enum, append a row to `kCommands`, then wire
// the selection in update.cpp's CommandPaletteSelect handler).

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
    Compact,
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
    CommandDef{Command::OpenModels,    "Open model picker",  "Switch the active model"},
    CommandDef{Command::OpenThreads,   "Open threads",       "Browse saved conversations"},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress"},
    CommandDef{Command::Compact,       "Compact thread",     "Summarise old turns into one synthetic message to free context"},
    CommandDef{Command::Quit,          "Quit",               "Exit moha"},
};

// Case-insensitive substring filter over kCommands. Returns the matching
// CommandDef pointers in their catalog order. Single source of truth for
// "what's visible in the palette right now" \u2014 both the view (rendering)
// and the dispatcher (resolving cursor index \u2192 Command) call this so they
// can never disagree about which command sits at which row.
//
// The previous design had the view filter independently and the dispatcher
// switch on the cursor's *raw* position into kCommands. With any non-empty
// query the two indices drift: typing "thread" left "Open threads" at
// visible row 1, but row 1 in the unfiltered enum was `ReviewChanges` \u2014
// pressing Enter ran the wrong command.
[[nodiscard]] inline std::vector<const CommandDef*>
filtered_commands(std::string_view query) {
    auto lower = [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    };
    std::string needle;
    needle.reserve(query.size());
    for (char c : query) needle.push_back(lower(static_cast<unsigned char>(c)));

    std::vector<const CommandDef*> out;
    out.reserve(kCommands.size());
    for (const auto& cmd : kCommands) {
        if (needle.empty()) { out.push_back(&cmd); continue; }
        std::string hay;
        std::string_view label{cmd.label};
        hay.reserve(label.size());
        for (char c : label) hay.push_back(lower(static_cast<unsigned char>(c)));
        if (hay.find(needle) != std::string::npos)
            out.push_back(&cmd);
    }
    return out;
}

// Sum-type state, same shape as the other picker variants in
// `runtime/picker.hpp`. The query buffer + selected index live ONLY
// inside the Open alternative — they cannot exist while the palette
// is closed (used to be a bool + two fields where the bool gated their
// validity by convention; now the type system enforces it).
namespace palette {
struct Closed {};
struct Open {
    std::string query;
    int         index = 0;
};
} // namespace palette

using CommandPaletteState = std::variant<palette::Closed, palette::Open>;

[[nodiscard]] inline bool is_open(const CommandPaletteState& s) noexcept {
    return std::holds_alternative<palette::Open>(s);
}
[[nodiscard]] inline       palette::Open* opened(CommandPaletteState& s)       noexcept { return std::get_if<palette::Open>(&s); }
[[nodiscard]] inline const palette::Open* opened(const CommandPaletteState& s) noexcept { return std::get_if<palette::Open>(&s); }

} // namespace moha
