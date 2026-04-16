# 06 — Message stream (the conversation list)

The middle region of the panel, between chrome and composer. This is
where the conversation lives: user bubbles, assistant text, tool cards,
checkpoint dividers, error callouts. It's vertically scrollable and
follows the tail when streaming.

## 1. Layout

The container:

```cpp
Element views::message_stream(const Model& m) {
    using namespace maya::dsl;

    if (m.current.messages.empty()) {
        return views::empty_state(m);
    }

    std::vector<Element> rows;
    for (size_t i = 0; i < m.current.messages.size(); ++i) {
        rows.push_back(render_message_block(m.current.messages[i], i, m));
    }
    if (m.stream_error) {
        rows.push_back(views::stream_error_callout(*m.stream_error));
    }

    auto stack = (v(std::move(rows)) | gap_<1>).build();

    // Wrap in a scrollable, capped at content max width
    auto column = (v(stack)
        | max_width(Dimension::fixed(120))
        | align_self_<Align::Center>
    ).build();

    return Scrollable(column)
        .with_offset(m.scroll.offset)
        .build();
}
```

Notes:

- `gap_<1>` between message blocks gives the breathing room. Don't
  encode it as `padding(1, ...)` on each block — that double-counts.
- `max_width(120)` + `align_self::Center` is the moha equivalent of
  Zed's `max_w(max_content_width).mx_auto()`.
- Wrap the **column**, not the individual messages. This keeps the
  centering math local to one box.

## 2. Empty state

```cpp
Element views::empty_state(const Model& m) {
    return (v(
        spacer(),
        h(spacer(),
          v(
              text("◆") | Bold | Style{}.with_fg(tokens::fg::muted),
              text("") ,
              text("What would you like me to help with?")
                | Style{}.with_fg(tokens::fg::text),
              text(""),
              text("Tell me to read a file, run a command, or design something.")
                | Dim
          ),
          spacer()),
        spacer()
    )).build();
}
```

A vertically + horizontally centered prompt. No "Send a message" button
— Zed doesn't have one, the editor below is the action. The icon (◆) is
moha's "agent identity" mark; pick a glyph that survives in 80-col
terminals.

## 3. Render dispatch per message

```cpp
Element render_message_block(const Message& msg, size_t idx, const Model& m) {
    using namespace maya::dsl;

    std::vector<Element> parts;

    // Checkpoint divider (only above user messages with a checkpoint id
    // AND only if not the first message)
    if (msg.role == Role::User && msg.checkpoint_id && idx > 0) {
        parts.push_back(views::checkpoint_divider(*msg.checkpoint_id));
    }

    if (msg.role == Role::User) {
        parts.push_back(views::user_bubble(msg, m));
    } else if (msg.role == Role::Assistant) {
        for (auto& chunk : views::assistant_chunks(msg, m)) {
            parts.push_back(std::move(chunk));
        }
    }

    return (v(std::move(parts)) | gap_<1>).build();
}
```

A "block" is one user message OR one assistant turn (which may itself
contain text + N tool cards + maybe more text). Tool cards belong to
the assistant turn that produced them.

## 4. User bubble

`thread_view.rs:4504-4650` is the Zed reference.

```
┌────────────────────────────────────────────────┐
│ Find every file in src/ that uses              │
│ MAYA_LEGACY_API and replace it with the v2 API │
└────────────────────────────────────────────────┘
```

```cpp
Element views::user_bubble(const Message& msg, const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    auto border_col = msg.editing
        ? tokens::border::focus
        : tokens::border::dim;

    // Body: either text or an embedded edit composer
    Element body;
    if (msg.editing) {
        body = views::user_edit_composer(msg, m);   // see 08_composer.md
    } else {
        body = (v(text(msg.text)
            | Style{}.with_fg(tokens::fg::text)
            | wrap_<TextWrap::Wrap>)
        ).build();
    }

    // Right-side action chips when editing
    Element chips = msg.editing
        ? h(
            text("[Esc] Cancel") | Dim,
            text("  ") | Dim,
            text("[Ctrl+Enter] Regenerate") | Style{}.with_fg(tokens::fg::link)
          ).build()
        : Element{TextElement{}};

    return (v(body, chips)
        | border(BorderStyle::Round)
        | bcolor(border_col)
        | bg_(tokens::bg::editor)
        | padding(1, 1, 1, 1)
    ).build();
}
```

Behaviors:

- Default state: bordered box, dim border, body text, no actions.
- `Enter` while focused on the bubble (or click — when mouse enabled) →
  enters edit mode (`editing = true`). The bubble's body becomes a
  composer pre-filled with the message text. Border switches to focus
  color.
- Edit composer: same widget as the main composer, but smaller, and
  Enter / Esc dispatch `RegenerateFrom{msg.id}` / `CancelEdit`.

### Subagent message variant

When a message is from a subagent (which moha may not have today, but
the data model can carry an `agent_id` field on `Message` to mark it),
the bubble:

- Border: `BorderStyle::Dashed` (already dashed when `agent_id` is not
  the primary)
