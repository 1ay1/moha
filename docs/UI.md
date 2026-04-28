# moha UI — maya widget reference

What every widget in moha's UI accepts (Config schema) and what moha
fills in. Read this alongside [`RENDERING.md`](RENDERING.md), which
walks the visual hierarchy and data flow.

The architectural rule:

> moha builds **widget Configs** from `Model` state. maya widgets own
> all rendering. moha constructs no Elements.

Concrete: `src/runtime/view/{view,thread,composer,changes,statusbar,permission}.cpp`
contain zero `Element{...}`, zero `dsl::v(...)`, zero `dsl::text(...)`.
Each file is a function `Model → SomeWidget::Config`. The single
`view(m)` call materializes everything via one
`maya::AppLayout{...}.build()`.

---

## 1. Top-level entry — `view(m)`

```cpp
// src/runtime/view/view.cpp
maya::Element view(const Model& m) {
    return maya::AppLayout{{
        .thread        = thread_config(m),
        .changes_strip = changes_strip_config(m),
        .composer      = composer_config(m),
        .status_bar    = status_bar_config(m),
        .overlay       = pick_overlay(m),
    }}.build();
}
```

That's the entire host-side view layer. The body is one declarative
struct expression — no imperative composition, no `if` branches around
layout primitives.

---

## 2. `maya::AppLayout` — top-level frame

`maya/include/maya/widget/app_layout.hpp`

```cpp
struct AppLayout::Config {
    Thread::Config         thread;
    ChangesStrip::Config   changes_strip;
    Composer::Config       composer;
    StatusBar::Config      status_bar;
    std::optional<Element> overlay;          // nullopt = no overlay
};
```

Composes four nested widget Configs into a vstack with the Thread
growing to fill, then z-stacks an Overlay on top when present. Caller
provides one nested Config tree per frame; AppLayout invokes the
sub-widgets internally.

---

## 3. `maya::Thread` — conversation viewport

`maya/include/maya/widget/thread.hpp`

```cpp
struct Thread::Config {
    bool                                     is_empty = false;
    WelcomeScreen::Config                    welcome;
    std::vector<Turn::Config>                turns;
    std::optional<ActivityIndicator::Config> in_flight;
};
```

Owns the empty-vs-populated branch:

- `is_empty == true`  → renders `maya::WelcomeScreen{welcome}`
- `is_empty == false` → renders `maya::Conversation{turns, in_flight}`

moha-side adapter (`thread.cpp`):

```cpp
Thread::Config thread_config(const Model& m) {
    Thread::Config cfg;
    if (m.d.current.messages.empty()) {
        cfg.is_empty = true;
        cfg.welcome  = welcome_config(m);
        return cfg;
    }
    // … walk visible window, push Turn::Config per message …
    cfg.in_flight = in_flight_indicator(m);
    return cfg;
}
```

---

## 4. `maya::WelcomeScreen` — empty-thread splash

`maya/include/maya/widget/welcome_screen.hpp`

```cpp
struct WelcomeScreen::Config {
    std::vector<std::string> wordmark;        // typically 3 rows
    Color                    wordmark_color = Color::magenta();
    std::string              tagline;
    Element                  model_badge;     // pre-built (e.g. ModelBadge)
    std::string              profile_label;   // raw — widget small-caps's it
    Color                    profile_color  = Color::magenta();
    std::string              starters_title = "Try";
    std::vector<std::string> starters;
    std::string              hint_intro     = "type to begin";
    std::vector<Hint>        hints;           // {key, label, key_color}
    Color                    accent_color = Color::magenta();
    Color                    text_color   = Color::bright_white();
};
```

Widget owns: wordmark gradient (last row dim), centering, small-caps
title in starters card, bottom hint row layout. moha owns the brand
content (the `m o h a` glyphs, tagline copy, starter prompts).

---

## 5. `maya::Conversation` — turn list

`maya/include/maya/widget/conversation.hpp`

