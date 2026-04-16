# 05 — Design tokens (colors, spacing, typography)

This is the **palette and rhythm reference**. Every other doc points
here for color names, cell counts, and typographic substitutes.

## 1. Color philosophy

**moha does not have a theming system.** No `tokens` namespace, no
`Theme` struct, no RGB literals scattered through the code. The
terminal's user-chosen palette (iTerm2, Ghostty, Alacritty, etc.) is
the source of truth for what every color *looks like*. Our job is to
pick the right semantic ANSI slot — `red`, `green`, `yellow`, etc. —
and let the user's terminal decide the actual hue.

Why: every past attempt at hard-coded RGB has fought the user's terminal
theme. A user on a Solarized Light terminal expects pale ANSI red, not
moha's idea of red-pink. Forcing 24-bit RGB makes moha look broken
inside their own setup.

### The rules

1. **For accents (status, severity, focus): use named ANSI colors.**
   `Color::red()`, `Color::green()`, `Color::yellow()`, `Color::blue()`,
   `Color::cyan()`, `Color::magenta()`, plus the `bright_*` variants.
   That's the whole palette.

2. **For body text: don't set a foreground.** An empty `Style{}`
   emits no SGR — the terminal renders body text in its default
   foreground. Do not pick "off-white" or "light gray." The terminal
   already knows.

3. **For dim/muted text: use `Style{}.with_dim()`, not a gray RGB.**
   `with_dim()` is the SGR `2` modifier; the terminal renders it as
   "less prominent than default" in whatever way fits the user's theme.

4. **For ambient borders / dividers: use `Color::bright_black()`.**
   It's a themed gray that respects the user's palette. Never
   `Color::rgb(50, 54, 62)`.

5. **No backgrounds on text.** Don't set `with_bg(...)` to tint message
   bubbles, code blocks, or callouts. The default terminal background
   wins. The exception is *intentional* full-row callouts where the
   bg is part of the signal (and even then, prefer leaving bg unset
   and using `with_inverse()` for emphasis).

6. **No `Color::rgb(...)` or `Color::hex(...)` in moha code.** Period.
   The `Color` class still has those constructors — they exist for
   maya users who want them — but moha does not use them.

## 2. The semantic mapping

Use this table when you need to express a state. Pick the named-ANSI
helper that matches the *meaning*, not a hue you have in mind.

| Meaning | Helper | SGR |
|---|---|---|
| Success / accept / "added" | `Color::green()` | `32` |
| Failure / error / "removed" / destructive | `Color::red()` | `31` |
| Warning / pending confirmation / amber accent | `Color::yellow()` | `33` |
| Info / link / focus / "blue accent" | `Color::blue()` | `34` |
| Agent / sub-agent / "purple-y" thing | `Color::magenta()` | `35` |
| Live data / running stream / "teal-y" thing | `Color::cyan()` | `36` |
| Themed gray (border, divider, dim icon) | `Color::bright_black()` | `90` |
| Body text | leave fg unset (`Style{}`) | — |
| Muted body text | `Style{}.with_dim()` | `2` |
| Strong / important | `Style{}.with_bold()` | `1` |
| Subtle hint | `Style{}.with_italic()` (where supported) | `3` |
| Selection / current row | `Style{}.with_inverse()` | `7` |

If you reach for an ANSI color you don't see above (e.g.,
`Color::white()`, `Color::black()`), stop and reconsider — those force
specific hues that fight light-themed terminals. The seven helpers
above plus their `bright_*` variants are what moha uses.

## 3. Tool card states (named-ANSI version)

| State | Border style | Border color | Notes |
|---|---|---|---|
| Pending / Running / Done | `Round` | `Color::bright_black()` | quiet themed gray |
| Failed | `Dashed` | `Color::red()` | dashed + red |
| Cancelled | `Dashed` | `Color::bright_black()` | dashed + neutral |
| Confirmation pending | `Round` | `Color::yellow()` | amber outline |
| Sub-agent (running) | `Round` | `Color::magenta()` | purple-ish accent |
| Edit applied | `Round` | `Color::green()` | success outline |

