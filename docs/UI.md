# Moha UI — Maya primitive reference

How the moha terminal UI is built, primitive by primitive. Every example here is copied from the moha source — file:line citations point you at the real call site.

The whole UI lives under `src/runtime/view/`. Every file in that directory pulls in `maya::dsl` via:

```cpp
using namespace maya;
using namespace maya::dsl;
```

Anything in the `dsl::` namespace becomes unqualified inside `moha::ui`. That's why you see `text(...)`, `h(...)`, `v(...)`, `border(...)` rather than `maya::dsl::text(...)`.

---

## 1. Top-level shape

The whole screen is composed in [`view.cpp:17-43`](../src/runtime/view/view.cpp). One vertical stack with optional centered overlay:

```cpp
auto base = (v(
    v(thread_panel(m)) | grow(1.0f),
    changes_strip(m),
    composer(m),
    status_bar(m)
) | pad<1> | grow(1.0f)).build();

if (has_overlay)
    return zstack({std::move(base),
        vstack().align_items(Align::Center).justify(Justify::End)(
            vstack().bg(Color::default_color())(std::move(overlay)))});
return base;
```

That's the whole tour: thread panel grows to fill, then the changes strip / composer / status bar pin to the bottom. Modals are layered with `zstack` and centered with the runtime `vstack` builder.

The runtime entry point is at [`runtime/main.cpp:223`](../src/runtime/main.cpp):

```cpp
maya::run<app::MohaApp>({.title = "moha", .fps = 0, .mode = maya::Mode::Inline});
```

`Mode::Inline` means the canvas lives in the terminal's native scrollback; `fps = 0` means renders are pure event-driven (no idle redraws). The Tick subscription supplies frames during streaming.

---

## 2. DSL builders (compile-time tree)

These are constexpr factories in `maya::dsl`. They build a node tree that materializes via `.build()` to a `maya::Element`.

### `v(children...)` — vertical stack
Source: [`maya/include/maya/dsl.hpp:620`](../maya/include/maya/dsl.hpp). `BoxNode<FlexDirection::Column, …>`. Used to stack rows.

```cpp
// src/runtime/view/login.cpp:84
return v(std::move(rows)).build();
```

### `h(children...)` — horizontal stack
Source: [`maya/include/maya/dsl.hpp:628`](../maya/include/maya/dsl.hpp). `BoxNode<FlexDirection::Row, …>`. Used for inline rows.

```cpp
// src/runtime/view/thread.cpp:130-137  — turn header
return (h(
    text(style.glyph, fg_of(style.color)),
    text(" ", {}),
    text(std::move(style.label), Style{}.with_fg(style.color).with_bold()),
    spacer(),
    text(std::move(meta), fg_dim(muted)),
    text(" ", {})
) | grow(1.0f)).build();
```

### `text(content, style?)` — runtime text node
Source: [`dsl.hpp:280`](../maya/include/maya/dsl.hpp). Returns `RuntimeTextNode` (deduces `string_view`/`string`/`int`/`double`). Pipeable. Defaults to `TextWrap::Wrap`; pass `TextWrap::TruncateEnd` as third arg for ellipsizing single-line cells.

```cpp
// src/runtime/view/login.cpp:75
text(std::string{"\xE2\x9A\xA0 "} + std::string{fail_msg}, fg_of(danger))
```

### `spacer()` — flex-grow gap
Source: [`dsl.hpp:342`](../maya/include/maya/dsl.hpp). Empty box with `grow=1.0f`. Inside a row it pushes its right siblings to the right edge.

```cpp
// src/runtime/view/diff_review.cpp:44-46
h(text(fc.path, fg_bold(fg)),
  spacer(),
  text(std::format("+{}", fc.added), fg_of(success)), …)
```

### `sep` — horizontal rule
Source: [`dsl.hpp:321`](../maya/include/maya/dsl.hpp). Single-line top/bottom border on a hollow box. Used as a visual divider inside modals.

```cpp
// src/runtime/view/diff_review.cpp:54
rows.push_back(sep);
```

