# moha UI — maya widget reference

What every widget in moha's UI accepts (Config schema) and what moha
fills in. Read this alongside [`RENDERING.md`](RENDERING.md), which
walks the visual hierarchy and data flow.

The architectural rule:

> moha builds **widget Configs** from `Model` state. maya widgets own
> all rendering. moha constructs no Elements.

Concrete: every `.cpp` under `src/runtime/view/` (except the legacy
overlay files in §24) contains zero `Element{...}`, zero `dsl::v(...)`,
zero `dsl::text(...)`. Each file is a function `Model → SomeWidget::Config`
and **one widget = one adapter file**: filenames mirror the widget
they adapt, and the directory layout mirrors the widget hierarchy
(see §25). The single `view(m)` call materializes everything via one
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
layout primitives. Each field is filled by exactly one adapter
function from a sibling file.

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

Adapter: `view.cpp::view(m)` (this is also the AppLayout adapter —
view.cpp builds the AppLayout::Config inline since it's the entry
point).

---

## 3. `maya::Thread` — conversation viewport

`maya/include/maya/widget/thread.hpp`

```cpp
struct Thread::Config {
    bool                  is_empty = false;
    WelcomeScreen::Config welcome;        // when is_empty
    Conversation::Config  conversation;   // when !is_empty
};
```

Owns the empty-vs-populated branch:

- `is_empty == true`  → renders `maya::WelcomeScreen{welcome}`
- `is_empty == false` → renders `maya::Conversation{conversation}`

Adapter: `thread/thread.cpp::thread_config(m)`.

```cpp
Thread::Config thread_config(const Model& m) {
    Thread::Config cfg;
    if (m.d.current.messages.empty()) {
        cfg.is_empty = true;
        cfg.welcome  = welcome_screen_config(m);
        return cfg;
    }
    cfg.conversation = conversation_config(m);
    return cfg;
}
```

Three lines: pick the branch, delegate to the sub-adapter for either
WelcomeScreen or Conversation. No element work, no rendering logic.

---

## 4. `maya::WelcomeScreen` — empty-thread splash

`maya/include/maya/widget/welcome_screen.hpp`

```cpp
struct WelcomeScreen::Config {
    std::vector<std::string> wordmark;        // typically 3 rows
    Color                    wordmark_color = Color::magenta();
    std::string              tagline;
    Element                  model_badge;     // pre-built (e.g. ModelBadge)
    std::string              profile_label;
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

Adapter: `thread/welcome_screen.cpp::welcome_screen_config(m)`.

---

## 5. `maya::Conversation` — turn list

`maya/include/maya/widget/conversation.hpp`

```cpp
struct Conversation::Config {
    std::vector<Turn::Config>                turns;
    std::optional<ActivityIndicator::Config> in_flight;
};
```

A vstack of turns (each one rendered by `maya::Turn`) separated by
thin dim `─` rules, with an optional `maya::ActivityIndicator` at
the bottom. The widget builds the Turn from each `Turn::Config`
internally — the host never sees an Element here.

Adapter: `thread/conversation.cpp::conversation_config(m)`. Walks the
visible message window (`m.ui.thread_view_start..end`), calls
`turn_config()` per message, then asks `activity_indicator_config()`
whether to show the bottom chip.

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

Adapter: `thread/turn/turn.cpp::turn_config(msg, idx, n, m)`.

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

Adapter: `thread/turn/agent_timeline/agent_timeline.cpp::agent_timeline_config(msg, frame, rail_color)`.
Walks `msg.tool_calls`, computes done/total/elapsed, picks per-category
colors, builds the events vector. Each event's body is filled via
`tool_body_preview_config(tc)`.

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
```

Adapter: `thread/turn/agent_timeline/tool_body_preview.cpp::tool_body_preview_config(tc)`.
Pure data extraction:

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

Pure helpers shared with `AgentTimeline` adapter live in
`thread/turn/agent_timeline/tool_helpers.cpp` (display name, category
color/label, event status, timeline detail) and
`thread/turn/agent_timeline/tool_args.cpp` (arg extraction).

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
Lives as a body slot inside `Turn::Config::body` when a tool is
awaiting approval.

