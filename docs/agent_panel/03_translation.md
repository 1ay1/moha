# 03 — Translation: GPUI → maya, pixels → cells

GPUI is Zed's hand-rolled Rust GUI framework. It's a sub-pixel-accurate
retained-mode renderer with a CSS-flexbox-flavored API. maya is a TUI
toolkit that renders into integer cells with a Yoga-flavored flexbox.
Many GPUI patterns translate cleanly, but some don't — this doc tells
you exactly how to map each one and where the substitutions are.

When you see GPUI code in `crates/agent_ui/`, this is your
translator's reference card.

## 1. Containers

| GPUI | maya |
|---|---|
| `div()` | `dsl::v()` (column) — most common single-child wrapper |
| `h_flex()` | `dsl::h(...)` |
| `v_flex()` | `dsl::v(...)` |
| `flex_col()` | `dsl::v(...)` |
| `.flex_grow()` | `\| grow_<1>` |
| `.flex_none()` | `\| grow_<0>` (default; usually omit) |
| `.flex_1()` | `\| grow_<1> \| shrink_<1>` |
| `.flex_basis(rems(N))` | layout `flex_basis = Dimension::fixed(cells)` |
| `.flex_shrink()` | `\| shrink_<1>` |
| `.size_full()` | `\| width_<Pct, 100> \| height_<Pct, 100>` (or just let parent grow) |
| `.w_full()` | `\| width_<Pct, 100>` |
| `.h_full()` | `\| height_<Pct, 100>` |
| `.max_w(rems(N))` | `\| max_width(Dimension::fixed(cells))` |
| `.gap_N()` | `\| gap_<cells>` |
| `.justify_between()` | `\| justify_<Justify::SpaceBetween>` (or use `spacer()` between children — usually clearer in practice) |
| `.justify_center()` | `\| justify_<Justify::Center>` |
| `.items_center()` | `\| align_items_<Align::Center>` |

### Key insight: prefer `spacer()` over `justify_between()`

In maya, the fluent way to push two halves of a row apart is:

```cpp
h(left_part, spacer(), right_part)
```

It reads cleaner than threading a justify modifier through.

## 2. Padding & margin

GPUI uses Tailwind-style spacing scale (1 unit = 4px). maya uses cells.
A typical 16-px default font terminal cell is ~8px wide × 16px tall, so
the rule of thumb is **divide GPUI's px-equivalent by 8 and round**:

| GPUI | px | TUI cells | maya |
|---|---|---|---|
| `.p_0()` | 0 | 0 | omit |
| `.p_0p5()` | 2 | 0 | omit (or use `pad<0>` and rely on gap) |
| `.p_1()` | 4 | 1 | `\| pad<1>` |
| `.p_1p5()` | 6 | 1 | `\| pad<1>` |
| `.p_2()` | 8 | 1 | `\| pad<1>` |
| `.p_3()` | 12 | 1–2 | `\| pad<1>` (vert) `\| padding(0,2,0,2)` (horiz) |
| `.p_5()` | 20 | 2 | `\| padding(0,2,0,2)` |

Per-axis equivalents:

| GPUI | maya |
|---|---|
| `.px_N()` | `\| padding(0, cells, 0, cells)` |
| `.py_N()` | `\| padding(cells, 0, cells, 0)` |
| `.pt_N()` | `\| padding(cells, 0, 0, 0)` |
| `.pb_N()` | `\| padding(0, 0, cells, 0)` |
| `.pl_N()` | `\| padding(0, 0, 0, cells)` |
| `.pr_N()` | `\| padding(0, cells, 0, 0)` |
| `.mx_N()` | use parent padding instead — maya doesn't add margin separately for inline blocks |
| `.my_N()` | put empty `text("")` rows around it, or use parent gap |

The biggest trap: **don't mechanically translate every `gap_2` to
`gap_<1>`**. Look at the visual rhythm. Cluster related controls with
gap_<0>, separate sections with gap_<1>, separate regions with gap_<2>.

## 3. Borders & rounded corners

