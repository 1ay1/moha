# 11 — Navigation (chrome, popovers, history, routes)

The agent panel has more than just a single conversation view. Users
need to:

- Switch between threads (history)
- Switch the active model
- Switch the active profile (Write/Ask/Minimal)
- Switch the operating mode (Fast on/off, Thinking on/off)
- Open settings
- Get help

This doc covers the **navigation surfaces**: the chrome (top bar),
the popovers it opens, the history view, and how the panel routes
between them. Specifics of each region's contents live elsewhere
(message stream → 06, composer → 08, diff review → 10); this doc is
about *getting there*.

References:
- Zed: `crates/agent_ui/src/agent_panel.rs:agent_panel_chrome`,
  `crates/agent_ui/src/history_store.rs`, the model selector dropdowns
  in `crates/agent_settings/...`.
- maya: `maya::widget::popup`, `maya::widget::menu`,
  `maya::widget::list`, `maya::widget::tabs`,
  `maya::widget::breadcrumb`, `maya::widget::badge`.

## 1. The chrome bar

The top row of the panel. Compact, single-line, monochromatic.

```
 ◆ moha · my-thread-name           ●  Streaming · 3.2s     gpt-4 ▾  ⚡ ◯  History  ?
```

Read left-to-right:

| Slot | Content | Notes |
|---|---|---|
| Logo / agent mark | `◆` (or whatever glyph) | Color: `text` (171, 178, 191) |
| App name | `moha` | Bold |
| Separator | `·` (middle dot, U+00B7) | Dim |
| Thread title | `my-thread-name` | Truncate middle if > 40 chars |
| Spacer | — | pushes status right |
| Phase indicator | `● Streaming · 3.2s` | Color from `05_design_tokens § 1` (Phase indicators) |
| Spacer | — | pushes the right cluster to the edge |
| Model selector | `gpt-4 ▾` | Click → popover (§ 3) |
| Fast Mode toggle | `⚡` | Click → toggle |
| Thinking toggle | `◯` / `◉` | Click → toggle |
| History | `History` | Click → history route (§ 4) |
| Help | `?` | Click → help/keymap modal (§ 6) |

Layout:

```cpp
Element views::chrome(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    return (h(
        text(" \xe2\x97\x86 ") | Style{}.with_fg(tokens::fg::text),
        text("moha") | Bold,
        text("  \xc2\xb7  ") | Dim,
        text(truncate_middle(m.current.title, 40))
            | Style{}.with_fg(tokens::fg::text),
        spacer(),
        views::phase_indicator(m),
        spacer(),
        views::model_selector_button(m),
        text("  "),
        views::fast_toggle_chrome(m),
        text(" "),
        views::thinking_toggle_chrome(m),
        text("   "),
        views::history_button(m),
        text("   "),
        views::help_button(m)
    )
    | bg_(tokens::bg::panel)
    | padding(0, 1, 0, 1)
    ).build();
}
```

The chrome **stays put across routes**. Only its right-cluster
adapts (e.g., model selector hidden in Diff Review route).

## 2. Phase indicator

Same color/glyph table as `05_design_tokens § 1` (Phase indicators):

| Phase | Glyph | Color | Label |
|---|---|---|---|
| Idle | `●` | teal (86, 182, 194) | "Idle" |
| Streaming | `⠋` (cycling) | amber (229, 192, 123) | `"Streaming · <elapsed>"` |
| AwaitingPermission | `⚠` | red-pink (224, 108, 117) | "Awaiting permission" |
| ExecutingTool | `●` | purple (198, 120, 221) | `"Running <tool>"` |

The streaming phase pulses the spinner glyph (cycling through
`⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏` every 80ms). Other phases hold a steady glyph.

```cpp
Element views::phase_indicator(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    auto [glyph, color, label] = phase_visual(m.phase, m.tick_index);
    return h(
        text(glyph) | Style{}.with_fg(color),
        text(" "),
        text(label) | Style{}.with_fg(color) | Dim
    ).build();
}
```

`m.tick_index` is incremented on every `Tick{80ms}`. Modulo 10 picks
the spinner frame.

## 3. Model selector

A dropdown popover. Trigger: click `gpt-4 ▾` or press `Ctrl+M`.

```
gpt-4 ▾   ←  click here

         ┌─ Model ─────────────────────────────┐
         │  ▶ claude-opus-4-6                  │
         │    claude-sonnet-4-6                │
         │    claude-haiku-4-5-20251001        │
         │    gpt-4o                           │
         │    gpt-4o-mini                      │
         │  ─────────────────                  │
         │    [Esc] close                      │
         └─────────────────────────────────────┘
```