### `vstack()` / `hstack()` / `zstack()` — runtime fluent builders
Source: `dsl.hpp` re-exports from `maya::detail` ([`dsl.hpp:648-653`](../maya/include/maya/dsl.hpp)). Use these when you need to set layout properties (e.g. `align_items`, `justify`, `bg`) before passing children.

```cpp
// src/runtime/view/view.cpp:38-40  — overlay centered at bottom
zstack({std::move(base),
    vstack().align_items(Align::Center).justify(Justify::End)(
        vstack().bg(Color::default_color())(std::move(overlay)))});

// src/runtime/view/statusbar.cpp:440 — status bar parts
auto left  = hstack()(std::move(lparts));
```

`zstack` takes a list/initializer-list of children; each child is a layer painted bottom-to-top.

---

## 3. Runtime pipes (`|` operators)

Source: [`dsl.hpp:760-953`](../maya/include/maya/dsl.hpp). Each pipe wraps the node in a `WrappedNode` that applies layout/style modifiers when `.build()` runs. Multiple pipes compose:

```cpp
// src/runtime/view/login.cpp:152-156
auto content = (v(std::move(body)) | padding(1, 2) | width(70));
return (v(content.build())
        | border(BorderStyle::Round) | bcolor(accent)
        | btext(" Sign in to moha ", BorderTextPos::Top, BorderTextAlign::Center)
        ).build();
```

Catalog:

| Pipe | Effect |
|---|---|
| `padding(all)` / `padding(v,h)` / `padding(t,r,b,l)` | Inner padding in cells. `pad<N>` is the compile-time form. |
| `margin(all)` / `margin(v,h)` / `margin(t,r,b,l)` | Outer margin. |
| `gap(n)` | Spacing between children. |
| `grow(f)` | Flex-grow factor. `grow(1.0f)` is "fill remaining space." |
| `width(n)` / `height(n)` | Fixed cell dimensions. |
| `border(BorderStyle)` | Sets the border glyph style (Round / Single / Bold / Double). |
| `bcolor(Color)` | Border color. Requires a border to be set. |
| `btext(s, pos?, align?)` | Border label inset into the top or bottom edge. |
| `fgc(Color)` / `bgc(Color)` | Foreground / background style on the wrapped node. |
| `align(Align)` | Cross-axis child alignment (Start / Center / End / Stretch). |
| `justify(Justify)` | Main-axis child distribution (Start / Center / End / SpaceBetween / SpaceAround). |
| `overflow(Overflow)` | Clip behavior. |

Compile-time variants exist as templates (`pad<1>`, `border_<Round>`, `bcol<R,G,B>`, `grow_<1>`); moha mostly uses the runtime variants because colors come from the palette helpers.

The `pad<1>` form on the outer view ([`view.cpp:23`](../src/runtime/view/view.cpp)) is compile-time — 1 cell of padding on every side, baked into the node type.

---

## 4. Element types and downcasts

Source: [`maya/include/maya/element/`](../maya/include/maya/element/). `maya::Element` is a variant over `TextElement`, `BoxElement`, `ElementList`, `ComponentElement`. Moha builds each variant directly when the DSL doesn't fit:

### `TextElement` — direct construction
Used to inject `StyledRun` cells where each character has its own color. The status bar's context gauge does this so the bar can transition green→amber→red across a single line:

```cpp
// src/runtime/view/statusbar.cpp:92-96
return Element{TextElement{
    .content = std::move(content),
    .style = {},
    .runs = std::move(runs),
}};
```

### `ComponentElement` — width-aware lambda
The render closure is invoked by maya at layout time with the available `(w, h)`. Lets the view make decisions (truncate / drop columns / change layout) based on terminal size.

