# 08 — Composer (the message editor at the bottom)

The composer is the bottom-of-panel input region. It's where the user
writes new messages, attaches context (files, fetches, threads),
toggles modes, and sends. In Zed this is `MessageEditor`
(`crates/agent_ui/src/message_editor.rs`); in moha this is the only
*interactive* widget on screen most of the time, so it carries a lot
of weight.

This doc covers:

1. Outer layout (the bordered box)
2. Editor area (multi-line text input)
3. Context strip (mention chips above the editor)
4. Toolbar (send button, mode toggles, model selector)
5. Token meter
6. Slash commands and `@` mentions
7. Submit / cancel / regenerate flows
8. Streaming (Stop button, disabled state)
9. Edit mode (when bubble enters edit, this is the embedded composer)
10. Visual checklist

## 1. Outer layout

```
                 ┌─ Composer ────────────────────────────────────┐
   gap-1 above → │ [@src/main.cpp] [↑1234.txt]                    │ ← context strip
                 │                                                │
                 │ Find every file in src/ that uses              │ ← editor (multi-line)
                 │ MAYA_LEGACY_API and replace it with v2 API     │
                 │                                                │
                 │ /                                              │ ← (slash command popup overlays here)
                 │                                                │
                 │ ─────────────────────────────────────────────── │
                 │ ❯ Write   gpt-4 ▾   ⚡ Fast  ◯ Think  12k/200k │ ← toolbar
                 │                          [↵ Send]    or [⏸ Stop]│
                 └────────────────────────────────────────────────┘
```

The container:

```cpp
Element views::composer(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    auto border_col = m.composer.focused
        ? tokens::border::focus
        : tokens::border::dim;

    return (v(
        // Context strip (chips), only when non-empty
        when(!m.composer.context.empty(),
             [&]{ return views::composer_context_strip(m); }),

        // Editor area
        views::composer_editor(m),

        // Slash command popup (inline, above toolbar)
        when(m.composer.slash_popup_open,
             [&]{ return views::slash_popup(m); }),

        // Divider
        text(std::string(120, '\xe2\x94\x80'))   // ─
            | Style{}.with_fg(tokens::border::base) | Dim,

        // Toolbar
        views::composer_toolbar(m)
    )
    | border(BorderStyle::Round)
    | bcolor(border_col)
    | bg_(tokens::bg::editor)
    | padding(0, 1, 0, 1)
    | max_width(Dimension::fixed(120))
    | align_self_<Align::Center>
    ).build();
}
```

Notes:

- One bordered box, **focus-tinted** when the editor has focus.
- Same 120-cell max width + center alignment as the message stream
  above it. They visually share the column.
- The `padding(0, 1, 0, 1)` is for the *content inside the box*. The
  border itself eats one cell on each side.
- One row of `gap_<1>` separates the composer from the message stream.

## 2. Editor area

This is a multi-line text input. maya has two candidates:

| Widget | Suitability | Why |
|---|---|---|
| `maya::Input` (`input.hpp`) | OK for one-line, mediocre for multi-line | Has `multiline=true` but lacks line-numbered viewport, scroll-on-overflow |
| `maya::TextArea` (`textarea.hpp`) | Better — has line scrolling, cursor up/down across lines, paste handling | Has `line_numbers` toggle (turn off for composer) |

Use **`maya::TextArea`** with line numbers off:

```cpp
TextAreaConfig cfg{
    .line_numbers    = false,
    .show_line_count = false,
    .max_lines       = 12,    // viewport height; scrolls past this
    .min_lines       = 3,     // grow from this floor
};
TextArea editor(cfg);
editor.set_placeholder("Send a message…");
editor.set_value(m.composer.text);
editor.on_change([](std::string_view v){ /* dispatch ComposerEdit{v} */ });
editor.on_submit([](std::string_view v){ /* dispatch ComposerSubmit{v} */ });
```

The editor is **borderless** — the outer composer box already has a
border. Don't double-border.