- Indent: `padding(0, 0, 0, 4)` left padding, plus a single-cell wide
  vertical line on the left at `border::dim` color

For now, skip the subagent variant. Add it if/when moha supports
multi-agent threads.

## 5. Assistant chunks

The assistant message in moha's data model is a single entity with
`text` + `tool_calls`. In Zed it's split into "blocks" by the model
output (text → tool → text → tool → text). Render the same shape.

```cpp
std::vector<Element> views::assistant_chunks(const Message& msg, const Model& m) {
    using namespace maya::dsl;
    std::vector<Element> chunks;

    // 1) Body text (streaming if not finalized)
    std::string_view body = msg.text.empty() ? msg.streaming_text : msg.text;
    if (!body.empty()) {
        chunks.push_back((v(markdown(std::string(body)))
            | padding(0, 2, 0, 2)).build());
    }

    // 2) Tool cards (in order)
    for (const auto& tc : msg.tool_calls) {
        chunks.push_back(views::tool_card(tc, msg, m));
    }

    return chunks;
}
```

Notes:

- Assistant text has **no border**. It sits inline against the
  conversation background, indented `padding(0, 2, 0, 2)`.
- Streaming behavior is "render the accumulated text every frame" —
  see `04_architecture.md` § for `markdown` vs `StreamingMarkdown`.
