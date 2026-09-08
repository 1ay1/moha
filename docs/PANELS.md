# Panels — the one overlay subsystem

Everything that opens over the thread — the command palette, the model
switcher, the settings panes, the diff review — is a **panel**. There is one
word, one widget, one slot, one priority function, and one Esc rule. This
document is the map: what the invariants are, which file owns each one, and
which alternatives were rejected and why.

Vocabulary, in one sentence:

> A **panel** is the surface that opens over the thread; it contains
> **items**; the item's **kind** decides what it does; **Esc** restores the
> panel you came from.

Four words used to cover this ground — *overlay*, *picker*, *pane*, and
*panel* — plus three names for the thing inside (*row*, *field*, *control*).
They named implementation history, not concepts. The remaining legitimate
survivors: `palette` (the ^K panel's proper name), `pick::` (the cursor
state-machine namespace), and `rag_settings` (the rag panel's state
namespace — `agentty::rag` is the retrieval engine, and colliding with it
would be worse than the suffix).

---

## The spine — five files

| File | Owns |
|---|---|
| `include/agentty/runtime/panel/slot.hpp` | **The slot.** `ui::panel::State` wraps a variant of every exclusive panel. Opening is assignment; "two panels open" is unrepresentable. Also: `From`, `descend`/`adopt`/`ascend` (§ Navigation). |
| `include/agentty/runtime/panel/top.hpp` | **`panel::top(m)`** — the single priority decision: login → permission → the slot → todo. |
| `include/agentty/runtime/panel/nav.hpp` | **`NavSpec`** — the declarative key grammar (Esc/Enter/arrows/j-k/PgUp/filter/toggle-close), translated once. |
| `src/runtime/view/view.cpp` | **What renders**: `pick_panel()` switches on `top(m)`. |
| `src/runtime/app/subscribe.cpp` | **Who gets keys**: the same `top(m)`, a parallel exhaustive switch. |

The load-bearing property: **the renderer and the key router call the same
function**, so what you see and what owns the keyboard cannot diverge. Both
switches are exhaustive on `Kind` (`-Wswitch`), so a new panel without arms
is a compile warning, not an invisible modal or a dead key.

### Deliberately outside the slot

Three surfaces are *not* slot members, and forcing them in would be the
classic over-unification mistake:

- **login** — a 9-state auth machine that gates the app before the Model
  is meaningful.
- **permission** — `m.d.pending_permission` is **domain** state raised
  mid-turn by the stream reducer. If it lived in the UI slot, opening any
  panel would structurally destroy a pending security question.
- **todo** — ambient: renders *under* the slot and its unclaimed keys fall
  through. The opposite of modal.

`top()` exists precisely to compose these four sources into one answer.

---

## Navigation — Esc is structural

Every slot alternative inherits `WithFrom`:

```cpp
struct WithFrom { From from; };   // the panel this one was opened OVER
```

`From` carries a **full snapshot of the parent's slot value** (an immutable
`shared_ptr<const Snapshot>`). Three operations on the slot replace all
per-panel Esc plumbing:

- **`descend(K)`** — open K over whatever is open; the parent is stashed
  automatically. Open sites use this. No caller stamps an origin.
- **`adopt(f)`** — the palette's select arm snapshots itself, dispatches the
  command, then gives *whatever the command opened* the palette as its Esc
  target. Generic over all commands; the registry has no per-command
  origin plumbing.
- **`ascend()`** — Esc restores the parent **verbatim**: query, cursor,
  nested chain. `app::detail::ascend()` (meta.cpp) wraps it and
  **revalidates** against the live model — cursors clamped to today's
  filtered lists, SmartMode's form rebuilt from live config keeping only
  view state (cursor + advanced toggle).

Properties worth knowing:

- **It is a stack without a stack container.** Each snapshot contains *its*
  `from`, so palette → settings list → pane unwinds level by level, and
  depth is bounded by real user descent. There is no separate stack to keep
  in sync with the slot.
