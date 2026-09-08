#pragma once
// Command palette — the enum, the label/description table, and the open
// modal's UI state, kept in a single header so adding a new command is a
// one-file change (extend the enum, append a row to `kCommands`, then wire
// the selection in update.cpp's PaletteSelect handler).

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agentty/runtime/fuzzy.hpp"

namespace agentty {

enum class Command : std::uint8_t {
    NewThread,
    ReviewChanges,
    ToggleChangesStrip,
    AcceptAll,
    RejectAll,
    CycleProfile,
    OpenModels,
    SwapModel,
    OpenProviders,
    OpenThreads,
    OpenPlan,
    RunCodeBlock,
    InspectToolOutputs,
    CompactContext,
    SmartMode,
    RewindCheckpoint,
    ForkThread,
    OpenPlugins,
    OpenCommands,
    OpenAgents,
    OpenHooks,
    OpenGeneralSettings,
    OpenRag,
    OpenLogin,
    SignOut,
    UpdateAgentty,
    Quit,
};

// Palette section a command belongs to. Rendered as a colour-coded badge
// (hue-stable under the cursor) so the flat list reads as grouped families,
// and folded into the fuzzy filter so `changes` surfaces the whole cluster.
// Order here defines the section order in the palette.
enum class Category : std::uint8_t {
    Thread,     // new / fork / compact / rewind
    Changes,    // review / accept / reject
    Navigate,   // threads / plan / tool outputs / code block
    Config,     // profile / model / provider / smart / rag / plugins / hooks / commands / agents
    Account,    // sign in / out
    General,    // quit / update
};

[[nodiscard]] constexpr std::string_view category_label(Category c) noexcept {
    switch (c) {
        case Category::Thread:   return "Thread";
        case Category::Changes:  return "Changes";
        case Category::Navigate: return "Go";
        case Category::Config:   return "Config";
        case Category::Account:  return "Account";
        case Category::General:  return "";
    }
    return "";
}

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
    const char* shortcut;   // direct global keybinding, or "" if palette-only
    Category    category = Category::General;
    bool        danger   = false;  // destructive: discards work / mutates worktree
};

inline constexpr std::array kCommands = std::array{
    // ── Thread ──────────────────────────────────────────────────────────
    CommandDef{Command::NewThread,     "New thread",         "Start a fresh conversation", "Ctrl+N", Category::Thread},
    CommandDef{Command::ForkThread,    "Fork thread",        "Branch into a fresh thread with near-zero context — the parent transcript stays readable on demand", "", Category::Thread},
    CommandDef{Command::CompactContext,"Compact context",    "Replace history with a structured summary", "", Category::Thread},
    CommandDef{Command::RewindCheckpoint,"Rewind to checkpoint","Restore files + conversation to any earlier turn", "", Category::Thread, /*danger=*/true},
    // ── Changes ─────────────────────────────────────────────────────────
    CommandDef{Command::ReviewChanges, "Review changes",     "Open the diff review pane", "Ctrl+R", Category::Changes},
    CommandDef{Command::ToggleChangesStrip, "Changes strip",   "Show / hide the persistent \"N changes\" banner after edits", "", Category::Changes},
    CommandDef{Command::AcceptAll,     "Accept all changes", "Apply every pending hunk", "", Category::Changes},
    CommandDef{Command::RejectAll,     "Reject all changes", "Discard every pending hunk", "", Category::Changes, /*danger=*/true},
    // ── Go (navigate) ───────────────────────────────────────────────────
    CommandDef{Command::OpenThreads,   "Open threads",       "Browse saved conversations", "Ctrl+J", Category::Navigate},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress", "Ctrl+T", Category::Navigate},
    CommandDef{Command::InspectToolOutputs, "Inspect tool outputs", "Read tool outputs — the running tool is the live top row", "Ctrl+O", Category::Navigate},
    CommandDef{Command::RunCodeBlock,  "Run code block",     "Run a fenced block from the last reply", "Ctrl+G", Category::Navigate},
    // ── Config ──────────────────────────────────────────────────────────
    CommandDef{Command::CycleProfile,  "Cycle profile",      "Write → Ask → Minimal", "Shift+Tab", Category::Config},
    CommandDef{Command::OpenModels,    "Switch model",       "Switch model across every signed-in provider", "Ctrl+/", Category::Config},
    CommandDef{Command::SwapModel,     "Swap to previous model", "Jump back to the model you used before (cross-provider)", "Ctrl+Tab", Category::Config},
    CommandDef{Command::OpenProviders, "Switch provider",    "Choose the LLM backend (Anthropic, OpenAI, …)", "Ctrl+P", Category::Config},
    CommandDef{Command::SmartMode,     "Smart Mode",         "Configure role-based routing — send cheap grunt work to a cheaper model", "Ctrl+S", Category::Config},
    CommandDef{Command::OpenRag,"Retrieval (RAG)",   "Proactive retrieval on / first turn / off, and which embedding backend to use", "", Category::Config},
    CommandDef{Command::OpenPlugins,   "MCP servers",        "Plugins / MCP servers (mcp.json) — list & remove; add with `agentty plugin add`", "", Category::Config},
    CommandDef{Command::OpenCommands,  "Slash commands",     "Discovered /commands — author in .agentty/commands/*.md", "", Category::Config},
    CommandDef{Command::OpenAgents,    "Subagents",          "Task agent types — built-ins + your .agentty/agents/*.md", "", Category::Config},
    CommandDef{Command::OpenHooks,     "Hooks",              "Lifecycle hooks + approval state (.agentty/hooks.json)", "", Category::Config},
    CommandDef{Command::OpenGeneralSettings, "Settings",     "Permission profile, Smart Mode, retrieval — the live toggles", "", Category::Config},
    // ── Account ─────────────────────────────────────────────────────────
    CommandDef{Command::OpenLogin,     "Sign in / add account", "Sign in — or add another OAuth / API-key account", "", Category::Account},
    CommandDef{Command::SignOut,       "Sign out",           "Remove saved credentials and re-open sign-in", "", Category::Account, /*danger=*/true},
    // ── General ─────────────────────────────────────────────────────────
    CommandDef{Command::UpdateAgentty, "Update agentty",     "Download + install the new release (shown when one is available)", "", Category::General},
    CommandDef{Command::Quit,          "Quit",               "Exit agentty", "Ctrl+C", Category::General},
};