Use `maya::widget::popup` anchored beneath the trigger. Note that
maya's popups are not "floating" in the OS sense — they're elements
positioned via a fixed offset within the panel's render tree. To
implement:

1. Render the chrome normally.
2. When `m.popover.kind == Popover::Model`, render an additional
   element at fixed coordinates (just under the chrome, right-edge
   aligned).
3. The popover element is a bordered list with one row per model.
4. `↑`/`↓` to highlight, `Enter` to accept, `Esc` to close.

```cpp
Element views::model_popover(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    std::vector<Element> rows;
    auto& models = m.available_models;
    for (size_t i = 0; i < models.size(); ++i) {
        bool selected = i == m.popover.highlight;
        bool active = models[i].id == m.composer.model_id;
        rows.push_back(h(
            text(selected ? "▶ " : "  "),
            text(models[i].label)
                | (active ? Style{}.with_fg(tokens::fg::link) : Style{})
                | (selected ? Bold : Style{})
        ).build());
    }

    return (v(
        text(" Model ") | Bold,
        v(std::move(rows)) | gap_<0>,
        text("─────────────────") | Dim,
        text(" [Esc] close ") | Dim
    )
    | border(BorderStyle::Round)
    | bcolor(tokens::border::dim)
    | bg_(tokens::bg::editor)
    | padding(0, 1, 0, 1)
    | min_width(40)
    ).build();
}
```

Position: Use a `Popup` widget with anchor coordinates from layout.
maya doesn't have automatic anchor-to-element today; for now, hardcode
an offset (top: 1 row below chrome; right: aligned to terminal width).

If maya later adds anchor support, switch over.

### Profile selector

Same pattern. Triggered by `Ctrl+P` or click on profile badge in
toolbar (§ 08 toolbar).

```
         ┌─ Profile ───────────────────────────┐
         │  ▶ Write                            │
         │    Ask                              │
         │    Minimal                          │
         │  ─────────────────                  │
         │  Write: full agency                 │
         │    auto-allows reads, asks for writes│
         │    [Esc] close                      │
         └─────────────────────────────────────┘
```

Below the list, show a brief description of the *highlighted* profile
(updates as you arrow). This makes it discoverable.

## 4. History view

A separate route showing all past threads in the workspace.
Triggered by clicking `History` in chrome or pressing `Ctrl+H`.

```
┌─ History ───────────────────────────── [Esc] back ─┐
│                                                    │
│  Search: ___________________                       │
│                                                    │
│  Today                                             │
│  ──────                                            │
│ ▶ Implement OAuth flow             10 min ago      │
│   Refactor token kicker           2 hours ago      │
│                                                    │
│  Yesterday                                         │
│  ──────                                            │
│   Investigate streaming bug        9 messages      │
│   Profile cycle bindings           4 messages      │
│                                                    │
│  Last week                                         │
│  ──────                                            │
│   Build out diff review                            │
│                                                    │
│  ────────────────────────────────────────────────  │
│  [⏎ Open]  [N] New thread  [D] Delete  [Esc] Back  │
└────────────────────────────────────────────────────┘
```

Layout:

```cpp
Element views::history_route(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    auto& h = *m.history_view;

    return v(
        // Header
        h(
            text(" History ") | Bold,
            spacer(),
            text("[Esc] back") | Dim
        ) | bg_(tokens::bg::panel) | padding(0, 1, 0, 1),

        // Search bar
        views::history_search(h),

        // Scrollable thread list
        Scrollable(views::history_thread_list(h)).build() | grow_<1>,

        // Footer
        h(
            text("[⏎ Open]") | Style{}.with_fg(tokens::fg::link),
            text("  "),
            text("[N] New thread") | Dim,
            text("  "),
            text("[D] Delete") | Dim,
            spacer(),
            text("[Esc] Back") | Dim
        ) | bg_(tokens::bg::panel) | padding(0, 1, 0, 1)
    ).build();
}
```

### Thread list grouping

Group by recency:

| Group | Threshold |
|---|---|
| Today | `< 24h` |
| Yesterday | `24-48h` |
| Last week | `2-7d` |
| Last month | `7-30d` |
| Older | `> 30d` |

Within group, sort newest-first.

Each row shows:
```
▶ <title>     <relative-time>
```

- `▶` arrow on focused row
- Title: `text` color, truncated middle to fit
- Timestamp: `Dim`, right-aligned

Optional second line per row: message count + first user message
preview, when row is focused. Adds vertical complexity; defer.

### Search

`/` or focus the search bar, then type. Filter threads by:
- Title substring (case-insensitive)
- Message text substring (across all messages)
- File mentions (e.g., search `src/main.cpp` finds threads that
  referenced it)

