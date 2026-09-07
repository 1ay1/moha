#pragma once
// Tool-output inspector — a scrollable overlay for reading the FULL stored
// output of any settled tool call in the current thread.
//
// Why an overlay and not expand/collapse on the timeline cards: the
// transcript's committed rows live in native terminal scrollback, which is
// immutable — growing a card in place would rewrite committed rows and
// force a destructive HardReset (the exact corruption class the frozen-
// prefix architecture exists to prevent). An overlay paints strictly over
// the live viewport (same machinery as the pickers / code-block Result
// card), so the transcript underneath is never touched.
//
// Flow:  Ctrl+O → list of recent tool outputs (newest first) → Enter →
// full output, scrollable → Esc back → Esc close. `y` copies the body.
//
// Entries snapshot their output bytes at OPEN time (bounded: kMaxEntries
// items, kSnapshotBudget total bytes) so the overlay is immune to the
// transcript mutating underneath it (a streaming turn appending messages,
// a late ToolExecOutput settling a card). The per-tool stored output is
// already clamped to 256 KiB (update/tool.cpp kStoredOutputCap), so the
// snapshot is cheap.
//
// This header is UI state only. Reducer wiring lives in
// update/tool_viewer.cpp, key dispatch in subscribe.cpp, the view in
// view/pickers.cpp — the same file split as the other pickers.

#include <string>
#include <variant>
#include <vector>

#include "agentty/domain/conversation.hpp"

namespace agentty {

namespace tool_viewer {

// Snapshot budget: newest-first collection stops once either cap trips.
inline constexpr std::size_t kMaxEntries     = 50;
inline constexpr std::size_t kSnapshotBudget = 4u * 1024 * 1024;  // 4 MiB

struct Entry {
    std::string name;      // raw tool name ("read") — drives the category colour
    std::string title;     // display name ("Read", "Git Commit")
    std::string detail;    // one-line detail ("src/foo.cpp @120 · 450 lines")
    std::string trailing;  // "ok · 4.2s · 48 KB"  or  "failed · 0.3s"
    std::string output;    // full stored output (≤ 256 KiB by upstream cap)
    bool        failed = false;
    // The live entry: the tool CURRENTLY running, pinned as row 0. Its
    // `output` is the growing progress_text (stdout+stderr tail) rather
    // than a settled snapshot, and the view tail-anchors it + labels the
    // row "Live". Rebuilt from the transcript on every tool event while
    // the viewer is open, so it streams in place. At most one per list.
    bool        is_live = false;
    // A snapshot of the settled tool call. The body stage renders it
    // through the SAME maya::ToolBodyPreview path the timeline uses
    // (show_all) — so edit/write show a coloured diff, read/write show
    // a line-number gutter, git_diff shows +/- bands, etc. — instead of
    // a flat monochrome text dump. Snapshotting the whole ToolUse (name
    // + args + terminal output) keeps the overlay immune to transcript
    // mutation and the copy is bounded by the 256 KiB output clamp.
    ToolUse     call;
};

struct Closed {};

struct Open {
    std::vector<Entry> entries;   // newest first
    int                index   = 0;
    bool               viewing = false;   // false = list, true = body stage
};

} // namespace tool_viewer

using ToolViewerState = std::variant<tool_viewer::Closed, tool_viewer::Open>;

[[nodiscard]] inline bool tool_viewer_is_open(const ToolViewerState& s) noexcept {
    return std::holds_alternative<tool_viewer::Open>(s);
}
[[nodiscard]] inline tool_viewer::Open*
tool_viewer_opened(ToolViewerState& s) noexcept {
    return std::get_if<tool_viewer::Open>(&s);
}
[[nodiscard]] inline const tool_viewer::Open*
tool_viewer_opened(const ToolViewerState& s) noexcept {
    return std::get_if<tool_viewer::Open>(&s);
}

} // namespace agentty
