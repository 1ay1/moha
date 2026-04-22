#pragma once
// moha::Model — the composed application state.
//
// This header imports each domain it aggregates and adds the UI-only
// sub-states that don't belong to any domain (composer, pickers, palette,
// modals).  Update / view code reach for domain-specific headers directly;
// only the runtime glue needs the full composite.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "moha/domain/catalog.hpp"
#include "moha/domain/conversation.hpp"
#include "moha/diff/diff.hpp"
#include "moha/domain/id.hpp"
#include "moha/domain/profile.hpp"
#include "moha/domain/session.hpp"
#include "moha/domain/todo.hpp"

namespace moha {

// ============================================================================
// UI sub-states — one concern each, declared next to the Model that owns them
// ============================================================================

struct ComposerState {
    std::string text;
    int  cursor   = 0;
    bool expanded = false;
    std::vector<std::string> queued;
};

struct ModelPickerState {
    bool open  = false;
    int  index = 0;
};

struct ThreadListState {
    bool open  = false;
    int  index = 0;
};

enum class Command : std::uint8_t {
    NewThread, ReviewChanges, AcceptAll, RejectAll,
    CycleProfile, OpenModels, OpenThreads, OpenPlan, Quit,
};

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
};

inline constexpr std::array kCommands = std::array{
    CommandDef{Command::NewThread,      "New thread",         "Start a fresh conversation"},
    CommandDef{Command::ReviewChanges,  "Review changes",     "Open diff review pane"},
    CommandDef{Command::AcceptAll,      "Accept all changes", "Apply every pending hunk"},
    CommandDef{Command::RejectAll,      "Reject all changes", "Discard every pending hunk"},
    CommandDef{Command::CycleProfile,   "Cycle profile",      "Write \u2192 Ask \u2192 Minimal"},
    CommandDef{Command::OpenModels,     "Open model picker",  ""},
    CommandDef{Command::OpenThreads,    "Open threads",       ""},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress"},
    CommandDef{Command::Quit,          "Quit",               "Exit moha"},
};

struct CommandPaletteState {
    bool open = false;
    std::string query;
    int index = 0;
};

struct DiffReviewState {
    bool open       = false;
    int  file_index = 0;
    int  hunk_index = 0;
};

struct TodoState {
    bool open = false;
    std::vector<TodoItem> items;
};

// ============================================================================
// Model — the composed application state
// ============================================================================

struct Model {
    // ── Domain ───────────────────────────────────────────────────────
    Thread              current;
    std::vector<Thread> threads;
    Profile             profile = Profile::Write;

    std::vector<ModelInfo> available_models;
    ModelId                model_id{std::string{"claude-opus-4-5"}};

    std::vector<FileChange>          pending_changes;
    std::optional<PendingPermission> pending_permission;

    // ── UI sub-states ────────────────────────────────────────────────
    ComposerState       composer;
    StreamState         stream;
    ModelPickerState    model_picker;
    ThreadListState     thread_list;
    CommandPaletteState command_palette;
    DiffReviewState     diff_review;
    TodoState           todo;
    int                 thread_scroll = 0;

    // ── View virtualization ──────────────────────────────────────────
    // Index of the first message the view should render.  Messages before
    // this point are considered committed to the terminal's native
    // scrollback (maya's InlineFrameState::commit_prefix was called for
    // their rows).  Advancing this counter + returning Cmd::commit_scrollback
    // keeps the Yoga/paint cost bounded to the window, not the full
    // transcript.
    int thread_view_start = 0;
};

} // namespace moha
