# 10 — Diff review (inline diffs and the multi-file review surface)

When the agent edits files, the user needs to **review** what
changed. There are two distinct surfaces:

1. **Inline diff** — the `Edit` tool card body (covered briefly in
   `07_tool_cards.md § 5.2`). One file, one hunk, displayed as
   `- old` / `+ new` lines. Always visible.
2. **Diff review surface** — a multi-file review screen that lets the
   user navigate hunks, accept or reject individual hunks, and
   confirm or undo the full set of edits. Opened explicitly.

This doc covers both, focused on the second (the larger surface).

References:
- Zed: `crates/agent_ui/src/agent_diff.rs` (the multi-file review
  view) and `crates/edit_prediction/...` for the per-hunk accept
  logic.
- maya: `maya::widget::InlineDiff`
  (`maya/include/maya/widget/inline_diff.hpp`) and
  `maya::widget::DiffView` (`maya/include/maya/widget/diff_view.hpp`).
  Plus `maya::widget::FileChanges` for the file-list summary.

## 1. When does the diff review surface open?

Three triggers:

1. **User presses `D` (or `R` for "Review")** while the agent has
   pending or applied edits. Opens the review modal route.
2. **The agent is configured to require explicit review** before
   applying edits (a permission profile setting). In this case, edits
   queue up as `Pending` and the agent surfaces a "Review changes
   before applying" callout in the message stream.
3. **End of a multi-edit turn** — once the model finishes, if 2+
   files changed, auto-show the review (configurable, off by default
   to avoid surprise).

For the initial rebuild, **only support trigger 1** (manual
`D`/`R`). Triggers 2 and 3 are quality-of-life additions; defer.

## 2. The diff review route

This is a **full-panel route** (covers the message stream + composer)
unlike permissions which are inline. Why a route?

- Multi-file diff requires a left-side file list + right-side hunk
  view. That's a 2-column layout that doesn't fit in the message
  stream's single-column rhythm.
- Hunk navigation (`J`/`K` to jump hunks) needs dedicated key
  handlers without conflicting with composer typing.
- Accept/reject is a destructive (or quasi-destructive) workflow that
  benefits from a focused screen.

```
┌─ Review Changes ──────────────────────────── [Esc] back ─┐
│                                                          │
│  Files (3)                Diff: src/render/canvas.cpp     │
│  ─────────────────        ──────────────────────────────  │
│ ▶src/render/canvas.cpp    @@ line 42                       │
│  src/api/v1.cpp           - damage_ = {0, 0, w_, h_};      │
│  docs/notes.md            + damage_ = {0, 0, 0, 0};        │
│                           + // Start with empty rect       │
│  ─────────────────        ──────────────────────────────  │
│  +12 / -7   3 hunks       @@ line 128                      │
│                           - region.invalidate();           │
│                           + region.clear();                │
│                                                            │
│                           [A] Accept   [R] Reject hunk     │
│                           [J] Next     [K] Prev            │
│                                                            │
│  ──────────────────────────────────────────────────────── │
│  [⏎ Apply All]  [✗ Reject All]  [Esc] Back                 │
└──────────────────────────────────────────────────────────┘
```

Layout:

```cpp
Element views::diff_review_route(const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    auto& dr = *m.diff_review;     // present only when route active

    return v(
        // Header bar (route title + close hint)
        h(
            text(" Review Changes ") | Bold | Style{}.with_fg(tokens::fg::text),
            spacer(),
            text("[Esc] back") | Dim
        ) | bg_(tokens::bg::panel) | padding(0, 1, 0, 1),

        // Body: 2-column split
        h(
            // Left: file list (30 cells wide)
            views::diff_file_list(dr) | size(30),

            // Vertical divider
            text("│") | Style{}.with_fg(tokens::border::dim),

            // Right: hunk view (rest)
            views::diff_hunk_view(dr) | grow_<1>
        ) | grow_<1>,

        // Footer: apply/reject all
        views::diff_footer(dr) | bg_(tokens::bg::panel) | padding(0, 1, 0, 1)
    ).build();
}
```

Total surface = full panel (under the chrome). The chrome bar at the
top stays, but its content swaps to "Review Changes mode" — see
`11_navigation § 1` (chrome adapts per route).

## 3. File list (left column)

Vertical list of files with summary stats:

```
Files (3)
─────────────────
▶src/render/canvas.cpp  +12 -7
 src/api/v1.cpp          +3 -3
 docs/notes.md          +20 -0

─────────────────
+35 / -10   3 files
```

Per-file row:

| Column | Content |
|---|---|
| Selection arrow | `▶` if selected, ` ` otherwise |
| File path | Truncated middle if too long; `text` color |
| Stats | `+N -M` (added/removed line counts), Dim |

Click (or press `↑`/`↓`) to switch the active file. Active file
governs the right pane.

