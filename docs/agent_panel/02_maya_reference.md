# 02 — maya Reference (the parts the agent panel needs)

maya is the in-tree TUI library at `maya/` (a git submodule). It's broad
— 60+ widgets, a flex layout engine, an Elm-style runtime — and most of
it is irrelevant to the agent panel. This doc inventories what we **will
use**, with real signatures and one-line idioms.

For the full library tour, read `maya/include/maya/maya.hpp` and walk
the headers. Everything below is grounded in real code in
`maya/include/maya/`.

## 1. Element model

`maya/include/maya/element/element.hpp`

Everything maya renders is an `Element` — a `std::variant` of:

```cpp
struct Element {
    using Variant = std::variant<
        BoxElement,        // flex container, optional border
        TextElement,       // styled text leaf, with rich-run spans
        ElementList,       // transparent fragment (splices into parent)
        ComponentElement   // post-layout lazy render (size-aware)
    >;
};
```

You almost never construct these directly — you use the **DSL** in
`maya/include/maya/dsl.hpp`.

### TextElement (`element/text.hpp:134`)

```cpp
struct TextElement {
    std::string content;
    Style       style;
    TextWrap    wrap = TextWrap::Wrap;
    std::vector<StyledRun> runs;   // inline rich text spans
};

struct StyledRun {
    std::size_t byte_offset;
    std::size_t byte_length;
    Style       style;
};

enum class TextWrap : uint8_t {
    Wrap, TruncateEnd, TruncateMiddle, TruncateStart, NoWrap,
};
```

