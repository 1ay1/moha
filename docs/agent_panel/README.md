# Agent Panel — Rebuild Documentation

This directory specifies, in painful detail, **how to build a faithful TUI port
of Zed's agent panel using maya**. It exists because past attempts have
underdelivered: visual mismatches, modal overlays where Zed has integrated
panels, ad-hoc spacing, fragmented state. The goal here is that an engineer
(or a fresh AI session) reading these docs end-to-end can rebuild the UI
without re-deriving any of the design decisions.

## Why this exists

moha's UX is supposed to feel like Zed's agent panel running in a terminal.
"Like Zed" means the things that make Zed's agent panel coherent:

- Tool calls render as **integrated cards inline with the conversation**, not
  as overlays that disappear.
- Permission requests are **footers attached to the relevant tool card**, not
  modal popups that interrupt flow.
- Edits and diffs render **inline in the card body**, with hunk-level
  accept/reject affordances.
- Checkpoint dividers appear **between turns** as a quiet horizontal rule,
  not a hard interruption.
- The composer is a **bordered box at the bottom** with mention chips and a
  visible token meter — not a featureless input line.
- Model / profile / mode selection happens through **popovers anchored to
  the chrome**, not full-screen modal lists.
- Failure shifts a card's border from `Round` → `Dashed`, not a red banner
  somewhere unrelated.

The current moha implementation gets only some of this right. The rest of
this doc set tells you exactly what to build, in what order, with what
maya primitives, to close the gap.

## How to read this

Read in order if you've never touched the codebase. Skim the index and jump
if you have.

| # | File | What it covers | Read if you're… |
|---|---|---|---|
| 01 | [zed_anatomy.md](01_zed_anatomy.md) | The Zed agent panel as a visual+behavioral spec, with file:line refs into `crates/agent_ui` | Designing/reviewing UI |
| 02 | [maya_reference.md](02_maya_reference.md) | The maya primitives and widgets that matter for this UI | Writing render code |
| 03 | [translation.md](03_translation.md) | Mapping Zed's GPUI calls (`div().border_1().rounded_md().bg(...)`) to maya `dsl::v / h / border / bcolor / padding` | Translating any Zed component |
| 04 | [architecture.md](04_architecture.md) | Elm-style Model/Msg/update/view, streaming integration, how SSE deltas reach the UI | Touching `update()` or wiring a new event source |
| 05 | [design_tokens.md](05_design_tokens.md) | Color palette mapped from Zed theme tokens to RGB triples, spacing rhythm, max content width, typography | Writing any styled element |
| 06 | [message_stream.md](06_message_stream.md) | User bubble, assistant text, thinking blocks, checkpoint dividers, scrolling | Touching the conversation list |
| 07 | [tool_cards.md](07_tool_cards.md) | The tool card spec: header, body, status icons, expand/collapse, dashed-on-failure, all per-tool variants | Adding/changing any tool's UI |
| 08 | [composer.md](08_composer.md) | Multi-line editor box, mention chips, send/stop button, token meter, Fast Mode toggle | Touching the input area |
| 09 | [permissions.md](09_permissions.md) | Inline permission footer on the tool card, "Always for X" patterns, granularity dropdown | Touching the permission flow |
| 10 | [diff_review.md](10_diff_review.md) | Inline diff cards, hunk navigation, accept/reject, the integrated-vs-modal decision | Touching diff rendering |
| 11 | [navigation.md](11_navigation.md) | Model/profile/mode selector popovers, thread history view, history vs new-thread routing | Touching chrome / sidebars |
| 12 | [keymap.md](12_keymap.md) | The full keybinding spec, with TUI substitutions where Cmd-keys aren't available | Touching subscriptions / keys |
| 13 | [rebuild_playbook.md](13_rebuild_playbook.md) | A concrete, ordered rebuild plan: what's done, what's broken, what's next, in dependency order | Actually shipping the rewrite |

## Conventions used in these docs

- **`zed/path/to/file.rs:N`** — refers to `/Users/ayush/projects/zed/crates/...`
- **`maya/include/maya/foo.hpp:N`** — refers to `maya/include/maya/...`
- **`src/main.cpp:N`** — refers to moha's source
- **TUI constraints** — terminals are monospaced, integer-cell, no
  sub-pixel rendering, no true hover (focus instead). When a Zed pattern
  doesn't translate, the doc names the substitution.
- **"Card" / "bubble" / "footer"** — terms used the same way Zed uses them
  in `agent_ui`.

## Source ground truth

These docs were written by reading the actual sources:

- **maya** at `/Users/ayush/projects/moha/maya/` (in-tree git submodule)
- **moha** at `/Users/ayush/projects/moha/`
- **Zed** at `/Users/ayush/projects/zed/`, primarily `crates/agent_ui/` and
  `assets/keymaps/default-macos.json`

When a doc statement claims a Zed behavior, it should be backed by a file:line
ref. When it claims a maya capability, same. If you find a claim with no ref,
it's a candidate for verification — fix it.
