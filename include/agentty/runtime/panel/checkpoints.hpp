#pragma once
// Checkpoint picker — "rewind the workspace + transcript to any earlier
// point in this thread", not just the last one.
//
// Every user turn submitted inside a git repo pins a worktree snapshot
// (see workspace/checkpoint.hpp) and renders a checkpoint divider above
// the turn. Those dividers advertise restorable points; this picker is
// what makes ALL of them reachable (the palette's old "Rewind" only ever
// hit the newest). Open it from the command palette (Ctrl+K → "Rewind to
// checkpoint"); there is no dedicated key binding (Ctrl+R is OpenDiffReview):
//
//   ↑↓ / j k   move between checkpointed turns (newest at the bottom,
//              nearest the composer — the same spatial order as the
//              transcript).
//   Enter      rewind to the highlighted turn: worktree files revert to
//              that snapshot AND the transcript truncates back to just
//              before it, with the old prompt refilled in the composer.
//   Esc        close, no change.
//
// Each row shows the turn number, a one-line preview of the prompt, a
// relative timestamp, and — filled in asynchronously so opening is
// instant even on a big repo — a "N files · +A −D" diff summary of what
// the worktree has changed SINCE that point, so a rewind is never blind.
//
// This header is UI-state only. Reducer wiring lives in
// update/checkpoint.cpp, key dispatch in subscribe.cpp, the view in
// view/pickers.cpp — the same split as the other pickers.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "agentty/domain/id.hpp"

namespace agentty {

namespace checkpoint_picker {

// One restorable point — a checkpointed user turn.
struct Entry {
    CheckpointId id;
    // 1-based ordinal among user turns in the thread (what the transcript
    // labels the turn). Purely for display.
    int          turn = 0;
    // First line of the prompt, chip placeholders already flattened to
    // their visible labels, truncated for the row.
    std::string  preview;
    // Wall-clock ms since epoch of the turn's submission — rendered as a
    // relative "3m ago" so the user can anchor on time as well as text.
    std::int64_t timestamp_ms = 0;

    // ── Async diff summary (filled by CheckpointDiffLoaded) ──────────
    // diff_state advances Loading → Ready|Failed exactly once per entry.
    // Until Ready the row shows a subtle "…"; Failed rows just omit the
    // stat (the checkpoint may have been pruned by git gc).
    enum class DiffState : std::uint8_t { Loading, Ready, Failed };
    DiffState    diff_state    = DiffState::Loading;
    int          files_changed = 0;
    int          insertions    = 0;
    int          deletions     = 0;
    // True when Ready and nothing changed since this point — rendered as
    // "no changes" so the user knows a rewind here is a no-op on disk
    // (it still truncates the transcript).
    bool         clean         = false;
};

struct Closed {};

struct Open {
    // Newest-last so the list reads top-to-bottom oldest→newest, matching
    // the transcript above it; the cursor opens on the last (newest) row.
    std::vector<Entry> entries;
    int                index = 0;
};

} // namespace checkpoint_picker

using CheckpointPickerState =
    std::variant<checkpoint_picker::Closed, checkpoint_picker::Open>;

[[nodiscard]] inline bool checkpoint_picker_is_open(
        const CheckpointPickerState& s) noexcept {
    return std::holds_alternative<checkpoint_picker::Open>(s);
}
[[nodiscard]] inline checkpoint_picker::Open*
checkpoint_picker_opened(CheckpointPickerState& s) noexcept {
    return std::get_if<checkpoint_picker::Open>(&s);
}
[[nodiscard]] inline const checkpoint_picker::Open*
checkpoint_picker_opened(const CheckpointPickerState& s) noexcept {
    return std::get_if<checkpoint_picker::Open>(&s);
}

} // namespace agentty