Use a simple in-memory walk over `m.history_view.threads`. Indexing
is overkill until thread count gets large.

### Actions

| Key | Action |
|---|---|
| `↑`/`↓` | Move selection |
| `Enter` | Open the focused thread (loads, switches route to message stream) |
| `N` | Start a new thread (route back to message stream + empty thread) |
| `D` | Delete focused thread (with confirm: "Delete `<title>`? [Y/N]") |
| `/` | Focus search input |
| `Esc` | Close history (return to previous route) |
| `R` | Rename focused thread (inline edit) |
| `E` | Export focused thread to file (chooses format: md / json) |

### History store

```cpp
namespace moha {

struct ThreadSummary {
    std::string id;
    std::string title;
    std::chrono::system_clock::time_point updated_at;
    int message_count;
    std::vector<std::string> referenced_files;
};

struct HistoryView {
    std::vector<ThreadSummary> threads;
    std::string search_query;
    int highlight = 0;
    ScrollState scroll;
};

} // namespace moha
```

Persist threads to `~/.config/moha/threads/<workspace_hash>/`. The
`HistoryView` is loaded on demand (when route opens). Keep the file
list in memory after first load and refresh on `StreamFinished` (so
the current thread's stats are up-to-date).

## 5. Routes

Top-level routes:

```cpp
enum class Route {
    MessageStream,    // default — composer + message stream
    DiffReview,       // multi-file diff (10_diff_review.md)
    History,          // thread browser (this doc § 4)
    Settings,         // settings route (this doc § 7)
    Help,             // keymap modal (this doc § 6)
};
```

Routing is just a `switch` in the top-level view:

```cpp
Element view(const Model& m) {
    using namespace maya::dsl;
    return v(
        views::chrome(m),
        [&]() -> Element {
            switch (m.route) {
                case Route::MessageStream: return views::main_panel(m);
                case Route::DiffReview:    return views::diff_review_route(m);
                case Route::History:       return views::history_route(m);
                case Route::Settings:      return views::settings_route(m);
                case Route::Help:          return views::help_route(m);
            }
            return views::main_panel(m);
        }(),
        // Bottom status bar (optional)
        views::status_bar(m)
    ).build();
}
```

### Transitions

- Going to a route stores the previous route in
  `m.route_stack` (`std::vector<Route>`).
- `Esc` from a non-default route pops to the previous one.
- Most routes preserve their state when popped (e.g., re-opening
  History should remember the search query and selection).

```cpp
[&](OpenRoute ev) -> std::pair<Model, Cmd<Msg>> {
    if (m.route != ev.target) {
        m.route_stack.push_back(m.route);
        m.route = ev.target;
    }
    return {m, route_open_cmd(ev.target)};   // load data if needed
},

[&](CloseRoute) -> std::pair<Model, Cmd<Msg>> {
    if (!m.route_stack.empty()) {
        m.route = m.route_stack.back();
        m.route_stack.pop_back();
    } else {
        m.route = Route::MessageStream;
    }
    return {m, Cmd<Msg>::none()};
},
```

## 6. Help / keymap modal

Triggered by `?` (when no input has focus) or `Ctrl+?`.

This is a **modal**, not a route — it overlays the current view
without replacing it. Implement with `maya::widget::modal` or a
fixed-position popup.

```
        ┌─ Keyboard Shortcuts ────────────────────────────┐
        │                                                  │
        │  Composing                                       │
        │    Enter            Send                          │
        │    Shift+Enter      New line                      │
        │    Esc              Cancel stream / clear focus   │
        │    Ctrl+R           Regenerate from focused msg   │
        │    @                Mention file                  │
        │    /                Slash command                 │
        │                                                  │
        │  Navigation                                       │
        │    Ctrl+H           History                       │
        │    Ctrl+M           Model selector                │
        │    Ctrl+P           Profile selector              │
        │    Ctrl+,           Settings                      │
        │    ?                Help                          │
        │                                                  │
        │  Tool cards                                       │
        │    Enter            Toggle expand                 │
        │    Y                Allow                         │
        │    N                Deny                          │
        │    A                Always allow                  │
        │    D                Open diff review              │
        │                                                  │
        │  ────────────────────────────────────────────── │
        │  [Esc] close                                     │
        └──────────────────────────────────────────────────┘
```

Two-column layout: key (left, monospace, `text` color) + description
(right, `text_muted`).

Source the rows from a single static table that's *also* used by
`12_keymap.md` so the help and the keymap reference can never drift.

```cpp
struct KeyHelpEntry {
    std::string key;
    std::string description;
    std::string section;     // "Composing", "Navigation", ...
};

inline const std::vector<KeyHelpEntry>& key_help_entries() {
    static const auto entries = std::vector<KeyHelpEntry>{
        {"Enter",       "Send",                                "Composing"},
        {"Shift+Enter", "New line",                            "Composing"},
        {"Esc",         "Cancel stream / clear focus",         "Composing"},
        {"Ctrl+R",      "Regenerate from focused msg",         "Composing"},
        // ...
    };
    return entries;
}
```

Help renders by grouping entries by section.

## 7. Settings route

A vertical list of settings with their current values. Each is
editable via a per-row dropdown / toggle / input.

```
┌─ Settings ─────────────────────────────── [Esc] back ─┐
│                                                       │
│  General                                              │
│  ──────                                               │
│   Theme                          One Dark             │
│   Default profile                Write                │
│   Default model                  claude-opus-4-6      │
│                                                       │
│  Behavior                                             │
│  ──────                                               │
│   Auto-open diff review          ◯ Off                │
│   Persist drafts                 ◉ On                 │
│   Keep history                   90 days              │
│                                                       │
│  Keys                                                 │
│  ──────                                               │
│   Submit binding                 Enter                │
│                                                       │
│  ────────────────────────────────────────────────── │
│  [⏎ Edit]   [Esc] back                                │
└───────────────────────────────────────────────────────┘
```

Implementation:

- Each setting is a row: `text(label) + spacer() + value_widget`
- `↑`/`↓` move highlight
- `Enter` activates the row's editor (popup for select, toggle
  flips, input opens overlay text input)
