#pragma once
// agentty::ui::panel — THE routing function for panel priority.
//
// The overlay state itself lives in ONE variant slot (m.ui.panel — see
// overlay_state.hpp): opening is assignment, exclusivity is structural.
// This header answers the one remaining question the slot cannot answer
// alone: WHO OWNS THE KEYBOARD when the slot's overlay coexists with the
// three deliberate non-members —
//
//   login       (its own state machine; auth gates everything)
//   permission  (domain state raised mid-turn by the stream reducer)
//   todo        (an ambient pane that stays open UNDER the slot)
//
// top(m) composes those four sources in canonical priority order.
// subscribe.cpp routes keys to top(m); view.cpp renders top(m). Same
// function ⇒ keys and pixels cannot diverge. Both consumers are
// exhaustive switches on Kind, so a new overlay without arms is a
// -Wswitch warning, not a silent dead key or an invisible modal.

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/panel/slot.hpp"

namespace agentty::ui::panel {

// The active (topmost) overlay, in canonical priority order:
//   1. login       — owns the whole keyboard until auth completes.
//   2. permission  — a blocked tool's y/n must be answerable over anything.
//   3. the slot    — whatever exclusive panel is open (at most one, by
//                    construction).
//   4. todo        — ambient; lowest priority, most keys fall through.
[[nodiscard]] inline Kind top(const Model& m) noexcept {
    if (login::is_open(m.ui.login))         return Kind::Login;
    if (m.d.pending_permission.has_value()) return Kind::Permission;
    if (const Kind k = kind_of(m.ui.panel); k != Kind::None) return k;
    if (pick::is_open(m.ui.todo.open))      return Kind::Todo;
    return Kind::None;
}

} // namespace agentty::ui::panel
