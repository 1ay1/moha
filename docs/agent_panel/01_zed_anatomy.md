# 01 — Zed Agent Panel: Anatomy

This is the visual + behavioral spec of Zed's agent panel. It is the
**source of truth** for what moha is trying to be. Every doc downstream of
this one references the regions and elements named here.

All file refs are to `/Users/ayush/projects/zed/crates/agent_ui/src/`.

## 1. Overall layout (5 regions, top to bottom)

```
╔════════════════════════════════════════════════════════════════════╗
║ ┌─ panel chrome ────────────────────────────────────────────────┐  ║
║ │ [agent ▾]                              [⤢] [⌥H] [⋯]            │  ║   (1) chrome
║ └────────────────────────────────────────────────────────────────┘  ║
║                                                                    ║
║ ┌─ message stream (scrollable, follows tail when generating) ──┐  ║
║ │  ┌────────────────────────────────────────────────────────┐    │  ║
║ │  │ user message bubble                                    │    │  ║   (2) stream
║ │  └────────────────────────────────────────────────────────┘    │  ║
║ │                                                                │  ║
║ │   assistant text in markdown, no bubble.                       │  ║
║ │   ╭─ Read ─────────────────────────────────────╮                │  ║
║ │   │ src/main.rs                       0.2s  ⌃ │                │  ║   (3) tool cards
║ │   ╰────────────────────────────────────────────╯                │  ║
║ │                                                                │  ║
║ │   ─── ↺ Restore Checkpoint ─────────────────                   │  ║   (4) divider
║ └────────────────────────────────────────────────────────────────┘  ║
║                                                                    ║
║ ┌─ message editor (composer) ──────────────────────────────────┐  ║
║ │  Ask me anything…                                              │  ║
║ │                                                                │  ║   (5) composer
║ │  [⚡Fast] [🤔Thinking]              [+]  4.2k/200k    [Send ↵] │  ║
║ └────────────────────────────────────────────────────────────────┘  ║
╚════════════════════════════════════════════════════════════════════╝
```

The five regions are:

1. **Panel chrome** — top toolbar
2. **Conversation view / message stream** — scrollable history
3. **Tool call cards** — embedded inline in the stream
4. **Checkpoint dividers** — horizontal rules between turns
5. **Message editor** — bottom composer

Everything happens inside one panel; there are no full-screen modals.
Selectors are popovers anchored to chrome buttons. Settings and history
are routes that swap the conversation pane out, not overlays on top of it.

## 2. Panel chrome (toolbar at top)

`agent_panel.rs:4158-4507`

```rust
h_flex()
    .size_full()
    .max_w(max_content_width)   // hard cap so chrome doesn't sprawl
    .mx_auto()                  // center within panel
    .flex_none()
    .justify_between()          // left section vs right section
    .gap_2()
```

### Left section
- **Agent selector**: icon + agent name + chevron, opens a context menu of
  available agents. When the thread is empty (v2), this button gets the
  large `Default` style; when there's an active conversation, it shrinks
  to icon-only with a back-button affordance.
- Style: `ButtonStyle::Default`, or `TintColor::Accent` when its menu is
  open.
- Loading state pulses opacity `0.2 → 0.6` over 1s while a thread is
  loading.

### Right section
- **Full-screen toggle**: `IconName::Maximize` ⇄ `Minimize`,
  `IconSize::Small`
- **History button**: only visible if there's history; opens
  `OpenHistory` action (`Cmd+Shift+H`)
- **Options menu** (`⋯`): dispatches `ToggleOptionsMenu`

### State machine
- **Empty thread** → big agent-selector button, no back arrow
- **Active thread** → back arrow, agent icon, history button
- **History/config view** → hide thread-specific controls
- **Full screen mode** → maximize ⇄ minimize swap

### TUI substitutions
- Hover-only affordances become focus-visible chips, or always-visible at
  half opacity.
- `mx_auto` + `max_content_width` translates to: at very wide terminals,
  cap chrome width to ~120 cols and center.

## 3. Conversation view / message stream

`conversation_view/thread_view.rs:4426-4845`

Scrollable list, follows the tail by default during streaming, with key
bindings to scroll back.