For long tool names that need to fade-into-background like Zed's
gradient, the right tool is `TruncateEnd` plus a styled trailing
spacer. (TUIs can't do real gradients — see `03_translation.md`.)

### BoxElement (`element/box.hpp:122`)

```cpp
struct BoxElement {
    FlexStyle    layout;
    Style        style;        // bg color, etc.
    BorderConfig border{.style = BorderStyle::None};
    Overflow     overflow = Overflow::Visible;
    std::vector<Element> children;
};
```

### ElementList

A transparent group. `dsl::v(a, b, c)` and `dsl::h(a, b, c)` produce
`BoxElement`s; `ElementList` is for "I have a runtime vector of
elements that should splice into the parent". Used a lot in `view()`
when you build `std::vector<Element> rows` and pass it to `v(rows)`.

### ComponentElement

The escape hatch for "render once you know my final size".

```cpp
struct ComponentElement {
    std::function<Element(int width, int height)> render;
    std::function<Size(int max_width)> measure;  // optional
    FlexStyle layout;
};
```

Useful when we need pixel-aware (cell-aware) layout — e.g., a tool
card header that wants to right-align the elapsed time at the available
width.

## 2. The DSL

`maya/include/maya/dsl.hpp`. Two flavors: compile-time (`t<"…">`,
`v(...)`) and runtime (`text(...)`, `dyn(...)`).

### Compile-time text

```cpp
using namespace maya::dsl;

auto el = t<"Read"> | Bold | Fg<152, 195, 121>;
```

- `t<"…">` is a `TextNode<S>` template; the literal becomes a
  non-type template parameter
- `Bold`, `Dim`, `Italic`, `Underline`, `Strike`, `Inverse` are tag
  constants
- `Fg<r,g,b>` and `Bg<r,g,b>` are template tags

### Runtime text

```cpp
auto el = text(std::format("{} / {}", in, max)) | Dim;
auto el = text(42);                         // numeric → string
auto el = text(s, Style{}.with_fg(...));    // explicit style
```

Pipe operators support both styles and layout modifiers depending on
the target. Always works on `text(...)`.

### Containers

```cpp
v(a, b, c)   // vertical (Column)
h(a, b, c)   // horizontal (Row)
```

Pipe layout/style modifiers:

```cpp
v(header, body, footer)
    | border(BorderStyle::Round)
    | bcolor(Color::rgb(50, 54, 62))
    | btext(" Read ", BorderTextPos::Top, BorderTextAlign::Start)
    | padding(0, 1, 0, 1)         // top, right, bottom, left (cells)
    | gap_<0>                      // 0 cells between children
    | grow_<1>                     // grow to fill main axis
```

Compile-time numeric pipes (`pad<1>`, `gap<2>`, `grow_<1>`) exist for
cases where the value is fixed. Runtime versions take ints.

### Conditional rendering

```cpp
when(condition, node_a, node_b)   // ternary
when(condition, node)              // empty if false
```

### Map a range to nodes

```cpp
map(items, [](const Item& it) {
    return text(it.label);
})
```

### Dynamic escape hatch

```cpp
dyn([&]{ return some_computed_element(); })
```

Use this when you can't put logic in a clean expression — for example
choosing a widget at runtime based on a string match.

### Spacer

`spacer()` (or `dsl::spacer`) — a `grow_<1>` placeholder. Right-aligns
the next child in an `h()` row.

## 3. Layout (yoga-flavored flexbox in cells)

`maya/include/maya/layout/yoga.hpp`

Everything works in **terminal cells** (integers). All Zed `rems_from_px`
values must be rounded to cells. See `05_design_tokens.md`.

### FlexStyle

```cpp
struct FlexStyle {
    FlexDirection direction;        // Row | Column | RowReverse | ColumnReverse
    FlexWrap      wrap;             // NoWrap | Wrap | WrapReverse
    Align         align_items;      // Start | Center | End | Stretch
    Align         align_self;
    Justify       justify_content;  // Start | Center | End | SpaceBetween | SpaceAround | SpaceEvenly

    float flex_grow   = 0.0f;
    float flex_shrink = 1.0f;
    Dimension flex_basis = Dimension::auto_();

    Dimension width, height;
    Dimension min_width, max_width, min_height, max_height;

    Edges<int> margin, padding, border;
    int gap = 0;

    Overflow overflow;     // Visible | Hidden
    Display  display;
};
```

### Dimension

```cpp
struct Dimension {
    enum class Kind { Auto, Fixed, Percent };
    Kind  kind;
    float value;

    static constexpr Dimension auto_();
    static constexpr Dimension fixed(int v);
    static constexpr Dimension percent(float p);
};
```

**Practical shapes** for the agent panel:

- **Card max-width**: `max_width(Dimension::fixed(120))` — caps the
  message-stream content column
- **Composer**: `flex_basis(Dimension::fixed(120))` plus `flex_shrink(1)`
- **Send button**: fixed `width(Dimension::fixed(8))`
- **Token meter**: `flex_basis(auto_())`, right-aligned via `spacer()`

## 4. Style + Color

`maya/include/maya/style/style.hpp`, `style/color.hpp`.

```cpp
struct Style {
    std::optional<Color> fg, bg;
    bool bold, dim, italic, underline, strikethrough, inverse;

    Style with_fg(Color c) const;
    Style with_bg(Color c) const;
    Style with_bold(bool=true) const;
    Style with_dim(bool=true)  const;
    // … etc
};

class Color {
public:
    static constexpr Color red();             // ANSI named
    static constexpr Color indexed(uint8_t);  // 256-color
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b);  // truecolor
    static consteval  Color hex(uint32_t);    // 0xRRGGBB

    constexpr Color lighten(float a) const;
    constexpr Color darken(float a)  const;
};
```

The agent panel uses **truecolor RGB** for everything (terminals that
don't support truecolor degrade gracefully via maya's color step-down).
The exact RGB values for each Zed token are in `05_design_tokens.md`.

## 5. Border

`maya/include/maya/style/border.hpp`

```cpp
enum class BorderStyle : uint8_t {
    None,
    Single,         // ┌─┐│┘─└│
    Double,         // ╔═╗║╝═╚║
    Round,          // ╭─╮│╯─╰│   ← default for tool cards
    Bold,           // ┏━┓┃┛━┗┃
    SingleDouble, DoubleSingle,
    Classic,        // +-+|+-+|   ASCII fallback
    Arrow,
    Dashed,         // ╭┄╮┆╯┄╰┆   ← tool cards on failure
};

struct BorderConfig {
    BorderStyle  style;
    BorderSides  sides;     // top/right/bottom/left toggles
    BorderColors colors;    // optional per-side
    std::optional<BorderText> text;
};

struct BorderText {
    std::string     content;
    BorderTextPos   position;   // Top | Bottom
    BorderTextAlign align;      // Start | Center | End
    int             offset = 0;
};
```

### Idiomatic uses

```cpp
// User message bubble
v(content)
    | border(BorderStyle::Round)
    | bcolor(border_color)
    | bg(editor_background)
    | padding(1, 1, 1, 1)          // py_3 → 1 cell, px_2 → 1 cell

// Tool card header label
| btext(" ✓ Read ", BorderTextPos::Top, BorderTextAlign::Start)

// Failed tool card
| border(BorderStyle::Dashed)
| bcolor(Color::rgb(120, 60, 65))
```

## 6. Theme

`maya/include/maya/style/theme.hpp`

```cpp
struct Theme {
    Color primary, secondary, accent;
    Color success, error, warning, info;
    Color text, inverse_text, muted;
    Color surface, background, border;
    Color diff_added, diff_removed, diff_changed;
    Color highlight, selection, cursor, link, placeholder, shadow, overlay;
};

namespace theme {
    inline constexpr Theme dark{...};
    inline constexpr Theme light{...};
    inline constexpr Theme dark_ansi{...};   // 16-color fallback
    inline constexpr Theme light_ansi{...};
}
```

The runtime theme is part of `RunConfig` (`.theme = theme::dark`) and
flows into `view()` indirectly via the canvas. **For the agent panel
we'll bypass the global Theme and hardcode the Zed-equivalent palette**
(see `05_design_tokens.md`) — the global Theme is too generic to match
Zed token-for-token.

## 7. The widgets we lean on

All in `maya/include/maya/widget/`. Each is a class with `Config`,
constructor, getters/setters, and `operator Element() const`.

### `tool_call.hpp` — the generic card

This is the foundation. Already used in moha. Header label, status icon
+ color, optional content, auto-collapse logic.

```cpp
ToolCall::Config cfg{.tool_name = "Read", .kind = ToolCallKind::Read};
ToolCall tc(cfg);
tc.set_status(ToolCallStatus::Completed);
tc.set_elapsed(0.3f);
tc.set_content(some_element);
tc.set_expanded(true);
Element el = tc;       // implicit conversion via operator Element()
```

Status enum (`Pending | Running | Completed | Failed | Confirmation`)
already maps cleanly to Zed.

### `bash_tool.hpp`, `read_tool.hpp`, `edit_tool.hpp`, `write_tool.hpp`

Specialized variants. Each has its own `Status` enum and content
shape. All five use `BorderStyle::Round` normally and `BorderStyle::Dashed`
on failure (after the patch documented in the prior session).

### `markdown.hpp`

```cpp
Element markdown(std::string_view source);

class StreamingMarkdown {
    void set_content(std::string_view);
    void append(std::string_view);
    void finish();
    Element build() const;
};
```

`StreamingMarkdown` is the right tool for assistant text deltas: feed it
chunks and it incrementally re-renders. For TUI you can't get partial
markdown formatting "right" — the rule of thumb is to render the
**accumulated text so far** as plain markdown each frame and accept the
brief flicker as code fences open/close.

### `message.hpp`

```cpp
struct UserMessage {
    static Element build(std::string_view content);
    static Element build(Element content);
};

struct AssistantMessage {
    static Element build(Element content);
};
```

The Zed-bordered user bubble + clean assistant block. Uses Round borders
and the right padding. Verify the token colors match the design tokens
doc; if not, override the style via the `Element` overload.

### `disclosure.hpp` — for thinking blocks and raw-input sections

```cpp
struct Disclosure::Config {
    std::string label;
    std::string open_icon   = "▼";
    std::string closed_icon = "▶";
};

Disclosure d(cfg);
d.toggle();
Element header_only = d;             // header only when closed
Element with_body   = d.build(body); // body shown when open
```

### `divider.hpp`

A horizontal rule; takes a label optionally. Use for the checkpoint
divider:

```
─── ↺ Restore Checkpoint ───────────────────
```

### `spinner.hpp`

```cpp
template <SpinnerStyle S = SpinnerStyle::Dots>
class Spinner {
    void advance(float dt);
    void set_style(Style);
    Element build() const;
};
```

Drive `advance()` from a `Sub<Msg>::every(60ms, Tick{})` subscription
when streaming is active. **Don't** show this inside individual messages
— show it on the send button (just like Zed) and optionally on the
chrome agent badge.

### `input.hpp` / `textarea.hpp`

`Input` is single-line by default; with `InputConfig{.multiline = true}`
it grows. Has `Signal<std::string> value()`, `on_submit`, `on_change`,
`handle_paste`, `handle(KeyEvent)`. Use for the composer.

### `button.hpp`

```cpp
enum class ButtonVariant { Default, Primary, Danger, Ghost };

Button b("Send", []{ … }, ButtonVariant::Primary);
b.handle(key_event);
Element el = b;
```

We need at least: Send/Stop, Allow, Deny, dropdown trigger.

### `popup.hpp`

A non-modal floating box. Different from `modal.hpp` which dims and
captures focus. **Selectors should use `popup`, not `modal`.** Anchor
to the trigger button's last known position (you may need to track
that yourself; see `11_navigation.md`).