- Settings are persisted to `~/.config/moha/settings.json` on every
  change (debounced 500ms).

For the initial rebuild, **defer settings**. Hardcode reasonable
defaults; expose CLI flags for the few overrides. Add the settings
route once defaults are stable.

## 8. Status bar (bottom)

A single-row bar at the very bottom of the panel.

```
 ↵ Send · /help · ◆ moha 0.3.0                 cwd: ~/projects/moha
```

| Slot | Content |
|---|---|
| Hint area (left) | Context-sensitive key hint (changes per route / focus) |
| App version (right) | `moha <version>` |
| Working directory (far right) | `cwd: <path>` (Dim) |

```cpp
Element views::status_bar(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    return (h(
        text(" "),
        views::context_hint(m) | Dim,
        spacer(),
        text("moha " MOHA_VERSION) | Dim,
        text("    "),
        text("cwd: " + abbreviate_home(m.cwd)) | Dim,
        text(" ")
    )
    | bg_(tokens::bg::panel)
    | padding(0, 0, 0, 0)
    ).build();
}
```

`context_hint(m)` returns the hint that's most useful right now:
- In composer with text: `"↵ Send · /help"`
- In composer empty: `"What can I help with?"`
- On a tool card with permission: `"[Y] allow [N] deny"`
- In history: `"↵ open · N new · D delete"`

This is a usability win — every screen tells the user the most
relevant key. Don't crowd it with everything.

## 9. Focus zones across the panel

The panel has a small set of focus zones:

| Zone | Description |
|---|---|
| `Chrome` | Cluster of buttons in the top bar |
| `Stream` | The conversation list |
| `Card[i]` | An individual tool card or message |
| `Composer` | The text input |
| `Toolbar` | The composer's bottom row |
| `Popover` | An open dropdown |
| `Modal` | An open modal (help, confirm) |
| `Route::*` | Owned by the active route (history list, diff hunks) |

`Tab` cycles `Chrome → Stream → Composer → Toolbar → Chrome → …`
when no popover/modal is open. Within a zone, `Tab` may further
subdivide:

- `Chrome`: model → fast → thinking → history → help
- `Toolbar`: profile → model → fast → thinking → send

`Shift+Tab` reverses.

When a popover or modal is open, `Tab` is captured by it (cycles
internal items), and `Esc` closes.

For the initial rebuild, **only implement Chrome ↔ Composer Tab**
plus `Shift+PageUp/Down` for message focus inside Stream. Full
focus traversal is polish.

## 10. Visual checklist

- [ ] Chrome renders single-line, with phase indicator pulsing during streaming
- [ ] Model selector opens a popover beneath the trigger
- [ ] Profile selector opens with description of highlighted entry
- [ ] `Ctrl+H` opens history route; chrome stays put
- [ ] History groups threads by Today/Yesterday/Last week
- [ ] History search filters in real time
- [ ] `Esc` from a route returns to previous route
- [ ] Help modal lists shortcuts grouped by section, sourced from one table
- [ ] Status bar shows context-sensitive hint
- [ ] Working directory abbreviates `$HOME` to `~`
- [ ] Popovers close on `Esc` or click outside
- [ ] Routes preserve their state when popped (history search query, etc.)
- [ ] Phase indicator color matches the design tokens table
