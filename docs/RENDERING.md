# Conversation rendering — moha as controller, maya as view

How a `Model` becomes terminal cells. moha is a pure data adapter: it
extracts state from the runtime model and emits **widget Configs**.
maya owns every Element, every chrome glyph, every layout decision,
every breathing animation. The host app constructs no Elements.

Read this alongside [`UI.md`](UI.md), which catalogs the maya DSL
primitives that the widgets are built on top of.

---

## 0. The screen, annotated

```
┌────────────────────────── terminal viewport ─────────────────────────────┐
│                                                                          │ ─┐
│  ─── [↺ Restore checkpoint] ─────────────────────────────────  ┐         │  │
│  ┃ ❯ You                                          12:34 · turn 1│        │  │
│  ┃                                                              │         │  │
│  ┃ refactor the login flow to use the new auth provider         │         │  │
│  ─────────────────────────────────────────────────────────────  ┘         │  │
│                                                                          │  │
│  ┃ ✦ Opus 4.7                              12:34 · 4.2s · turn 1│        │  │ Thread
│  ┃                                                              │         │  │ (welcome
│  ┃ I'll start by exploring the current auth structure.          │         │  │  if empty,
│  ┃                                                              │         │  │  conversation
│  ┃ ╭─ ACTIONS · 3/3 · 1.8s ─────────────────────────────╮       │         │  │  otherwise)
│  ┃ │ I N S P E C T 2 · M U T A T E 1                    │       │         │  │
│  ┃ │ ╭─ ✓ Read    src/auth/login.ts   42ms              │       │         │  │
│  ┃ │ │  │  import { Session } from './session';         │       │         │  │
│  ┃ │ │  │  ··· 80 hidden ···                            │       │         │  │
│  ┃ │ │  │  export default login;                        │       │         │  │
│  ┃ │ ├─ ✓ Grep    provider in src/auth   190ms          │       │         │  │
│  ┃ │ │  │  src/auth/login.ts:14: const provider = …     │       │         │  │
│  ┃ │ ╰─ ✓ Edit    src/auth/login.ts (+5 -2)   1.6s      │       │         │  │
│  ┃ │    │  - const provider = legacyAuth();             │       │         │  │
│  ┃ │    │  + const provider = await NewAuth.create({    │       │         │  │
│  ┃ │ ✓ DONE   3 actions   1.8s                          │       │         │  │
│  ┃ ╰────────────────────────────────────────────────────╯       │         │ ─┘
│                                                                          │
│  ╭─────────────────────────────────────────────────────────────────╮     │ ─┐
│  │ Changes (2 files)        Ctrl+R review  A accept  X reject      │     │  │ ChangesStrip
│  │ 2 files changed  +12  -3                                        │     │  │ (only when
│  │   ~ src/auth/login.ts        +5 -2                              │     │  │  pending)
│  │   + src/auth/types.ts        +7 -1                              │     │  │
│  ╰─────────────────────────────────────────────────────────────────╯     │ ─┘
│                                                                          │
│  ╭─ ⠋ — type to queue… ──────────────────────────────────╮              │ ─┐
│  │ ❯ ▎                                                   │              │  │ Composer
│  │                                                       │              │  │
│  │ ↵ send  ·  ⇧↵/⌥↵ newline  ·  ^E expand    ▎ Write    │              │ ─┘
│  ╰────────────────────────────────────────────────────────╯              │
│  ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔             │ ─┐
│   ▎ Title  ·  ▌ ⠋ Streaming  4.2s    ⚡ 23.4 t/s  ▁▂▃▅▇  ●Opus  CTX 18% │  │
│                                                                          │  │ StatusBar
│   ^K palette  ·  ^J threads  ·  ^T todo  ·  ^N new  ·  ^C quit          │  │
│  ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁             │ ─┘
└──────────────────────────────────────────────────────────────────────────┘

  When a modal/picker is open, an Overlay floats above the base, centered
  horizontally, pinned to the bottom edge, with an opaque background.
```

---

## 1. The architectural rule

> **moha constructs no Elements.** Every `Element{...}`, every
> `dsl::v(...)`, every `dsl::h(...)`, every `dsl::text(...)` lives in
> a maya widget. moha extracts state into widget Configs and lets
> maya render.

Concrete: in `src/runtime/view/`, the files `thread.cpp`,
`composer.cpp`, `changes.cpp`, `statusbar.cpp`, `permission.cpp`,
`view.cpp` contain **zero** Element construction. Each one is a
function that takes a `const Model&` and returns the matching widget's
`Config` struct.