```rust
list(self.list_state.clone(), …)
    .with_sizing_behavior(ListSizingBehavior::Auto)
    .flex_grow()
```

### 3a. User message — **bubble**

`thread_view.rs:4504-4650`

- Container: bordered, rounded, has a background tint.
- `border_1()` + `cx.theme().colors().border`
- `bg(cx.theme().colors().editor_background)`
- `rounded_md()` (~5px radius; in TUI this is just a `Round` border)
- `py_3()`, `px_2()` inside the bubble
- Text uses `MarkdownStyle::themed(MarkdownFont::Agent, …)`
- Font size `text_xs` (smaller than UI default)

#### States
- **Editable + has checkpoint**: a "Restore Checkpoint" button shows above
  the message with `Divider::horizontal()` on either side.
- **In edit mode (focused)**: border color → `border_focused` (accent);
  inline action buttons appear absolutely positioned top-right (`-3p5`
  top, `right_3`): Cancel ✕, Regenerate ↩.
- **Subagent message**: border becomes **dashed** (`border_dashed()`);
  `shadow_sm()` if indented.
- **Indented (subagent context)**: `pl_5()`, a 1px-wide left line
  `border.opacity(0.6)`, and `bg(panel_background.opacity(0.2))`.

### 3b. Assistant message — **inline, no bubble**

`thread_view.rs:4651-4721`

- No container; renders inline against the panel background
- `px_5()`, `py_1p5()` (so it sits flush with the content column)
- `gap_3()` between successive markdown chunks
- Markdown rendered through the `MarkdownFont::Agent` style

#### Sub-blocks
- **Regular text**: a `Markdown` entity, streaming-ready
- **Thinking block**: collapsible disclosure — `IconName::ToolThink`
  beside "View Thinking…" closed; expanded shows full thought; subtle
  bordered container, `border_1`, `rounded_md`
- **Blank messages**: filtered (`is_blank` check), never rendered

### 3c. Streaming behavior

- New text appears inline as chunks arrive
- No artificial typewriter delay — raw text appended
- No spinner inside the message; the **send button** in the composer
  shows the current "Stop" state, and the agent selector pulses

### 3d. Checkpoint / turn divider

`thread_view.rs:4517-4535`

```rust
h_flex()
    .px_3()
    .gap_2()
    .child(Divider::horizontal())
    .child(Button::new("restore-checkpoint", "Restore Checkpoint")
        .start_icon(Icon::new(IconName::Undo))
        .label_size(LabelSize::XSmall)
        .color(Color::Muted))
    .child(Divider::horizontal())
```

Appears between user message and following entries when:
- the message has `checkpoint.show == true`
- the thread supports truncate
- the user has edit permissions

Clicking restores all files to the snapshot at that message point.

## 4. Tool call cards

This is **the** signature visual element of Zed's agent UX. Get this
right and 70 % of the "feels like Zed" is done.

`thread_view.rs:6240-7550`

### 4a. Card structure

```
╭─ Tool icon · tool name ──────────── 0.3s · ⌄ ─╮   header
│ ······································       │
│ <body — diff, output, terminal, summary>     │   body
│ ······································       │
├──────────────────────────────────────────────┤
│ [✓ Allow]  [✕ Deny]      [Always for X ▾]    │   footer (when needs confirmation)
╰──────────────────────────────────────────────╯
```

Box layout:

```rust
v_flex()
    .my_1p5()                                       // 6px vertical margin
    .mx_5()                                         // 20px horizontal indent
    .rounded_md()                                   // ~5px corners
    .border_1()                                     // 1px border
    .border_color(self.tool_card_border_color(cx))  // border.opacity(0.8)
    .bg(cx.theme().colors().editor_background)
    .overflow_hidden()
```

### 4b. Border color & dashed-on-failure

```rust
fn tool_card_border_color(&self, cx) -> Hsla {
    cx.theme().colors().border.opacity(0.8)
}

.when(failed_or_canceled, |this| this.border_dashed())
```