### `scrollable.hpp`

A viewport. Wrap the message stream in this. Currently moha doesn't —
this is one of the gaps.

```cpp
Scrollable sv;
sv.set_content(message_stack);
sv.set_height(viewport_h);
sv.scroll_by(delta);
sv.scroll_to_end();
Element el = sv;
```

Emits scroll-position changes; consume to know whether the user has
scrolled away from the tail (so you stop auto-following).

### `callout.hpp`

```cpp
Callout::error("Execution Failed", "stderr text");
Callout::warning("Rate limit reached", "Retrying in 12s");
Callout::info("Authentication required", action_button);
```

Use for inline error states between messages.

### `badge.hpp` / `model_badge.hpp`

Small colored tags. `model_badge` is specialized to display a model
identifier. We need badges for profile (Write/Ask/Minimal) and probably
mode in chrome.

### `file_ref.hpp`

A clickable file path with breadcrumb-style segmentation. Useful in the
header of edit/read cards — the file path is a `file_ref` rather than a
plain string.

### `key_help.hpp`

Renders a row of keybindings — `[K] action  [J] action  [/] search`.
Use it in the status bar / footer.

## 8. Runtime: Program / Cmd / Sub

`maya/include/maya/app/app.hpp`, `maya/core/cmd.hpp`, `maya/app/sub.hpp`.