```cpp
struct Conversation::Config {
    std::vector<Element> turns;
    Element              in_flight;
    bool                 has_in_flight = false;
};
```

A vstack of turn Elements with thin dim `─` rules between consecutive
turns and an optional in-flight indicator at the bottom. Each `turns[i]`
comes from `Turn{tc}.build()`; Conversation doesn't know about Turn's
internal structure — it just receives pre-built Elements.

---

## 6. `maya::Turn` — single speaker turn

`maya/include/maya/widget/turn.hpp`

```cpp
struct Turn::Config {
    std::string           glyph;             // ✦ for assistant, ❯ for user
    std::string           label;             // "Opus 4.7", "You"
    Color                 rail_color;
    std::string           meta;              // "12:34 · 4.2s · turn 3"
    std::vector<BodySlot> body;              // typed; Turn auto-spaces between
    std::string           error;             // empty = no error banner
    bool                  checkpoint_above = false;
    std::string           checkpoint_label = "Restore checkpoint";
    Color                 checkpoint_color = Color::yellow();
};
```

`BodySlot` is the discriminated body variant:

```cpp
using BodySlot = std::variant<
    PlainText,             // user text path: { content, color }
    MarkdownText,          // markdown path:  { content }
    AgentTimeline::Config, // tool calls panel
    Permission::Config,    // inline permission card
    Element                // escape hatch — only for cached StreamingMarkdown
>;
```

Turn:
1. Renders the header row.
2. Walks each body slot, dispatching via `std::visit` to the matching
   widget. Inserts a blank line between consecutive non-empty slots —
   callers don't push spacers.
3. Optional error banner (`⚠` row).
4. Wraps in the bold left-only border (the speaker rail) at
   `rail_color`.
5. Optional `CheckpointDivider` above the rail.

**Why typed slots:** moha can't construct `Element{TextElement{}}` for
spacers anymore. Turn handles spacing itself; moha just lists the
content slots in order.

---

## 7. `maya::AgentTimeline` — Actions panel for tool calls

`maya/include/maya/widget/agent_timeline.hpp`

```cpp
struct AgentTimeline::Config {
    std::string                          title;           // " ACTIONS · 3/5 · Bash "
    Color                                border_color = Color::bright_black();
    int                                  frame        = 0;   // for breathing/spinner
    std::vector<AgentTimelineStat>       stats;            // {label, count, color}
    std::vector<AgentTimelineEvent>      events;
    std::optional<AgentTimelineFooter>   footer;           // {glyph, text, color, summary}
};

struct AgentTimelineEvent {
    std::string             name;              // "Bash", "Read"
    std::string             detail;            // "npm test  ·  exit 0"
    float                   elapsed_seconds = 0.0f;
    Color                   category_color  = Color::blue();
    AgentEventStatus        status          = AgentEventStatus::Pending;
    ToolBodyPreview::Config body;              // typed body — no Element
};
```

Widget owns: round border, title/footer rendering, tree glyph
selection (`──` / `╭─` / `├─` / `╰─` per position), status icon
(braille spinner / `✓` / `✗` / `⊘`), inter-event connector colors,
duration formatting, category-color application.

moha-side: `agent_timeline_config(msg, frame, rail_color)` walks
`msg.tool_calls`, computes done/total/elapsed, picks per-category
colors, builds the events vector. Each event's body is filled via
`tool_body_config(tc)` which returns a `ToolBodyPreview::Config`.

---

## 8. `maya::ToolBodyPreview` — discriminated body content

`maya/include/maya/widget/tool_body_preview.hpp`

Drives the content under each timeline event's `│` stripe. Picks one
of five renderers based on `kind`:

```cpp
enum class Kind {
    None,        // empty
    CodeBlock,   // dim'd head+tail preview
    Failure,     // CodeBlock in red
    EditDiff,    // multi-hunk per-side diff with head+tail elision
    GitDiff,     // unified diff with per-line +/-/@@ coloring
    TodoList,    // ✓ ◍ ○ checkbox list
};

struct Config {
    Kind kind = Kind::None;

    // CodeBlock / Failure / GitDiff
    std::string text;
    Color       text_color = Color::bright_white();

    // EditDiff
    std::vector<EditHunk> hunks;        // { old_text, new_text }

    // TodoList
    std::vector<TodoItem> todos;        // { content, status }

    // Tunables
    int code_head           = 4;
    int code_tail           = 3;
    int edit_head_per_side  = 6;
    int edit_tail_per_side  = 2;
    int max_edit_hunks_shown = 4;
    int max_todos_shown     = 8;
};
```

Widget owns: line splitting, head+tail elision math, the
`··· N hidden ···` middle marker, per-line styling for diff coloring,
todo glyph selection (`✓` completed / `◍` in_progress / `○` pending).

moha-side `tool_body_config(tc)` is pure data extraction:

| Tool                                     | Resulting Kind            |
|------------------------------------------|---------------------------|
| `edit` (with hunks)                      | `EditDiff`                |
| `write`                                  | `CodeBlock` (content arg) |
| `bash` / `diagnostics` (terminal)        | `CodeBlock` (stripped output) |
| `bash` (running, with progress text)     | `CodeBlock` (live stdout) |
| `git_diff` (terminal)                    | `GitDiff`                 |
| `read`/`list_dir`/`grep`/`glob`/etc.     | `CodeBlock` (output)      |
| `todo` (with todos)                      | `TodoList`                |
| any failed tool with output              | `Failure`                 |
| anything else                            | `None`                    |

---

## 9. `maya::Permission` — inline permission card

`maya/include/maya/widget/permission.hpp`

```cpp
struct Permission::Config {
    std::string tool_name;
    std::string description;
    bool        show_always_allow = false;
};
```

Renders the "tool wants to do X" prompt with `[y] allow [n] deny [a] always` keys.
moha-side `inline_permission_config(pp, tc)` is pure data:

```cpp
maya::Permission::Config inline_permission_config(
    const PendingPermission& pp, const ToolUse& tc)
{
    std::string desc;
    if (tc.name == "bash" || tc.name == "diagnostics") desc = tc.args.value("command", "");
    else if (tc.name == "read" || tc.name == "edit" /* … */) desc = tc.args.value("path", "");
    // … per-tool description extraction …

    Permission::Config cfg;
    cfg.tool_name = tc.name.value;
    cfg.description = desc.empty() ? pp.reason : desc;
    cfg.show_always_allow = true;
    return cfg;
}
```

---

## 10. `maya::CheckpointDivider`

`maya/include/maya/widget/checkpoint_divider.hpp`

```cpp
struct CheckpointDivider::Config {
    std::string label = "Restore checkpoint";
    Color       color = Color::yellow();
};
```

`─── [↺ Restore checkpoint] ───` — full-width rule that lives outside
the rail, above a turn. Triggered by `Turn::Config::checkpoint_above`.

---

## 11. `maya::ActivityIndicator`

`maya/include/maya/widget/activity_indicator.hpp`

```cpp
struct ActivityIndicator::Config {
    Color       edge_color = Color::cyan();
    std::string spinner_glyph;
    std::string label;
};
```

`▎ ⠋ streaming…` — floats at the bottom of the thread when the model
is mid-stream and the active turn has no Timeline visible (Timeline
already carries the in-flight signal).

---

## 12. `maya::ChangesStrip` — pending edits banner

`maya/include/maya/widget/changes_strip.hpp`

```cpp
struct ChangesStrip::Config {
    std::vector<FileChange> changes;
    Color border_color = Color::yellow();
    Color text_color   = Color::bright_white();
    Color accept_color = Color::green();
    Color reject_color = Color::red();
};
```

Header row (`Changes (N) … Ctrl+R review · A accept · X reject`) plus
a `maya::FileChanges` body with the file list. When `changes` is
empty, renders to an empty Element so the AppLayout slot collapses
without a host-side `if`.