Use `maya::widget::FileChanges` if it provides this — otherwise
hand-roll with `dsl::v(...)` over the file list. The widget exists
(`maya/include/maya/widget/file_changes.hpp`) but check its API
matches what we need.

### File status icons

Optional but useful — show the per-file outcome:

| Icon | Meaning |
|---|---|
| `●` | Modified (default) |
| `+` | New file |
| `−` | Deleted |
| `✓` | Already accepted (when partial accept supported) |
| `✗` | Already rejected |

Color-tint per status:
- `●` modified → `text`
- `+` new → `success`
- `−` deleted → `error`
- `✓` → `success` (Dim)
- `✗` → `error` (Dim)

## 4. Hunk view (right column)

Shows the *currently selected file's* hunks, vertically stacked.
Each hunk:

```
@@ line 42
- damage_ = {0, 0, w_, h_};
+ damage_ = {0, 0, 0, 0};
+ // Start with empty rect
```

```cpp
Element render_hunk(const Hunk& h, bool is_focused) {
    using namespace maya::dsl;
    using moha::tokens;

    std::vector<Element> rows;

    // Hunk header
    rows.push_back(
        text("@@ line " + std::to_string(h.start_line))
            | Style{}.with_fg(tokens::diff::context) | Dim);

    // Old lines
    for (auto& line : h.old_lines) {
        rows.push_back(text("- " + line)
            | Style{}.with_fg(tokens::diff::removed)
            | bg_(tokens::diff::removed_bg));
    }

    // New lines
    for (auto& line : h.new_lines) {
        rows.push_back(text("+ " + line)
            | Style{}.with_fg(tokens::diff::added)
            | bg_(tokens::diff::added_bg));
    }

    auto border_col = is_focused
        ? tokens::border::focus
        : tokens::border::dim;

    return (v(std::move(rows))
        | border(BorderStyle::Round)
        | bcolor(border_col)
        | padding(0, 1, 0, 1)).build();
}
```

The full hunk view:

```cpp
Element views::diff_hunk_view(const DiffReview& dr) {
    using namespace maya::dsl;

    auto& f = dr.files[dr.active_file_index];
    std::vector<Element> hunks;
    for (size_t i = 0; i < f.hunks.size(); ++i) {
        hunks.push_back(render_hunk(f.hunks[i], i == dr.active_hunk_index));
    }

    auto stack = (v(std::move(hunks)) | gap_<1>).build();

    return Scrollable(stack)
        .with_offset(dr.scroll.offset)
        .build();
}
```

Wrap in `Scrollable` because long files have many hunks.

### Per-hunk actions (focused hunk only)

A small action bar appears under the focused hunk:

```
[A] Accept   [R] Reject hunk
[J] Next     [K] Prev
```

Render as an inline `h(...)` row outside the bordered hunk box, only
for the focused hunk. Already-decided hunks (Accept/Reject pressed)
get a status overlay:

```
✓ Accepted    or    ✗ Rejected
```

The `Hunk` type:

```cpp
struct Hunk {
    int start_line;
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    enum class Decision { Pending, Accepted, Rejected } decision = Decision::Pending;
};
```

## 5. The use of `maya::InlineDiff`

`InlineDiff` already does word-level diff highlighting. Use it for
the per-hunk render instead of hand-rolling — less code, better
output:

```cpp
InlineDiff diff;
diff.set_label("");                         // we'll add the @@ header ourselves
diff.set_before(join_lines(h.old_lines));
diff.set_after(join_lines(h.new_lines));

InlineDiffConfig cfg;
cfg.show_line_numbers = false;              // we already have @@
cfg.intra_line_diff   = true;               // word-level highlight
diff.set_config(cfg);

return Element(diff);
```

Check the actual config struct fields — adjust to what
`inline_diff.hpp` exposes. The intent: word-level highlights for
small changes (`{0, 0, w_, h_}` → `{0, 0, 0, 0}`) without exploding
to line-replacements.

For the inline diff inside an `Edit` tool card (the smaller surface),
use the same widget with `intra_line_diff = false` (busy enough as
plain `- / +` lines).

## 6. Footer (apply/reject all)

```
[⏎ Apply All]  [✗ Reject All]  [Esc] Back
```

| Action | Effect |
|---|---|
| `Enter` | Apply only the **Accepted** hunks; revert **Rejected**; leave **Pending** as a no-op (default = accept) |
| `Shift+Enter` | Apply ALL hunks regardless of per-hunk decision |
| `R` (when not on hunk) | Reject all hunks (mass mark Rejected) |
| `Esc` | Close review without applying (changes still queued) |

The `Apply` action calls `apply_diff_cmd(dr.files)`, which actually
mutates the working tree. After success, the route closes and a
toast confirms "12 changes applied to 3 files."

If apply fails (file changed on disk between staging and applying,
write-permission denied, etc.), the route stays open with an inline
error banner above the footer:

```
⚠ Could not apply src/render/canvas.cpp — file changed on disk.
  [R] Reload  [O] Overwrite anyway  [Esc] Cancel
```