```cpp
// src/runtime/view/thread.cpp:163-178  — inter-turn divider
Element{ComponentElement{
    .render = [](int w, int /*h*/) -> Element {
        std::string line;
        int indent = 3;
        for (int i = 0; i < indent; ++i) line += ' ';
        for (int i = indent; i < w; ++i) line += "\xe2\x94\x80";  // ─
        return Element{TextElement{
            .content = std::move(line),
            .style = Style{}.with_fg(Color::bright_black()).with_dim(),
        }};
    },
    .layout = {},
}};
```

The composer's hint row uses the same pattern to drop secondary shortcuts on narrow widths ([`composer.cpp:263-274`](../src/runtime/view/composer.cpp)). The status bar's right-side group is another big example ([`statusbar.cpp:589-636`](../src/runtime/view/statusbar.cpp)).

### `maya::detail::box()` — low-level builder
When the DSL borders aren't expressive enough (e.g. asymmetric border sides). Used to draw the per-turn left rail:

```cpp
// src/runtime/view/thread.cpp:147-156
Element with_turn_rail(Element content, Color rail_color) {
    return maya::detail::box()
        .direction(FlexDirection::Row)
        .border(BorderStyle::Bold, rail_color)
        .border_sides({.top = false, .right = false,
                       .bottom = false, .left = true})
        .padding(0, 0, 0, 2)
        .grow(1.0f)
      (std::move(content));
}
```

`border_sides({…})` lets you turn individual edges on/off — only the left bar is drawn, in the speaker's color, running the full height of the turn.

### Downcasts
Moha doesn't currently use `as_text` / `as_box` / `as_list` in this commit's tree, but they exist in `maya::` for consumers that want to walk the element tree.

---

## 5. Styles and the moha palette

### `maya::Style` — fluent style builder
Source: [`maya/include/maya/style/style.hpp`](../maya/include/maya/style/style.hpp). Chainable: `with_fg`, `with_bg`, `with_bold`, `with_dim`, `with_italic`, `with_underline`, `with_strikethrough`, `with_inverse`. Used directly when a chip needs a combo not in the palette helpers:

```cpp
// src/runtime/view/statusbar.cpp:430-432  — status-bar leading rail
Style rail_style = breathing
    ? Style{}.with_fg(pcolor).with_bold()
    : Style{}.with_fg(pcolor).with_dim();
```

### Palette (moha-side)
Source: [`include/moha/runtime/view/palette.hpp`](../include/moha/runtime/view/palette.hpp). Named ANSI only — the user's terminal theme always wins.

| Name | ANSI color | Role |
|---|---|---|
| `fg` | `bright_white` | Primary body text (max contrast) |
| `muted` | `bright_black` | Chrome, metadata, dim labels |
| `accent` | `magenta` | Brand / Write profile |
| `info` | `blue` | Ask profile / threads / "in progress" |
| `success` | `green` | Done / accepted / on-track |
| `warn` | `yellow` | Pending / caution |
| `danger` | `red` | Errors / rejected / "stalling" |
| `highlight` | `cyan` | Command palette / queue depth / mentions |

### Palette helpers
Same file. Compose `Style` instances so call sites stay tight:

| Helper | Returns |
|---|---|
| `fg_of(c)` | `Style{}.with_fg(c)` |
| `fg_bold(c)` | `Style{}.with_fg(c).with_bold()` |
| `fg_dim(c)` | `Style{}.with_fg(c).with_dim()` — except for `muted` (already bright_black; stacking dim makes it unreadable on dark themes), which returns the plain color |
| `fg_italic(c)` | `Style{}.with_fg(c).with_italic()` |
| `dim()` / `bold()` / `italic()` | Style with that attribute, no fg override (terminal default) |

```cpp
// src/runtime/view/diff_review.cpp:45-49
h(text(fc.path, fg_bold(fg)),               // bright bold
  spacer(),
  text(std::format("+{}", fc.added), fg_of(success)),
  text(" "),
  text(std::format("-{}", fc.removed), fg_of(danger)))
```

### Phase-driven colors
[`helpers.cpp:42-55`](../src/runtime/view/helpers.cpp) maps the streaming phase to a color:

```cpp
maya::Color phase_color(const Phase& p) noexcept {
    if (Idle)               return muted;
    if (Streaming)          return Color::bright_cyan();
    if (AwaitingPermission) return Color::bright_yellow();
    if (ExecutingTool)      return Color::bright_green();
}
```

Used by the composer's border, the status bar's leading rail, and the bottom edge accents — so all three pieces of state read in the same color.

---

## 6. Layout enums

| Enum | Where moha uses it |
|---|---|
| `FlexDirection::Row / Column` | `with_turn_rail` row direction; DSL `v`/`h` set these implicitly |
| `Justify::SpaceBetween / Center / End / Start` | Overlay centering in [`view.cpp:39`](../src/runtime/view/view.cpp) (`justify(Justify::End)`) |
| `Align::Center / Start / End / Stretch` | Overlay alignment ([`view.cpp:39`](../src/runtime/view/view.cpp)) |
| `BorderStyle::Round / Single / Bold / Double` | `Round` is the dominant choice for modals + composer; `Bold` is used for the per-turn rail |
| `BorderTextPos::Top / Bottom` | Modal titles on top, line counts on bottom |
| `BorderTextAlign::Start / Center / End` | Modal titles centered, line counts right-aligned |
| `TextWrap::Wrap / TruncateEnd / NoWrap` | Default `Wrap`; `TruncateEnd` for single-line truncation |
| `BorderSides{top,right,bottom,left}` | Asymmetric borders — only the left rail in `with_turn_rail` |

---

## 7. Maya widgets used by moha

Each widget is a stateful builder you instantiate, configure, then `.build()` into an `Element`. Inclusion lives in the cpp that uses it.

### Markdown — `markdown.hpp`
Two flavors:

- `maya::markdown(text)` — one-shot. Used for finalized assistant messages because the text is immutable; cached forever via `MessageMdCache::finalized`.
- `maya::StreamingMarkdown` — incremental. `set_content()` accepts the growing buffer; internal block-boundary cache makes each delta `O(new_chars)` rather than re-parsing the whole transcript.

```cpp
// src/runtime/view/thread.cpp:34-48  — both paths
Element cached_markdown_for(const Message& msg, const ThreadId& tid, std::size_t msg_idx) {
    auto& cache = message_md_cache(tid, msg_idx);
    if (msg.text.empty()) {
        if (!cache.streaming)
            cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_content(msg.streaming_text);
        return cache.streaming->build();
    }
    if (!cache.finalized) {
        cache.finalized = std::make_shared<Element>(maya::markdown(msg.text));
        cache.streaming.reset();
    }
    return *cache.finalized;
}
```

### `ToolCall` — `tool_call.hpp`
Generic tool card chrome (border + title + icon + expanded body). Used as the fallback when no specialized widget exists ([`tool_card.cpp:70-85`](../src/runtime/view/tool_card.cpp)):

```cpp
maya::ToolCall::Config cfg;
cfg.tool_name = name;
cfg.kind = kind;
cfg.description = desc;
maya::ToolCall card(cfg);
card.set_expanded(expanded);
card.set_status(tc_status(status));
card.set_elapsed(elapsed);
if (!output.empty())
    card.set_content(text(output, fg_of(muted)));
return card.build();
```

`ToolCallStatus` enum: `Running / Completed / Failed`. `ToolCallKind` is a hint for the icon family (Edit / Read / Other / etc.).

### Per-tool widgets — `read_tool.hpp`, `write_tool.hpp`, `edit_tool.hpp`, `bash_tool.hpp`, `fetch_tool.hpp`, `search_result.hpp`, `git_commit_tool.hpp`, `git_status.hpp`, `git_graph.hpp`, `todo_list.hpp`
Each models its tool's specific affordances:

- **`ReadTool`** — `set_start_line`, `set_total_lines`, `set_max_lines`, status enum `ReadStatus::{Reading, Failed, Success}`. Used for both `read` and `list_dir` ([`tool_card.cpp:185-215`](../src/runtime/view/tool_card.cpp)).
- **`WriteTool`** — `set_content`, `set_max_preview_lines`. Status `WriteStatus::{Writing, Failed, Written}`. Falls back to a "(streaming…)" header while args are still arriving ([`tool_card.cpp:217-243`](../src/runtime/view/tool_card.cpp)).
- **`EditTool`** — `set_old_text` / `set_new_text` for legacy single-edit shape; `set_edits(vector<EditPair>)` for the canonical `edits[]` array. Always expanded — the diff IS the point ([`tool_card.cpp:245-317`](../src/runtime/view/tool_card.cpp)).
- **`BashTool`** — `set_output`, `set_exit_code`, `set_max_output_lines`. While running, the output field is fed `tc.progress_text()` (live stdout snapshot); on completion it gets the final fenced output ([`tool_card.cpp:319-338`](../src/runtime/view/tool_card.cpp)).
- **`SearchResult`** — `add_group(SearchFileGroup)`, `set_max_matches_per_file`. Two `SearchKind` modes (`Grep` / `Glob`); the parser handles both moha's markdown grep output and the legacy `path:line:content` shape ([`tool_card.cpp:87-173`](../src/runtime/view/tool_card.cpp)).
- **`FetchTool`** — `set_url`, `set_status_code`, `set_content_type`, `set_body`. Used for both `web_fetch` and `web_search` (the search results body is rendered as the fetch body) ([`tool_card.cpp:390-436`](../src/runtime/view/tool_card.cpp)).
- **`GitStatusWidget`** — `set_branch`, `set_ahead/behind`, `set_dirty(M, S, U)`. Parser walks `--porcelain=v2` output ([`tool_card.cpp:438-478`](../src/runtime/view/tool_card.cpp)).
- **`GitGraph`** + **`GitCommit`** — Each commit added with `add_commit()`. Renders an ASCII commit graph ([`tool_card.cpp:480-523`](../src/runtime/view/tool_card.cpp)).
- **`GitCommitTool`** — Specialized commit card with subject, body, output. ([`tool_card.cpp:551-560`](../src/runtime/view/tool_card.cpp)).
- **`TodoListTool`** + **`TodoListItem`** — `add(TodoListItem)` per todo; status enum `TodoItemStatus::{Pending, InProgress, Completed}` ([`tool_card.cpp:562-587`](../src/runtime/view/tool_card.cpp)).

### `DiffView` — `diff_view.hpp`
Renders a unified-diff string into colored hunks. Used inside `git_diff` cards and in the diff review modal:

```cpp
// src/runtime/view/diff_review.cpp:66-67
DiffView dv(fc.path, h_.patch);
rows.push_back((v(dv.build()) | padding(0, 0, 0, 2)).build());
```

### `FileChanges` — `file_changes.hpp`
Compact summary of pending file changes — the strip below the thread when there are uncommitted edits to review:

```cpp
// src/runtime/view/changes.cpp:16-22
FileChanges fc;
for (const auto& c : m.d.pending_changes) {
    auto kind = c.original_contents.empty()
        ? FileChangeKind::Created
        : FileChangeKind::Modified;
    fc.add(c.path, kind, c.added, c.removed);
}
```

### `Permission` — `permission.hpp`
Inline approval card. `Permission::Config` carries `tool_name` + `description` + `show_always_allow`:

```cpp
// src/runtime/view/permission.cpp:35-40
Permission::Config cfg;
cfg.tool_name = tc.name.value;
cfg.description = desc.empty() ? pp.reason : desc;
cfg.show_always_allow = true;
Permission perm(std::move(cfg));
return perm.build();
```

### `ModelBadge` — `model_badge.hpp`
Brand chip for the current model. `set_model(id)` parses the model id; `set_compact(true)` shrinks for the status bar:

```cpp
// src/runtime/view/statusbar.cpp:489-494
ModelBadge mb;
mb.set_model(m.d.model_id.value);
mb.set_compact(true);
right_parts.push_back(mb.build());
```

