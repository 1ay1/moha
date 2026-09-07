#pragma once
// agentty::form::keys — the ONE key map every form obeys.
//
// A form has three interaction modes and each owns the keyboard exclusively;
// which one is active is `Form::focus`, the same value the view reads. So the
// router is a pure function of (focus, key) -> intent, and no pane writes its
// own navigation. That is what stops the panes from drifting apart the way
// five hand-rolled pickers did — and it means the key map is documented in
// exactly one place:
//
//   Browsing   ↑↓/kj move · ←→ adjust in place · Enter edit/open/toggle/fire
//              x reset · ^S save · Esc close
//   Editing    printable → insert · ←→ caret · ^A/^E home/end · ^U clear
//              Enter/Esc leave the field
//   Choosing   ↑↓/kj highlight · Enter commit · Esc cancel (value untouched)
//
// Note what Choosing does NOT have: a filter. A dropdown holds an enum, and
// an option set large enough to need searching is a Pick row that hands off
// to a real picker overlay instead.
//
// The router returns an INTENT, not a Msg: intents are pane-agnostic, so each
// pane maps them onto its own message set without re-deriving the key map.

#include <cstdint>
#include <optional>

#include <maya/terminal/input.hpp>

#include "agentty/runtime/panel/form.hpp"

namespace agentty::form::keys {

enum class Intent : std::uint8_t {
    None,
    // Browsing
    MovePrev, MoveNext,
    MoveFirst, MoveLast,       // Home/End — jump to the first/last field
    MovePageUp, MovePageDown,  // PgUp/PgDn — viewport-sized strides
    AdjustDown, AdjustUp,      // ←/→ on a Toggle/Choice/Number/Slider
    Activate,                  // Enter
    ResetField,                // x
    Save,                      // ^S
    Close,                     // Esc (Browsing) — the pane decides what that means
    // Editing
    Insert,                    // `ch` carries the code point
    Backspace, DeleteForward,
    CaretLeft, CaretRight, CaretHome, CaretEnd,
    ClearField,
    LeaveField,                // Enter/Esc while editing
    // Choosing
    MenuPrev, MenuNext,
    MenuCommit, MenuCancel,
};

struct Action {
    Intent   intent = Intent::None;
    char32_t ch     = 0;       // meaningful for Insert / MenuInput
};

// Translate one key against the form's CURRENT focus. Returns nullopt when the
// key means nothing here, so the caller can fall through to ambient handling
// (^C quit, etc.). Every mode returns a value for printable keys it owns, so a
// keystroke can never leak from a text field into global navigation.
//
// Takes the two focus FLAGS rather than the Form, because that is the entire
// dependency — and the caller lives in `subscribe()`, which runs every frame.
// Passing the Form there meant deep-copying every row's strings per frame,
// which is precisely the input lag this router exists to keep out of the
// hot path.
[[nodiscard]] std::optional<Action> translate(bool editing, bool choosing,
                                              const maya::KeyEvent& ev);

// Convenience for callers that already hold the form (tests, reducers).
[[nodiscard]] std::optional<Action> translate(const Form& f, const maya::KeyEvent& ev);

// Apply an intent to the form. Returns true when the form's VALUES changed
// (so the pane can mark itself dirty / invalidate a probe). Intents the form
// cannot service itself — Save, Close, Activate-on-Action — are returned to
// the caller via `escaped`/`fired` rather than handled here.
struct Applied {
    bool changed  = false;     // a value was mutated
    bool fired    = false;     // an Action row was activated
    bool hand_off = false;     // a Pick row wants its picker opened
    bool save     = false;     // ^S
    bool close    = false;     // Esc at the outermost level
};
Applied apply(Form& f, Action a);

} // namespace agentty::form::keys