**Translation rule**: in maya, normal cards use `BorderStyle::Round`;
failed/cancelled cards use `BorderStyle::Dashed`. Both colors come from
the same dim-ish border palette but the **failed card border tints
toward red** in moha (the original work in `bash_tool.hpp` etc. uses
`Color::rgb(120, 60, 65)`).

### 4c. Card header

```rust
h_flex()
    .group(&card_header_id)
    .relative()
    .w_full()
    .justify_between()
    .when(use_card_layout, |this| {
        this.p_0p5()
            .rounded_t(rems_from_px(5.))
            .bg(self.tool_card_header_bg(cx))   // element_bg + 2.5% of fg
    })
```

#### Left side (icon + tool name)

`thread_view.rs:7261-7276`:

```rust
match tool_call.kind {
    ToolKind::Read    => IconName::ToolSearch,
    ToolKind::Edit    => IconName::ToolPencil,
    ToolKind::Delete  => IconName::ToolDeleteFile,
    ToolKind::Move    => IconName::ArrowRightLeft,
    ToolKind::Search  => IconName::ToolSearch,
    ToolKind::Execute => IconName::ToolTerminal,
    ToolKind::Think   => IconName::ToolThink,
    ToolKind::Fetch   => IconName::ToolWeb,
    _                 => IconName::ToolHammer,
}
```

Icon color `Color::Muted`, `IconSize::Small`. Tool name is
markdown-rendered (so `Edit src/main.rs` can have a clickable file path).
Font size `rems_from_px(13.)`.

A right-side **gradient overlay** fades the long tool name into the
header background so a long path doesn't bleed into the right-side
controls (`thread_view.rs:7278-7303`).

#### Right side (status + disclosure)

- **Disclosure toggle**: `ChevronUp` / `ChevronDown`,
  `IconSize::XSmall` — only when content exists and not in confirmation
  state
- **Status icon**:
  - Pending / In-progress → spinner (rotating arrow, 1s cycle, opacity
    pulses 0.2 → 0.6)
  - Completed → ✓ check
  - Failed → red ✕
  - Cancelled → ✕
  - Rejected → (hidden)
- **Elapsed time**: only if `> 10s`, formatted via `duration_alt_display`,
  `LabelSize::XSmall`, `Color::Muted`, in parens

### 4d. Tool variants

#### Edit / Diff card (`thread_view.rs:7534+`)

- Body is an **inline diff viewer** — embedded mini-editor with syntax
  highlighting
- Hunk-level expand/collapse
- Loading state: a stack of 5 progressively shorter pulsing bars (`w_4_5`,
  `w_1_4`, `w_2_4`, `w_3_5`, `w_2_5`) — visualizes "writing the diff" before
  any actual diff arrives
- For TUI: see `10_diff_review.md`

#### Terminal / Execute card (`thread_view.rs:5877-6168`)

Header sub-row:

```
[working directory path (truncatable)]    ⏱ 12s  · Truncated · ⏸
```

Body (when expanded):

- Command display (collapsible via `render_collapsible_command`)
- Terminal output in a scrollable container
- `h_72()` (~72 rems height cap) — output beyond that scrolls inside the
  card
- `editor_background`, `text_xs`, buffer (mono) font

Error state: red ✕ in header if exit code ≠ 0; tooltip shows the exit
code.

#### Generic tool card

- Body is markdown output
- Inline images supported
- **Raw input section** (collapsible, default closed): a disclosure showing
  the raw input JSON the model sent the tool. `thread_view.rs:6328-6391`

### 4e. Permission footer

`thread_view.rs:6666-7083`. Documented in detail in
`09_permissions.md`. The summary:

- A `border_t_1()` separator inside the same card
- `p_1()`, `gap_2()`, `justify_between()`
- Left: `[✓ Allow]` `[✕ Deny]` (small buttons, `LabelSize::Small`)
- Right: a granularity dropdown — "Only this time", "Always for X",
  "Always for selected" — opened as a `PopoverMenu` anchored
  bottom-right

When the footer is present, the card auto-expands so the user can see
what they're approving.

## 5. Message editor (composer)

`message_editor.rs:1-160` + `thread_view.rs:3202-3350`

