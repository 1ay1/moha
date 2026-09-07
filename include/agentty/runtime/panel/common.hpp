#pragma once
// agentty::ui::pick — picker state machines as sum types.
//
// Replaces the canonical anti-pattern this codebase used to have:
//
//     struct FusedPickerState { bool open = false; int index = 0; };
//
// where `index` is meaningless when `open == false`. A reader had to
// remember which fields were valid in which combination; a writer had to
// remember to reset `index` (or not) when toggling `open`. No type
// system help.
//
// Now: each picker is a closed `std::variant<Closed, Open{cursor…}>`.
// The cursor data is owned by the `Open` alternative — it cannot exist
// when the picker is closed. Adding a new picker shape (e.g. multi-
// select) means a new variant arm + a new `pick::opened` overload; the
// compiler tells you everywhere that needs to change.
//
// Three shapes cover today's pickers:
//   OneAxis  — model list, thread list (single index cursor)
//   TwoAxis  — diff review (file × hunk cursor)
//   Modal    — todo overlay (no cursor; just gates rendering)

#include <concepts>
#include <string>
#include <string_view>
#include <variant>

namespace agentty::ui::pick {

struct Closed {};

// One-axis picker (model list, thread list). `query` is an optional
// incremental-search buffer — only the model picker uses it today; pickers
// that don't touch it leave it empty and behave exactly as before.
// `confirm_remove` carries the label/id of the row pending a two-press
// delete (thread picker only, mirroring SettingsListRemove / AccountRemove).
// Any move/select/new/close clears it, so a stray `d` can never delete
// a thread with no way back.
struct OpenAt {
    int index = 0;
    std::string query;
    std::string confirm_remove;
    // Fused model picker only: when non-empty, the browse/filter list is
    // scoped to THIS provider id (^/ scopes to the selected row's provider;
    // pressing ^/ again clears it). Empty = every provider, the default.
    std::string provider_scope;
};
using OneAxis = std::variant<Closed, OpenAt>;

// Two-axis picker (diff review: file × hunk). `body_scroll` is the vertical
// scroll offset (in diff lines) INSIDE the focused hunk's capped body window
// — ^D/^U/PgDn/PgUp page it; any cursor move resets it to 0 (a new hunk
// starts at its top). `confirm_reject_all` arms the two-press ^X guard:
// the first ^X sets it (footer turns into a warning), the second executes
// the full revert; any other key disarms — mirroring OpenAt's
// confirm_remove so a stray ^X can never throw away a whole changeset.
struct OpenAtCell {
    int file_index = 0;
    int hunk_index = 0;
    int body_scroll = 0;
    bool confirm_reject_all = false;
};
using TwoAxis = std::variant<Closed, OpenAtCell>;

// Modal-style picker that toggles open/close without a cursor (e.g.
// the todo overlay — items live alongside on the owning struct).
struct OpenModal {};
using Modal = std::variant<Closed, OpenModal>;

// ── Helpers ───────────────────────────────────────────────────────────
// A picker state is any variant that carries the `Closed` alternative (all
// three shapes above do). Naming the requirement means `is_open` rejects a
// non-picker type with a one-line concept error instead of a cryptic failure
// deep inside std::holds_alternative.
template <class P>
concept PickerState = requires(const P& p) {
    { std::holds_alternative<Closed>(p) } -> std::same_as<bool>;
};

// Generic "is this variant in any non-Closed state?" — works for all
// three shapes so call sites read uniformly: `if (pick::is_open(x))`.
template <PickerState P>
[[nodiscard]] inline bool is_open(const P& p) noexcept {
    return !std::holds_alternative<Closed>(p);
}

// Cursor accessors. Return nullptr when the picker is closed; callers
// use `if (auto* c = pick::opened(p)) …` to combine the open-check
// with cursor extraction in one expression — the moral equivalent of
// `if let Open(c) = p` in Rust.
[[nodiscard]] inline OpenAt*       opened(OneAxis& p)       noexcept { return std::get_if<OpenAt>(&p); }
[[nodiscard]] inline const OpenAt* opened(const OneAxis& p) noexcept { return std::get_if<OpenAt>(&p); }
[[nodiscard]] inline OpenAtCell*       opened(TwoAxis& p)       noexcept { return std::get_if<OpenAtCell>(&p); }
[[nodiscard]] inline const OpenAtCell* opened(const TwoAxis& p) noexcept { return std::get_if<OpenAtCell>(&p); }

// Convenience: index when open, -1 otherwise. Use only at read sites
// that genuinely don't care about the closed-vs-open distinction
// (e.g. comparing to a list size). Prefer `opened()` when possible.
[[nodiscard]] inline int index_or(const OneAxis& p, int fallback = -1) noexcept {
    if (auto* o = std::get_if<OpenAt>(&p)) return o->index;
    return fallback;
}

// Case-insensitive substring test used by incremental-search pickers.
// Empty needle matches everything (an empty query shows the full list).
// ASCII-fold only — model ids are ASCII in practice, and a full Unicode
// case-fold would be overkill for a live filter.
[[nodiscard]] inline bool fuzzy_contains(std::string_view hay,
                                         std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto lower = [](char c) noexcept -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    const std::size_t last = hay.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j)
            if (lower(hay[i + j]) != lower(needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

} // namespace agentty::ui::pick