| GPUI | maya |
|---|---|
| `.border_1()` | `\| border(BorderStyle::Round)` |
| `.border_dashed()` | `\| border(BorderStyle::Dashed)` |
| `.border_color(c)` | `\| bcolor(c)` |
| `.rounded_md()` | `BorderStyle::Round` (the ╭╮╰╯ characters) |
| `.rounded_xs()` | `BorderStyle::Round` (TUI can't pick radii smaller than 1 cell) |
| `.rounded_t(N)` | not directly supported — see workarounds |
| `.shadow_sm()` | not supported (no z-axis); skip |
| `.border_t_1()` | `BorderSides{.top=true, others=false}` |
| `.border_b_1()` | `BorderSides{.bottom=true, others=false}` |

### "Rounded only on top"

Zed's tool card header has `rounded_t(rems_from_px(5.))`. In maya you
have one border per box. Two options:

1. **Live with rounded all around** — the visual gain from rounding only
   the top corners is small.
2. **Stack two boxes**: a header box with `BorderSides::horizontal()`
   on top + a body box without a top border. They'll join cleanly.

Use option 1 unless you need a tight visual seam.

### Border title (Zed's overlapping label)

Zed renders the tool name inline at the top-left of the card. maya does
this with `BorderText`:

```cpp
| btext(" ✓ Read ", BorderTextPos::Top, BorderTextAlign::Start)
```

**Always include the leading and trailing space** in the label —
otherwise the border characters touch the text and it reads like a
glyph soup.

## 4. Backgrounds

| GPUI | maya |
|---|---|
| `.bg(color)` | `Style{}.with_bg(color)` (set on the box's `style` field) |
| Theme-derived bg | look it up in `05_design_tokens.md` |
| `.opacity(0.8)` | `color.darken(0.2)` or `color.lighten(0.2)` for lighter; no true alpha in TUIs |

**Important**: terminals can't do alpha compositing. When Zed says
`.bg(border.opacity(0.8))`, the right TUI translation is "use a slightly
darker variant of the border color" — not "render at 80% alpha". maya's
`.darken(0.2)` is the closest equivalent.

For backgrounds specifically, **prefer no background at all** in most
places. The terminal background already provides "the canvas". A few
genuine cases for explicit `bg`:

- The user message bubble (so it stands out from the conversation)
- The tool card body (so it sits on a slightly different surface from
  the assistant text)
- The composer (to demarcate the input area)

Everywhere else, leave it transparent (terminal background).

## 5. Text & typography

GPUI has a real font system. Terminals have the user's terminal font.
Most styling translates 1:1 (`.bold()`, `.italic()`, `.underline()`),
but **size and family don't translate** — there's only one font, one
size.

| GPUI | maya |
|---|---|
| `.text_color(c)` | `Style{}.with_fg(c)` |
| `.text_xs()`, `.text_sm()`, etc. | no equivalent — there's one size |
| `.font_weight(...)` | `\| Bold` (only bold/dim available) |
| `.italic()` | `\| Italic` |
| `.underline()` | `\| Underline` |
| `.opacity(0.6)` (on text) | `\| Dim` (close enough) |
| `.font_buffer()` (mono) | the terminal IS mono; ignore |
| Markdown rendering | use `maya::markdown(src)` or `StreamingMarkdown` |

The lack of font-size variation is significant. Zed uses `LabelSize::XSmall`
for elapsed-time labels and `LabelSize::Small` for buttons. In TUI, the
substitute for "smaller, less prominent" is `Dim` style (lower contrast)
plus possibly different colors (e.g., muted gray vs full text).

## 6. Icons → unicode glyphs

GPUI ships SVG icons. We use unicode glyphs. The mapping for the agent
panel:

| Zed Icon | maya glyph | Codepoint | Notes |
|---|---|---|---|
| `IconName::ToolSearch` | 🔍 (or `▢`) | U+1F50D | for read |
| `IconName::ToolPencil` | ✎ | U+270E | for edit |
| `IconName::ToolDeleteFile` | ✗ | U+2717 | for delete |
| `IconName::ToolTerminal` | ❯ | U+276F | for execute |
| `IconName::ToolThink` | ⋯ or 🧠 | U+22EF | for thinking |
| `IconName::ToolWeb` | ⌥ or 🌐 | | for fetch |
| `IconName::ToolHammer` | ⚒ | U+2692 | generic |
| `IconName::ArrowRightLeft` | ⇄ | U+21C4 | for move |
| `IconName::Check` | ✓ | U+2713 | success |
| `IconName::Close` | ✗ | U+2717 | failure / deny |
| `IconName::ChevronDown` | ▾ | U+25BE | dropdown |
| `IconName::ChevronUp` | ▴ | U+25B4 | collapse |
| `IconName::Send` | ↵ or → | U+21B5 | send |
| `IconName::Stop` | ■ | U+25A0 | stop |
| `IconName::AlertCircle` | ⚠ | U+26A0 | warning |
| `IconName::Undo` | ↺ | U+21BA | restore checkpoint |
| `IconName::Maximize` | ⤢ | U+2922 | full screen |
| `IconName::Minimize` | ⤡ | U+2921 | exit full screen |
| `IconName::Plus` | + | U+002B | add context |

**Avoid emoji where possible**. Plain BMP unicode (✓, ✗, ▾) renders
correctly in nearly every terminal at consistent width. Emoji can render
at 2 cells wide on some terminals and 1 cell on others, which breaks
layout.

Spinner during loading uses maya's built-in spinner (`SpinnerStyle::Dots`
gives ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏) — never a literal emoji.

## 7. State patterns

### `.hover(...)` — terminals have no hover

Translation: **focus**, not hover. Use the focus model in
`maya/include/maya/core/focus.hpp`. When an element gains focus (via
Tab navigation or explicit click in mouse-enabled terminals), apply the
"hover" style.

For tool card disclosure chevrons — Zed shows `[⌄]` only on hover. In
TUI: show it always, but `Dim` when the card isn't focused.

### `.focus(...)` — translates directly

Use `FocusNode` in interactive widgets (Input, Button, etc.). When
focused, swap border color to `border_focused` (accent).

### `.active(...)`, `.selected(...)` — translate to a style swap

State carried in the Model; the view checks the state and styles
accordingly. There's no implicit pseudo-class system.

### Animation

GPUI has `pulsating_between(0.2, 0.6)` for the loading thread pulse.
TUI can do this via:

```cpp
Sub<Msg>::every(60ms, Tick{})

// in update:
m.pulse_phase = (m.pulse_phase + 0.06f);
auto opacity_proxy = 0.4f + 0.2f * std::sin(m.pulse_phase);

// in view:
auto style = (opacity_proxy < 0.5f) ? Dim : Style{};
```

We can't actually animate opacity. We can flip between dim and full
contrast at the right cadence. Same effect, different mechanism.

## 8. Anchors & popovers

| GPUI | maya |
|---|---|
| `PopoverMenu::new(...).anchor(Corner::TopRight).attach(Corner::BottomRight)` | `popup.hpp` widget; you must track the trigger position yourself |
| `ContextMenu::build(...).toggleable_entry(...)` | a `popup` containing a `v(...)` of styled entries with a focused index |
| Modal | `modal.hpp` (only for destructive confirmations) |

In TUI we can't anchor to "bottom-right of a button" with sub-cell
precision. Two strategies:

1. **Inline replacement** — when the user opens the model picker, swap
   the chrome region's content from `[model ▾]` to a vertical menu in
   the same column. Zero anchoring.
2. **Floating popup at known coords** — render the popup at a fixed
   absolute position (e.g., 2 rows below the chrome bar, 4 cols from
   the right) using a `ComponentElement` that knows its parent size.
   Less elegant but matches Zed's visual.

Default to option 1. It's terminal-native and it works.

## 9. Lists & scrolling

| GPUI | maya |
|---|---|
| `list(state, ...).flex_grow()` | wrap content in `Scrollable`; consume scroll msgs to track position |
| `with_sizing_behavior(Auto)` | maya's `Scrollable` is auto-sized by default |
| Follow-tail behavior | track `at_bottom` flag in Model; when streaming + `at_bottom` → call `scroll_to_end()` after every delta |
| `ScrollOutputPageUp/Down` | bind to `PageUp` / `PageDown` |

The follow-tail rule:

```cpp
// In update() — after appending streaming text:
if (m.thread_at_bottom) {
    m.scroll_target = ScrollTarget::End;
}

// In view() — set the scrollable position from m.scroll_target.

// When user scrolls up → m.thread_at_bottom = false.
// When user scrolls back to bottom → m.thread_at_bottom = true.
```

## 10. Markdown

GPUI uses an `Editor` entity with markdown styling. maya has
`maya::markdown(...)` and `maya::StreamingMarkdown`.

Translation rule for **streaming** assistant text:

```cpp
// In Model:
struct AssistantBlock {
    std::string text_so_far;
    StreamingMarkdown md;  // or rebuild each frame
};

// On StreamTextDelta:
m.last_assistant.text_so_far.append(delta);

// In view:
markdown(m.last_assistant.text_so_far)
```

Rebuilding from accumulated text every frame is fine — markdown parsing
is fast and the worst case (huge response) still fits in <50ms parse.

If you measure performance issues, switch to `StreamingMarkdown::append`
which incrementally extends the parse tree.

## 11. The big things terminals can't do (and what to do instead)

| Zed feature | TUI substitute |
|---|---|
| Sub-pixel rendering | accept rounding |
| Variable font sizes | use `Dim` and color contrast for "smaller" |
| Multiple font families | terminal font only — embrace it |
| Real shadows | omit, or fake with darker `bg` on neighboring rows |
| True alpha | use `color.darken(amount)` |
| Linear gradients | hard cut, or alternate-cell `Dim`/`Style{}` for a stipple effect (rarely worth it) |
| Hover states | focus + always-show-dim |
| Smooth animations | tick-based, snap to discrete frames at ~60ms |
| Anchored popovers | inline content swap or fixed-position popup |
| SVG icons | unicode glyphs |
| Multiline overflow tooltips | inline truncation; reveal on focus |
| Drag and drop | not supported; redesign the action |

## 12. A worked example: tool card from GPUI to maya

Zed (`thread_view.rs:6466-6481`):

```rust
v_flex()
    .my_1p5()
    .mx_5()
    .rounded_md()
    .border_1()
    .border_color(self.tool_card_border_color(cx))
    .bg(cx.theme().colors().editor_background)
    .overflow_hidden()
    .child(header)
    .child(body_or_collapsed)
    .when(needs_confirmation, |c| c.child(permission_footer))
```

maya equivalent:

```cpp
auto card_border = failed_or_canceled
    ? BorderStyle::Dashed
    : BorderStyle::Round;
auto bcol = failed_or_canceled
    ? Color::rgb(120, 60, 65)
    : Color::rgb(50, 54, 62);

auto rows = std::vector<Element>{};
rows.push_back(header);
if (expanded) rows.push_back(body);
if (needs_confirmation) rows.push_back(permission_footer);

auto card = (v(std::move(rows))
    | border(card_border)
    | bcolor(bcol)
    | btext(" " + status_icon + " " + tool_name + " ",
            BorderTextPos::Top, BorderTextAlign::Start)
    | padding(0, 1, 0, 1)
    | max_width(Dimension::fixed(120)))
    .build();

// "indent into the conversation column":
auto indented = h(spacer_<2>(), card, spacer_<2>()).build();
```

The mechanical translation is straightforward; the **interesting** part
is recognizing that `.mx_5()` (20px horizontal margin) becomes a 2-cell
indent on each side, and `.my_1p5()` (6px vertical margin) becomes a
single blank row before/after the card (which we get for free from
`gap_<1>` on the parent stream).

## 13. Translation checklist

Before declaring a translation done, check:

- [ ] All borders are `Round` (or `Dashed` for failed states)
- [ ] All padding values are in cells, not px
- [ ] All colors are RGB triples from `05_design_tokens.md`
- [ ] No literal emoji unless the target glyph isn't in BMP unicode
- [ ] All hover states have a focus equivalent
- [ ] All popovers use `popup.hpp` (or inline swap), not `modal.hpp`
- [ ] All scrolling regions are wrapped in `Scrollable`
- [ ] All variable font sizes are encoded as `Dim` + color tier
- [ ] All right-aligned items use `spacer()`, not `justify_between`
- [ ] `max_width` is set on the content column to prevent sprawl in
      wide terminals