```rust
h_flex()
    .p_2()                          // 8px outer padding
    .bg(editor_bg_color)
    .justify_center()
    .border_t_1()                   // top border only when messages exist
    .border_color(cx.theme().colors().border)
    .child(
        v_flex()
            .flex_basis(max_content_width)
            .flex_shrink()
            .justify_between()
            .gap_2()
            .child(editor)         // multi-line, AutoHeight
            .child(controls_row)   // [Fast] [Think]   [+] tokens [Send]
    )
```

### 5a. Editor area
- Mode: `AutoHeight { min_lines: 1, max_lines: Some(N) }`
- Expanded mode (`Shift+Alt+Esc`): grows to `vh(0.8, window)` (80% of
  viewport)
- Font: `text_xs`, monospace-friendly
- Placeholder: "Ask me anything…" (varies per agent)

### 5b. Mention chips
- Inline pills inside the editor for `@file`, `@symbol`, `@thread`
- Triggered by `@` or via the `+` button (Add Context)
- Visually: rounded background with the mention text; clickable ✕ to remove

### 5c. Send button (bottom-right)

```rust
let (label, icon, color) = if generating {
    ("Stop", IconName::Stop, Color::Error)
} else {
    ("Send", IconName::Send, Color::Default)
};

Button::new("send", label)
    .label_size(LabelSize::Small)
    .start_icon(Icon::new(icon))
    .key_binding(/* Cmd+Enter or Shift+Enter */)
```

States:
- **Idle** with empty editor → grayed out
- **Idle** with content → "Send"
- **Generating** → "Stop", red icon

### 5d. Token / context indicator
- Below or beside the send button
- `format!("{} / {}", used_tokens, max_tokens)` in `LabelSize::XSmall`
- Color tier: `Color::Disabled` (low) → `Color::Muted` (mid) →
  `Color::Warning` (high)

### 5e. Fast Mode + Thinking toggles

`message_editor.rs:3712-3818` (~):

- **Fast Mode**: `[⚡ Fast]` button, `TintColor::Accent` when on
- **Thinking**: `[🤔 Thinking]` button, `TintColor::Accent` when on
- If thinking is on, an **effort selector** (Low / Medium / High) appears

These belong to a controls row above the send button.

## 6. Selectors (model / profile / mode)

Three popovers, each anchored to a button in chrome or near the composer.
See `11_navigation.md` for full detail.

Pattern (from `mode_selector.rs`, `profile_selector.rs`,
`model_selector_popover.rs`):

```rust
PopoverMenu::new("mode-selector")
    .trigger(button)              // closed: small button + chevron
    .menu(|window, cx| {
        ContextMenu::build(window, cx, |menu, _, cx| {
            for entry in entries {
                menu = menu.toggleable_entry(
                    entry.label, is_selected, IconPosition::End, …);
            }
            menu
        })
    })
    .anchor(Corner::TopRight)
    .attach(Corner::BottomRight)
```

- Closed: button with current value + `ChevronDown`,
  `LabelSize::Small`, `Color::Muted`
- Open: dropdown menu with checkmarked current entry
- Width: `rems(18.)`–`rems(20.)`; max height `rems(20.)` with scroll

## 7. Empty / error / streaming states

### 7a. Empty thread
- Centered prompt: "What would you like me to help with?"
- Large agent icon + name
- Composer expands to fill the space (no top border)

### 7b. Error callouts (inline, between messages)

`thread_view.rs:8182-8280`:

```rust
Callout::new()
    .icon(IconName::AlertCircle)
    .title("Execution Failed")
    .description(stderr_text)
    .bg(cx.theme().status().error.opacity(0.1))
```

Variants:
- Rate limit → orange callout, "Rate limit reached"
- Server overload → orange, "Provider unavailable"
- Auth required → blue info callout, with "Authenticate" action
- Invalid API key → red error callout
- No model selected → prominent error in empty thread

### 7c. Streaming
- No spinner inside messages
- The send button shows "Stop"
- The agent selector pulses
- Text and tool deltas write into the conversation in place

## 8. Color tokens used

(From `cx.theme().colors()` — full mapping in `05_design_tokens.md`.)