The single exception is `cached_markdown_for` in `thread.cpp`: it
returns an `Element` because `maya::StreamingMarkdown` is stateful and
its block-cache must persist across frames. moha caches the widget
*instance*, calls `set_content()` on it, and slots the resulting
`Element` into a Turn body slot via the typed `Element` variant. No
`Element{...}` literals — only `widget.build()` calls.

---

## 2. Widget hierarchy

```
maya::AppLayout                               top-level chat-app frame
├── maya::Thread                              conversation viewport
│   │
│   ├── maya::WelcomeScreen                   (when messages.empty())
│   │       wordmark + tagline + chips +
│   │       starters card + hint row
│   │
│   └── maya::Conversation                    (when !messages.empty())
│       │       list of turns + dim dividers + optional in-flight
│       │
│       ├── maya::Turn[*]                     one speaker turn (rail + header + body)
│       │   │
│       │   ├── maya::CheckpointDivider       (above turn, outside rail)
│       │   │       "─── [↺ Restore checkpoint] ───"
│       │   │
│       │   └── body slots (typed variant — Turn auto-spaces between):
│       │       ├── PlainText                 user message text
│       │       ├── MarkdownText              maya::markdown(content)
│       │       ├── maya::AgentTimeline       Actions panel (one per assistant turn)
│       │       │   │
│       │       │   ├── stats row             "INSPECT 2 · MUTATE 1"
│       │       │   ├── per-event header      tree glyph + status icon + name + detail + duration
│       │       │   ├── maya::ToolBodyPreview body content under │ stripe
│       │       │   │     │   discriminated by Kind:
│       │       │   │     ├── CodeBlock        head+tail preview, dimmed
│       │       │   │     ├── EditDiff         multi-hunk per-side diff
│       │       │   │     ├── GitDiff          per-line +/-/@@ coloring
│       │       │   │     ├── TodoList         ✓ ◍ ○ checkbox list
│       │       │   │     └── Failure          red preview block
│       │       │   └── footer                 "✓ DONE  3 actions  1.4s"
│       │       │
│       │       ├── maya::Permission          inline permission card
│       │       │
│       │       └── Element                   escape hatch (cached StreamingMarkdown)
│       │
│       └── maya::ActivityIndicator          (optional, bottom of thread)
│               "▎ ⠋ streaming…" — only when active and no Timeline visible
│
├── maya::ChangesStrip                       pending-edits banner
│       │
│       ├── header row                       "Changes (2 files)  Ctrl+R review  A accept  X reject"
│       └── maya::FileChanges                file list with +/− line counts
│
├── maya::Composer                           bordered input box
│       │
│       ├── prompt + body rows               state-driven color (idle/streaming/awaiting)
│       └── hint row (width-adaptive)
│           │
│           ├── shortcuts (left)             ↵ send · ⇧↵ newline · ^E expand
│           └── ambient (right)              queue · words · tokens · profile chip
│
├── maya::StatusBar                          bottom panel (5 rows tall — fixed-height)
│       │
│       ├── maya::PhaseAccent (top)          ▔▔▔▔▔▔ in phase color, dim
│       │
│       ├── activity row (width-adaptive)    breadcrumb · phase chip · tok/s · model · ctx
│       │   │
│       │   ├── maya::PhaseChip              colored glyph + verb + elapsed (breathing)
│       │   ├── compact tok/s sparkline      ⚡ rate · ▁▂▃▄ · total
│       │   └── maya::ContextGauge           CTX usage with green/amber/red zones
│       │
│       ├── status row                       error/toast banner (always 1 row to prevent jitter)
│       │
│       ├── maya::ShortcutRow                ^K palette · ^J threads · …
│       │
│       └── maya::PhaseAccent (bottom)       ▁▁▁▁▁▁ in phase color, dim
│
└── maya::Overlay                            (when a modal is open)
        │
        ├── base                             everything above (z-stacked underneath)
        └── overlay                          centered horizontally, pinned bottom,
                                             opaque background to mask the base
```

Every name above is a real widget at `maya/include/maya/widget/<name>.hpp`.

---

## 3. Data flow — `view(m)` to terminal cells

