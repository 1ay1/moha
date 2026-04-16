# 12 — Keymap (the complete keybinding reference)

This is the **canonical key reference** for the agent panel. Every
binding here must be:

1. Implemented in the update arm
2. Surfaced in the help modal (`11_navigation.md § 6`)
3. Reachable without a mouse

If a binding isn't in this doc, it doesn't exist. If a doc references
a binding not listed here, the binding goes here first.

## 1. Modifier conventions

The TUI lives in a terminal — modifiers are constrained:

| Notation | Maps to | Notes |
|---|---|---|
| `Ctrl+X` | `KEY_CTRL('X')` | Universally available |
| `Alt+X` / `Meta+X` / `⌥X` | `KEY_ALT('X')` | Terminal must send the ESC prefix; iTerm2/Ghostty/Alacritty do |
| `Shift+X` | `KEY_SHIFT('X')` | Shift is implicit for capital letters |
| `Cmd+X` | (not supported in TUI) | Substitute with `Ctrl+X`; document |
| `Enter` | `\n` / `\r` | Same on most terminals |
| `Esc` | `\x1B` | Solo Esc; not the prefix to other keys |
| `Tab` / `Shift+Tab` | `\t` / `KEY_BACKTAB` | |
| `↑↓←→` | arrow escape sequences | |
| `PageUp` / `PageDown` | `KEY_PPAGE` / `KEY_NPAGE` | |
| `Home` / `End` | `KEY_HOME` / `KEY_END` | |
| `F1`-`F12` | `KEY_F(n)` | Used sparingly |

`Cmd` doesn't reach the TUI on macOS — the OS swallows it. Replace
all Zed `Cmd+X` bindings with `Ctrl+X` on translation.

## 2. Global bindings (active everywhere)