## 7. Data model

```cpp
namespace moha {

struct DiffFile {
    std::string path;
    std::vector<Hunk> hunks;
    int added_lines = 0;
    int removed_lines = 0;
    bool is_new = false;
    bool is_deleted = false;
};

struct DiffReview {
    std::vector<DiffFile> files;
    size_t active_file_index = 0;
    size_t active_hunk_index = 0;
    ScrollState scroll;
};

struct Model {
    // ...
    std::optional<DiffReview> diff_review;   // present when route is active
};

} // namespace moha
```

The `Route::DiffReview` enum value (see `04_architecture § 4`) gates
which top-level view renders. Update arm:

```cpp
[&](OpenDiffReview) -> std::pair<Model, Cmd<Msg>> {
    // Collect all pending edits from the current thread
    DiffReview dr = collect_edits(m.current);
    if (dr.files.empty()) {
        return {m, toast_cmd("No pending changes to review")};
    }
    m.diff_review = std::move(dr);
    m.route = Route::DiffReview;
    return {m, Cmd<Msg>::none()};
},
```

`collect_edits()` walks the thread's `tool_calls` for kind=`Edit` /
`Write` and aggregates them by path. Use the underlying file's
current contents as the "before" — don't trust the model's claimed
before-text (it might be stale).

## 8. Key handling

When `m.route == Route::DiffReview`:

| Key | Action |
|---|---|
| `↑` / `↓` (in file list) | Switch active file |
| `J` / `K` (in hunk view) | Next / previous hunk |
| `Tab` | Toggle focus between file list and hunk view |
| `A` | Accept the focused hunk |
| `R` | Reject the focused hunk (or reject all in footer mode) |
| `Enter` | Apply (in footer mode) |
| `Esc` | Close review |
| `PgUp` / `PgDn` | Scroll hunk view |
| `G G` | Jump to first hunk in file (vim-style) |
| `Shift+G` | Jump to last hunk in file |

Two focus zones (file list / hunk view); `Tab` toggles. Within the
hunk view, focus tracks `active_hunk_index`. There's no third focus
target (the footer reacts to `Enter`/`Esc` from anywhere, and
`Shift+Enter` for "Apply All").

## 9. Streaming and dynamism

Edits often arrive while the model is streaming. The diff review
surface should:

- Accept new edits live: when a new `Edit` tool result lands, append
  to the appropriate file (or create a new file entry).
- Preserve `active_file_index` / `active_hunk_index` across updates
  when possible (re-resolve by file path + hunk start line).
- Highlight newly-arrived hunks briefly (e.g., `border::focus` for
  ~2 seconds, then fade to default). This is a polish detail; defer
  for the initial rebuild.

If the user opens the review surface mid-stream, render what's
already there — the user can scroll while more arrives.

## 10. Inline diff vs review surface — when to use which

| Concern | Inline (in Edit card) | Review surface (route) |
|---|---|---|
| Quick glance at one edit | ✓ | — |
| See all edits in one place | — | ✓ |
| Accept/reject per hunk | — | ✓ |
| Default visibility | Always (expanded) | On demand (`D`/`R` to open) |
| Word-level highlight | Optional (off by default) | On |
| Side-by-side / split view | — | Future enhancement (defer) |

Both share the same `Hunk` data shape and the same color tokens —
the only difference is the surrounding container.

## 11. Defer list

For the initial rebuild, **defer** these:

- Side-by-side diff layout (current spec is unified `- / +` only)
- Word-level intra-line highlighting (text-level diff is enough)
- Per-hunk accept/reject (just have "Apply all" / "Reject all")
- Auto-open at end of multi-edit turn
- Pre-apply review profile setting

Implement the simplest thing that works:
1. `D`/`R` opens a route with a list of changed files
2. Right column shows all hunks for the active file
3. Footer has Apply All / Reject All / Esc
4. That's it.

Add per-hunk accept and word-level highlights once the basic flow
feels right.

## 12. Visual checklist

After implementing diff review, verify:

- [ ] `D` from a tool card or assistant message opens the review route
- [ ] File list shows all changed files with `+N -M` stats
- [ ] Right pane shows hunks for the active file
- [ ] `↑`/`↓` switch active file; selection visible (`▶` arrow)
- [ ] `J`/`K` move between hunks; focused hunk has accent border
- [ ] Removed lines red-on-darker-red; added lines green-on-darker-green
- [ ] `@@ line N` header per hunk in dim
- [ ] Footer buttons render at bottom
- [ ] `Enter` applies changes; toast confirms with file count
- [ ] `Esc` closes route, returns to message stream (changes still queued)
- [ ] If apply fails, error banner appears above footer
- [ ] Live updates: new edits during streaming append to file list
- [ ] Long files scroll within the right pane (Scrollable)
- [ ] Inline diff in Edit card body uses same colors as review surface