```
moha::ui::view(m)                                       [view.cpp:36]
    ↓
    builds maya::AppLayout::Config { … }
        .thread          = thread_config(m)             [thread.cpp]
        .changes_strip   = changes_strip_config(m)      [changes.cpp]
        .composer        = composer_config(m)           [composer.cpp]
        .status_bar      = status_bar_config(m)         [statusbar.cpp]
        .overlay         = pick_overlay(m)              [view.cpp]
    ↓
    AppLayout{cfg}.build()
        ↓
        v(
            Thread{cfg.thread}.build()         | grow(1.0f),
            ChangesStrip{cfg.changes_strip}.build(),
            Composer{cfg.composer}.build(),
            StatusBar{cfg.status_bar}.build()
        ) | pad<1> | grow(1.0f)
        ↓
        Overlay{base, cfg.overlay}.build()
        ↓
        Element  (one tree of BoxElement / TextElement / ComponentElement)
        ↓
    maya layout engine → Canvas → terminal cells
```

`view()` is one declarative struct expression. No imperative chaining,
no `if` branches around `zstack` / `vstack`, no element construction —
just a `Config` populated from `Model` data.

```cpp
// src/runtime/view/view.cpp — the entire body of view():
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

---

## 4. Inside `maya::AppLayout::build()`

```cpp
auto base = (v(
    v(Thread{cfg_.thread}.build()) | grow(1.0f),
    ChangesStrip{cfg_.changes_strip}.build(),
    Composer{cfg_.composer}.build(),
    StatusBar{cfg_.status_bar}.build()
) | pad<1> | grow(1.0f)).build();

Overlay::Config oc;
oc.base = std::move(base);
if (cfg_.overlay) { oc.overlay = *cfg_.overlay; oc.present = true; }
return Overlay{std::move(oc)}.build();
```

That's the whole top-level layout. Four sections in a vstack with the
Thread growing to fill, all wrapped in `pad<1>`. If `overlay` is
present, `Overlay` z-stacks it on top with center-bottom alignment.

---

## 5. Inside `maya::Thread::build()`

```cpp
if (cfg_.is_empty)
    return WelcomeScreen{cfg_.welcome}.build();

Conversation::Config conv;
conv.turns.reserve(cfg_.turns.size());
for (const auto& tc : cfg_.turns)
    conv.turns.push_back(Turn{tc}.build());

if (cfg_.in_flight) {
    conv.in_flight     = ActivityIndicator{*cfg_.in_flight}.build();
    conv.has_in_flight = true;
}
return Conversation{std::move(conv)}.build();
```

Thread's only job is the empty-vs-populated branch and forwarding the
optional in-flight indicator. All chrome lives in WelcomeScreen and
Conversation.

---

## 6. Inside `maya::Turn::build()` — the body-slot dispatch

The most interesting widget. `Turn::Config::body` is a typed variant:

```cpp
using BodySlot = std::variant<
    PlainText,             // user/plain text path
    MarkdownText,          // string → maya::markdown()
    AgentTimeline::Config, // tool-calls Actions panel
    Permission::Config,    // inline permission card
    Element                // escape hatch (cached StreamingMarkdown)