---

## 13. `maya::Composer` — bordered input box

`maya/include/maya/widget/composer.hpp`

```cpp
struct Composer::Config {
    std::string text;
    int         cursor = 0;

    enum class State { Idle, AwaitingPermission, Streaming, ExecutingTool };
    State       state         = State::Idle;
    Color       active_color  = Color::cyan();    // when state is Streaming/ExecutingTool

    Color       text_color      = Color::bright_white();
    Color       accent_color    = Color::magenta();   // "primed" border, idle+text
    Color       warn_color      = Color::yellow();
    Color       highlight_color = Color::cyan();      // queue chip

    std::size_t queued = 0;
    ProfileChip profile;        // { label, color }

    bool expanded = false;
};
```

State drives:

- Border + prompt color (idle/streaming/awaiting/has-text → muted/active/warn/accent)
- Placeholder text ("type a message…" / "running tool — type to queue…")
- Prompt boldness (active/has-text → bold; empty-idle → dim)
- Height pin (during activity, height pins to `min_rows=3` to prevent
  vertical bobbing as layout reflows above)

Hint row is width-adaptive (drops `expand` then `newline` keys on
narrow widths). Right-side ambient indicators: queue depth, words /
~tokens counters, profile chip.

---

## 14. `maya::StatusBar` — bottom panel

`maya/include/maya/widget/status_bar.hpp`

Five fixed rows (always 5 — the status row never grows or shrinks, so
the composer above never bobs vertically when a toast appears).

```cpp
struct StatusBar::Config {
    PhaseSpec       phase;            // glyph, verb, color, breathing, frame, elapsed
    std::string     breadcrumb_title; // empty = hide
    TokenStreamSpec token_stream;     // show/rate/total/history/color/live
    Element         model_badge;      // pre-built (e.g. ModelBadge)
    ContextSpec     context;          // {used, max}

    std::string     status_text;      // empty = blank slot (1 row reserved)
    bool            status_is_error = false;

    std::vector<ShortcutRow::Binding> shortcuts;

    int breadcrumb_min_width    = 130;   // raise to 160 while streaming
    int token_stream_min_width  = 110;
    int ctx_bar_min_width       = 55;

    Color text_color = Color::bright_white();
};
```

Composes internal sub-widgets:

- `maya::PhaseAccent` (top + bottom) — soft `▔▔▔` / `▁▁▁` rule in phase color
- `maya::PhaseChip` — colored glyph + verb + elapsed; breathing animation
- `maya::ContextGauge` — fuel-gauge bar with green/amber/red zones
- `maya::ShortcutRow` — width-adaptive keyboard hints

---

## 15. `maya::PhaseChip` — phase indicator

`maya/include/maya/widget/phase_chip.hpp`

```cpp
struct PhaseChip::Config {
    std::string glyph;
    std::string verb;
    Color       color        = Color::cyan();
    bool        breathing    = false;
    int         frame        = 0;
    int         verb_width   = 10;     // 0 = drop verb (very narrow)
    float       elapsed_secs = -1.0f;  // < 0 = omit
};
```

Owns the breathing animation cadence (32-frame cycle, bold half / dim
half — slightly slower than resting heart-rate so the indicator feels
*alive* without becoming a tick). `verb_width` truncates-or-pads to
exactly N display columns so the chips to the right stay pinned as
the verb changes.

---

## 16. `maya::ContextGauge` — context-window fuel gauge

`maya/include/maya/widget/context_gauge.hpp`

```cpp
struct ContextGauge::Config {
    int  used     = 0;
    int  max      = 0;
    int  cells    = 10;       // bar width
    bool show_bar = true;     // false = drop bar + ratio (very narrow)
};
```

Owns: 1/8-gradation block bar with per-cell threshold coloring (cells
0–60% green, 60–80% amber, 80–100% red). When `used == 0`, renders a
dim placeholder slot the same width as the live version, so the
right-side chips don't shove leftward when the first usage event
fires mid-stream.

