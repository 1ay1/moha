# 07 — Tool cards

Tool cards are the **signature** element of Zed's agent UX. They turn
opaque tool calls into legible, status-bearing units: each one has a
header (what's running), a body (what it produced), an optional footer
(permission decision), and a clear life cycle (pending → running →
done/failed). Get the tool cards right and the panel feels
professional. Get them wrong and the panel feels like a debug log.

This doc is the per-tool spec. It covers:

1. The shared shell (every tool card looks like this on the outside)
2. The status / border / header rules
3. Expand / collapse behavior
4. Per-tool variants: `read`, `edit`, `write`, `bash`, `search`,
   `fetch`, `think`, `agent`, generic `other`
5. Permission footer integration (cross-link to `09_permissions.md`)
6. The data model (`ToolUse`) and how each variant maps to it

References:
- Zed: `crates/agent_ui/src/agent_panel.rs`,
  `crates/agent_ui/src/thread_view.rs:6240-7550` (tool card render
  paths). The `ToolUseCard` struct is the per-tool render entry point.
- maya: `maya/include/maya/widget/tool_call.hpp` (generic shell),
  plus per-tool widgets `bash_tool.hpp`, `edit_tool.hpp`,
  `read_tool.hpp`, `write_tool.hpp`, `fetch_tool.hpp`,
  `agent_tool.hpp`. These already implement the shape — this doc tells
  you how to *use* and *extend* them.

## 1. The shared shell

Every tool card has the same outer shape, regardless of tool kind:

```
╭─ <icon> <tool name> ──────────────────────────╮
│ <header line: description / file path / cmd>   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈│   ← only when expanded
│ <body: per-tool content>                       │   ← only when expanded
│ ─────────────────────────────────────────────── │   ← only if permission needed
│ [A]llow  [D]eny  [⌥A] Always allow             │   ← permission footer
╰────────────────────────────────────────────────╯
```

Visual properties:

| Element | Value | Notes |
|---|---|---|
| Border | `BorderStyle::Round` (normal) / `Dashed` (failed/cancelled) | See § 3 |
| Border color | `tokens::border::dim` (50, 56, 66) by default | See § 3 |
| Border label | `" <icon> <tool_name> "` | Top-Start position; spaces around |
| Background | none (transparent over conversation bg) | Zed uses `editor_background`; for TUI we leave it blank — borders are enough |
| Padding | `padding(0, 1, 0, 1)` | Border eats one cell on each side already |
| Margin | One row above + below (via parent's `gap_<1>`) | Don't add `padding(1, 0, 1, 0)` to the card itself |
| Max width | Inherits parent column (120 cells) | Don't constrain inside the card |
| Indent from chrome | 2 cells left, 2 cells right | Same as assistant text — they share the column |

The `ToolCall` widget already enforces this:

```cpp
auto card = ToolCall(ToolCall::Config{
    .tool_name = "Read",
    .kind      = ToolCallKind::Read,
    .description = "src/main.cpp",
});
card.set_status(ToolCallStatus::Completed);
card.set_elapsed(0.3f);
card.set_expanded(false);
Element e = card;   // implicit operator Element()
```

For per-tool variants (Edit, Bash, Read, …), use the typed widgets
instead of `ToolCall`. They produce the same shell but render a
tool-specific body.

## 2. Header anatomy

The header lives **inside** the card body, immediately under the
border label. It carries:

- **Description** (path, command, search query, URL) — primary info
- **Elapsed time** (right-aligned, dim) — appears once `elapsed > 0`
- For `Edit` cards specifically: a `(+N -M)` summary if hunks counted

Render rules (from `tool_call.hpp:106-135`):

- Description: `text` color (171, 178, 191), no wrap
- Two spaces between description and elapsed
- Elapsed: `Dim` style. Format:
  - `< 1s` → `"123ms"` (no decimal)
  - `1s ≤ x < 60s` → `"4.2s"` (one decimal)
  - `≥ 60s` → `"2m13s"`

The header **does not wrap**. Long paths are truncated by maya's
text rendering; if we want explicit truncation, set
`wrap_<TextWrap::TruncateMiddle>` on the description run.

## 3. Status, border style, border color

This is the **single most important table** in this doc. Every tool
card derives its visual from `status`:

| Status | Icon | Icon color | Border style | Border color |
|---|---|---|---|---|
| `Pending` | `○` (U+25CB) | `text_subtle` (92, 99, 112), Dim | Round | `border::dim` (50, 56, 66) |
| `Running` | `●` (U+25CF) | `warning` (229, 192, 123) | Round | `border::dim` (50, 56, 66) |
| `Completed` / `Success` / `Applied` / `Done` / `Written` | `✓` (U+2713) | `success` (152, 195, 121) | Round | `border::dim` (50, 56, 66) |
| `Failed` | `✗` (U+2717) | `error` (224, 108, 117) | **Dashed** | `border::failed` (120, 60, 65) |
| `Cancelled` | `✗` (U+2717) | `text_subtle` (92, 99, 112) | **Dashed** | `text_subtle` (92, 99, 112) |
| `Confirmation` (awaiting permission) | `⚠` (U+26A0) | `warning` (229, 192, 123) | Round | `border::warning` (120, 100, 50) |

Notes:

- Each per-tool widget has its own status enum (`BashStatus`,
  `EditStatus`, `ReadStatus`, etc.) — the *meanings* match this table
  but the names differ slightly (`Success` vs `Completed` vs `Done`).
  When a moha tool callback resolves, **map** to the typed enum. Don't
  invent a 7th status.
- The `Cancelled` status is not yet present in any of maya's
  per-tool widgets. For now, fold cancellation into `Failed` with a
  more explicit message ("Cancelled by user"). Add a proper `Cancelled`
  enum value if cancellation becomes a first-class affordance.
- `Confirmation` lives only on the generic `ToolCall` widget. The
  typed widgets are rendered *after* permission is granted, so they
  never enter `Confirmation` state. The card you see while waiting for
  permission is always the generic shell + a permission footer.

### Why dashed for failed?

Solid Round border + red color reads as "this is a normal card, just
red." Dashed border signals "this didn't complete the way it was
supposed to." The eye picks it up faster. Zed uses solid borders
throughout; the dashed variant is a moha addition that survives the
loss of color (e.g., on accessibility-mode terminals that drop to
mono). Keep it.

## 4. Expand / collapse

### Default state

The card defaults to **collapsed**: header only, no body. The
disclosure cue is the absence/presence of the `┈┈┈` separator line
(visible only when expanded).

| Tool | Default state | Reasoning |
|---|---|---|
| `read` (small file < 50 lines) | Collapsed | Header `path · 42 lines` is enough |
| `read` (large file or partial) | Collapsed | Same — disclosed body shows the actual lines |
| `write` (new file) | Collapsed | Header `path · 142 lines written` is enough |
| `edit` | **Expanded** by default | The diff is the whole point |
| `bash` (success, short output) | Collapsed | Just show the command |
| `bash` (failed) | **Expanded** by default | Failure output is what the user wants |
| `search` (results found) | Collapsed | Header `query · 12 matches` |
| `search` (no results) | Collapsed | Header `query · no matches` |
| `fetch` | Collapsed | Header `url · 200 OK · 4.5kb` |
| `think` (extended thinking) | **Expanded** when streaming, collapsed once done | Reading the model think feels live |
| `agent` (subagent invocation) | Collapsed | Disclosed body shows nested cards |

This isn't enforced by the widgets — the *caller* (moha) decides the
initial `expanded` flag based on tool kind + outcome. Centralize the
rule:

```cpp
bool default_expanded(const ToolUse& tu) {
    if (tu.kind == ToolKind::Edit) return true;
    if (tu.kind == ToolKind::Bash && tu.status == ToolStatus::Failed) return true;
    if (tu.kind == ToolKind::Think && tu.status == ToolStatus::Running) return true;
    return false;
}
```

### Toggle

Pressing `Enter` (or `Space`) on a focused card toggles expand state.
This is `Msg::ToolCardToggle{tool_id}` — the update arm flips
`tu.expanded` and re-renders. Don't store expanded state inside the
widget instance; store it in the `ToolUse` record so it survives
re-renders.

### Indicator

Zed adds a subtle `▾` / `▸` chevron at the right end of the header. We
**don't** in moha — the `┈┈┈` separator + body presence is the cue.
Adding a chevron crowds the header and forces a right-align computation
that's awkward in our DSL. Skip it.

If usability testing shows the disclosure isn't discoverable, add a
single-cell `▸`/`▾` marker as the first character of the header line.

## 5. Per-tool variants

Each variant is a typed widget that produces the shared shell + a
custom body. Use the typed widget when you have structured data; fall
back to `ToolCall` (generic) only for unknown tools.

### 5.1 Read

`maya::widget::read_tool.hpp` — file read operations.

```
╭─ ✓ Read ───────────────────────────────────────╮
│ src/main.cpp                            0.3s   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│  1: #include <iostream>                        │
│  2: #include "moha/agent.hpp"                  │
│ …                                              │
│ 42: int main() {                               │
╰────────────────────────────────────────────────╯
```

Header: `<path>` only (no kbytes/lines summary today). Add
`<path> · <total_lines> lines` once we wire it up.

API:
```cpp
ReadTool rt;
rt.set_file_path("src/main.cpp");
rt.set_content(file_contents);
rt.set_status(ReadStatus::Success);
rt.set_elapsed(0.3f);
rt.set_start_line(1);
rt.set_max_lines(50);   // body shows first 50 lines, then "... N more"
rt.set_total_lines(312);
rt.set_expanded(false);
```

Body: line numbers + content, gutter-styled. Only rendered when
`expanded`. If `total_lines > max_lines`, append `… N more lines`.

### 5.2 Edit

`maya::widget::edit_tool.hpp` — file edits (search/replace style).

```
╭─ ✓ Edit ───────────────────────────────────────╮
│ src/render/canvas.cpp                   0.1s   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ - damage_ = {0, 0, width_, height_};           │
│ + damage_ = {0, 0, 0, 0};                      │
│ + // Start with empty damage rect              │
╰────────────────────────────────────────────────╯
```

API:
```cpp
EditTool et("src/render/canvas.cpp");
et.set_old_text("damage_ = {0, 0, width_, height_};");
et.set_new_text("damage_ = {0, 0, 0, 0};\n// Start with empty damage rect");
et.set_status(EditStatus::Applied);
et.set_elapsed(0.1f);
et.set_expanded(true);
```

Body: `- ` lines in `error` color, `+ ` lines in `success` color. The
prefix has its own dim style so the +/- doesn't shout. Wraps long
lines. **Default: expanded** (overrides the global default).

**Multi-edit (live).** When the model emits the canonical
`edits: [{old_text, new_text}, ...]` shape, the card renders **every**
hunk during streaming, not just the first. Each edit gets its own
`- / +` block with an `edit N/M` separator between them:

```
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈ edit 1/3 ┈┈┈┈┈┈┈┈┈┈┈┈┈┈        │
│ -  damage_ = {0, 0, width_, height_};          │
│ +  damage_ = {0, 0, 0, 0};                     │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈ edit 2/3 ┈┈┈┈┈┈┈┈┈┈┈┈┈┈        │
│ -  region.invalidate();                        │
│ +  region.clear();                             │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈ edit 3/3 ┈┈┈┈┈┈┈┈┈┈┈┈┈┈        │
│ -  return ok;                                  │
│ +  return Ok(());                              │
```

This works because Anthropic's fine-grained tool streaming
(`eager_input_streaming: true`, see `04_architecture.md` § 7) emits
`edits[i].old_text` and `edits[i].new_text` chunk-by-chunk on the
wire, so the reducer can mirror the entire `edits[]` array into
`tc.args["edits"]` as it grows. The widget API is
`et.set_edits(vector<EditPair>)` (multi) or the single-edit
`set_old_text`/`set_new_text` for legacy / top-level shapes.

For full diff review (multi-file, hunk navigation, accept/reject),
see `10_diff_review.md` — that's a separate UI surface, not an
inline tool card.

### 5.3 Write

`maya::widget::write_tool.hpp` — new file or overwrite.

```
╭─ ✓ Write ──────────────────────────────────────╮
│ docs/notes.md                           0.2s   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ # Notes                                        │
│                                                │
│ Some content here…                             │
│ … 47 more lines                                │
╰────────────────────────────────────────────────╯
```

API:
```cpp
WriteTool wt;
wt.set_file_path("docs/notes.md");
wt.set_content(full_content);
wt.set_status(WriteStatus::Written);
wt.set_max_preview_lines(20);
```

Body: first N lines of content + `… M more lines`. Header could show
`<path> · <bytes> bytes` once we wire it; not implemented today.

### 5.4 Bash / Execute

`maya::widget::bash_tool.hpp` — shell command execution.

```
╭─ ✓ Execute ────────────────────────────────────╮
│ $ npm install --save-dev typescript     2.3s   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ added 1 package in 2.3s                        │
╰────────────────────────────────────────────────╯
```

Failed:
```
╭┄ ✗ Execute ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄╮
┆ $ cmake --build build                   1.1s   ┆
┆ exit code 2                                    ┆
┆ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   ┆
┆ src/foo.cpp:42:1: error: expected ';'          ┆
┆     return 0                                   ┆
┆           ^                                    ┆
┆     ;                                          ┆
╰┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄╯
```

API:
```cpp
BashTool bt("npm install --save-dev typescript");
bt.set_output("added 1 package in 2.3s\n");
bt.set_status(BashStatus::Success);
bt.set_elapsed(2.3f);
bt.set_exit_code(0);
bt.set_max_output_lines(20);   // truncate w/ "... N more lines"
bt.set_expanded(false);

// During streaming:
bt.set_status(BashStatus::Running);
bt.append_output(new_chunk);
```

Special:
- Header: `$ ` prompt in `success` color + bold, command in slightly
  brighter text (200, 204, 212).
- On failure, an `exit code N` line appears between the command and
  the dashed separator (always visible, even when collapsed).
- **Default expanded if failed.**
- During streaming, append to `output_` and force re-render. Body
  scrolls as content grows; the parent `Scrollable` handles overall
  thread scrolling.

### 5.5 Search

No typed widget yet — use `ToolCall` (generic) until we add one.

```
╭─ ✓ Grep ───────────────────────────────────────╮
│ "MAYA_LEGACY_API" in src/         12 matches   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ src/api/v1.cpp:42                              │
│   if (MAYA_LEGACY_API) { return false; }       │
│ src/api/v1.cpp:128                             │
│   #ifdef MAYA_LEGACY_API                       │
│ … 10 more matches                              │
╰────────────────────────────────────────────────╯
```

Use `ToolCall::Config{.tool_name="Grep", .kind=ToolCallKind::Search,
.description = "\"<query>\" in <path>  N matches"}` and set the body
via `set_content(...)` — pass a pre-rendered Element. The body is a
list of `<path>:<line>` followed by a 1-line preview (dim).

When we add a typed `SearchTool` widget, extract this into
`maya/include/maya/widget/search_tool.hpp` with:
```cpp
struct Match { std::string path; int line; std::string preview; };
class SearchTool {
    std::string query_;
    std::string path_;
    std::vector<Match> matches_;
    int max_matches_shown_ = 10;
    // …
};
```

### 5.6 Fetch

`maya::widget::fetch_tool.hpp` — HTTP fetches (URL → body).

```
╭─ ✓ Fetch ──────────────────────────────────────╮
│ https://api.example.com/v1/users  200 · 4.5kb  │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ Content-Type: application/json                  │
│                                                │
│ {                                              │
│   "users": [                                   │
│     { "id": 1, "name": "ada" },                │
│     ...                                        │
│   ]                                            │
│ }                                              │
│ … 23 more lines                                │
╰────────────────────────────────────────────────╯
```

API:
```cpp
FetchTool ft;
ft.set_url("https://api.example.com/v1/users");
ft.set_status_code(200);
ft.set_content_type("application/json");
ft.set_body(json_body);
ft.set_status(FetchStatus::Done);
ft.set_max_body_lines(20);
```

Header description format: `<url>  <code> · <bytes>kb`. Long URLs are
truncated with middle ellipsis.

For non-2xx responses, treat as `Failed` (dashed border) regardless
of whether the request itself completed. Surface the body so the user
can see the error message.

### 5.7 Think

No typed widget. Render with `ToolCall` (generic) + a body of
markdown-rendered thinking text. Use `ToolCallKind::Think`.

```
╭─ ✓ Thinking ───────────────────────────────────╮
│ 12 tokens                                3.1s  │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ The user wants me to find files using          │
│ MAYA_LEGACY_API. Let me start by searching     │
│ the src/ directory…                            │
╰────────────────────────────────────────────────╯
```

Note: this overlaps with the "thinking block" disclosure described in
`06_message_stream.md § 5`. Difference:

| | Thinking block (§ 06.5) | Think tool card |
|---|---|---|
| Source | Anthropic `thinking` content type | A tool named `think` (model-invoked) |
| Today | Not differentiated in moha (mixed into text) | Doesn't exist as a separate tool |
| When to use | Extended thinking output | If the model uses an explicit `think` tool |

Until either is added, this section is forward-looking. Skip both
implementations for the initial rebuild.

### 5.8 Agent (subagent)

`maya::widget::agent_tool.hpp` — when the agent invokes another
agent ("dispatch a subagent to do X").

```
╭─ ● Agent ──────────────────────────────────────╮
│ research-bug-cause · claude-opus-4-5    8.4s   │
│ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈   │
│ ┌─ subagent thread ───────────────────────────│
│ │ <user prompt summary…>                       │
│ │ ╭─ ✓ Read ──────────────────────────╮       │
│ │ │ src/foo.cpp                  0.2s  │       │
│ │ ╰─────────────────────────────────────╯       │
│ │ <assistant text…>                            │
│ ╰──────────────────────────────────────────────│
╰────────────────────────────────────────────────╯
```

API:
```cpp
AgentTool at;
at.set_description("research-bug-cause");
at.set_model("claude-opus-4-5");
at.set_status(AgentStatus::Running);
at.set_elapsed(8.4f);
```

Body: a nested message stream rendered with the same primitives —
just compose `views::message_stream(subagent_model)` inside this card.
Subagent message bubbles get the **dashed** border + left rule
treatment described in `06_message_stream.md § 4 (subagent variant)`.

moha doesn't support subagents today — defer this implementation.
Track in `13_rebuild_playbook.md`.

### 5.9 Other / generic

For any tool not in the typed list, fall back to `ToolCall`:

```cpp
ToolCall::Config cfg{
    .tool_name   = tu.name,                    // e.g. "WebSearch"
    .kind        = ToolCallKind::Other,
    .description = format_input_summary(tu.input),
};
ToolCall card(cfg);
card.set_status(map_status(tu.status));
card.set_elapsed(tu.elapsed_seconds);
card.set_expanded(tu.expanded);
if (tu.expanded) {
    card.set_content(render_input_output_disclosure(tu));
}
```

`render_input_output_disclosure` should produce a body with two
sections:
```
Input
  { "query": "...", "max_results": 10 }
Output
  { "results": [...] }
```

Both rendered as JSON code blocks (use `maya::markdown` with a JSON
fence). Keep them collapsed initially — generic cards default
collapsed.

## 6. Permission footer

When a tool is in `Confirmation` state (awaiting user permission),
the card swaps its body for a permission footer. This is *not* a
modal — it's inline at the bottom of the card.

```
╭─ ⚠ Bash ───────────────────────────────────────╮
│ $ rm -rf node_modules                          │
│ ────────────────────────────────────────────── │
│ This command will delete files. Allow?         │
│                                                │
│ [A]llow  [D]eny  [⌥A] Always allow `rm -rf`    │
╰────────────────────────────────────────────────╯
```

The footer renders, key handlers wait for `A` / `D` / `Esc` /
`Alt+A`. Once decided:

- `Allow`: status → `Pending` → `Running` → `Completed/Failed`,
  border color/style update accordingly.
- `Deny`: status → `Failed` with body "Denied by user", dashed
  border.
- `Always allow`: same as `Allow`, plus persist the rule in
  permissions store.

Full footer spec, "Always for X" semantics, and dropdown granularity
(per-command vs per-tool vs per-session) are all in
`09_permissions.md`. Don't duplicate that logic here — this section
just notes that the footer **lives inside the card**, not in a
separate banner or modal.

## 7. The `ToolUse` data model

The render loop walks `msg.tool_calls : std::vector<ToolUse>` and
dispatches per-tool. Recommended `ToolUse` shape:

```cpp
namespace moha {

enum class ToolKind {
    Read, Edit, Write, Bash, Search, Fetch, Think, Agent, Other,
};

enum class ToolStatus {
    Pending,         // queued, not yet started
    Confirmation,    // awaiting user permission
    Running,         // actively executing
    Completed,       // success
    Failed,          // error
    Cancelled,       // user cancelled
};

struct ToolUse {
    std::string id;              // stable id from the model (`toolu_…`)
    std::string name;            // canonical tool name
    ToolKind    kind;            // mapped from `name`
    nlohmann::json input;        // structured input
    std::optional<std::string> output;  // raw output (text or JSON)
    ToolStatus  status = ToolStatus::Pending;
    float elapsed_seconds = 0.0f;
    bool  expanded = false;
    std::optional<std::string> error_message;  // when status == Failed

    // Permission state — only populated when status == Confirmation
    std::optional<PendingPermission> pending_permission;
};

} // namespace moha
```

The render dispatch:

```cpp
Element views::tool_card(const ToolUse& tu, const Message& msg, const Model& m) {
    if (tu.status == ToolStatus::Confirmation) {
        return views::tool_card_with_permission(tu, m);
    }
    switch (tu.kind) {
        case ToolKind::Read:   return render_read_tool(tu);
        case ToolKind::Edit:   return render_edit_tool(tu);
        case ToolKind::Write:  return render_write_tool(tu);
        case ToolKind::Bash:   return render_bash_tool(tu);
        case ToolKind::Fetch:  return render_fetch_tool(tu);
        case ToolKind::Agent:  return render_agent_tool(tu);
        case ToolKind::Search:
        case ToolKind::Think:
        case ToolKind::Other:
        default:               return render_generic_tool(tu);
    }
}
```

Each `render_*` does the typed widget construction + sets status,
elapsed, expanded, and body content from `tu`.

Centralize the kind mapping (string → `ToolKind`) in one place:

```cpp
ToolKind tool_kind_from_name(std::string_view name) {
    if (name == "read"  || name == "Read")    return ToolKind::Read;
    if (name == "edit"  || name == "str_replace_based_edit_tool")
                                              return ToolKind::Edit;
    if (name == "write" || name == "create_file")
                                              return ToolKind::Write;
    if (name == "bash"  || name == "shell")   return ToolKind::Bash;
    if (name == "grep"  || name == "search" || name == "find")
                                              return ToolKind::Search;
    if (name == "fetch" || name == "web_fetch") return ToolKind::Fetch;
    if (name == "think" || name == "thinking") return ToolKind::Think;
    if (name == "dispatch_agent")             return ToolKind::Agent;
    return ToolKind::Other;
}
```

When the model adds a new tool, you add one line here and (optionally)
a typed widget. Until then, generic `Other` handles it correctly.

## 8. Streaming behavior

Tool cards have three streaming-relevant moments:

1. **`StreamToolUseStart`**: Insert the `ToolUse` with
   `status=Pending`, no output, no elapsed. Card renders
   immediately with `○` icon. Set `expanded` per default rules.
2. **`StreamToolUseDelta`** (if input streams): Update `tu.input`
   incrementally. Header description re-renders each frame.
3. **`StreamToolUseStop`**: Move to `Confirmation` (if permission
   needed) or `Running` (executing) or directly `Completed/Failed`
   (if synchronous and no permission).

Once the tool actually executes, a `Tick{}` subscription updates
`elapsed_seconds` every ~1s so the user sees "0.5s → 1.5s → 2.5s …".
Don't update faster than 1s — sub-second jitter is noise.

When the result lands (`ToolResult{id, output, status}`), patch the
`ToolUse` and let the next render cycle pick it up. No special
animation; the icon flip from `●` to `✓`/`✗` is enough.

## 9. Layout details and gotchas

- **The card is a `dsl::v(...)` of rows.** Don't try to lay header +
  body horizontally; vertical-only keeps the wrapping math simple.
- **Don't nest a Scrollable inside a tool card.** Tool cards grow with
  their content; the *thread* is what scrolls. Putting a viewport
  inside a card creates double-scroll surfaces and confuses focus.
  Truncate long content with `… N more lines` instead (see § 4).
- **Border label encodes the icon + tool name.** Don't also add the
  icon as the first character of the header — that double-renders.
- **Padding inside the card is `(0, 1, 0, 1)`.** The border eats the
  outer cell on each side; one extra cell of horizontal padding gives
  breathing room. Don't add vertical padding — let `gap_<1>` between
  rows handle spacing.
- **Per-tool widgets currently inline-render their borders.** That
  means you can't separately style the border outside the widget. If
  you need a different look (focus highlight, e.g.), wrap the widget
  in a wrapper that draws *another* border — but that creates
  double-bordered cards which look bad. Instead, add a focus accent
  inside (e.g., a one-cell-wide left bar in `border::focus`).

## 10. Focus and key handlers

Per-card key handling (when the card is focused):

| Key | Action | Notes |
|---|---|---|
| `Enter` / `Space` | Toggle expand/collapse | `Msg::ToolCardToggle{tu.id}` |
| `Y` / `A` | Allow (only if `Confirmation`) | See `09_permissions.md` |
| `N` / `D` | Deny (only if `Confirmation`) | |
| `Alt+A` | Always allow | |
| `D` | Open diff (only if `Edit` and applied) | Routes to `10_diff_review.md` |
| `O` | Open file in editor (if `Read`/`Edit`/`Write`) | Dispatch `OpenFile{path}` |
| `C` | Copy command/path/url to clipboard | `Msg::CopyText{value}` |

Defer `Tab`-based focus traversal until the rebuild has settled —
global key dispatch is enough initially (apply to the focused card or
the most-recently-visible card).

## 11. Visual checklist

After implementing tool cards, verify:

- [ ] Read card collapsed shows `path` only; expanded shows
      numbered lines
- [ ] Edit card defaults expanded; shows `-` red, `+` green lines
- [ ] Bash success collapsed; bash failure expanded with `exit code N`
- [ ] Failed cards have **dashed** border in red darkened
- [ ] Confirmation cards have amber Round border + warning icon
- [ ] Elapsed time updates every 1s during running, then freezes
- [ ] Status icon transitions `○ → ● → ✓` (or `✗`) without flicker
- [ ] Long paths/URLs truncate, don't overflow the card
- [ ] Bash output truncates with `… N more lines` past
      `max_output_lines`
- [ ] Permission footer renders inside card, not as separate banner
- [ ] No tool card expands to fill the screen (max width inherits
      from conversation column = 120 cells)
- [ ] No spinner glyph inside individual cards (the icon `●` already
      indicates running; spinning the icon would distract)
- [ ] Subagent cards render nested message stream (when supported)