>;
std::vector<BodySlot> body;
```

Turn:
1. Renders the header row (`<glyph> <label> ___ <meta>`).
2. Walks each body slot, dispatches via `std::visit` to the right
   widget invocation, and inserts a blank line between consecutive
   non-empty slots — callers don't push spacers.
3. If `error` is non-empty, appends a `⚠ <message>` row.
4. Wraps everything in the bold left-only border (the speaker rail) in
   `rail_color`.
5. If `checkpoint_above`, prepends a `CheckpointDivider` outside the
   rail (between-turns concern, not inside the rail).

Per-slot widget invocation:

| BodySlot variant      | Renderer inside Turn                |
|-----------------------|-------------------------------------|
| `PlainText`           | `text(content, fg)`                 |
| `MarkdownText`        | `maya::markdown(content)`           |
| `AgentTimeline::Config` | `AgentTimeline{cfg}.build()`      |
| `Permission::Config`  | `Permission{cfg}.build()`           |
| `Element`             | the Element itself (escape hatch)   |

The escape-hatch `Element` slot exists for one reason: cross-frame
caching. `maya::StreamingMarkdown` keeps a per-block parse cache that
must survive between renders, so moha holds the widget instance in
its `MessageMdCache` and feeds the resulting `Element` back through
the slot list. That's the only `Element`-producing call moha makes.

---

## 7. Inside `maya::AgentTimeline::build()`

The Actions panel. Composition:

```
╭─ ACTIONS · 3/5 · Bash ─────────────────────────────╮
│  I N S P E C T  2  ·  M U T A T E  1               │   ← stats row (only when events > 1)
│                                                    │
│  ╭─ ⠋ Bash    npm test          1.2s               │   ← per-event header
│  │   │  PASS test/foo.test.ts                      │   ← ToolBodyPreview rows under │ stripe
│  │   │  ✓ all 5 tests passed                       │
│  │                                                  │   ← inter-event connector (next status color)
│  ├─ ✓ Read    src/foo.ts        38ms               │
│  │   │  import { bar } from './bar';               │
│  ╰─ ✓ Edit    src/foo.ts        210ms              │
│   │  edit 1/2  ·  −1 / +3                          │
│   │  - const provider = …                          │
│   │  + const provider = await …                    │
│                                                    │
│  ✓ DONE   3 actions   1.4s                          │   ← footer (only when all settled)
╰─────────────────────────────────────────────────────╯
```

Each `AgentTimelineEvent` carries:

```cpp
struct AgentTimelineEvent {
    std::string             name;              // "Bash", "Read", …
    std::string             detail;            // "npm test  ·  exit 0"
    float                   elapsed_seconds;
    Color                   category_color;    // inspect/mutate/execute/plan/vcs
    AgentEventStatus        status;            // Pending/Running/Done/Failed/Rejected
    ToolBodyPreview::Config body;              // typed body — no Elements
};
```

For each event the widget:
1. Picks the tree glyph (`──` / `╭─` / `├─` / `╰─` based on position).
2. Picks the status icon (10-frame braille spinner for active states;
   `✓ ✗ ⊘` for terminal).
3. Renders `name + detail + (optional duration)`.
4. Builds the body via `ToolBodyPreview{ev.body}.build()` and stripes
   each line with the `│` connector, in `event_connector_color(status)`.
5. Inserts a short inter-event connector colored by the *next*
   event's status (so the lane visually flows into the upcoming
   event).

Footer is rendered only when every event is terminal: `✓ DONE` /
`✗ N FAILED` / `⊘ N REJECTED` + count + total elapsed.

---

## 8. Inside `maya::ToolBodyPreview::build()`

A discriminated body widget. `Config::kind` picks the renderer:

| Kind        | Inputs              | Rendering                                                   |
|-------------|---------------------|-------------------------------------------------------------|
| `None`      | —                   | empty Element (skipped)                                     |
| `CodeBlock` | `text`, `text_color`| head+tail preview (4+3 lines) with `··· N hidden ···` mark  |
| `Failure`   | `text`              | same as CodeBlock but in `Color::red()`                     |
| `EditDiff`  | `hunks[]`           | per-hunk header `edit i/N · −k / +m`, head+tail per side    |
| `GitDiff`   | `text`              | per-line styling (+green / -red / @@dim / context plain)    |
| `TodoList`  | `todos[]`           | `✓` completed (dim), `◍` in-progress, `○` pending           |

All elision math (split lines → keep first `head` + last `tail` →
insert dim middle marker) lives inside the widget. moha just provides
the raw `text` / `hunks[]` / `todos[]`.

---

## 9. The other top-level widgets

### `maya::WelcomeScreen` — empty-thread splash

```
                        ┌┬┐┌─┐┬ ┬┌─┐
                        ││││ │├─┤├─┤
                        ┴ ┴└─┘┴ ┴┴ ┴

                a calm middleware between you and the model

                ● Opus 4.7              ▌ WRITE ▐                  ← chips row

              ╭─ T R Y ──────────────────────────────╮
              │                                       │
              │ • Implement a small feature           │
              │ • Refactor or clean up this file      │
              │ • Explain what this code does         │
              │ • Write tests for ...                 │
              ╰───────────────────────────────────────╯

      type to begin  ·  ^K palette  ·  ^J threads  ·  ^N new