Inside the composer it gets `padding(0, 0, 0, 0)` and grows
horizontally. Vertical size grows with content from `min_lines` (3)
up to `max_lines` (12), then internally scrolls.

### Cursor and key handling

`TextArea` already handles:
- Arrow keys (left/right/up/down across lines)
- `Home`/`End` (line start/end)
- `Ctrl+A`/`Ctrl+E` (line start/end, emacs-style)
- `Ctrl+K` (kill to end of line)
- `Backspace` / `Delete`
- Plain insertion + paste

We layer on top:
- **`Enter`** → submit (dispatch `ComposerSubmit`)
- **`Shift+Enter`** → insert newline (default TextArea behavior)
- **`Esc`** → if popups are open, close them; else clear focus
- **`Ctrl+C`** → cancel current stream (dispatch `StreamCancel`)
- **`/` at line start** → open slash command popup
- **`@`** → open mention popup
- **`Up` at first line** → cycle to previous user message in history
  (dispatch `ComposerHistoryPrev`)
- **`Down` at last line** → cycle to next message in history

Wire this by intercepting key events in the composer's update arm
*before* delegating to `TextArea`. If a key matches a composer-level
binding, handle it; otherwise pass to `TextArea`.

### Placeholder

When the value is empty, render the placeholder in `text_subtle`
color. `TextArea`'s `set_placeholder` does this automatically. Use:

| Mode | Placeholder text |
|---|---|
| Idle, empty thread | `"What would you like me to help with?"` |
| Idle, mid-thread | `"Reply to assistant…"` |
| Streaming | `"Streaming response… press Esc to interrupt"` (and disable input) |
| Awaiting permission | `"Decide on the permission card above…"` (disabled) |
| Editing a previous message | `"Edit your message…"` |

Pick based on `m.phase` and `m.composer.mode`.

## 3. Context strip (mention chips)

When the user has attached context (files, fetches, threads, search
results), they appear as **chips** above the editor. One row, wraps
to additional rows if needed.

```
[@src/main.cpp]  [↑README.md]  [⌕ "MAYA_LEGACY_API"]  [⌘ url:zed.dev]
```

Chip shape:

```
[<icon> <label> <×>]
```

- Surrounded by `[` and `]`.
- Icon depends on context kind (see § 6 / `05_design_tokens § 4`).
- Label: file path (truncated to 30 chars middle-ellipsis), or
  query, or URL.
- `×` (multiplication sign, U+00D7) is a delete affordance — pressing
  `Backspace` while the chip is "focused" (last in the list, just
  inserted) removes it.

Use `maya::Badge` as the chip primitive:

```cpp
Badge chip;
chip.label = "src/main.cpp";
chip.fg    = tokens::fg::link;
chip.bg    = tokens::bg::element;
chip.border = tokens::border::dim;
```

Or assemble manually:

```cpp
auto file_chip = (h(
    text("[", muted),
    text(icon_for(ctx.kind), muted),
    text(" "),
    text(truncate_middle(ctx.label, 30), Style{}.with_fg(fg::link)),
    text(" ×", muted),
    text("]", muted)
) | bg_(tokens::bg::element)
  | padding(0, 0, 0, 0)).build();
```

Layout: `dsl::h(spacer_<0>, chip1, gap, chip2, gap, chip3, ...)` with
1-cell horizontal gap. Wrap to next row when overflowing 116 cells
(120 – 2 padding – 2 border).

### Adding context

Three ways context enters:

1. **`@` mention popup** — user types `@`, popup shows fuzzy file
   picker (recent files + search). Selecting inserts a chip.
2. **Drag-and-drop** — not in TUI; skip.
3. **Slash command** — e.g., `/file src/main.cpp` inserts a chip
   without keeping the slash command in the editor.