These match what's hard-coded in the maya widgets after the cleanup —
`maya/include/maya/widget/tool_call.hpp`, `bash_tool.hpp`,
`edit_tool.hpp`, etc.

## 4. Diff colors (named-ANSI version)

| Where | Style |
|---|---|
| Added line text (`+ ...`) | `Style{}.with_fg(Color::green())` |
| Removed line text (`- ...`) | `Style{}.with_fg(Color::red())` |
| Diff prefix (the `+`/`-` glyphs) | `with_fg(green/red).with_dim()` |
| Hunk header (`@@ line N`) | `Style{}.with_dim()` |
| Unchanged context | leave fg unset |

No background tints. The `+`/`-` glyph is enough signal — terminals on
Solarized Light don't need a green wash behind every added line.

## 5. Profile / mode badges

| Profile | Color |
|---|---|
| `Write` | `Color::magenta()` |
| `Ask` | `Color::blue()` |
| `Minimal` | `Color::cyan()` |

Apply with `with_bold()` so the badge reads as a label.

## 6. Phase indicators (chrome status)

| Phase | Color |
|---|---|
| `Idle` | unset fg + `with_dim()` |
| `Streaming` | `Color::yellow()` |
| `AwaitingPermission` | `Color::red()` |
| `ExecutingTool` | `Color::magenta()` |

The active phase pulses by toggling `with_bold()` on/off via a
subscription tick — no color change needed.

## 7. Spacing (cells, not px)

Terminals work in cells. The Zed → cell mapping:

| Zed | px | cells (horizontal) | cells (vertical) |
|---|---|---|---|
| `gap_0p5` | 2 | 0 | 0 |
| `gap_1` | 4 | 1 | 0 (or 1) |
| `gap_2` | 8 | 1 | 1 |
| `gap_3` | 12 | 2 | 1 |
| `gap_5` | 20 | 2 | 2 |
| `p_1` | 4 | 1 | 0 |
| `p_2` | 8 | 1 | 1 |
| `p_3` | 12 | 2 | 1 |
| `p_5` | 20 | 2 | 2 |
| `m_1p5` (margin) | 6 | 1 | 1 |

Why "horizontal" and "vertical" differ: terminal cells are typically
~8px wide × ~16px tall, so 1 vertical cell ≈ 2 horizontal cells of
visual weight.

### Concrete spacing rules for the agent panel

| Element | Padding | Margin |
|---|---|---|
| User message bubble inside | `padding(1, 1, 1, 1)` | gap-1 separation |
| Assistant text | `padding(0, 2, 0, 2)` | gap-1 separation |
| Tool card outer | `padding(0, 1, 0, 1)` (border eats one cell) | 1 row above + below |
| Tool card header | `padding(0, 1, 0, 1)` | none |
| Tool card body | `padding(1, 1, 1, 1)` | none |
| Permission footer | `padding(0, 1, 0, 1)` | none |
| Composer outer | `padding(0, 1, 0, 1)` | gap-1 above |
| Status bar | `padding(0, 1, 0, 1)` | none |
| Chrome | `padding(0, 1, 0, 1)` | none |
| Conversation column max width | 120 cells | indent 2 cells from chrome edges |

### The "max content width" rule

Zed caps content width at ~`max_content_width` (read from settings,
typically 600–900px). In a terminal, cap at **120 cells**. At wider
terminals, the conversation column stays 120 cells wide and is centered
by `mx_auto` equivalent — i.e., wrap in `h(spacer(), content, spacer())`
or set `max_width(Dimension::fixed(120))` and `align_self::Center`.

This prevents the conversation from sprawling across an ultrawide
terminal, which makes long lines unreadable.

## 8. Typography

There's only one font and one size in a terminal. Substitute by:

| Zed | TUI substitute |
|---|---|
| `text_xs` | normal weight |
| `text_sm` | normal weight |
| `text_md` | normal weight |
| `text_lg` | `Bold` |
| `LabelSize::XSmall` | `Dim` |
| `LabelSize::Small` | normal weight + `Dim` |
| `LabelSize::Default` | normal weight |
| `font_weight(Bold)` | `Bold` |
| `italic()` | `Italic` (terminals that support it) |
| `font_buffer()` (mono) | the terminal IS mono, ignore |
| Markdown headings | `Bold` + blank line above/below |
| Code spans | `Dim` |
| Code blocks | leave fg unset; rely on `inverse` for highlight |