```

moha supplies brand content (wordmark glyphs, tagline, starter
prompts, hint keys); the widget owns the layout, the wordmark gradient
("last row dim"), the small-caps title, the centering.

### `maya::ChangesStrip` — pending edits banner

```
╭─────────────────────────────────────────────────────╮
│ Changes (2 files)   Ctrl+R review  A accept  X reject│
│ 2 files changed  +12  -3                             │
│   ~ src/auth/login.ts        +5 -2                   │
│   + src/auth/types.ts        +7 -1                   │
╰─────────────────────────────────────────────────────╯
```

When `cfg.changes` is empty, the widget renders to an empty Element so
the slot collapses cleanly without an `if` in the host.

### `maya::Composer` — bordered input box

State-driven: border + prompt color reflect activity (idle / awaiting
permission / streaming / executing tool); placeholder text changes
("type a message…" / "running tool — type to queue…"); height pins to
`min_rows` during activity to prevent vertical jitter from layout
reflows above. Hint row is width-adaptive — drops `expand` then
`newline` keys as width shrinks; right side carries queue depth /
word-and-token counters / profile chip.

### `maya::StatusBar` — bottom panel

Five fixed rows:
1. `PhaseAccent` (top) — ▔▔▔▔ in phase color, dim
2. activity row — breadcrumb · phase chip · tok/s sparkline · model · CTX
3. status row — toast banner or blank (always 1 row tall to prevent jitter)
4. `ShortcutRow` — width-adaptive key/label list
5. `PhaseAccent` (bottom) — ▁▁▁▁ in phase color, dim

`PhaseChip` owns the breathing animation cadence (32-frame cycle, bold
half / dim half — perceptible motion below resting heart-rate).
`ContextGauge` owns the green/amber/red zones (`<60%` safe, `60–80%`
warn, `>80%` danger) plus a placeholder slot when no usage data has
arrived yet (so the right-side chips don't shove leftward when the
first usage event fires mid-stream). `ShortcutRow` owns the priority
ordering — drops `S-Tab` and `^/` first on narrow widths, then drops
labels entirely (key-only mode).

### `maya::Overlay` — modal layer

A thin coordinator: `present=false` collapses to just `base`. When
present, z-stacks `overlay` on top, centered horizontally, pinned to
the bottom edge, with an opaque background to mask the base.

---

## 10. moha's adapter side — what the host actually does

Every per-section file in `src/runtime/view/` is a function that
takes `const Model&` and returns a widget Config. No Elements.

| File                    | Function                       | Returns                          |
|-------------------------|--------------------------------|----------------------------------|
| `view.cpp`              | `view(m)`                      | `Element` (the one `.build()`)   |
| `thread.cpp`            | `thread_config(m)`             | `maya::Thread::Config`           |
| `composer.cpp`          | `composer_config(m)`           | `maya::Composer::Config`         |
| `changes.cpp`           | `changes_strip_config(m)`      | `maya::ChangesStrip::Config`     |
| `statusbar.cpp`         | `status_bar_config(m)`         | `maya::StatusBar::Config`        |
| `permission.cpp`        | `inline_permission_config(...)`| `maya::Permission::Config`       |

Inside `thread.cpp`, the per-message helpers also return Configs:

| Function                              | Returns                            |
|---------------------------------------|------------------------------------|
| `welcome_config(m)`                   | `maya::WelcomeScreen::Config`      |
| `turn_config(msg, idx, turn_num, m)`  | `maya::Turn::Config`               |
| `agent_timeline_config(msg, frame, c)`| `maya::AgentTimeline::Config`      |
| `tool_body_config(tc)`                | `maya::ToolBodyPreview::Config`    |
| `in_flight_indicator(m)`              | `optional<ActivityIndicator::Config>` |
| `cached_markdown_for(msg, tid, idx)`  | `Element` (cross-frame cache; only escape hatch) |

All other helpers (`speaker_style_for`, `format_turn_meta`,
`tool_timeline_detail`, `tool_event_status`, `tool_display_name`,
`tool_category_color`, `tool_category_label`, `assistant_elapsed`)
return strings, enums, or POD structs. No `maya::*` calls.

---

## 11. Caching

Two thread-local caches in `cache.hpp` / `cache.cpp`:

| Cache            | Key                           | Holds                                                    |
|------------------|-------------------------------|----------------------------------------------------------|
| `message_md_cache(tid, idx)` | `(thread_id, msg_idx)`        | `shared_ptr<Element>` (finalized) + `shared_ptr<StreamingMarkdown>` (live) |

`StreamingMarkdown` is the only widget held across frames — its
internal block-boundary cache makes each delta `O(new_chars)` rather
than re-parsing the full transcript. moha keeps the instance alive,
calls `set_content(streaming_text)` each frame, slots
`instance.build()` into the Turn body via the `Element` variant.

Once `finalize_turn` moves `streaming_text` → `text`, the next render
takes the finalized branch: builds `maya::markdown(text)` once,
caches the resulting `Element`, returns the same pointer every
subsequent frame.

The previous `tool_card_cache` and the entire `tool_card.cpp`
(614 lines, an alternative per-tool widget catalog reachable via
`render_tool_call`) were dead — never called from any path. Both were
removed when the rendering pipeline collapsed onto
`AgentTimeline + ToolBodyPreview`.

---

## 12. Data flow for one assistant turn with tools

End to end, what happens when an assistant turn with two tool calls
needs rendering:

```
view(m)                                    ┐
   AppLayout::Config{ .thread = ..., …}    │
   AppLayout{cfg}.build()                  │ host
      ↓                                    ┘
   Thread{thread_cfg}.build()
      ↓
   for each Turn::Config in thread_cfg.turns:
      Turn{tc}.build()
         ↓
      header = h(glyph, label, meta) | grow(1.0f)
      for each BodySlot:
         visit(slot):
            MarkdownText →   maya::markdown(content)
                               (or cached StreamingMarkdown.build()
                                via Element variant)
            AgentTimeline::Config →
                AgentTimeline{cfg}.build()
                   ↓
                for each AgentTimelineEvent:
                    header row (tree glyph, status icon, name, detail, duration)
                    ToolBodyPreview{event.body}.build()
                       ↓
                       switch(kind):
                          CodeBlock  → head_tail(text, 4, 3) → vstack of lines
                          EditDiff   → for each hunk: header + push_diff_side(old, '-', red) + push_diff_side(new, '+', green)
                          GitDiff    → per-line pick_style (+/-/@@) + head+tail
                          TodoList   → for each: glyph + content (status-styled)
                          Failure    → CodeBlock in red
                    inter-event connector (colored by next status)
                stats row (if events > 1)
                footer (if all terminal)
                | border<Round> | bcolor(rail_color) | btext("ACTIONS · 3/3 …")
            Permission::Config →   Permission{cfg}.build()
      | rail (Bold left border in rail_color)
      → Element
   collected as Conversation::Config.turns
   Conversation{conv_cfg}.build()
      → list of turn Elements separated by dim ─── dividers
      + optional ActivityIndicator at bottom
   → Element
   (slotted into AppLayout's vstack alongside changes_strip / composer / status_bar)
```

Every transition is `widget.build()` returning an `Element`. moha
participates only at the entry: building the top-level Config tree.

---

## 13. Files

### maya widgets

```
maya/include/maya/widget/
├── app_layout.hpp           top-level frame: Thread + ChangesStrip + Composer + StatusBar + Overlay
├── thread.hpp               welcome | conversation branch
├── conversation.hpp         list of turns + dim dividers + in-flight slot
├── turn.hpp                 single turn: rail + header + typed body slots
├── checkpoint_divider.hpp   "─── [↺ Restore checkpoint] ───"
├── activity_indicator.hpp   "▎ ⠋ streaming…"
├── welcome_screen.hpp       wordmark + chips + starters + hints
├── agent_timeline.hpp       Actions panel for tool calls
├── tool_body_preview.hpp    discriminated tool body content (CodeBlock/EditDiff/GitDiff/TodoList/Failure)
├── changes_strip.hpp        pending edits banner
├── composer.hpp             bordered input box (state-driven color, hint row)
├── status_bar.hpp           bottom panel — composes PhaseAccent/PhaseChip/ContextGauge/ShortcutRow
├── phase_accent.hpp         soft horizontal rule in phase color
├── phase_chip.hpp           breathing colored glyph + verb + elapsed
├── context_gauge.hpp        CTX usage fuel-gauge with zones
├── shortcut_row.hpp         width-adaptive keyboard hints
└── overlay.hpp              z-stack base + centered modal
```

### moha adapters

```
src/runtime/view/
├── view.cpp                 view(m) → AppLayout config + build
├── thread.cpp               Thread/Turn/AgentTimeline/ToolBodyPreview/WelcomeScreen configs
├── changes.cpp              ChangesStrip config
├── composer.cpp             Composer config
├── statusbar.cpp            StatusBar config
├── permission.cpp           Permission config (from PendingPermission + ToolUse)
├── cache.cpp                MessageMdCache (StreamingMarkdown / finalized Element)
├── helpers.cpp              format_elapsed_5, small_caps, profile_color, phase_*
├── tool_args.cpp            safe_arg, pick_arg, count_lines, strip_bash_output_fence, parse_exit_code
└── (login / pickers / diff_review — overlay modals, still Element-building; pending widgetization)
```

The remaining `login.cpp`, `pickers.cpp`, `diff_review.cpp` are modal
overlays that still construct elements directly — they predate the
controller-only refactor. Future widgetization: `maya::LoginModal`,
`maya::Picker` (or `CommandPalette` / `ThreadList` / `TodoModal`),
`maya::DiffReview`.