The chip is **stored as a structured field**, not as text in the
editor. Submitting the composer sends the chip metadata as a
separate "context" array alongside the editor text. (Anthropic's API
takes attachments as separate content blocks; for plain text, prepend
the file contents to the user message.)

## 4. Toolbar

Fixed bottom row of the composer. From left to right:

```
❯ Write   gpt-4 ▾   ⚡ Fast  ◯ Think  12k / 200k       [↵ Send]
```

| Element | Notes |
|---|---|
| **Profile badge** (`❯ Write`) | One of `Write`/`Ask`/`Minimal`. Click cycles. Color from `05_design_tokens § 1` (Profile / mode badges). |
| **Model selector** (`gpt-4 ▾`) | Click opens dropdown. See `11_navigation.md`. |
| **Fast Mode toggle** (`⚡ Fast`) | Click toggles. `⚡` = U+26A1. Color: yellow when on, dim when off. |
| **Thinking toggle** (`◯ Think` / `◉ Think`) | Click toggles. Open circle off, filled circle on. |
| **Token meter** (`12k / 200k`) | Read-only. Color shifts to amber > 80%, red > 95%. See § 5. |
| **Spacer**  | `dsl::spacer()` to push the Send button right. |
| **Send / Stop button** | Primary button, far right. See § 7. |

Layout:

```cpp
auto toolbar = (h(
    profile_badge(m),
    text("  "),
    model_selector_button(m),
    text("  "),
    fast_toggle(m),
    text("  "),
    thinking_toggle(m),
    text("  "),
    token_meter(m),
    spacer(),                       // pushes the send button right
    send_or_stop_button(m)
) | padding(0, 0, 0, 0)).build();
```

Each clickable element is a `maya::Button` with `ButtonVariant::Ghost`
(no border, just text):

```cpp
Button fast_btn;
fast_btn.set_label(m.composer.fast_mode ? "⚡ Fast" : "⚡ Fast");
fast_btn.set_variant(ButtonVariant::Ghost);
fast_btn.on_click([]{ /* dispatch ToggleFastMode */ });
```

For keyboard-only operation, each toolbar element has a hotkey:

| Element | Hotkey |
|---|---|
| Profile cycle | `Ctrl+P` |
| Model selector | `Ctrl+M` |
| Fast Mode | `Ctrl+F` |
| Thinking | `Ctrl+T` |
| Send | `Enter` (in editor) or `Ctrl+S` |
| Stop | `Esc` (when streaming) or `Ctrl+C` |

These all live in `12_keymap.md`; the toolbar's job is to surface
them visually so users can discover the affordances even if they
never click.

## 5. Token meter

```
12k / 200k        ← normal
156k / 200k       ← amber, >80%
194k / 200k ⚠     ← red, >95%
```

Format:

- `< 1000`: show literal (`"347 / 200k"`)
- `≥ 1000`: round to nearest k (`"12k"`, `"156k"`)
- Always include divider + budget (`/ 200k`)
- At >95%, append a `⚠` glyph

Color rules:

| Usage | Color | Notes |
|---|---|---|
| `usage / budget < 0.5` | `text_muted` | Default, nothing notable |
| `0.5 ≤ usage / budget < 0.8` | `text` | Normal text color |
| `0.8 ≤ usage / budget < 0.95` | `warning` | Amber |
| `usage / budget ≥ 0.95` | `error` | Red, with `⚠` |

Implementation:

```cpp
Element token_meter(const Model& m) {
    using namespace maya::dsl;
    int used = m.current.token_usage;
    int budget = m.current.token_budget;
    float frac = budget > 0 ? float(used) / float(budget) : 0.f;

    auto color = (frac >= 0.95f) ? tokens::status::error
               : (frac >= 0.80f) ? tokens::status::warning
               : (frac >= 0.50f) ? tokens::fg::text
               :                   tokens::fg::muted;

    std::string label = format_count(used) + " / " + format_count(budget);
    if (frac >= 0.95f) label += " \xe2\x9a\xa0";   // ⚠

    return text(label) | Style{}.with_fg(color);
}
```