- **Restore, don't reconstruct.** The predecessor (`settings_origin`) named
  the parent kind plus hand-picked fields, and `back_to()` rebuilt the
  parent from them. Every field the reconstruction forgot — the palette's
  half-typed query — was user state silently thrown away. A snapshot cannot
  forget fields.
- **Restore ≠ descend.** The fused-picker hand-off (Smart Mode → model
  picker → back) parks its `From` in `m.ui.smart_assign_from` and restores
  with plain assignment. A `descend` at a restore site would stash the
  child as its own parent's parent, and Esc would cycle instead of unwind.
- **Staleness is the caller's problem, on purpose.** The model may change
  while a child is open (a stream ends, a plugin disconnects). The slot
  restores bytes; `ascend()` in meta.cpp owns making them valid again.

The escape guarantee (`close_msg` + the rescue clause in subscribe.cpp)
still backstops panels whose handlers mishandle Esc — see
`tests/escape_guarantee_test.cpp`.

---

## The widget — one Panel, items, kinds

maya has exactly **one** overlay widget: `maya::Panel`
(`maya/include/maya/widget/panel.hpp`, umbrella). It replaced Picker and
Form, which were two implementations of the same box; every fix to one had
to be repeated in the other, and the ones that weren't became bugs.

The family is modular, one file per concern:

```
maya/include/maya/widget/panel/
├── item.hpp      the ONE Item (badge · leading · highlight · trailing ·
│                 help-under-focused-row · error · locked · origin)
├── item/         ★ one widget per kind: value struct + renderer, side by side
│   ├── label  header  toggle  choice  pick  number
│   └── slider text    secret  path    action
├── control.hpp   the closed variant over the kinds + one render() dispatch
├── context.hpp   ItemCtx — the ONLY facts an item renderer may know
├── caret.hpp     windowed caret splicing (Text/Path share it)
├── menu.hpp      the inline Choice dropdown (◉ committed vs ❯ cursor)
├── config.hpp    everything a host supplies (items, prebuilt, viewport…)
└── theme.hpp     the palette
```

Enforcement is by types, not discipline:

- **`ItemCtx` is the modularity guarantee.** An item renderer receives its
  own value plus `{theme, dropdown-open, edit budget}` — never the Config
  or the item list — so an item widget *cannot* grow panel logic.
- **Dispatch is overload resolution** inside `std::visit`: a new kind
  without a `render` overload is a compile error, not a blank cell.
- **`Header` is a kind, not a bool.** `is_header=true` beside a live
  control was two fields describing one mutually-exclusive fact. As a kind,
  a header structurally cannot also carry a value. (`Item::is_header()`
  survives as a derived method.)
- **`Secret` holds a character count, not a string.** No render path,
  present or future, can leak a credential — there is nothing to leak.

Kind boundaries that are design law (each documented in its widget file):
`Choice` never gets a search box — a set big enough to need one is a `Pick`,
which hands off to a real panel. That split is what stops the inline
dropdown from becoming a second, worse picker.

### Model vs projection — why `form::Field` still exists

`agentty::form` (in `runtime/panel/form.hpp`) looks like a duplicate of the
maya kinds. It is not, and **must not be folded in**:

| | `form::field` (agentty) | `maya::panel` kind |
|---|---|---|
| Secret | the real string + cursor | a character **count** |
| Number | value + clamp bounds | just the value |
| Choice | ids, labels, hints, index | the display label |

The form side is the editable **model** (reducer state); the maya side is
the render **projection** (deliberately less). `form_common.cpp`'s
`control_for()` is the projection boundary — ~60 lines whose job is
real-string → count, options+index → label. Deleting it would mean either
maya holds credentials or the reducer imports widget types. The asymmetry
is the security property.