| Token | Used for |
|---|---|
| `editor_background` | message body bg, tool card bg, composer bg |
| `editor_foreground` | primary text |
| `text` | UI text |
| `text_muted` | secondary text, dim labels |
| `border` | dividers, card outlines (× 0.8 opacity for cards) |
| `border_focused` | focused message bubble border (accent) |
| `element_background` | tool card header bg base |
| `element_hover` | hover bg |
| `element_selected` | selected entry bg |
| `panel_background` | panel/sidebar bg, indented subagent bg (× 0.2) |
| `status().warning` | rate limit, near token-limit |
| `status().error` | failed tool, invalid key |

## 9. Spacing rhythm (rems → terminal cells)

Zed uses a base rem of ~16px. Terminals don't have rems; we round to
cells. The translation table lives in `05_design_tokens.md` but the
common values are:

| Zed | px | TUI cells |
|---|---|---|
| `gap_0p5` | 2 | 0 (or merge) |
| `gap_1` | 4 | 1 |
| `gap_2` | 8 | 1 |
| `gap_3` | 12 | 2 |
| `px_2` | 8 | 1 |
| `px_3` | 12 | 1–2 |
| `px_5` | 20 | 2 |
| `py_1` | 4 | 0–1 |
| `py_2` | 8 | 1 |
| `py_3` | 12 | 1 |

The principle: keep the **relative rhythm**. If something is twice the
gap of something else in Zed, keep that ratio in the TUI even if the
absolute cell count is smaller.

## 10. Keyboard

Full spec in `12_keymap.md`. The Zed defaults from
`assets/keymaps/default-macos.json`:

| Key | Action |
|---|---|
| `Cmd+N` | New thread |
| `Cmd+Shift+H` | Open history |
| `Cmd+Alt+/` | Toggle model selector |
| `Cmd+I` | Toggle profile selector |
| `Shift+Tab` | Cycle mode |
| `Alt+Tab` | Cycle favorite models |
| `Cmd+Shift+J` | Navigation menu |
| `Cmd+Alt+M` | Options menu |
| `Cmd+>` | Add selection to thread |
| `Cmd+Shift+Enter` | Continue thread |
| `Cmd+Y` | Allow once |
| `Cmd+Alt+A` | Open permission dropdown |
| `Cmd+Alt+Z` | Reject once |
| `Shift+Alt+Esc` | Expand composer |
| `PageUp/Down` | Scroll |
| `Shift+PageUp/Down` | Jump to prev/next message |

TUI substitutes (terminals don't always pass `Cmd`):
- `Cmd+Enter` → `Ctrl+Enter` (or just `Enter` if multiline is `Alt+Enter`)
- `Cmd+Y` → `Y` (alone, when permission footer is focused)
- `Cmd+Alt+/` → `Ctrl+/`
- `Cmd+Shift+H` → `Ctrl+H`
- etc.

## 11. What this means for moha — the gap list

Specific gaps the rebuild has to close (cross-referenced from the moha
audit):

1. **Modal overlays** for model picker / thread list / command palette /
   diff review — should become **integrated panels or popovers**, not
   v-stacked overlays at the bottom of the screen.
2. **No scrolling viewport** in the thread panel — `ScrollThread` Msg
   exists in `src/main.cpp` but is never wired to a `Scrollable` widget.
3. **Diff review** is currently a separate modal (`src/main.cpp:673-716`)
   — should be inline diff cards inside the conversation, with a
   persistent right-side panel for batch review.
4. **Composer** is a bare bordered input — no mention chips, no
   send/stop button, no token meter, no Fast/Think toggles.
5. **Permission footer** is rendered (good), but the granularity dropdown
   is missing — moha only offers `Y / N / A` with one auto-derived
   pattern.
6. **No agent selector** in chrome.
7. **No checkpoint restore** — the divider renders but the action is a
   stub (`src/main.cpp:1182`).
8. **Tool cards** use `Round` borders correctly and `Dashed` on failure
   (good); but the **header gradient fade** for long names is missing,
   and **elapsed time** isn't shown.
9. **Thinking blocks** aren't rendered as collapsible disclosures.
10. **Status indicators** in the chrome (the agent pulse during loading)
    are absent.

Use `13_rebuild_playbook.md` to sequence the work to close these.
