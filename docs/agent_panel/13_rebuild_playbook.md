# 13 — Rebuild playbook

This is the **execution plan**. The other 12 docs describe what the
panel *should* look like; this doc says *what to do, in what order,
to get there*. It's written for an engineer (or future me) who:

1. Has read at least the README and `01_zed_anatomy.md`
2. Can build moha (`cmake --build build`) and run it locally
3. Knows where the source files live (or can find them in a few
   minutes)

Read this top to bottom. Each phase has an explicit "you're done with
this phase when…" checklist. Resist the urge to skip ahead — the
ordering exists because each phase removes a class of risk before the
next.

## 0. Prerequisites

Before starting:

- [ ] Read `README.md`, `01_zed_anatomy.md`, `04_architecture.md`,
      `05_design_tokens.md`. (~30 min)
- [ ] Skim `02_maya_reference.md` and `03_translation.md` for the
      primitives table. (~15 min)
- [ ] Build moha from source. Run it, send a message, watch a tool
      call complete. Look at the panel — note 3 specific things you
      want to change.
- [ ] Open Zed's agent panel side by side. Same prompt. Note the
      visual differences.
- [ ] If the local moha build is broken, **fix the build first**.
      Don't start UI work on a non-compiling base.

## 1. The principle

The current moha panel diverges from Zed in many small ways. Don't
try to converge in one sweep — that's how the previous attempts
failed. Instead, **convert one region at a time**, from the bottom
up:

