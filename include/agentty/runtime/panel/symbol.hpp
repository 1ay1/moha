#pragma once
// #symbol picker — opens above the composer when the user types `#`
// at a word boundary. Same shape as MentionState (Closed | Open
// {query, index, candidates}) but the candidate set is workspace
// symbols instead of file paths. Symbol = (name, path, line) — the
// chip we attach on select carries all three so submit-time expansion
// can splice an excerpt of the file around the declaration.
//
// The workspace scanner that produces SymbolEntry (and the filter
// helper) live in `workspace/symbols.hpp`; this header is
// UI-state-only and re-imports SymbolEntry for the Open variant's
// vector.

#include <string>
#include <variant>
#include <vector>

#include "agentty/workspace/symbols.hpp"  // SymbolEntry

namespace agentty {

namespace symbol {

struct Closed {};

struct Open {
    std::string              query;
    int                      index = 0;
    std::vector<SymbolEntry> entries;
    /// Filter result cache. Same rationale as mention —
    /// filter_symbols is O(N × query_cost); reducer + view callers
    /// share the memoised indices via `symbol_filtered` so a single
    /// keystroke pays at most one O(N) pass.
    mutable std::vector<std::size_t> cached_matches;
    mutable std::string              cached_query;
    mutable bool                     cached_valid = false;
};

} // namespace symbol

using SymbolState = std::variant<symbol::Closed, symbol::Open>;

[[nodiscard]] inline bool symbol_palette_is_open(const SymbolState& s) noexcept {
    return std::holds_alternative<symbol::Open>(s);
}
[[nodiscard]] inline       symbol::Open* symbol_palette_opened(SymbolState& s)       noexcept { return std::get_if<symbol::Open>(&s); }
[[nodiscard]] inline const symbol::Open* symbol_palette_opened(const SymbolState& s) noexcept { return std::get_if<symbol::Open>(&s); }

[[nodiscard]] inline const std::vector<std::size_t>&
symbol_filtered(const symbol::Open& o) {
    if (!o.cached_valid || o.cached_query != o.query) {
        o.cached_matches = filter_symbols(o.entries, o.query);
        o.cached_query   = o.query;
        o.cached_valid   = true;
    }
    return o.cached_matches;
}

} // namespace agentty