### Program concept

```cpp
struct App {
    using Model = …;
    using Msg   = std::variant<…>;

    static auto init() -> std::pair<Model, Cmd<Msg>>;
    static auto update(Model, Msg) -> std::pair<Model, Cmd<Msg>>;
    static auto view(const Model&) -> Element;
    static auto subscribe(const Model&) -> Sub<Msg>;
};

int main() { maya::run<App>({.title = "moha", .mode = Mode::Fullscreen}); }
```

### Cmd<Msg>

Side effects as data:

```cpp
Cmd<Msg>::none()
Cmd<Msg>::quit()
Cmd<Msg>::after(100ms, Tick{})
Cmd<Msg>::set_title("…")
Cmd<Msg>::write_clipboard("…")
Cmd<Msg>::task([](auto dispatch){
    // run on background thread; call dispatch(Msg{...}) from anywhere
})
Cmd<Msg>::batch({a, b, c})
```

`Cmd::task` is the streaming hook: SSE deltas dispatched from the libcurl
thread land in `update()` on the UI thread.

### Sub<Msg>

Subscriptions:

```cpp
Sub<Msg>::on_key([](const KeyEvent& k) -> std::optional<Msg> { … })
Sub<Msg>::on_mouse([](const MouseEvent& e) -> std::optional<Msg> { … })
Sub<Msg>::on_resize([](Size s) -> Msg { return Resize{s}; })
Sub<Msg>::on_paste([](std::string s) -> Msg { return Paste{s}; })
Sub<Msg>::every(16ms, Tick{})
Sub<Msg>::batch({…})
```

Use `every()` for the spinner tick during streaming; turn it off when
not streaming so we don't burn CPU.

### key_map convenience

```cpp
key_map<Msg>({
    {'q', Quit{}},
    {'+', Inc{}},
    {SpecialKey::Up, MoveUp{}},
})
```

### RunConfig / Mode