Adapter: `thread/turn/permission.cpp::inline_permission_config(pp, tc)`.

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
the rail, above a turn. Triggered by `Turn::Config::checkpoint_above`
(no separate adapter — Turn's adapter sets the flag based on
`msg.checkpoint_id`).

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

Adapter: `thread/activity_indicator.cpp::activity_indicator_config(m)`
returns `optional<ActivityIndicator::Config>` so the bottom slot
collapses cleanly when nothing is in flight.

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

Adapter: `changes_strip.cpp::changes_strip_config(m)`.

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

Adapter: `composer.cpp::composer_config(m)`.

---

## 14. `maya::StatusBar` — bottom panel

`maya/include/maya/widget/status_bar.hpp`

Five fixed rows (always 5 — the status row never grows or shrinks, so
the composer above never bobs vertically when a toast appears).
StatusBar::Config nests **typed sub-widget Configs** so each
sub-widget gets its own moha adapter:

```cpp
struct StatusBar::Config {
    Color phase_color = Color::cyan();      // top/bottom PhaseAccent + leading rail

    // Activity row sub-widget configs.
    TitleChip::Config            breadcrumb;       // empty title = hide
    PhaseChip::Config            phase;
    TokenStreamSparkline::Config token_stream;
    Element                      model_badge;
    ContextGauge::Config         context;          // max=0 = hide

    // Status row.
    StatusBanner::Config         status_banner;

    // Shortcut row.
    ShortcutRow::Config          shortcuts;

    // Width thresholds for activity-row pieces.
    int breadcrumb_min_width    = 130;
    int token_stream_min_width  = 110;
    int ctx_bar_min_width       = 55;
    int phase_verb_min_width    = 50;     // < this drops phase verb
    int phase_elapsed_min_width = 80;     // < this drops phase elapsed
};
```

Adapter: `status_bar/status_bar.cpp::status_bar_config(m)` is a thin
composer — calls the sub-adapters and assembles:

```cpp
maya::StatusBar::Config status_bar_config(const Model& m) {
    StatusBar::Config cfg;
    cfg.phase_color   = phase_color(m.s.phase);
    cfg.breadcrumb    = title_chip_config(m);
    cfg.phase         = phase_chip_config(m);
    cfg.token_stream  = token_stream_sparkline_config(m);
    cfg.model_badge   = model_badge_config(m).build();
    cfg.context       = context_gauge_config(m);
    cfg.status_banner = status_banner_config(m);
    cfg.shortcuts     = shortcut_row_config(m);
    // … width thresholds …
    return cfg;
}
```

---

## 15. `maya::TitleChip` — leading-edge title chip

`maya/include/maya/widget/title_chip.hpp`

```cpp
struct TitleChip::Config {
    std::string title;                          // empty = blank
    Color       edge_color = Color::cyan();
    Color       text_color = Color::bright_white();
    std::size_t max_chars  = 28;
};
```

`▎ implement /loop dynamic m…` — leading colored ▎ + bold title with
middle-truncation. Used in StatusBar's activity row to show the
current thread title. Renders nothing when `title` is empty.

(Distinct from `maya::Breadcrumb`, which is a multi-segment path
chip — `TitleChip` is the simpler single-label cousin.)

Adapter: `status_bar/title_chip.cpp::title_chip_config(m)`.

---

## 16. `maya::PhaseChip` — phase indicator

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

Adapter: `status_bar/phase_chip.cpp::phase_chip_config(m)`. The
`verb_width` and `elapsed_secs` are *defaults* — `StatusBar` overrides
them per-frame based on terminal width.

---

## 17. `maya::TokenStreamSparkline` — compact tok/s + sparkline

`maya/include/maya/widget/token_stream_sparkline.hpp`

```cpp
struct TokenStreamSparkline::Config {
    float              rate    = 0.0f;
    int                total   = 0;
    std::vector<float> history;
    Color              color   = Color::cyan();
    bool               live    = false;     // false = dim (frozen)
};
```

Stable-width 37-cell slot: `⚡ ▕rate 5▏ t/s ▕spark 16▏ ▕total 5▏`.
Every segment is fixed display width so the slot occupies the same
cells whether rate is 0.5 or 1234 — surrounding chips don't shove
leftward as numbers tick.