- Tool cards belong to the same gap rhythm as everything else (the
  parent's `gap_<1>` separates them).

### Code blocks

`maya::markdown` should already render fenced code blocks with
`tokens::bg::editor` background. If not, wrap manually:

```cpp
auto code_block = [](std::string_view code, std::string_view lang) {
    return (v(text(code)
        | Style{}.with_fg(Color::rgb(220, 220, 220))
        | wrap_<TextWrap::NoWrap>)
        | bg_(tokens::bg::element)
        | padding(0, 1, 0, 1)
        | btext(lang.empty() ? std::string{""} : " " + std::string(lang) + " ",
                BorderTextPos::Top, BorderTextAlign::Start)
        | border(BorderStyle::Round)
        | bcolor(tokens::border::dim)
    ).build();
};
```

For now, trust `maya::markdown` and only override if you see a problem.

### Thinking blocks

When the model emits an extended thinking block (Claude's `thinking`
content type), render as a collapsible disclosure — closed by default:

```
▶ View Thinking…
```

Open:

```
▼ Thinking
  The user wants me to find files using MAYA_LEGACY_API. Let me start
  by searching the src/ directory…
```

```cpp
Element views::thinking_block(const ThinkingChunk& th, const Model& m) {
    using namespace maya::dsl;

    Disclosure::Config cfg{
        .label = th.expanded ? "Thinking" : "View Thinking…",
        .open_icon = "▼",
        .closed_icon = "▶",
    };
    Disclosure d(cfg);
    d.set_expanded(th.expanded);

    if (!th.expanded) return Element(d);

    auto body = (v(markdown(th.text))
        | padding(0, 2, 0, 2)
        | Style{}.with_fg(tokens::fg::muted)).build();

    return d.build(body);
}
```

Today, the moha data model has no `ThinkingChunk` — Claude's `thinking`
content arrives in `StreamTextDelta` undifferentiated. Adding a
distinct streaming type is a model-side change (anthropic.cpp). Track
this as a deferred enhancement in `13_rebuild_playbook.md`.

## 6. Checkpoint divider

`thread_view.rs:4517-4535`:

```
─── ↺ Restore Checkpoint ─────────────────
```

```cpp
Element views::checkpoint_divider(std::string_view checkpoint_id) {
    using namespace maya::dsl;

    auto dim_style    = Style{}.with_fg(tokens::fg::subtle);
    auto label_style  = Style{}.with_fg(tokens::fg::muted);

    return h(
        text("─── ", dim_style),
        text("↺ Restore Checkpoint", label_style),
        text(" ", label_style),
        text("───────────────────────────────────────────────",
             dim_style)
            | wrap_<TextWrap::TruncateEnd>,
        spacer()
    ).build();
}
```

The trailing dash run is just a long string truncated to fit. We don't
have a real "horizontal rule that grows to the available width"
primitive — `divider.hpp` has one, but it doesn't compose well with the
inline label.

### Behavior
- Press `Ctrl+Z` while focused on a checkpoint divider → dispatch
  `RestoreCheckpoint{id}`.
- Visual feedback: when the user moves focus across messages with
  `Shift+PageUp / Shift+PageDown`, highlight the focused divider
  (border-focused color on the label).
- `Restore` returns the working tree (and the thread state) to the
  snapshot at this point. moha's persistence layer does not yet
  implement snapshot/restore (`src/main.cpp:1182` is a stub) — see
  `13_rebuild_playbook.md`.

For the initial rebuild, render the divider but make `RestoreCheckpoint`
a no-op + toast: "Checkpoint restore is not implemented yet". Don't
hide the divider — its absence breaks the visual rhythm.

## 7. Stream error callout

When `m.stream_error` is set:

```cpp
Element views::stream_error_callout(std::string_view error) {
    using namespace maya::dsl;

    return (v(
        h(
            text("⚠ ") | Style{}.with_fg(tokens::status::error),
            text("Stream error") | Bold | Style{}.with_fg(tokens::status::error),
            spacer()
        ),
        text(""),
        text(error) | Style{}.with_fg(tokens::fg::muted) | wrap_<TextWrap::Wrap>,
        text(""),
        h(
            text("[R]") | Style{}.with_fg(tokens::fg::link),
            text(" Retry  "),
            text("[Esc]") | Style{}.with_fg(tokens::fg::link),
            text(" Dismiss")
        ) | Dim
    )
    | border(BorderStyle::Round)
    | bcolor(tokens::status::error)
    | bg_(Color::rgb(58, 36, 38))    // tokens::status::error_bg
    | padding(1, 1, 1, 1)
    ).build();
}
```

For network errors (rate limit, overload), use orange/warning instead
of red/error. For auth errors, use blue/info plus an explicit
`[L] Login` action.

The `R` (retry) handler should re-issue the last `launch_stream_cmd`.
The `Esc` handler clears `m.stream_error`.

## 8. Scrolling

### Scroll state

```cpp
struct ScrollState {
    int offset = 0;        // current scroll offset, 0 = top
    bool at_bottom = true; // are we currently following the tail?
};
```

### Behavior

- **At init**: `at_bottom = true`, `offset = 0` (or `END_SENTINEL`).
- **On user scroll up** (PgUp / arrow up): `at_bottom = false`,
  decrement `offset`.
- **On user scroll back to bottom**: `at_bottom = true` (detect via
  `Scrollable`'s `scrolled_to_end` callback).
- **On every `StreamTextDelta` / `StreamToolUseStart`**: if
  `at_bottom`, set `offset = END_SENTINEL` so the next layout pins to
  bottom.
- **On `Tick`** (every 80ms during streaming): if `at_bottom`, force
  `offset = END_SENTINEL` to keep up with content growth.

### Key bindings

| Key | Action |
|---|---|
| `↑` / `↓` | Scroll line up/down |
| `PgUp` / `PgDn` | Scroll page up/down |
| `Home` | Scroll to top |
| `End` | Scroll to bottom (sets `at_bottom = true`) |
| `Shift+PgUp` | Jump to previous message (move focus too) |
| `Shift+PgDn` | Jump to next message |

### Implementation note

maya's `Scrollable` (`maya/include/maya/widget/scrollable.hpp`)
provides the viewport. moha currently doesn't wrap its message stack in
one — the gap is documented in the audit and tracked in the rebuild
playbook.

## 9. Focus and per-message actions

Focus moves between **focusable items**: each message, each tool card
disclosure, each checkpoint divider. `Tab` moves to the next; `Shift+Tab`
moves back.

Per-message actions when focused:

- **User message**: `Enter` → enter edit mode; `Cmd+R` → regenerate
- **Assistant message**: `Enter` on a tool card → toggle expand/collapse;
  `D` → open diff (if applicable)
- **Checkpoint divider**: `Enter` → restore checkpoint

For the initial rebuild, you can defer Tab navigation and just rely on
keyboard shortcuts dispatched globally. Add focus traversal once the
shape settles.

## 10. Streaming visual feedback

What changes during streaming:

- **Composer's send button** says "Stop" with a spinner glyph
  (`⠋ Stop`) — drives via the `Tick{}` subscription
- **Chrome agent badge** pulses dim ↔ normal at 1s cycle
- **The last message's text grows** as deltas arrive (no special UI on
  the message itself)
- **Tool card status icon** is the spinner when running
- **No** progress bars, no separate "thinking…" line below the message

This restraint is part of what makes Zed's UI feel calm. Resist the
temptation to add more spinners.

## 11. Auto-save hook

After each `StreamFinished` (and on permission decisions), persist the
thread:

```cpp
[&](StreamFinished) -> std::pair<Model, Cmd<Msg>> {
    auto [m2, cmd] = finalize_turn(std::move(m));
    m2.current.updated_at = std::chrono::system_clock::now();
    return {m2, Cmd<Msg>::batch({cmd, save_thread_cmd(m2.current)})};
},
```

Don't save on every text delta — it'd thrash the disk. Once per
turn-end is enough.

## 12. Visual checklist

After implementing the message stream, verify:

- [ ] Empty thread shows centered prompt (not a blank panel)
- [ ] User messages render in bordered bubbles
- [ ] Assistant text renders without a bubble, indented
- [ ] gap-1 between messages, no double-padding
- [ ] Conversation column max-width 120 cells, centered in wider terminals
- [ ] Streaming text appears incrementally, no flicker
- [ ] Checkpoint divider renders between turns when `checkpoint_id` set
- [ ] Scrolling works with PgUp/PgDn
- [ ] Auto-follow tail during streaming, breaks when user scrolls up
- [ ] Stream errors render as inline callout, not a banner
- [ ] No spinners inside individual messages
