// form_keys.cpp — the one key map every form obeys.
//
// Pure: (focus, key) -> intent, then intent -> mutation. No pane re-derives
// navigation, so the panes cannot drift apart, and the whole interaction model
// is testable by feeding synthetic KeyEvents with no terminal attached.

#include "agentty/runtime/panel/form_keys.hpp"

#include "agentty/runtime/panel/nav.hpp"

namespace agentty::form::keys {

namespace {

using maya::KeyEvent;
using maya::SpecialKey;

[[nodiscard]] const SpecialKey* special(const KeyEvent& ev) {
    return std::get_if<SpecialKey>(&ev.key);
}

} // namespace

std::optional<Action> translate(const Form& f, const KeyEvent& ev) {
    return translate(f.editing(), f.choosing(), ev);
}

std::optional<Action> translate(bool editing, bool choosing, const KeyEvent& ev) {
    const auto sk = special(ev);
    const auto ch = ui::nav::char_view(ev);   // folds raw ctrl bytes to letter+ctrl

    // ── Choosing: the floating list owns everything ──────────────────
    // A dropdown only ever holds an enum, so the key map is small on purpose:
    // no filter, no query. A set that needs searching is a Pick row that
    // opens a real picker instead.
    if (choosing) {
        if (sk) switch (*sk) {
            case SpecialKey::Escape: return Action{Intent::MenuCancel};
            case SpecialKey::Enter:  return Action{Intent::MenuCommit};
            case SpecialKey::Up:     return Action{Intent::MenuPrev};
            case SpecialKey::Down:   return Action{Intent::MenuNext};
            default: break;
        }
        if (ch && !ch->ctrl) {
            if (ch->c == U'k') return Action{Intent::MenuPrev};
            if (ch->c == U'j') return Action{Intent::MenuNext};
            if (ch->c == U' ') return Action{Intent::MenuCommit};
        }
        // Swallow everything else: an open menu must not leak keys to the
        // form beneath it.
        return Action{Intent::None};
    }

    // ── Editing: printable keys edit, they do not navigate ─────────
    if (editing) {
        if (sk) switch (*sk) {
            case SpecialKey::Escape:
            case SpecialKey::Enter:     return Action{Intent::LeaveField};
            // ↑/↓ leave the field AND move (apply() ends the edit first).
            // Without this the editing table swallowed them — Enter on a
            // text row then any arrow read as "the panel froze", because
            // the only exits were the two keys nothing advertised.
            case SpecialKey::Up:        return Action{Intent::MovePrev};
            case SpecialKey::Down:      return Action{Intent::MoveNext};
            case SpecialKey::Backspace: return Action{Intent::Backspace};
            case SpecialKey::Delete:    return Action{Intent::DeleteForward};
            case SpecialKey::Left:      return Action{Intent::CaretLeft};
            case SpecialKey::Right:     return Action{Intent::CaretRight};
            case SpecialKey::Home:      return Action{Intent::CaretHome};
            case SpecialKey::End:       return Action{Intent::CaretEnd};
            default: break;
        }
        if (ch && ch->ctrl) {
            if (ch->c == U'u') return Action{Intent::ClearField};
            if (ch->c == U'a') return Action{Intent::CaretHome};
            if (ch->c == U'e') return Action{Intent::CaretEnd};
            return Action{Intent::None};    // don't leak ^X to navigation
        }
        if (ch) return Action{Intent::Insert, ch->c};
        return Action{Intent::None};
    }

    // ── Browsing ─────────────────────────────────────────────────────
    if (sk) switch (*sk) {
        case SpecialKey::Escape:   return Action{Intent::Close};
        case SpecialKey::Enter:    return Action{Intent::Activate};
        case SpecialKey::Up:       return Action{Intent::MovePrev};
        case SpecialKey::Down:     return Action{Intent::MoveNext};
        case SpecialKey::Left:     return Action{Intent::AdjustDown};
        case SpecialKey::Right:    return Action{Intent::AdjustUp};
        case SpecialKey::Home:     return Action{Intent::MoveFirst};
        case SpecialKey::End:      return Action{Intent::MoveLast};
        case SpecialKey::PageUp:   return Action{Intent::MovePageUp};
        case SpecialKey::PageDown: return Action{Intent::MovePageDown};
        default: break;
    }
    if (ch && ch->ctrl) {
        if (ch->c == U's') return Action{Intent::Save};
        return std::nullopt;                // ^C and friends stay ambient
    }
    if (ch) {
        if (ch->c == U'k') return Action{Intent::MovePrev};
        if (ch->c == U'j') return Action{Intent::MoveNext};
        if (ch->c == U'h') return Action{Intent::AdjustDown};
        if (ch->c == U'l') return Action{Intent::AdjustUp};
        if (ch->c == U' ') return Action{Intent::Activate};
        if (ch->c == U'x') return Action{Intent::ResetField};
    }
    return std::nullopt;
}

Applied apply(Form& f, Action a) {
    Applied out;
    Field* row = f.focused();

    switch (a.intent) {
        case Intent::None: break;

        case Intent::MovePrev:
        case Intent::MoveNext:
            // Arrows while EDITING: leave the field (commit what was typed —
            // text edits are in-place, so "commit" is just dropping the
            // caret), then move. Ending the edit first keeps the invariant
            // that Editing focus always refers to the cursor row.
            if (f.editing()) (void)escape(f);
            move(f, a.intent == Intent::MovePrev ? -1 : +1);
            break;

        case Intent::MoveFirst: if (f.editing()) (void)escape(f);
                                 move_edge(f, /*last=*/false); break;
        case Intent::MoveLast:  if (f.editing()) (void)escape(f);
                                 move_edge(f, /*last=*/true);  break;
        // Viewport-sized strides. The form does not know the viewport (a
        // widget concern), so a fixed stride matching the default
        // panel_viewport_h keeps PgUp/PgDn meaningful without a layout
        // back-channel; move_page CLAMPS at the edges (no wrap) and skips
        // headers.
        case Intent::MovePageUp:   if (f.editing()) (void)escape(f);
                                   move_page(f, -10); break;
        case Intent::MovePageDown: if (f.editing()) (void)escape(f);
                                   move_page(f, +10); break;

        case Intent::AdjustDown:
        case Intent::AdjustUp:
            if (row && row->editable()) {
                adjust(row->value, a.intent == Intent::AdjustUp ? +1 : -1);
                f.dirty = true;
                out.changed = true;
            }
            break;

        case Intent::Activate: {
            const auto r = activate(f);
            out.fired    = (r == Activated::FiredAction);
            out.hand_off = (r == Activated::HandOff);
            out.changed  = (r == Activated::Changed);
            break;
        }

        case Intent::ResetField:
            // The pane owns what "default" means for a row, so this only
            // signals intent; a pane that has no default simply ignores it.
            break;

        case Intent::Save:  out.save  = true; break;
        case Intent::Close: out.close = escape(f); break;

        // ── Editing-only intents ──────────────────────────────────
        // Guarded by the TRUE focus, not the caller's belief. subscribe.cpp
        // translates a whole input batch against ONE FormFocus snapshot
        // (rebuilt only after the batch drains), so a paste of "abc\ndef"
        // translates d/e/f as Insert even though the \n already left the
        // field — without this guard those characters mutate a row the
        // user is no longer editing. The reducer is the only layer that
        // sees the real mode; intent from a stale snapshot dies here.
        case Intent::Insert:
            if (f.editing() && row && row->editable()) {
                insert(row->value, a.ch);
                out.changed = true;
            }
            break;
        case Intent::Backspace:
            if (f.editing() && row && row->editable()) {
                backspace(row->value);
                out.changed = true;
            }
            break;
        case Intent::DeleteForward:
            if (f.editing() && row && row->editable()) {
                delete_forward(row->value);
                out.changed = true;
            }
            break;
        case Intent::CaretLeft:
            if (f.editing() && row) move_cursor(row->value, -1);
            break;
        case Intent::CaretRight:
            if (f.editing() && row) move_cursor(row->value, +1);
            break;
        case Intent::CaretHome:
            if (f.editing() && row) cursor_home(row->value);
            break;
        case Intent::CaretEnd:
            if (f.editing() && row) cursor_end(row->value);
            break;
        case Intent::ClearField:
            if (f.editing() && row && row->editable()) {
                clear(row->value);
                out.changed = true;
            }
            break;
        case Intent::LeaveField:
            (void)escape(f);
            break;

        case Intent::MenuPrev:      dropdown_move(f, -1); break;
        case Intent::MenuNext:      dropdown_move(f, +1); break;
        case Intent::MenuCommit:    out.changed = dropdown_commit(f); break;
        case Intent::MenuCancel:    (void)escape(f); break;
    }

    if (out.changed) f.dirty = true;
    return out;
}

} // namespace agentty::form::keys