Adapter: `status_bar/token_stream_sparkline.cpp::token_stream_sparkline_config(m)`.
Resolves the displayed rate from live deltas (after a 250 ms warm-up)
or the most recent ring-buffer sample when paused.

**Sparkline persistence:** the `rate_history` ring buffer is **not**
wiped on `StreamStarted`. It carries across sub-turns and tool gaps so
the user sees a continuous trace of generation rate over the whole
session. The per-burst rate accumulator (`live_delta_bytes`,
`first_delta_at`) does still reset, so the rate *number* measures
only the current burst.

---

## 18. `maya::ContextGauge` — context-window fuel gauge

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

Adapter: `status_bar/context_gauge.cpp::context_gauge_config(m)`.

---

## 19. `maya::StatusBanner` — transient toast row

`maya/include/maya/widget/status_banner.hpp`

```cpp
struct StatusBanner::Config {
    std::string text;                 // empty = blank slot
    bool        is_error = false;
    Color       muted_color = Color::bright_black();
    Color       error_color = Color::red();
};
```

Single-row banner with a leading edge mark + italic text:
- Empty `text` → 1-cell blank placeholder (keeps the surrounding
  StatusBar's row count fixed — no jitter when toasts come and go).
- `is_error=true` → red edge + ⚠ glyph.

Adapter: `status_bar/status_banner.cpp::status_banner_config(m)`.

---

## 20. `maya::ShortcutRow` — width-adaptive hint row

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

Adapter: `status_bar/shortcut_row.cpp::shortcut_row_config(m)`.

---

## 21. `maya::PhaseAccent` — soft horizontal rule

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

No moha adapter — used internally by `StatusBar` (top + bottom
strips), driven by `StatusBar::Config::phase_color`.

---

## 22. `maya::ModelBadge` — colored model chip

`maya/include/maya/widget/model_badge.hpp`

Compact `● Opus` / `● Sonnet` / `● Haiku` brand chip. Used in two
places: WelcomeScreen's chip row and StatusBar's activity row.

Adapter: `status_bar/model_badge.cpp::model_badge_config(m)`. Returns
the configured `ModelBadge` widget; callers `.build()` it where they
need an Element (the widget predates the strict Config-pattern
reshape).

---

## 23. `maya::Overlay` — modal layer

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

The "adapter" is `view.cpp::pick_overlay(m)` which returns
`optional<Element>` based on which modal is currently open (login,
pickers, command palette, diff review, todo). Each modal's Element
still comes from the legacy DSL-based files in §24; once those are
widgetized, `pick_overlay` becomes a config-builder too.

---

## 24. moha adapter functions

Every per-widget adapter file under `src/runtime/view/` has the same
shape: one function `Model → SomeWidget::Config`.

| Adapter file | Function | Returns |
|---|---|---|
| `view.cpp` | `view(m)` | `Element` (the one `.build()`) |
| `view.cpp` | `pick_overlay(m)` | `optional<Element>` |
| `thread/thread.cpp` | `thread_config(m)` | `Thread::Config` |
| `thread/welcome_screen.cpp` | `welcome_screen_config(m)` | `WelcomeScreen::Config` |
| `thread/conversation.cpp` | `conversation_config(m)` | `Conversation::Config` |
| `thread/activity_indicator.cpp` | `activity_indicator_config(m)` | `optional<ActivityIndicator::Config>` |
| `thread/turn/turn.cpp` | `turn_config(msg, idx, n, m)` | `Turn::Config` |
| `thread/turn/permission.cpp` | `inline_permission_config(pp,tc)` | `Permission::Config` |
| `thread/turn/agent_timeline/agent_timeline.cpp` | `agent_timeline_config(msg, frame, c)` | `AgentTimeline::Config` |
| `thread/turn/agent_timeline/tool_body_preview.cpp` | `tool_body_preview_config(tc)` | `ToolBodyPreview::Config` |
| `composer.cpp` | `composer_config(m)` | `Composer::Config` |
| `changes_strip.cpp` | `changes_strip_config(m)` | `ChangesStrip::Config` |
| `status_bar/status_bar.cpp` | `status_bar_config(m)` | `StatusBar::Config` |
| `status_bar/title_chip.cpp` | `title_chip_config(m)` | `TitleChip::Config` |
| `status_bar/phase_chip.cpp` | `phase_chip_config(m)` | `PhaseChip::Config` |
| `status_bar/token_stream_sparkline.cpp` | `token_stream_sparkline_config(m)` | `TokenStreamSparkline::Config` |
| `status_bar/context_gauge.cpp` | `context_gauge_config(m)` | `ContextGauge::Config` |
| `status_bar/status_banner.cpp` | `status_banner_config(m)` | `StatusBanner::Config` |
| `status_bar/shortcut_row.cpp` | `shortcut_row_config(m)` | `ShortcutRow::Config` |
| `status_bar/model_badge.cpp` | `model_badge_config(m)` | `maya::ModelBadge` |

Pure helpers (no maya types touched): under
`thread/turn/agent_timeline/tool_helpers.cpp` (display name, category
color/label, event status, timeline detail) and
`thread/turn/agent_timeline/tool_args.cpp` (`safe_arg`, `pick_arg`,
`count_lines`, `strip_bash_output_fence`, `parse_exit_code`).

Top-level shared (no widget binding): `cache.cpp`, `helpers.cpp`,
`palette.hpp`.

The single `Element`-returning function inside an adapter is
`cached_markdown_for` (private to `thread/turn/turn.cpp`) — it exists
because `maya::StreamingMarkdown` is stateful (per-block parse cache
must persist across frames). moha holds the widget instance, calls
`set_content()` per frame, and slots `instance.build()` into a Turn
body via the typed `Element` variant.

---

## 25. Directory layout

The adapter tree mirrors the widget hierarchy.

```
src/runtime/view/
├── view.cpp                          # AppLayout
├── changes_strip.cpp                 # ChangesStrip
├── composer.cpp                      # Composer
├── cache.cpp · helpers.cpp           # shared (not adapters)
├── login.cpp · pickers.cpp · diff_review.cpp   # legacy modals (§26)
├── thread/
│   ├── thread.cpp                    # Thread
│   ├── welcome_screen.cpp            # WelcomeScreen      (empty branch)
│   ├── conversation.cpp              # Conversation       (non-empty branch)
│   ├── activity_indicator.cpp        # ActivityIndicator  (bottom of conversation)
│   └── turn/
│       ├── turn.cpp                  # Turn
│       ├── permission.cpp            # Permission         (body slot)
│       └── agent_timeline/
│           ├── agent_timeline.cpp    # AgentTimeline      (body slot)
│           ├── tool_body_preview.cpp # ToolBodyPreview    (per-event body)
│           ├── tool_helpers.cpp      # per-tool helpers
│           └── tool_args.cpp         # arg parsers
└── status_bar/
    ├── status_bar.cpp                # StatusBar
    ├── title_chip.cpp                # TitleChip          (activity row)
    ├── phase_chip.cpp                # PhaseChip          (activity row)
    ├── token_stream_sparkline.cpp    # TokenStreamSparkline (activity row)
    ├── context_gauge.cpp             # ContextGauge       (activity row)
    ├── model_badge.cpp               # ModelBadge         (activity row)
    ├── status_banner.cpp             # StatusBanner       (status row)
    └── shortcut_row.cpp              # ShortcutRow        (shortcut row)
```

Headers mirror the same layout under `include/moha/runtime/view/`.

---

## 26. Caching

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

**Streaming-rate sparkline** (separate from this cache) lives in
`StreamState::rate_history` — a 16-slot ring buffer that survives
across sub-turns and tool gaps (only the per-burst rate accumulator
resets on `StreamStarted`).

---

## 27. The DSL (for widget authors and overlay modals)

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

---

## 28. Pending widgetization

Three host files still build elements directly — overlay modals that
predate the strict controller-only rule:

- `src/runtime/view/login.cpp` — login modal
- `src/runtime/view/pickers.cpp` — model picker, thread list, command palette, todo modal
- `src/runtime/view/diff_review.cpp` — pending-changes review modal

Future widgets to absorb them:

- `maya::LoginModal`
- `maya::Picker` (or `CommandPalette` + `ThreadList` + `ModelPicker` + `TodoModal`)
- `maya::DiffReview`

Once those land, every host file under `src/runtime/view/` is a pure
data adapter and the `using namespace maya::dsl` line disappears from
moha entirely.