### Italic caveats

Some terminals (Terminal.app classic) don't render italic. maya emits
the SGR sequence regardless. If you find a render is unclear without
italic, fall back to `Dim` instead.

## 9. Glyphs (icons)

The complete table is in `03_translation.md` § 6. The most-used:

```
✓  U+2713  success                  ✗  U+2717  error / deny
▾  U+25BE  closed disclosure        ▴  U+25B4  open disclosure
▶  U+25B6  arrow right (chevron)    ◀  U+25C0  arrow left
●  U+25CF  filled status dot        ○  U+25CB  empty status dot
↺  U+21BA  restore checkpoint       ⏸  U+23F8  stop
⤢  U+2922  full screen              ⤡  U+2921  exit full screen
⚠  U+26A0  warning                  ⓘ  U+24D8  info
↵  U+21B5  send                     ❯  U+276F  prompt / terminal
⏱  U+23F1  elapsed time             ⌛ U+231B  waiting
─  U+2500  divider horizontal       │  U+2502  divider vertical
```

For tool kind icons:

```
read    →   ▢   (U+25A2)  empty rectangle
edit    →   ✎   (U+270E)
write   →   📝  (U+1F4DD) avoid — emoji width unreliable, use ✎ + bold
bash    →   ❯   (U+276F)
search  →   ⌕   (U+2315)
fetch   →   ⌘   (U+2318)  or ⏵  (U+23F5)
think   →   ⋯   (U+22EF)
delete  →   ⌫   (U+232B)
move    →   ⇄   (U+21C4)
agent   →   ⚒   (U+2692)  generic hammer
```

When in doubt: use a BMP unicode glyph that's monospaced. Skip emoji.

## 10. Spinner frames

`SpinnerStyle::Dots` (default in maya):

```
⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
```

10 frames. Advance every ~80ms (`Sub::every(80ms, Tick{})`).

Place the spinner:
- On the **send button label** during streaming (label becomes
  "⠋ Stop" → "⠙ Stop" → ...)
- Optionally next to the agent name in chrome
- **Never** inside an individual message — Zed doesn't, and it
  becomes visual noise.

## 11. Composing all of this — example

```cpp
using namespace maya::dsl;

auto user_bubble = [](std::string_view content) {
    return (v(text(content))                       // body — terminal default fg
        | border(BorderStyle::Round)
        | bcolor(Color::bright_black())            // themed gray
        | padding(1, 1, 1, 1)
        | max_width(Dimension::fixed(120))
    ).build();
};

auto failed_tool_card = [](Element body) {
    return (v(std::move(body))
        | border(BorderStyle::Dashed)
        | bcolor(Color::red())
        | padding(0, 1, 0, 1)
    ).build();
};
```

Notice: no `tokens::*` import, no theme lookup, no RGB. The
`bright_black()` and `red()` calls give the terminal what it needs to
render in *its* palette.

## 12. Verifying in a real terminal

After every significant style change:

1. Build moha (`cmake --build build`) and run inside iTerm2, Ghostty,
   Alacritty (truecolor terminals). Also try **Terminal.app classic**
   — it has the most restrictive color and modifier support, so
   anything that looks wrong there is a real issue.
2. Compare side by side with Zed (Activity Bar → Agent Panel) on a
   matching dark theme. Match the *layout and spacing*; the colors
   will differ by terminal palette and that's fine.
3. **Switch your terminal theme** — try Solarized Light, Tomorrow
   Night, Gruvbox. moha should look consistent inside each one. If
   anything reads as "wrong color" relative to the terminal palette,
   there's a stray RGB literal somewhere.
4. Check at three widths: 80 cols (laptop), 120 cols (typical), 200+
   cols (ultrawide).
5. Check at three heights: 25 rows (small), 50 rows (typical), 80+ rows
   (tall).

The 80-col case is where layout problems surface. The ultrawide case
is where the missing `max_width` cap surfaces. The theme-switch case
is where RGB-literal regressions surface. Test all three.