---

## 17. `maya::ShortcutRow` — width-adaptive hint row

`maya/include/maya/widget/shortcut_row.hpp`

```cpp
struct ShortcutRow::Binding {
    std::string key;
    std::string label;
    Color       key_color = Color::cyan();
    int         priority  = 0;     // higher = kept longer
};

struct ShortcutRow::Config {
    std::vector<Binding> bindings;
    int label_min_width = 110;     // < this drops labels (key-only)
    int full_min_width  = 55;      // < this drops lower-priority half
    Color text_color = Color::bright_white();
};
```

Helix / Lazygit / k9s style: bold key in default fg, dim label, no
chip background. Drops less-important bindings on narrow widths
(priority-sorted) and drops labels entirely below `label_min_width`.

---

## 18. `maya::PhaseAccent` — soft horizontal rule

`maya/include/maya/widget/phase_accent.hpp`

```cpp
struct PhaseAccent::Config {
    Color    color    = Color::cyan();
    Position position = Position::Top;     // Top → ▔, Bottom → ▁
};
```

Width-aware row of half-block glyphs in the phase color, dim. Reads as
a "soft state shelf" rather than a hard line — the color carries
app-state information without using extra chrome characters.

---

## 19. `maya::Overlay` — modal layer

`maya/include/maya/widget/overlay.hpp`

```cpp
struct Overlay::Config {
    Element base;
    Element overlay;        // empty Element + present=false collapses
    bool    present = false;
};
```

Z-stacks `overlay` over `base`, centered horizontally, pinned to the
bottom edge, with an opaque background so base content doesn't bleed
through. When `present = false` collapses to just `base`.

`AppLayout` accepts `std::optional<Element>` and translates internally
— host code never has to construct an empty placeholder Element.

---

## 20. moha adapter functions

Every per-section file in `src/runtime/view/`:

| File              | Function                          | Returns                            |
|-------------------|-----------------------------------|------------------------------------|
| `view.cpp`        | `view(m)`                         | `Element` (the one `.build()`)     |
| `view.cpp`        | `pick_overlay(m)`                 | `optional<Element>`                |
| `thread.cpp`      | `thread_config(m)`                | `Thread::Config`                   |
| `thread.cpp`      | `welcome_config(m)`               | `WelcomeScreen::Config`            |
| `thread.cpp`      | `turn_config(msg, idx, n, m)`     | `Turn::Config`                     |
| `thread.cpp`      | `agent_timeline_config(msg, …)`   | `AgentTimeline::Config`            |
| `thread.cpp`      | `tool_body_config(tc)`            | `ToolBodyPreview::Config`          |
| `thread.cpp`      | `in_flight_indicator(m)`          | `optional<ActivityIndicator::Config>` |
| `thread.cpp`      | `cached_markdown_for(msg, …)`     | `Element` *(only escape hatch)*    |
| `composer.cpp`    | `composer_config(m)`              | `Composer::Config`                 |
| `changes.cpp`     | `changes_strip_config(m)`         | `ChangesStrip::Config`             |
| `statusbar.cpp`   | `status_bar_config(m)`            | `StatusBar::Config`                |
| `permission.cpp`  | `inline_permission_config(pp,tc)` | `Permission::Config`               |

Pure data helpers (no maya types touched): `speaker_style_for`,
`format_turn_meta`, `tool_timeline_detail`, `tool_event_status`,
`tool_display_name`, `tool_category_color`, `tool_category_label`,
`assistant_elapsed`, `running_tool_name`, `ordered_rate_history`.

The single `Element`-returning function is `cached_markdown_for` — it
exists because `maya::StreamingMarkdown` is stateful (per-block parse
cache must persist across frames). moha holds the widget instance,
calls `set_content()` per frame, and slots `instance.build()` into a
Turn body via the typed `Element` variant. No `Element{...}` literal,
no `dsl::*` call.

---

## 21. Caching