`format_count(n)` returns `"347"`, `"12k"`, `"156k"`, etc.

When `usage > budget`, render `usage / budget` (e.g., `"212k / 200k"`)
in red — the model will likely refuse, the user should see why.

The budget value comes from per-model metadata. Default: 200k for
Claude 4.x, 128k for GPT-4. Centralize in
`include/moha/model_meta.hpp`.

## 6. Slash commands and `@` mentions

### Slash commands

Trigger: `/` typed at the **start of a line** (or as the first
character of an empty editor).

The slash popup is **inline** — it renders between the editor and the
toolbar, not as a floating overlay. This keeps the composer tall but
predictable. Layout:

```
┌────────────────────────────────────────────────┐
│ /he|                                           │ ← editor showing typed prefix
│                                                │
│  /help     Show keyboard shortcuts             │ ← popup
│  /history  Show recent threads                 │
│ ▶/hold     Pause streaming                     │ ← highlighted = focused
│                                                │
│ ──────────────────────────────────────────────  │
│ <toolbar>                                       │
└────────────────────────────────────────────────┘
```

Popup behaviors:
- Filters as the user types (substring match on command name).
- `↑`/`↓` move highlight; `Tab` and `Enter` accept; `Esc` close.
- Accepting replaces the editor's slash prefix with the full command
  + a space (or fires the command directly if zero-arg).

Use `maya::widget::popup` for the overlay element, but place it as a
*child* of the composer's `dsl::v`, not as an OS-level popup. This
avoids the layered z-axis problem entirely.

Initial command set (defer additions to a config later):

| Command | Effect |
|---|---|
| `/help` | Show keymap modal (`12_keymap.md`) |
| `/history` | Open thread history view |
| `/new` | Start new thread (with confirm if current has unsaved changes) |
| `/clear` | Clear current thread (with confirm) |
| `/model <name>` | Switch model |
| `/profile <name>` | Switch profile |
| `/file <path>` | Add a file as context |
| `/fetch <url>` | Add URL fetch as context |

### `@` mentions

Trigger: `@` typed anywhere.

The `@` popup is a **fuzzy file picker** — recent files at the top,
fuzzy-matched as the user types. Keys:

- `↑`/`↓` move highlight
- `Tab`/`Enter` accept (inserts a chip in the context strip and
  removes the `@<query>` from the editor)
- `Esc` close

Sources to populate the file list (in order):
1. Files in the current thread's context already
2. Files recently opened in the user's editor (read from
   `~/.config/zed/recent` or `~/.config/nvim/...` — defer wiring,
   start with cwd recursive walk)
3. All files under cwd, fuzzy-matched

The `@` popup uses the same inline-layout approach as the slash popup.

## 7. Submit / cancel / regenerate

### Submit

User presses `Enter` (and `Shift` is not held). Update arm:

```cpp
[&](ComposerSubmit) -> std::pair<Model, Cmd<Msg>> {
    if (m.composer.text.empty() && m.composer.context.empty())
        return {m, Cmd<Msg>::none()};
    if (m.phase == Phase::Streaming || m.phase == Phase::AwaitingPermission)
        return {m, Cmd<Msg>::none()};   // ignore — streaming/locked

    Message user_msg{
        .id        = uuid_v4(),
        .role      = Role::User,
        .text      = m.composer.text,
        .context   = m.composer.context,
        .checkpoint_id = generate_checkpoint_id(m.current),
    };

    Model m2 = std::move(m);
    m2.current.messages.push_back(std::move(user_msg));
    m2.composer.text.clear();
    m2.composer.context.clear();
    m2.composer.history_pos = std::nullopt;
    m2.phase = Phase::Streaming;

    return {std::move(m2), launch_stream_cmd(m2.current)};
},
```

Don't clear context until the message is in the thread. If the
network call fails immediately (e.g., no auth), the user shouldn't
have lost their attached files.