```cpp
struct RunConfig {
    std::string_view title;
    int  fps;                    // 0 = event-driven
    bool mouse;
    Mode mode;                   // Inline | Fullscreen
    Theme theme;
};

enum class Mode { Inline, Fullscreen };
```

**For moha**: `Mode::Fullscreen` while in the agent panel (alt screen,
diff-rendered). `Mode::Inline` would be appropriate for a ":one-shot"
command but isn't what we want for the panel.

## 9. Input events

`maya/include/maya/terminal/input.hpp`.

```cpp
struct CharKey { char32_t codepoint; };
enum class SpecialKey { Up, Down, Left, Right, Home, End, PageUp, PageDown,
                        Tab, BackTab, Backspace, Delete, Insert,
                        Enter, Escape, F1..F12 };
using Key = std::variant<CharKey, SpecialKey>;

struct Modifiers { bool ctrl, alt, shift, super_; };

struct KeyEvent {
    Key       key;
    Modifiers mods;
    std::string raw_sequence;
};

struct MouseEvent {
    MouseButton    button;       // Left | Right | Middle | ScrollUp | ScrollDown
    MouseEventKind kind;         // Press | Release | Move
    Columns x; Rows y;
    Modifiers mods;
};
```

Predicates:

```cpp
key_is(k, 'q')
key_is(k, SpecialKey::Up)
ctrl_is(k, 'c')
alt_is(k, '\r')
```

Bracketed paste arrives as `PasteEvent { content }` — handle it in the
composer to avoid re-firing per-character. This matters for a long
paste of code.

## 10. Rendering pipeline

`maya/include/maya/render/renderer.hpp`. Mostly opaque to us; just know:

- Layout pass: yoga walks the tree, computes a `LayoutNode` (Rect) for
  every node
- Paint pass: visits each Element, paints box borders, then text runs,
  with per-cell ANSI codes
- In `Mode::Fullscreen`, only changed cells are sent to the terminal
  (diff-based double buffer)
- `overflow: Hidden` clips children; use it inside `Scrollable`

You should not need to call into the renderer directly — it's invoked
by `maya::run<App>()`.

## 11. What we will NOT use (and why)

- `modal.hpp` — wrong for selectors; use `popup.hpp` instead. The only
  legitimate `modal.hpp` use in the agent panel is for destructive
  confirmation (e.g., "Delete thread?") — not for picking a model.
- `command_palette.hpp` — Zed's agent panel doesn't have a command
  palette overlay; everything is via popovers / chrome. If we add one
  later, fine, but don't conflate it with model picking.
- `bar_chart`, `flame_chart`, `git_graph`, etc. — visualization widgets
  that don't appear in the agent panel.
- `calendar` — no.
- `system_banner` — Zed uses inline `Callout` rather than top banners
  for agent errors.

## 12. Quick reference: the "cheat sheet" for this UI

```cpp
using namespace maya::dsl;

// User bubble
v(text(content) | wrap_<TextWrap::Wrap>)
    | border(BorderStyle::Round)
    | bcolor(theme_border)
    | bg(editor_bg)
    | padding(1, 1, 1, 1)
    | max_width(120);

// Assistant text (no bubble)
v(StreamingMarkdown(text_so_far).build())
    | padding(0, 2, 0, 2);

// Tool card (use ToolCall widget directly)
ToolCall tc({.tool_name = "Read", .kind = ToolCallKind::Read});
tc.set_status(...);
tc.set_elapsed(elapsed_s);
tc.set_content(read_body);
Element card = tc;

// Failed → dashed (already wired in tool_call.hpp / bash_tool.hpp etc.)

// Checkpoint divider
h(
    text("─── ", dim_style),
    text("↺ Restore Checkpoint", muted_style),
    text(" ", muted_style),
    spacer()
)

// Composer
v(
    Input(/*multiline*/).build(),
    h(
        Button("Fast", on_toggle_fast),
        Button("Think", on_toggle_think),
        spacer(),
        text(token_meter) | dim,
        Button(send_label, on_send, send_variant)
    )
)
| border(BorderStyle::Round)
| bcolor(theme_border)
| padding(0, 1, 0, 1);
```

That's enough vocabulary to write any element in the agent panel. Where
a Zed pattern doesn't fit — `03_translation.md` covers the substitutions.