```cpp
// include/moha/runtime/view/cache.hpp
struct MessageMdCache {
    std::shared_ptr<maya::Element>           finalized;
    std::shared_ptr<maya::StreamingMarkdown> streaming;
};

[[nodiscard]] MessageMdCache& message_md_cache(const ThreadId& tid,
                                               std::size_t msg_idx);
```

One thread-local cache, keyed on `(thread_id, msg_idx)`. Streaming
messages hold a live `StreamingMarkdown` instance whose internal
block-cache makes each delta `O(new_chars)`; finalized messages cache
the resulting `Element` once and return the same pointer forever.

The previous `tool_card_cache` was removed when the rendering pipeline
collapsed onto `AgentTimeline + ToolBodyPreview`.

---

## 22. The DSL (for widget authors and overlay modals)

`maya/include/maya/dsl.hpp`. moha's main view files don't import it
anymore — they only build Configs. But:

1. **Widget authors** use it inside `maya/include/maya/widget/*.hpp`
   when implementing `build()`.
2. **Overlay modals** in moha (`login.cpp`, `pickers.cpp`,
   `diff_review.cpp`) still construct elements via DSL; they predate
   the controller-only refactor and will be widgetized next.

Quick reference (full primer in `maya/include/maya/dsl.hpp` header
comments):

| Form                          | Returns                                         |
|-------------------------------|-------------------------------------------------|
| `t<"...">`                    | Compile-time TextNode                           |
| `text(s)` / `text(s, style)`  | Runtime TextNode                                |
| `v(c1, c2, …)`                | Vertical box (`FlexDirection::Column`)          |
| `h(c1, c2, …)`                | Horizontal box (`FlexDirection::Row`)           |
| `spacer()`                    | Flex-grow gap (`grow=1.0f`)                     |
| `blank()`                     | Empty 1-line text                               |
| `when(cond, then, else?)`     | Conditional branch                              |
| `map(range, proj)`            | Project a range into nodes                      |
| `dyn([&]{ return E; })`       | Runtime escape hatch                            |
| `\| Bold` / `\| Dim` / `\| Italic` | Compile-time style pipe                    |
| `\| fg<0xHEX>` / `\| Fg<R,G,B>`  | Compile-time foreground color               |
| `\| pad<T,R,B,L>` / `\| border_<Round>` / `\| grow_<1>` | Compile-time layout pipe |
| `\| fgc(c)` / `\| padding(...)` / `\| border(BS)` / `\| bcolor(c)` / `\| btext(s, pos, align)` / `\| grow(f)` / `\| height(h)` / `\| width(w)` | Runtime layout pipes |

Element types (variants of `maya::Element`):

| Variant            | Purpose                                              |
|--------------------|------------------------------------------------------|
| `TextElement`      | Single line of text + optional `vector<StyledRun>`   |
| `BoxElement`       | Container with layout + border + children            |
| `ElementList`      | Heterogeneous list (rare; `v(...)` produces this)    |
| `ComponentElement` | Lazy `(w,h) → Element` callback for width-aware UI   |

The widget-author idiom is "build a node tree with the DSL, optionally
wrap with runtime pipes for dynamic colors / borders, return the
`.build()` result." Inside the widget you can use `Style{}.with_fg(c).with_bold()`
for runtime styling that doesn't need pipes.

---

## 23. Pending widgetization

Three host files still build elements directly — overlay modals that
predate the strict controller-only rule:

- `src/runtime/view/login.cpp` — login modal
- `src/runtime/view/pickers.cpp` — model picker, thread list, command palette, todo modal
- `src/runtime/view/diff_review.cpp` — pending-changes review modal

Future widgets to absorb them:

- `maya::LoginModal`
- `maya::Picker` (or `CommandPalette` + `ThreadList` + `TodoModal`)
- `maya::DiffReview`

Once those land, every host file under `src/runtime/view/` is a pure
data adapter and the `using namespace maya::dsl` line disappears from
moha entirely.