### `compact_token_stream` — `token_stream.hpp`
Live tok/s number + sparkline. Returns an Element directly (free function, not a builder class):

```cpp
// src/runtime/view/statusbar.cpp:480-485
right_parts.push_back(compact_token_stream(
    disp_rate, static_cast<int>(approx_tok),
    std::span<const float>{hist.data(), hist.size()},
    highlight, /*live=*/is_streaming));
```

### `PlanView` + `TaskStatus` — `plan_view.hpp`
Used by the `/plan` modal to render the agent's todo list. `TaskStatus::{Pending, InProgress, Completed}`:

```cpp
// src/runtime/view/pickers.cpp:129-139
maya::PlanView plan;
for (const auto& item : m.ui.todo.items) {
    maya::TaskStatus ts;
    switch (item.status) {
        case TodoStatus::Pending:    ts = maya::TaskStatus::Pending;    break;
        case TodoStatus::InProgress: ts = maya::TaskStatus::InProgress; break;
        case TodoStatus::Completed:  ts = maya::TaskStatus::Completed;  break;
    }
    plan.add(item.content, ts);
}
rows.push_back(plan.build());
```

### `Spinner<SpinnerStyle::Dots>`
Lives on `StreamState::spinner` ([`include/moha/domain/session.hpp`](../include/moha/domain/session.hpp)). The reducer's Tick handler calls `m.s.spinner.advance(dt)` to step the frame; the view calls `.build()` (or pulls the frame index for use in a custom row).

---

## 8. The cache layer (moha-side, not maya)

Three caches in [`include/moha/runtime/view/cache.hpp`](../include/moha/runtime/view/cache.hpp):

- **`tool_card_cache(ToolCallId)`** — keyed cache of rendered tool-card Elements, only populated for terminal-state tools. Re-rendered if `compute_render_key()` (FNV over output size + status + expanded) doesn't match. Purpose: a chat with 40 finished tool calls otherwise rebuilds 40 borders + Yoga layouts every frame ([`tool_card.cpp:601-612`](../src/runtime/view/tool_card.cpp)).
- **`message_md_cache(ThreadId, msg_idx)`** — finalized markdown built once and reused; streaming markdown reuses the same `StreamingMarkdown` instance across deltas so block-boundary caching kicks in ([`thread.cpp:34-48`](../src/runtime/view/thread.cpp)).
- **(in this commit, no whole-turn cache)** — render_message rebuilds the turn shell every frame; later commits added a `TurnElementCache` for inline-mode scrollback stability.

---

## 9. Cmds and Subs (side effects + event sources)

### `Cmd<Msg>` factories used by moha
Source: [`maya/include/maya/core/cmd.hpp`](../maya/include/maya/core/cmd.hpp). Returned from the reducer to ask the runtime to do work.

| Factory | Used in moha | What it does |
|---|---|---|
| `Cmd<Msg>::none()` | Many reducer arms | No side effect. |
| `Cmd<Msg>::task(fn)` | [`cmd_factory.cpp:57, 73, 240, 253, 264`](../src/runtime/app/cmd_factory.cpp) | Run `fn(dispatch)` on a worker thread; `fn` calls `dispatch(Msg)` to push events back. Used for streaming, tool execution, model fetching, browser open, compaction. |
| `Cmd<Msg>::after(d, msg)` | [`cmd_factory.cpp:195`](../src/runtime/app/cmd_factory.cpp) | Emit `msg` after duration `d`. Used for tool-watchdog timeouts. |
| `Cmd<Msg>::batch(cmds…)` | [`cmd_factory.cpp:236`](../src/runtime/app/cmd_factory.cpp) | Combine multiple cmds into one return. |
| `Cmd<Msg>::commit_scrollback(rows)` | Inline virtualization (added in later commits) | Tell maya "rows 0..N are now in terminal scrollback; don't try to update them." |
| `Cmd<Msg>::reset_frame()` | Inline ghost-fix paths (added in later commits) | Force the next compose down the FRESH-DRAW path. |

