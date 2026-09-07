#pragma once
// @file mention picker — opens above the composer when the user types
// `@` on an empty buffer. The Open alternative carries the typed query,
// the cursor index into the visible row list, and a snapshot of the
// workspace's file paths captured at open time (so subsequent typing
// just filters the snapshot rather than rewalking the disk).
//
// Same shape as command_palette.hpp — Closed/Open variant, query +
// index in Open, plus a `files` vector since unlike the static command
// catalog, the candidate set comes from disk.
//
// The disk-walking + filter helpers live in `workspace/files.hpp`;
// this header is UI-state-only.

#include <string>
#include <variant>
#include <vector>

#include "agentty/workspace/files.hpp"

namespace agentty {

namespace mention {

struct Closed {};

struct Open {
    /// Filter buffer — what the user has typed since opening.
    std::string query;
    /// Cursor into the visible (filtered) rows.
    int         index = 0;
    /// Snapshot of workspace-relative file paths at open time. Sorted
    /// once for deterministic order; filtering walks this list each
    /// frame against the current query.
    std::vector<std::string> files;
    /// Filter result cache. `filter_files` is O(N × query_cost); on
    /// large workspaces it dominates per-keystroke latency. The
    /// reducer (bounds check) and the view (row rendering) used to
    /// each call it once per frame, plus another call on Select —
    /// three passes per arrow press. Memoise: when `query` changes,
    /// the cache is invalidated and recomputed once; subsequent reads
    /// in the same frame are O(1).
    mutable std::vector<std::size_t> cached_matches;
    mutable std::string              cached_query;
    mutable bool                     cached_valid = false;
};

} // namespace mention

using MentionPaletteState = std::variant<mention::Closed, mention::Open>;

[[nodiscard]] inline bool mention_is_open(const MentionPaletteState& s) noexcept {
    return std::holds_alternative<mention::Open>(s);
}
[[nodiscard]] inline       mention::Open* mention_opened(MentionPaletteState& s)       noexcept { return std::get_if<mention::Open>(&s); }
[[nodiscard]] inline const mention::Open* mention_opened(const MentionPaletteState& s) noexcept { return std::get_if<mention::Open>(&s); }

// Memoised filter accessor. Refreshes the open palette's cache iff
// `query` changed since the last call; otherwise returns the stored
// indices in O(1). All three call sites (reducer bounds-check on
// Move, reducer resolve on Select, view row builder) go through this
// helper so a single keystroke pays at most one O(N) filter pass.
[[nodiscard]] inline const std::vector<std::size_t>&
mention_filtered(const mention::Open& o) {
    if (!o.cached_valid || o.cached_query != o.query) {
        o.cached_matches = filter_files(o.files, o.query);
        o.cached_query   = o.query;
        o.cached_valid   = true;
    }
    return o.cached_matches;
}

} // namespace agentty
