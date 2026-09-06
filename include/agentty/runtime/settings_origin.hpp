#pragma once
// agentty::ui::settings_origin — where a settings pane was opened FROM.
//
// The settings panes are reachable three ways: a Ctrl+K palette row, the
// settings list (Ctrl+K → Settings → Retrieval / Smart Mode), and a direct
// chord (Ctrl+S). Esc has to unwind ONE level, which means the answer depends
// on how you got there — and a pane cannot know that unless it is told.
//
// It used to be a constant in each close handler: CloseRagSettings always
// returned to the command palette, so entering from the settings list dropped
// you to the thread and lost your place. CloseSmartMode always returned to the
// thread, so entering from the palette threw the palette away. Both were right
// for one entry point and wrong for the others, and no amount of care in the
// handler could fix that — the information simply was not there.
//
// Same fix, same shape as login::Origin (runtime/login.hpp), which solved this
// exact problem for the auth flow: each frame CARRIES its parent, with the
// parent's data, so "return to the wrong place" stops being representable
// rather than being a bug to avoid. Where login's Back enum could only name a
// KIND of parent and lost WHICH provider's account list, a bare
// "came_from_palette" bool here would lose WHICH palette row and WHICH
// settings category.
//
// Deliberately NOT a stack. These panes nest one level, and a vector would
// invite arbitrary depth that nothing pops correctly — one parent, held by
// value, is the whole requirement.

#include <cstdint>
#include <variant>

#include "agentty/runtime/command_palette.hpp"
#include "agentty/runtime/settings_categories.hpp"

namespace agentty::ui::settings_origin {

// Opened by a direct chord (^S) or from the thread. Esc closes to the thread:
// you did not navigate anywhere, so there is nothing to go back to.
struct Thread {};

// Opened from a Ctrl+K palette row. Esc reopens the palette WITH THE CURSOR ON
// THAT ROW — the row is carried, not re-derived, so the palette comes back
// exactly where it was left even if the command list is filtered differently
// next time.
struct Palette { Command row = Command::OpenRagSettings; };

// Opened from the settings list. Esc reopens that list on the SAME category
// AND the SAME row — the row you activated, not the top.
//
// The row is carried for the same reason Palette carries one: coming back to
// the right LIST but the wrong PLACE in it is still losing the user's place.
// A settings list is a menu you walk down; returning to its first entry after
// every excursion makes configuring two adjacent things needlessly tedious,
// which is the tedium the stack was supposed to remove.
struct SettingsList {
    settings::Category category = settings::Category::General;
    int                row      = 0;
};

using Origin = std::variant<Thread, Palette, SettingsList>;

} // namespace agentty::ui::settings_origin