### `Sub<Msg>` factories used by moha
Source: [`maya/include/maya/app/sub.hpp`](../maya/include/maya/app/sub.hpp). Returned from `subscribe(Model)` to declare event sources.

```cpp
// src/runtime/app/subscribe.cpp:222-260  — subscriptions
auto key_sub   = Sub<Msg>::on_key(/* keymap filter */);
auto paste_sub = Sub<Msg>::on_paste([in_login](std::string s) -> Msg { … });
if (m.s.active()) {
    auto tick = Sub<Msg>::every(std::chrono::milliseconds(33), Tick{});
    return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub), std::move(tick));
}
return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub));
```

`Sub::every` is the spinner pump — only subscribed while `m.s.active()` (anything but Idle). When the stream ends and the phase drops back to Idle, the Tick subscription unsubscribes and idle moha goes to 0% CPU until the user presses a key.

---

## 10. Putting it all together — the composer at rest

[`composer.cpp:55-296`](../src/runtime/view/composer.cpp) is the densest single example of moha's UI conventions. It uses:

- **State-driven color** — `box_color` switches on phase / has-text / awaiting-permission so the border and prompt arrow read as the input box's status indicator (no extra chrome needed). [`composer.cpp:78-83`](../src/runtime/view/composer.cpp)
- **Manual cursor injection** — a `▎` glyph is inserted into the text at the byte cursor index so when the lines are split on `\n` the cursor lands on the right visual row. [`composer.cpp:89-91`](../src/runtime/view/composer.cpp)
- **Compile-time + runtime composition** — `v(body_rows) | padding(0, 1) | height(rows)` builds the inner box with the runtime pipes. [`composer.cpp:153`](../src/runtime/view/composer.cpp)
- **`ComponentElement` for responsive layout** — the hint row is a render closure that takes the current width and drops "newline" / "expand" hints below 90 / 60 cols. [`composer.cpp:181-198, 263-274`](../src/runtime/view/composer.cpp)
- **Tabular formatting for jitter-free numbers** — `tabular_int(words, 4)` / `tabular_int(toks, 4)` make sure the right-side counters don't bob left/right as the user types. [`composer.cpp:235, 239`](../src/runtime/view/composer.cpp)
- **Border + bcolor + btext composition** — round border in the state color, with a bottom-right line-count caption when the input wraps. [`composer.cpp:284-294`](../src/runtime/view/composer.cpp)

If you want to read just one file to see how every primitive in this doc plays together, start there.

---

## Index of view files

| File | What it renders |
|---|---|
| [`view.cpp`](../src/runtime/view/view.cpp) | Top-level layout + overlay composition |
| [`thread.cpp`](../src/runtime/view/thread.cpp) | Conversation panel, turn headers, rails, dividers, tool timeline |
| [`tool_card.cpp`](../src/runtime/view/tool_card.cpp) | Per-tool widget dispatch + terminal-state caching |
| [`composer.cpp`](../src/runtime/view/composer.cpp) | Multi-line input with state-driven border + responsive hint row |
| [`statusbar.cpp`](../src/runtime/view/statusbar.cpp) | Phase chip, breadcrumb, tok/s sparkline, context gauge, shortcut row |
| [`changes.cpp`](../src/runtime/view/changes.cpp) | Pending-changes summary strip |
| [`diff_review.cpp`](../src/runtime/view/diff_review.cpp) | Per-hunk accept/reject modal |
| [`pickers.cpp`](../src/runtime/view/pickers.cpp) | Model / threads / command palette / todo modals |
| [`login.cpp`](../src/runtime/view/login.cpp) | OAuth + API-key entry modal |
| [`permission.cpp`](../src/runtime/view/permission.cpp) | Inline tool-permission card + checkpoint divider |
| [`helpers.cpp`](../src/runtime/view/helpers.cpp) | `phase_color`, `phase_glyph`, `small_caps`, `tabular_int`, etc. |
| [`cache.cpp`](../src/runtime/view/cache.cpp) | Thread-local UI element caches |