// Live context that decides which conditionally-visible rows appear. A dead
// row (Accept-all with no diff, Run-code-block with no fenced reply) trains
// users to distrust the palette, so we hide it — the same discipline already
// applied to "Update agentty". Defaults keep every row visible so callers
// that don't care (tests, palette_index_of) behave as before.
struct PaletteContext {
    bool update_available   = true;
    bool has_pending_changes = true;   // gates Review / Accept-all / Reject-all
    bool has_code_block     = true;    // gates Run code block
};

// True iff `cmd` should be VISIBLE given the live context. Keeping this in one
// predicate (rather than scattered `if`s) means the view and dispatcher agree
// on visibility for free, since both go through filtered_commands().
[[nodiscard]] inline bool command_visible(const CommandDef& cmd,
                                          const PaletteContext& ctx) noexcept {
    switch (cmd.id) {
        case Command::UpdateAgentty: return ctx.update_available;
        // "Review changes" is the ENTRY POINT — keep it always discoverable so
        // you can find it (it toasts "no pending changes to review" when the
        // queue is empty). Only the BULK actions are gated: accepting/rejecting
        // "all" is a genuine no-op with nothing pending, and a dead destructive
        // row erodes trust.
        case Command::AcceptAll:
        case Command::RejectAll:     return ctx.has_pending_changes;
        case Command::RunCodeBlock:  return ctx.has_code_block;
        default:                     return true;
    }
}

// ── Fuzzy matcher ────────────────────────────────────────────────────
// Command LABELS are scored with the shared subsequence matcher (fuzzy.hpp)
// so the palette ranks the way every other picker does: "re" → Review /
// Reject / Rewind (label prefixes), not "New th-re-ad" (a "re" buried in a
// description). Description/shortcut/category hits are kept but ranked below
// every label hit.

// Case-insensitive substring test over a field (description / shortcut /
// category) — used only to KEEP a row whose label didn't match, ranked below
// every label match. Discovery-by-intent ("api" → Switch provider) without
// letting description noise outrank a real label hit.
[[nodiscard]] inline bool field_contains(const char* field, std::string_view needle) {
    if (!field) return false;
    std::string hay;
    for (const char* p = field; *p; ++p) hay.push_back(
        static_cast<char>(std::tolower((unsigned char)*p)));
    return hay.find(needle) != std::string::npos;
}