| Key | Action | Msg |
|---|---|---|
| `Ctrl+H` | Open History route | `OpenRoute{History}` |
| `Ctrl+M` | Open Model selector popover | `OpenPopover{Model}` |
| `Ctrl+P` | Open Profile selector popover | `OpenPopover{Profile}` |
| `Ctrl+,` | Open Settings route | `OpenRoute{Settings}` |
| `Ctrl+N` | Start new thread | `NewThread{}` |
| `Ctrl+W` | Close current popover/modal/route | (varies) |
| `Ctrl+L` | Clear the message stream view (cosmetic; doesn't delete data) | `RedrawAll{}` |
| `?` | Open help modal (only when no input has focus) | `OpenModal{Help}` |
| `Ctrl+/` | Same as `?` (always works) | `OpenModal{Help}` |
| `F5` | Refresh current view (re-fetch state) | `Refresh{}` |
| `F11` | Toggle fullscreen-ish (hide chrome + status bar) | `ToggleFullscreen{}` |
| `Ctrl+Q` | Quit moha (with confirm if streaming) | `Quit{}` |

## 3. Composer bindings

Active when focus is on the composer text area or its toolbar.

### Editing

| Key | Action |
|---|---|
| `Enter` | Submit current text (if not empty + idle) |
| `Shift+Enter` | Insert newline |
| `Ctrl+J` | Insert newline (alternative) |
| `Backspace` | Delete previous char (or chip if at start) |
| `Delete` | Delete next char |
| `Ctrl+Backspace` / `Alt+Backspace` | Delete previous word |
| `Ctrl+W` | Delete previous word (when in editor — overrides global "Close popover" only when popover is closed) |
| `Ctrl+K` | Kill to end of line |
| `Ctrl+U` | Kill to start of line |
| `Ctrl+Y` | Yank (paste from kill ring) |
| `Ctrl+A` / `Home` | Move to start of line |
| `Ctrl+E` / `End` | Move to end of line |
| `←` / `→` | Move one char |
| `↑` / `↓` | Move one line within editor; if at first/last, history scroll |
| `Ctrl+←` / `Ctrl+→` | Move one word |
| `PageUp` / `PageDown` | Scroll the editor viewport |

### Composer-level actions

| Key | Action |
|---|---|
| `Esc` | If popover open, close it. Else if streaming, cancel. Else clear focus / blur composer. |
| `Ctrl+C` | Cancel current stream (always, even with focus elsewhere) |
| `Ctrl+R` | Regenerate the last assistant turn |
| `Ctrl+T` | Toggle Thinking mode |
| `Ctrl+F` | Toggle Fast mode |
| `Ctrl+S` | Submit (alternative to Enter, for users who set Enter = newline) |
| `Ctrl+D` | Open diff review (10_diff_review.md) |
| `Ctrl+Shift+R` | Reset thread (clear messages, keep settings) |

### Mention / slash

| Key | Action |
|---|---|
| `@` (any position) | Open mention popup |
| `/` (line start) | Open slash command popup |
| `Tab` (in popup) | Accept highlighted item |
| `↑` / `↓` (in popup) | Move highlight |
| `Esc` (in popup) | Close popup |

## 4. Message stream bindings

Active when focus is on the message stream area (not composer).

| Key | Action |
|---|---|
| `↑` / `↓` | Scroll line up/down |
| `PageUp` / `PageDown` | Scroll page up/down |
| `Home` | Scroll to top |
| `End` | Scroll to bottom (re-engages tail follow) |
| `Shift+PageUp` | Jump to previous message + focus it |
| `Shift+PageDown` | Jump to next message + focus it |
| `g g` | Vim: scroll to top |
| `G` | Vim: scroll to bottom |
| `Tab` | Move focus between focusable items (messages, tool cards, dividers) |
| `Shift+Tab` | Reverse focus |

### Per-message (when focused)

#### User message

| Key | Action |
|---|---|
| `Enter` | Enter edit mode (composer becomes embedded in the bubble) |
| `Ctrl+R` | Regenerate from this message (truncates everything after, restreams) |
| `Ctrl+C` | Copy message text to clipboard |
| `D` | Delete this message and everything after (with confirm) |

#### Assistant message

| Key | Action |
|---|---|
| `Ctrl+C` | Copy entire assistant turn (text + tool outputs) |
| `D` | Open diff review for any edits in this turn |
| `R` | Same as `Ctrl+R` (regenerate) — easier reach when stream-focused |

### Per-tool-card (when focused)

| Key | Action |
|---|---|
| `Enter` / `Space` | Toggle expand/collapse |
| `Y` / `A` | Allow (only when status = Confirmation) |
| `N` / `D` | Deny (only when Confirmation) |
| `Shift+A` | Always allow (Confirmation, persistent) |
| `Shift+D` | Always deny (Confirmation, persistent) |
| `Ctrl+C` | Copy command/path/url (whatever's relevant for the tool kind) |
| `O` | Open file in `$EDITOR` (Read/Edit/Write tools) |
| `J` | Jump to next tool card |
| `K` | Jump to previous tool card |

### Per-checkpoint-divider (when focused)

| Key | Action |
|---|---|
| `Enter` / `R` | Restore checkpoint (with confirm; see `06_message_stream § 6`) |
| `D` | Delete checkpoint (only the marker; the messages stay) |

## 5. Diff review route bindings

Active when `m.route == DiffReview`.

| Key | Action |
|---|---|
| `↑` / `↓` (file list focus) | Switch active file |
| `Tab` | Toggle focus between file list and hunk view |
| `J` | Next hunk in current file |
| `K` | Previous hunk |
| `A` | Accept focused hunk |
| `R` | Reject focused hunk |
| `Shift+A` | Accept all hunks in current file |
| `Shift+R` | Reject all hunks in current file |
| `Enter` | Apply (accepted hunks; pending → accepted by default) |
| `Shift+Enter` | Apply ALL hunks regardless of decision |
| `Esc` | Close route (changes remain queued) |
| `g g` / `G` | Top/bottom of hunk view |
| `O` | Open the active file in `$EDITOR` |
| `D` | Toggle word-level intra-line diff |

## 6. History route bindings

Active when `m.route == History`.

| Key | Action |
|---|---|
| `↑` / `↓` | Move selection |
| `Enter` | Open the focused thread |
| `N` | New thread |
| `D` | Delete focused thread (with confirm) |
| `R` | Rename focused thread (inline edit) |
| `E` | Export focused thread |
| `/` | Focus search bar |
| `Esc` | Close route |

## 7. Popovers / modals (when open)

| Key | Action |
|---|---|
| `↑` / `↓` | Move highlight |
| `Enter` / `Tab` | Accept highlighted item |
| `Esc` | Close popover/modal without selection |
| `Ctrl+W` | Same as Esc (close) |
| `/` | Filter (in popovers that support search, e.g., model selector) |

## 8. Conflicts and precedence

When the same key has multiple meanings, the highest-priority handler
wins:

```
1. Modal (highest priority — captures everything)
2. Popover (captures everything but Esc)
3. Active route (history, diff review, settings)
4. Composer (when focused)
5. Message stream / focused card
6. Global
```

Example: `Ctrl+W` in a popover closes the popover (level 2). `Ctrl+W`
in the composer when no popover is open deletes the previous word
(level 4). `Ctrl+W` with no focus closes the topmost route (level
6 → falls back to global).

The dispatcher walks the stack from highest to lowest, calling
`handle_key()` on each level. Each `handle_key()` returns:

- `Handled` — stop dispatching
- `NotHandled` — fall through to next level

```cpp
KeyDispatchResult handle_global_key(KeyEvent ev, Model& m, EventBus& bus);

void route_key_event(KeyEvent ev, Model& m, EventBus& bus) {
    if (m.modal) {
        if (handle_modal_key(ev, *m.modal, m, bus) == Handled) return;
    }
    if (m.popover) {
        if (handle_popover_key(ev, *m.popover, m, bus) == Handled) return;
    }
    switch (m.route) {
        case Route::DiffReview:
            if (handle_diff_review_key(ev, m, bus) == Handled) return;
            break;
        case Route::History:
            if (handle_history_key(ev, m, bus) == Handled) return;
            break;
        // ...
    }
    if (m.composer.focused) {
        if (handle_composer_key(ev, m, bus) == Handled) return;
    }
    if (m.focused_message) {
        if (handle_message_key(ev, m, bus) == Handled) return;
    }
    handle_global_key(ev, m, bus);
}
```

## 9. Differences from Zed

Zed uses `Cmd` heavily (macOS GPUI). Translations:

| Zed | moha |
|---|---|
| `Cmd+Enter` | `Enter` (composer submit) — Zed needs `Cmd` to disambiguate from newline; we make `Shift+Enter` newline by default |
| `Cmd+K` | `Ctrl+K` |
| `Cmd+/` | `Ctrl+/` |
| `Cmd+Shift+P` (command palette) | Slash command popup; no separate palette |
| `Cmd+P` (file picker) | `@` mention |
| `Cmd+Click` | (no equivalent in TUI; rely on focus + Enter) |
| `Cmd+,` (settings) | `Ctrl+,` |
| `Cmd+W` (close pane) | `Ctrl+W` |
| Right-click context menu | Press `Tab` to focus, then a key to act (no menu surface) |

If a Zed binding has no good TUI equivalent (mouse hover popovers,
e.g.), document the gap and pick a reasonable substitute.

## 10. Customization

Defer customization. Hardcode the bindings above in
`include/moha/keymap.hpp`. Adding `~/.config/moha/keymap.json`
support is a quality-of-life feature; not needed for the rebuild.

When customization is added later, the structure should be:

```json
{
  "version": 1,
  "bindings": {
    "Ctrl+M":   { "action": "OpenPopover", "args": { "kind": "Model" }},
    "Ctrl+H":   { "action": "OpenRoute",   "args": { "target": "History" }}
  }
}
```

Validate at load time; fall back to defaults for any unknown key.

## 11. Discoverability checklist

For every binding above, verify that:

- [ ] It actually fires the listed `Msg` (test in update arm)
- [ ] It appears in the help modal (sourced from `key_help_entries()`)
- [ ] If it appears in a context-sensitive hint (e.g., status bar),
      that hint is correct
- [ ] If two bindings overlap (e.g., `Ctrl+W`), the precedence order
      gives the user-friendly behavior
- [ ] It has a Zed equivalent OR is explicitly noted as a TUI-only
      addition

The single source of truth is `key_help_entries()` (the table that
feeds the help modal). If you add a binding to the update arm
without adding it there, it's undiscoverable — which means it
doesn't exist in any meaningful sense.

## 12. Visual checklist

After implementing the keymap, verify:

- [ ] Help modal opens with `?` and `Ctrl+/`
- [ ] Help modal lists all sections: Composing, Stream, Tool cards, etc.
- [ ] `Esc` exits popovers, modals, routes in that priority order
- [ ] `Ctrl+C` cancels stream regardless of focus
- [ ] Vim bindings (`g g`, `G`, `J`, `K`) work in scrollable surfaces
- [ ] No global binding shadows a useful composer binding when composer is focused
- [ ] Status bar shows the most useful key for the current context
- [ ] All bindings work in iTerm2, Ghostty, Alacritty, Terminal.app
- [ ] Where Terminal.app limits modifiers (e.g., Alt), there's a `Ctrl` fallback