Same layering, interaction side: `FormFocus` (navigating / editing /
dropdown — a sum type, not two bools) lives on the **panel**, not the item.
Exactly one item can be in edit mode, and that invariant is only
enforceable where the one-ness lives.

---

## File layout — one panel, one name, three predictable places

For every panel `<name>`, the SAME filename appears in three places:

```
include/agentty/runtime/panel/<name>.hpp    its state (slot alternative)
src/runtime/app/update/<name>.cpp           its reducer
src/runtime/view/panels/<name>.cpp          its view (Model → Panel::Config)
```

models · providers · thread_list · palette · mention · symbol · smart_mode ·
code_blocks · tool_output · checkpoints · fork · rag · settings_list · todo —
all follow it. (Small deviations: smart_mode/rag state lives in
smart_form.hpp/rag.hpp with their form builders; settings_list's state is
panel/settings/. code_blocks' result card lives with code_blocks — same
subsystem.)

The COMMON machinery is clearly separate:

```
include/agentty/runtime/panel/    slot.hpp top.hpp nav.hpp common.hpp
                                  form.hpp form_keys.hpp   (the form machine)
src/runtime/panel/                their .cpps + per-panel form builders
src/runtime/view/panels/          panels_prologue.hpp (shared includes),
                                  panels_common.hpp (kPanel* widths,
                                  panel_viewport_h), form_common.cpp
                                  (form::Form → Panel::Config projection)
src/runtime/app/update/meta.cpp   ascend() + cross-cutting arms
```