### Cancel (during streaming)

User presses `Esc` or `Ctrl+C`, or clicks the Stop button.

```cpp
[&](StreamCancel) -> std::pair<Model, Cmd<Msg>> {
    if (m.phase != Phase::Streaming) return {m, Cmd<Msg>::none()};
    m.phase = Phase::Idle;
    return {m, cancel_stream_cmd(m.stream_handle)};
},
```

The cancellation primitive in the SSE layer is "drop the connection."
Whatever partial assistant message exists stays as-is — don't truncate
or mark it specially. If a tool was mid-execution, mark its
`ToolUse.status = Cancelled`.

### Regenerate

User presses `Ctrl+R` while focused on a previous user message (or
`Ctrl+Enter` in the embedded edit composer). Removes everything after
that message and re-streams.

```cpp
[&](RegenerateFrom evt) -> std::pair<Model, Cmd<Msg>> {
    auto& msgs = m.current.messages;
    auto it = std::find_if(msgs.begin(), msgs.end(),
                           [&](auto& msg){ return msg.id == evt.id; });
    if (it == msgs.end()) return {m, Cmd<Msg>::none()};
    // Truncate everything *after* the user message
    msgs.erase(it + 1, msgs.end());
    // Apply edits to the user message itself if from edit-mode
    if (evt.new_text) it->text = *evt.new_text;
    m.phase = Phase::Streaming;
    return {m, launch_stream_cmd(m.current)};
},
```

The deleted assistant turn(s) and any tool results attached to them
go away — they have no checkpoint of their own. The user message
keeps its checkpoint id (so you can `RestoreCheckpoint` back to it
later).

## 8. Streaming state — Stop button

When `m.phase == Streaming`, the Send button becomes a Stop button:

```
[⠋ Stop]
```

- Glyph cycles through spinner frames (see `05_design_tokens § 5`).
- `Tick{80ms}` subscription advances the frame index.
- Variant: `ButtonVariant::Danger` (red border).
- Click: dispatches `StreamCancel`.

The editor itself is **not disabled** — the user can type the next
message while the current one streams. But pressing `Enter` while
streaming is a no-op (see Submit guard above). Tooltip / placeholder
hints at this: `"Streaming response… press Esc to interrupt"` lives
in the *placeholder* slot only when the editor is empty.

Some other panels disable the editor visually (gray out the text,
cursor, etc.). Don't — Zed lets you compose while streaming, and
that's a quality-of-life win for power users. Match.

## 9. Edit mode (embedded composer)

When a user message enters edit mode (`Enter` on the bubble), the
bubble's body becomes a smaller composer:

```
┌────────────────────────────────────────────────┐
│ Find every file in src/ that uses              │
│ MAYA_LEGACY_API and replace it with v2 API     │
│                                                │
│      [Esc] Cancel    [Ctrl+Enter] Regenerate   │
└────────────────────────────────────────────────┘
```

Differences from the main composer:

| Aspect | Main composer | Edit composer |
|---|---|---|
| Container | Bottom of panel | Inside the user bubble (replaces text) |
| Border | Round, focus tint | Inherits bubble border (focus tint) |
| Toolbar | Full | None — use chip row instead (`[Esc] Cancel  [Ctrl+Enter] Regenerate`) |
| Context strip | Yes | No (can't add new context to a past message) |
| Slash popup | Yes | No |
| `@` popup | No (mentions on past messages don't make sense) | No |
| Submit binding | `Enter` | `Ctrl+Enter` (because plain `Enter` should insert newline in edit mode — risk of accidental submit on an old message) |
| Cancel binding | `Esc` (clears focus) | `Esc` (exits edit mode, restores original text) |

Implementation:

```cpp
Element views::user_edit_composer(const Message& msg, const Model& m) {
    using namespace maya::dsl;
    TextAreaConfig cfg{
        .line_numbers    = false,
        .show_line_count = false,
        .max_lines       = 8,
        .min_lines       = 1,
    };
    TextArea editor(cfg);
    editor.set_value(m.composer.edit_buffer);
    editor.on_change([&](std::string_view v){
        /* dispatch ComposerEditEditing{msg.id, std::string{v}} */
    });

    return v(
        editor,
        text(""),
        h(
            spacer(),
            text("[Esc] Cancel") | Dim,
            text("    "),
            text("[Ctrl+Enter] Regenerate") | Style{}.with_fg(tokens::fg::link)
        )
    ).build();
}
```

`m.composer.edit_buffer` (separate from `m.composer.text`) holds the
in-progress edit so leaving and returning to edit mode preserves it.
The user's original message text isn't mutated until they press
`Ctrl+Enter`.

## 10. Composer state in the Model

Recommended struct:

```cpp
namespace moha {

struct ComposerState {
    std::string                       text;
    std::vector<ContextRef>           context;          // chips
    int                               cursor = 0;
    bool                              focused = false;
    bool                              fast_mode = false;
    bool                              thinking = false;
    Profile                           profile = Profile::Write;
    std::string                       model_id = "claude-opus-4-5";

    // History cycling (Up/Down arrows past start/end of editor)
    std::optional<size_t>             history_pos;

    // Slash command popup
    bool                              slash_popup_open = false;
    std::string                       slash_query;
    int                               slash_highlight = 0;

    // Mention popup
    bool                              mention_popup_open = false;
    std::string                       mention_query;
    int                               mention_highlight = 0;
    std::vector<FileRef>              mention_results;

    // Edit mode (per-message)
    std::optional<std::string>        editing_msg_id;
    std::string                       edit_buffer;
};

struct ContextRef {
    enum class Kind { File, Fetch, Search, Thread } kind;
    std::string label;        // display label (path, url, query)
    std::string payload;      // resolved content (file body, etc.)
};

} // namespace moha
```

### Persisting drafts

Save `composer.text` and `composer.context` to the thread file on
every change *that idles for more than 1 second* (debounced
auto-save). This way an accidental quit doesn't lose a long message.

Don't save on every keystroke — it'll thrash. A single `Tick{1s}`
+ "dirty" flag handles this:

```cpp
[&](ComposerEdit ev) {
    m.composer.text = ev.value;
    m.composer.dirty = true;
    return {m, Cmd<Msg>::none()};
},
[&](Tick) {
    if (m.composer.dirty) {
        m.composer.dirty = false;
        return {m, save_draft_cmd(m.current.id, m.composer)};
    }
    return {m, Cmd<Msg>::none()};
},
```

## 11. Visual checklist

After implementing the composer, verify:

- [ ] Composer is bordered, focus-tinted blue when editor focused
- [ ] Editor grows from 3 to 12 lines, then scrolls
- [ ] `Enter` submits, `Shift+Enter` inserts newline
- [ ] `Esc` clears popups before clearing focus
- [ ] Slash popup renders inline between editor and toolbar (not floating)
- [ ] `@` popup behaves the same way (inline, fuzzy file list)
- [ ] Context chips render above the editor, wrap to multi-row if needed
- [ ] Backspace at start of editor (when next to a chip) deletes the chip
- [ ] Toolbar shows profile, model, fast, thinking, token meter, send
- [ ] Send button is right-aligned (via `spacer()`)
- [ ] Token meter colors shift to amber > 80% and red > 95%
- [ ] During streaming, send button becomes `[⠋ Stop]` (spinner + Danger variant)
- [ ] Stop click dispatches StreamCancel
- [ ] Editor stays usable during streaming (typing allowed, Enter no-op)
- [ ] User bubble edit mode shows embedded composer, `Ctrl+Enter` to regenerate
- [ ] Drafts auto-save on 1s idle
- [ ] Composer remembers attached context across send (until cleared)
- [ ] Placeholder text changes per phase (idle / streaming / awaiting permission)