1. Tokens (foundational, no visible change)
2. Composer (one bordered box, fastest visible win)
3. Message stream containers (the column wrap + max width)
4. User bubbles (small, isolated)
5. Tool cards (the big lift; do them one tool kind at a time)
6. Assistant text (less than you think — it's just markdown + indent)
7. Permissions (overlay onto already-correct tool cards)
8. Chrome (top bar; visible but small surface area)
9. Status bar (cherry on top)
10. Routes (history, diff review)

Each phase you can ship as its own commit / PR. After each, the panel
should still work end-to-end — no week-long branches.

## 2. Phase 1 — Tokens (no visible change)

**Goal**: Centralize colors and spacing constants. Replace inline RGB
literals with named tokens. Code change is mechanical; visual is
identical.

**Tasks**:
1. Create `include/moha/tokens.hpp` with the namespaces from
   `05_design_tokens § 1`.
2. Search the source for `Color::rgb(` and replace each with the
   appropriate token.
3. Audit current file: every color in moha source must be derivable
   from tokens. If you find a one-off color that doesn't match any
   token, **add a new token to `tokens.hpp`** rather than leaving the
   inline literal — exception: status badges with semantic-only meaning
   (already covered).

**You're done when**:
- [ ] No file outside `tokens.hpp` constructs `Color::rgb(...)` directly
- [ ] `cmake --build build` succeeds
- [ ] `git diff` shows only color literal swaps
- [ ] Visual diff vs `master`: byte-identical render

**Common pitfalls**:
- Forgetting `include/moha/tokens.hpp` in the include path → linker
  errors. They're constexpr, so fine to put in a header.
- Confusing `border::dim` (50, 56, 66) with `bg::editor` (40, 44, 52).
  When in doubt, look up in the design tokens doc.

## 3. Phase 2 — Composer

**Goal**: Replace the existing input handling with a bordered,
focus-tinted composer using `maya::TextArea`.

**Tasks**:
1. Add `ComposerState` to the model (see `08_composer § 10`).
2. Wire `Msg::ComposerEdit{string}` and `Msg::ComposerSubmit{}`.
3. In `view()`, render a single bordered box at the bottom with:
   - Border color: focus when `composer.focused`, dim otherwise
   - Inside: `TextArea` with `min_lines=3`, `max_lines=12`,
     line numbers off
   - Below `TextArea`: a divider, then a placeholder toolbar row with
     just `[↵ Send]`
4. Wire the keyboard:
   - `Enter` → `ComposerSubmit`
   - `Shift+Enter` → newline (TextArea default)
   - `Esc` → blur composer
   - `Ctrl+C` → `StreamCancel`

**You're done when**:
- [ ] Composer renders bordered, blue when focused, dim otherwise
- [ ] Typing works; multi-line works; paste works
- [ ] Submitting fires the existing message-send pipeline
- [ ] No regressions in basic chat
- [ ] `Esc` cancels a streaming response

**Defer**:
- Token meter (added in phase 9)
- Mention chips (added in phase 9)
- Slash command popup (phase 9)
- Mode toggles (phase 9)

## 4. Phase 3 — Message stream container

**Goal**: Wrap the message list in a `Scrollable`, cap content width
at 120 cells, center the column.

**Tasks**:
1. Add `ScrollState` to `Model` (see `06_message_stream § 8`).
2. In `views::message_stream`, wrap the row stack in:
   ```cpp
   auto column = (v(stack)
       | max_width(Dimension::fixed(120))
       | align_self_<Align::Center>
   ).build();
   return Scrollable(column).with_offset(m.scroll.offset).build();
   ```
3. Wire `↑/↓/PgUp/PgDn/Home/End`.
4. Implement tail-follow: on `StreamTextDelta` and `StreamToolUseStart`,
   if `at_bottom`, snap to end.

**You're done when**:
- [ ] In an 80-col terminal, conversation fills the width
- [ ] In a 200-col terminal, conversation is 120 cells, centered
- [ ] PgUp/PgDn scroll without breaking the tail
- [ ] During streaming, view auto-follows new content
- [ ] User scrolling up disables auto-follow
- [ ] `End` re-engages auto-follow

## 5. Phase 4 — User bubbles

**Goal**: Render user messages as bordered, padded boxes with focus
tint.

**Tasks**:
1. Implement `views::user_bubble` per `06_message_stream § 4`.
2. Add `Message::editing` flag and `Msg::EnterEdit{id}` /
   `Msg::CancelEdit{}` / `Msg::RegenerateFrom{id, text}`.
3. When `editing`, render an embedded composer in the bubble
   (re-use TextArea; no toolbar; chip row "[Esc] Cancel
   [Ctrl+Enter] Regenerate").
4. Apply `gap_<1>` between message blocks at the parent level.

**You're done when**:
- [ ] User messages render in `Round`-bordered boxes
- [ ] Border color changes from dim to focus when entering edit mode
- [ ] `Enter` on a focused user bubble enters edit mode
- [ ] Edit mode preserves the original text; Cancel restores it
- [ ] Regenerate truncates and restreams (use existing pipeline)
- [ ] Visible spacing between user bubbles and assistant text

**Defer**:
- Subagent variant (dashed border + indent) — there's no subagent
  data today

## 6. Phase 5 — Tool cards (one tool kind at a time)

**Goal**: Replace the current ad-hoc tool rendering with the typed
widgets from `maya/include/maya/widget/*_tool.hpp`.

This is the biggest phase. Subdivide by tool kind. Suggested order:

1. **Bash** — most visible; users see tool execution most often
2. **Read** — simplest; shakes out the integration
3. **Edit** — the diff body is real meat
4. **Write** — similar to Read
5. **Fetch** — straightforward
6. **Search**, **Think**, **Agent**, **Other** (generic) — last

For each tool kind:

1. Implement `render_<kind>_tool(const ToolUse& tu)` per the spec
   in `07_tool_cards § 5.x`.
2. Map `ToolStatus` to the widget's per-tool status enum.
3. Wire elapsed-time updates: in your `Tick{}` arm, walk active tool
   uses and bump their `elapsed_seconds`.
4. Wire expand/collapse: `Msg::ToolCardToggle{tool_id}`.
5. Apply default-expanded rules from `07_tool_cards § 4`.

After each tool kind:
- [ ] Run a real prompt that exercises it. Verify visually.
- [ ] Compare to Zed side-by-side. Note differences.
- [ ] Fix any color / border / icon mismatches against
      `05_design_tokens` and `07_tool_cards`.

**You're done when**:
- [ ] All tool kinds render with the typed widgets
- [ ] `tokens::border::dim` for normal, `Dashed + failed color` for failed
- [ ] Elapsed time appears and updates every ~1s
- [ ] Status icons match the table in `07_tool_cards § 3`
- [ ] No tool kind falls back to ad-hoc rendering except `Other`

## 7. Phase 6 — Assistant text + markdown

**Goal**: Assistant text renders as inline markdown (no bubble),
indented `padding(0, 2, 0, 2)`. Code blocks render with the right bg.

**Tasks**:
1. Confirm `maya::markdown(...)` is available and renders fenced code.
2. Replace the current assistant text rendering with `markdown(text)`
   wrapped in `padding(0, 2, 0, 2)`.
3. Verify code blocks pick up `tokens::bg::editor` background. If not,
   wrap fenced code blocks manually (see `06_message_stream § 5`).
4. Verify links are styled in `tokens::fg::link`.

**You're done when**:
- [ ] Assistant text appears without a bubble
- [ ] Indented 2 cells from chrome edges
- [ ] Code blocks have a darker background and stand out
- [ ] Inline `code spans` render in `text_muted` color
- [ ] Lists, headings render with sensible spacing
- [ ] Long lines wrap

## 8. Phase 7 — Permissions

**Goal**: Inline permission card inside tool cards (not modal).

**Tasks**:
1. Add `PendingPermission` and `ToolStatus::Confirmation` to the
   data model.
2. Implement gating-key derivation per tool kind
   (`09_permissions § 3`).
3. Implement layered lookup (per-thread → workspace → global).
4. Implement profile defaults (`09_permissions § 5`).
5. In the kick loop (`04_architecture § kick_pending_tools`), gate
   on permission before executing.
6. Render `views::tool_card_with_permission(tu, m)` when
   `tu.status == Confirmation`.
7. Wire keys: `Y/A/Shift+A/N/D/Shift+D/Esc`.
8. Persist workspace permissions to
   `~/.config/moha/permissions/<workspace_hash>.json`.

**You're done when**:
- [ ] Bash with a never-allowed command pops a permission card
- [ ] `Y` allows once; `Shift+A` always-allows
- [ ] Always-allow persists across restarts
- [ ] Profile `Write` auto-allows reads, asks for writes
- [ ] Profile `Minimal` auto-denies write/bash
- [ ] Composer is disabled while permission is pending
- [ ] No modal popup; no banner; everything inline

## 9. Phase 8 — Chrome + status bar

**Goal**: Single-line top bar (chrome) and bottom-line status bar.

**Tasks**:
1. Implement `views::chrome(m)` per `11_navigation § 1`.
2. Implement `views::status_bar(m)` per `11_navigation § 8`.
3. Wire the phase indicator: `Tick{80ms}` advances spinner index
   when streaming.
4. Add `Msg::OpenPopover{kind}` and render model/profile popovers
   (defer the dropdown body — just open and close, with placeholder
   "TODO" content). Real popover bodies in phase 10.
5. Wire `Ctrl+H` (history), `Ctrl+M` (model), `Ctrl+P` (profile),
   `?` (help).

**You're done when**:
- [ ] Top bar shows `◆ moha · <thread title>  ●Idle  …`
- [ ] Phase indicator pulses spinner when streaming
- [ ] Bottom status bar shows context-sensitive hint
- [ ] `?` opens help modal (with at least 5 sections from
      `key_help_entries()`)
- [ ] `Ctrl+H/M/P` open empty/placeholder popovers — fine for now

## 10. Phase 9 — Composer toolbar + extras

**Goal**: Bring composer up to spec — toolbar, mode toggles, token
meter, mention chips, slash commands.

**Tasks**:
1. Toolbar layout: profile / model / fast / thinking / token meter /
   spacer / send button (`08_composer § 4`).
2. Token meter color tiers (`08_composer § 5`).
3. Mention popup (`@`) with fuzzy file picker (`08_composer § 6`).
4. Slash command popup (`/`) with at least `/help` and `/new`.
5. Context chip row above editor.
6. Stop button replaces Send during streaming.

Order within this phase: token meter → mode toggles → slash popup
→ mention popup → context chips. Each shippable independently.

**You're done when**:
- [ ] Toolbar matches the visual in `08_composer § 1`
- [ ] Token meter colors shift correctly past 80% / 95%
- [ ] `Ctrl+F` toggles Fast Mode (icon flips)
- [ ] `Ctrl+T` toggles Thinking
- [ ] `/` opens slash popup; arrow + Enter selects
- [ ] `@` opens mention popup; selection inserts a chip
- [ ] Chips render above editor; Backspace at line-start removes
- [ ] Send button shows `[⠋ Stop]` during streaming, click cancels

## 11. Phase 10 — Routes (history + diff review)

**Goal**: Add routes that aren't the message stream.

### History route

1. `Msg::OpenRoute{History}` populates `m.history_view` from
   `~/.config/moha/threads/<workspace>/`.
2. Render the layout from `11_navigation § 4`.
3. Group + search.
4. Open / new / delete actions.

### Diff review route

1. `Msg::OpenDiffReview` collects edits from the current thread's
   tool calls into `m.diff_review`.
2. Render the layout from `10_diff_review § 2`.
3. File list left, hunk view right.
4. `↑/↓` switch file; `J/K` move hunks.
5. `Enter` applies; `Esc` returns.

**You're done when**:
- [ ] `Ctrl+H` shows past threads grouped by recency
- [ ] Search filters the list
- [ ] Selecting a thread loads it (route swap, chrome shows new title)
- [ ] `Ctrl+D` (or `D` on a tool card) opens the diff review
- [ ] Hunks render with red-/-green colors
- [ ] Apply All actually writes files; Esc leaves them queued
- [ ] Both routes use `m.route_stack` for back-navigation

## 12. Phase 11 — Polish

These are the visible-but-small details that make the panel feel
finished. Do them last.

- [ ] Checkpoint dividers (`06_message_stream § 6`) — visible only,
      restore is a no-op for now (toast: "Not implemented")
- [ ] Stream error callout (`06_message_stream § 7`)
- [ ] Auto-save threads on `StreamFinished` and on permission decisions
- [ ] Auto-save composer drafts on 1s idle
- [ ] Help modal with full keymap
- [ ] Settings route (deferred until defaults stabilize)
- [ ] Per-hunk accept/reject in diff review (deferred unless asked)

## 13. What to defer permanently (or until specific user ask)

These are explicitly **out of scope for the rebuild**:

- Subagent threads / `dispatch_agent` tool (no data model)
- `ThinkingChunk` differentiation (Anthropic streams thinking
  un-typed; needs anthropic.cpp work)
- Side-by-side diff layout
- Word-level diff highlighting in review surface
- Granularity dropdown for permissions (`G` key)
- Global permissions store
- Auto-open diff review at end of multi-edit turn
- User-customizable keymap
- Light theme
- Mouse / hover effects (TUI-acceptable; defer entirely)
- Command palette (Zed-style); use slash commands instead
- Inline image previews
- Voice input (lol)

## 14. Test plan per phase

Each phase has a "you're done when" checklist. Beyond that, run
this every-phase sanity test:

1. **Smoke test**: Open the panel, type "hi", press Enter, watch
   response stream in. Should look correct.
2. **Tool test**: Type "list files in src/". Watch the bash tool
   card appear, run, and complete. Card should be:
   - Bordered Round when running
   - `●` icon during running
   - `✓` icon when complete
   - Elapsed time showing
3. **Permission test**: Type "delete /tmp/foo.txt". Should pop a
   permission card. Press N to deny. Card should turn dashed red.
4. **Edit test**: Type "edit README.md to add a TODO at the top".
   Watch the edit tool card show the diff. Default-expanded.
5. **Cancel test**: Type a long-prompt, then press Esc mid-stream.
   Stream should stop; partial response stays.
6. **Resize test**: Resize the terminal from 80 cols to 200 cols.
   Conversation column should stay 120 cells, centered in wider
   terminals, and span full width in narrow ones.

If any of these regress, **fix before moving to the next phase**.
The whole point of the phased rollout is that each commit is a
working improvement, not a step toward something later.

## 15. Common pitfalls

A short list of things every previous rebuild attempt got wrong:

1. **Inline `Color::rgb(...)` literals scattered everywhere.** Phase 1
   exists to fix this once and for all. Don't skip phase 1.
2. **Treating tool cards as one big render branch.** They're 8+
   variants. Use the typed widgets per kind. Don't write one
   500-line function.
3. **Modal permissions.** They feel "professional" because of OS
   conventions, but they break flow. Inline always.
4. **Big `view()` function.** Split per region. Each render function
   takes `const Model&` and returns an Element. Top-level `view()`
   is just a vertical stack of region renders + route switch.
5. **No `Scrollable` wrapping the message stream.** Without this, the
   conversation becomes unscrollable past one terminal page.
6. **Putting border colors inside the widget instead of from
   tokens.** The widgets in maya already do this for status; for
   focus tint, you compute the color outside and pass it.
7. **Forgetting `gap_<1>` between message blocks.** Without it,
   bubbles touch and the panel looks crammed.
8. **Stuffing things into the main composer that belong on the
   chrome (model selector, history button, etc.).** The composer is
   for *composing*. Navigation is on chrome. Don't blur the lines.
9. **Trying to make popovers float OS-style.** maya doesn't do that.
   Position fixed within the panel, accept the limitation.
10. **Adding spinners to individual tool cards.** The icon `●`
    already says "running." Spinning anything else is noise.

## 16. Acceptance criteria (the whole panel is done when…)

- [ ] Panel renders correctly at 80, 120, 200 cols
- [ ] Empty thread shows centered prompt
- [ ] User → assistant → tool → assistant → tool flow looks like Zed
- [ ] All ~10 tool kinds have typed cards; colors and borders match
      tokens
- [ ] Permission cards appear inline, never as modals or banners
- [ ] Streaming feels live; phase indicator pulses
- [ ] Composer supports multi-line, mentions, slash commands, mode
      toggles, token meter
- [ ] History route opens, lists threads, can switch
- [ ] Diff review route opens, shows hunks, can apply
- [ ] Help modal lists every binding from `12_keymap.md`
- [ ] No modal overlays except confirms (delete thread) and Help
- [ ] Side-by-side comparison with Zed feels like the same product
      family
- [ ] Build is clean, tests pass, no leaked debug prints

## 17. After the rebuild — maintenance

When new features arrive:

- Adding a new tool kind:
  1. Update `tool_kind_from_name()` in `07_tool_cards § 7`
  2. Add a typed widget in `maya/include/maya/widget/<kind>_tool.hpp`
  3. Add `render_<kind>_tool` in moha
  4. Update permission gating-key in `09_permissions § 3`
  5. Add icon to `05_design_tokens § 4`
- Adding a new key binding:
  1. Add to `12_keymap.md`
  2. Add to `key_help_entries()` (so it appears in help modal)
  3. Implement in the appropriate `handle_*_key` dispatcher
- Changing a color: edit `tokens.hpp` only. Never edit downstream
  files for color changes.
- Adding a route: add to `Route` enum, switch in top-level `view()`,
  add to `route_stack` handling.

The 13 docs are not write-once. They're the **contract**. When the
panel changes, the docs change in the same commit. If a doc and the
code disagree, the doc is canonical and the code is wrong.

That's how the rebuild stays a rebuild rather than a one-time effort
that drifts.