// A scored, ranked match result carrying the label-highlight positions.
struct CommandMatch {
    const CommandDef* cmd;
    int               score;
    std::vector<int>  positions;   // label offsets to highlight (empty if none)
};

// THE matcher. Returns visible commands ranked best-first, each with its
// label-highlight offsets. Single source of truth for view + dispatcher.
[[nodiscard]] inline std::vector<CommandMatch>
match_commands(std::string_view query, PaletteContext ctx) {
    std::string needle;
    needle.reserve(query.size());
    for (char c : query) needle.push_back(
        static_cast<char>(std::tolower((unsigned char)c)));

    std::vector<CommandMatch> out;
    out.reserve(kCommands.size());
    for (const auto& cmd : kCommands) {
        if (!command_visible(cmd, ctx)) continue;
        if (needle.empty()) { out.push_back({&cmd, 0, {}}); continue; }

        fuzzy::Match lm = fuzzy::score(cmd.label, needle);
        if (lm.matched()) {
            out.push_back({&cmd, lm.score + 1000, std::move(lm.positions)});
        } else if (field_contains(cmd.description, needle)
                || field_contains(cmd.shortcut, needle)
                || (!category_label(cmd.category).empty()
                    && field_contains(std::string{category_label(cmd.category)}.c_str(), needle))) {
            // Description/shortcut/category hit: keep, but rank below every
            // label match (no +1000) and by catalog position (index below).
            out.push_back({&cmd, 0, {}});
        }
    }
    if (!needle.empty())
        std::stable_sort(out.begin(), out.end(),
            [](const CommandMatch& a, const CommandMatch& b) {
                return a.score > b.score;   // stable keeps catalog order within a tie
            });
    return out;
}

// Pointer-only view of match_commands, ranked. Kept for the dispatcher +
// existing call sites/tests that only need "which command is at row N".
[[nodiscard]] inline std::vector<const CommandDef*>
filtered_commands(std::string_view query, PaletteContext ctx) {
    auto matches = match_commands(query, ctx);
    std::vector<const CommandDef*> out;
    out.reserve(matches.size());
    for (const auto& m : matches) out.push_back(m.cmd);
    return out;
}

// Back-compat overload: the original (query, update_available) shape. Keeps
// existing call sites + tests working; new call sites pass a PaletteContext.
[[nodiscard]] inline std::vector<const CommandDef*>
filtered_commands(std::string_view query, bool update_available = true) {
    return filtered_commands(query, PaletteContext{.update_available = update_available});
}

// Sum-type state, same shape as the other picker variants in
// `runtime/panel/common.hpp`. The query buffer + selected index live ONLY
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

using PaletteState = std::variant<palette::Closed, palette::Open>;

// Index of `cmd` in the palette list AS RENDERED for `ctx`. Used to re-open
// the palette focused on the row the user last selected, e.g. after Esc-ing
// back out of a settings picker.
//
// The context matters: rows are GATED (Review/Accept-all need pending changes,
// Run-code-block needs a fenced reply, Update needs an available update), so
// the rendered list is shorter than kCommands and every index after a hidden
// row shifts. Restoring a cursor computed against the unfiltered table lands
// on a neighbour — which reads as "Esc moved my selection", the opposite of
// coming back where you left.
//
// Returns 0 if not visible (defensive; a row the user just used is visible by
// construction).
[[nodiscard]] inline int palette_index_of(Command cmd,
                                          PaletteContext ctx = {}) noexcept {
    int i = 0;
    for (const auto& c : kCommands) {
        if (!command_visible(c, ctx)) continue;
        if (c.id == cmd) return i;
        ++i;
    }
    return 0;
}

[[nodiscard]] inline bool is_open(const PaletteState& s) noexcept {
    return std::holds_alternative<palette::Open>(s);
}
[[nodiscard]] inline       palette::Open* opened(PaletteState& s)       noexcept { return std::get_if<palette::Open>(&s); }
[[nodiscard]] inline const palette::Open* opened(const PaletteState& s) noexcept { return std::get_if<palette::Open>(&s); }

} // namespace agentty
