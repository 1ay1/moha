#pragma once
// moha::Model — the composed application state.
//
// This header imports each domain it aggregates and adds the UI-only
// sub-states that don't belong to any domain (composer, pickers, palette,
// modals).  Update / view code reach for domain-specific headers directly;
// only the runtime glue needs the full composite.

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
#include "moha/runtime/command_palette.hpp"

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
// Model — the composed application state, split into three concerns:
//   d   — Domain: what the conversation is (persisted, sent to provider).
//   s   — Session: the in-flight request's state machine + cancel handle.
//   ui  — UI: picker/modal/view-virtualization state, pure ephemeral.
//
// The split lets call sites communicate their scope: a function that only
// touches `m.ui.*` can't accidentally mutate the conversation; a reducer
// fragment that reads `m.d.*` doesn't need to thread picker state through.
// ============================================================================

struct Model {
    struct Domain {
        Thread              current;
        std::vector<Thread> threads;
        Profile             profile = Profile::Write;

        std::vector<ModelInfo> available_models;
        ModelId                model_id{std::string{"claude-opus-4-5"}};

        std::vector<FileChange>          pending_changes;
        std::optional<PendingPermission> pending_permission;
    };

    struct UI {
        ComposerState       composer;
        ModelPickerState    model_picker;
        ThreadListState     thread_list;
        CommandPaletteState command_palette;
        DiffReviewState     diff_review;
        TodoState           todo;
        int                 thread_scroll = 0;
        // Index of the first message the view should render.  Messages
        // before this point are committed to the terminal's native
        // scrollback (maya's InlineFrameState::commit_prefix was called
        // for their rows).  Advancing this counter + returning
        // Cmd::commit_scrollback keeps the Yoga/paint cost bounded to the
        // visible window, not the full transcript.
        int                 thread_view_start = 0;
    };

    Domain      d;
    StreamState s;
    UI          ui;
};

} // namespace moha