View TUs were previously grouped by VINTAGE (`nav_panels.cpp` = "what was
split out of pickers.cpp"), which meant understanding one panel required
finding which unrelated bag it lived in; reducers for three panels shared a
1.8k-line panels.cpp. One-file-per-panel replaced both: `grep -l <name>`
now finds a panel's three files by its one name.

`settings_registry.hpp` stays in `runtime/`, *outside* the panel domain:
it is the ~35-knob config table — the thing the settings panel displays,
not the panel. That line answers "why is there settings code without a
settings panel": there was one file of settings and three files of panel
wearing the same prefix.

Panel width floors are named, not numeric: `kPanelNarrow/Standard/Wide`
(`view/panels/panels_common.hpp`). They are floors on a flex box — they
bind only on narrow terminals — so a new panel picks a name describing its
content instead of inventing a magic number.

---

## Rendering conventions

- **Description under the focused item, not crammed into the row.**
  `Item::help` renders below the cursor's item only (full width); the
  trailing cell carries short reference data — a shortcut, a timestamp —
  marked `trailing_secondary` so it yields before the label does. The rag
  panel established the pattern; the palette adopted it.
- **The widget owns all chrome** — border, viewport, scrollbar math,
  keep-selection-in-view. Hosts fill a `Config` and never touch layout.
- A grep-shaped test (`fused_models_test`, via `AGENTTY_PICKERS_SRC_DIR`)
  guards style rules that have no runtime seam: primary labels are never
  dimmed, trailing chips are secondary, badge padding measures columns.

---

## Adding a panel — the checklist

1. State header in `include/agentty/runtime/panel/<name>.hpp`; the open
   struct inherits `WithFrom` via its slot alternative.
2. Add the alternative to the variant + `Kind` in `slot.hpp` (one visitor
   arm in `kind_of`).
3. Messages `Open<Name>` / `Close<Name>` / `<Name>Move…` in `msg.hpp`;
   reducer arm dispatches `ascend(m)` on close and `descend(...)` on open.
4. View `src/runtime/view/panels/<name>.cpp`: a function
   `Model → maya::Panel::Config`. Pick a named width floor.
5. Arms in `pick_panel()` (view.cpp) and the subscribe switch — the
   compiler lists every site you missed (`-Wswitch`).
6. Navigation: fill a `NavSpec`; do not hand-roll the key table.
7. If it needs revalidation after Esc-restore, add a branch in
   `ascend()` (meta.cpp). Free text needs nothing; live-list cursors
   need a clamp.
8. Visible state feeds the frame gate AUTOMATICALLY: `visual_hash` walks
   the panel slot structurally (`visual::mix_any`, visual.hpp). Only if
   your state type has bases + members, private members, or a SECRET
   field do you add a `visual_parts` list beside it
   (panel/visual_parts.hpp) — and `parts_cover_all` fails the build if
   the list misses a member.

Steps 2 and 5 are the remaining hand-maintained arms. The next design step,
if the count ever grows: derive variant, `Kind`, and both dispatch switches
from one compile-time panel list (descriptor registry), making a missing
arm impossible rather than a warning. Not done yet — at ~16 panels the
exhaustive switches are honest work, and the registry's template cost to
the 1.7 s edit loop is unmeasured.

## The frame gate: a structural walk, not a hand-written mirror

`visual_hash` (program.hpp) gates repaints. It USED to hand-enumerate
every model facet the view renders — a parallel description of the view's
dependency set that produced a whole bug class when facets were forgotten
(login inputs, both forms, the rag probe verdict, the result-card scroll).

Retired. `visual::mix_any` (runtime/visual.hpp) DERIVES the hash from the
state types: scalars, strings, variants, optionals and ranges walk
automatically; plain aggregates decompose member-by-member (P1061
structured-binding packs), so a member added tomorrow is hashed tomorrow.
A type the walk cannot decompose — bases + members, private state, or a
field holding a SECRET — must declare `visual_parts(t)` beside its
definition (panel/visual_parts.hpp): one entry per base/member, each
walked, projected (lengths), or `visual::exempt`ed with a written reason.
`static_assert(parts_cover_all<T>)` fails the build if the list misses a
member, so exemption is the explicit reviewable act and coverage is the
default — the inversion that makes the old bug class unrepresentable.

Secrets: `field::Secret` and `EmbedConfig::api_key` digest LENGTH-only via
their parts lists; login's key/code buffers likewise. `visual_walk_test`
pins it: a same-length credential overwrite is hash-invisible, a length
change is visible, and no non-secret facet can be mutated without moving
the hash. The parent `From` snapshot is exempt (not rendered while its
child is open) — also pinned.

Cost: ~540 ns to walk the heaviest panel (rag, 22-field form) —
measured, ≌0.002% of a 30fps frame.

Hand-mixed remnants, on purpose: only NON-PANEL domains now (thread/
stream render keys, composer — whose undo/redo stacks and clocks make a
blind walk wasteful; its hand-mix is a considered projection). Inside the
panel domain everything walks: the slot, login, todo, plugins, catalogs
(open-gated), and the body-scroll ScrollStates — whose parts list is the
one TRUSTED (unprovable) case: ScrollState is a non-aggregate (custom
destructor), so parts_cover_all cannot bind its member count; the x/y
list is reviewed prose, flagged as such at the definition.

---

## Tests that pin the design

| Test | Pins |
|---|---|
| `tests/panel_test.cpp` | widget invariants: line-count mirror, header non-selection, viewport windows |
| `tests/settings_nav_test.cpp` | Esc restores pane AND state (query survives — snapshot, not reconstruction) |
| `tests/escape_guarantee_test.cpp` | no panel can trap the keyboard |
| `tests/palette_nav_test.cpp` | header-aware cursor motion |
| `tests/panel_sections_render_test.cpp` | section headers in the model panel |
| `tests/smart_slot_panel_stack_test.cpp` | the fused-picker hand-off round-trip |
| `maya/tests/test_widgets.cpp` | Panel rendering + scroll behaviour |

History: the merge and renames landed as maya `315e4f2` (item-kind widgets),
`03ef3a0` (Row→Item), `6d9cdb6` (Header as kind); agentty `caada52d`
(descend/adopt/ascend), `0c2a88a2` (file layout), `77817a22` (vocabulary).
