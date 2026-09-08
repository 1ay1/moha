# Rendering, scrollback, and streaming corruption — code-only analysis

Pure code-trace audit of the inline-render / scrollback / streaming
pipeline. Comments and commit messages are ignored; conclusions are
based on what the code actually does.

Scope of "corruption":

- **Live-frame corruption** — cells on the terminal disagree with the
  model state the next render is about to emit (ghost rows, stale
  cards, mismatched expanded/collapsed state).
- **Scrollback corruption** — rows that have already overflowed the
  inline viewport into the terminal's native (immutable) scrollback
  retain content that no longer matches what the renderer thinks it
  emitted. Inline mode cannot repaint these rows; once wrong, they
  stay wrong until the host emulator's own redraw or a resize-driven
  full clear.

Findings are ordered by severity / concreteness. Each entry names the
file:line trigger, the invariant violated, and the visible failure
mode.

---

## Architecture recap (one-screen mental model)

- **Inline render path**: `maya::compose_inline_frame`
  (`maya/src/render/serialize.cpp`) does a per-row, per-cell-span diff
  against `InlineFrameState::prev_cells` and emits cursor moves + the
  changed cells. Three first-frame sub-cases distinguished by
  `(prev_width, prev_rows)`:
  - `(0, 0)` — fresh state, serialize from cursor (startup / post
    Divergent-clear).
  - `(>0, 0)` — `force_redraw` soft redraw (Ctrl-L). `cursor_up` +
    serialize last `term_h` rows + `\x1b[J`.
  - `(>0, >0)` — normal row diff.
- **Coherence variants**:
  `coherent::InlineSynced{InlineFrameState}` vs `coherent::Divergent`.
  Resize / write-fail collapses to Divergent, which on the next
  render emits `\x1b[2J\x1b[3J\x1b[H` and a full repaint via case (A).
  Startup is *not* Divergent — Runtime pre-seeds `InlineSynced{}` so
  the first frame does not wipe scrollback.
- **Virtualization**: agentty caps the live transcript at
  `kViewWindow = 20` messages, slicing `kSliceChunk = 8` old messages
  at a time (`include/agentty/runtime/app/update/internal.hpp`).
  `maybe_virtualize` (`src/runtime/app/update/submit.cpp:84-103`)
  advances `m.ui.thread_view_start` and emits
  `Cmd::commit_scrollback_overflow()`. That Cmd routes through
  `Runtime::commit_inline_overflow` (`maya/include/maya/app/app.hpp:390-399`)
  which calls `state.commit(state.scrollback_marker(prev_rows - term_h))`
  — strictly `max(0, prev_rows - term_h)` rows, a tight lower bound
  on what's already off-screen.
- **View cache** (`src/runtime/view/cache.{hpp,cpp}`): keyed by
  `(thread_id, MessageId)`. Stores both `Turn::Config` and the
  pre-built `maya::Element` for resolved turns. LRU cap 32. A turn is
  "resolved" iff `streaming_text.empty()` AND every tool is terminal
  AND no pending permission still points at one of its tools
  (`is_turn_resolved`, `src/runtime/view/thread/turn/turn.cpp:194-228`).
- **Streaming pacer**: `pending_stream → streaming_text` drips at
  `clamp(size/8, 32, 256)` bytes per Tick (~30 Hz) in
  `meta.cpp:181-198`. The drained `streaming_text` is what the view
  feeds to `StreamingMarkdown::set_content` every frame.
- **Cmd ordering**: `execute_cmd` (`maya/include/maya/app/app.hpp:752+`)
  runs *synchronously* on the UI thread immediately after the
  reducer. `render(view(model))` happens later in the same loop tick
  (line 1247). Batched Cmds execute left-to-right.

---

## Finding 1 — `ToggleToolExpanded` mutates a cached resolved-turn Element with no invalidation [RESOLVED]

**Status when re-audited:** already fixed. `Message::compute_render_key()`
mixes `tc.expanded` for every tool call (via `ToolUse::compute_render_key()`),
and both `turn_config` and `turn_element` re-check the live render key on
every cache hit (`src/runtime/view/thread/turn/turn.cpp` ~lines 256, 359).
The original audit's claim that `compute_render_key` had "zero callers"
is stale — grep shows it wired through the cache predicates. Leaving the
entry below as historical record.

**Files**

- `src/runtime/app/update/meta.cpp:160-167`
- `src/runtime/view/thread/turn/turn.cpp:354-369` (`turn_element` cache hit)
- `src/runtime/view/thread/turn/turn.cpp:194-228` (`is_turn_resolved`)
- `include/agentty/domain/conversation.hpp:177` (`compute_render_key()` — **never called**)

**The reducer**

```cpp
[&](ToggleToolExpanded& e) -> Step {
    for (auto& msg_ : m.d.current.messages)
        for (auto& tc : msg_.tool_calls)
            if (tc.id == e.id) tc.expanded = !tc.expanded;
    return done(std::move(m));
},
```

**The cache hit**

```cpp
const bool can_cache = !synthetic && is_turn_resolved(msg, m);
if (can_cache) {
    auto& slot = m.ui.view_cache.turn_config(m.d.current.id, msg.id);
    if (slot.element && slot.element_continuation == continuation) {
        return {slot.element, continuation};
    }
}
```

**Invariant violated.** The cached `slot.element` is keyed on
`(thread_id, msg.id, continuation)`. `tc.expanded` is not part of the
key. `compute_render_key()` exists on `ToolUse` but `grep` shows zero
callers — it's dead code.

**Trigger.** Bind a key to `ToggleToolExpanded` (already wired in
subscribe). Press it on a tool call whose enclosing assistant turn is
resolved (every tool terminal, no pending permission, streaming done).

**Symptom.** The model state flips `expanded`, but the next frame
hands maya the *cached* pre-toggle Element. The tool card visibly
does nothing. A subsequent invalidation event (compaction, app
restart, the turn falling out of the LRU after 32 newer accesses)
clears it.

**Severity.** Broken UX feature. Not a rendered-row corruption per
se, but live-frame state desync — the canvas literally cannot reach
the post-toggle representation.

**Cheapest fix.** Either (a) wire `compute_render_key()` into the
cache-hit check so `tc.expanded` participates, or (b) on
`ToggleToolExpanded` drop the cached element for the matching
message id.

---

## Finding 2 — StreamError text-prepend forces `StreamingMarkdown::clear()` + re-feed; scrollback ghost on multi-sub-turn [RESOLVED]

**Fix applied:** the StreamError handler in
`src/runtime/app/update/stream.cpp` now records
`prepended_into_committed_text` when the partial `streaming_text` is
appended onto a non-empty `text` field, and batches
`Cmd::force_redraw()` alongside the retry-after / status-clear Cmd it
would otherwise return. The next render takes maya's Divergent path:
the viewport is wiped (`\x1b[2J\x1b[3J\x1b[H`) and the full live frame
is painted fresh from the cursor's current position, resolving the
scrollback↔viewport seam mismatch.

**Files**

- `src/runtime/app/update/stream.cpp` (StreamError arm, ~line 749 onwards)
- `src/runtime/view/thread/turn/turn.cpp:44-83` (`cached_markdown_for`)
- `maya/src/widget/markdown.cpp:4785-4815` (`StreamingMarkdown::set_content`)
- `maya/src/widget/markdown.cpp:4840-4870` (`StreamingMarkdown::clear`)

**The hot path**

`cached_markdown_for` every frame:

```cpp
const std::string& source =
    msg.text.empty() ? msg.streaming_text : msg.text;
cache.streaming->set_content(source);
```

`set_content` has two fast paths and one replace path:

```cpp
// Fast: unchanged.
// Fast: growth with identical prefix → append delta through StreamSink.

// Replace path: the new content diverges from the old prefix...
clear();
std::string safe = sink_.feed(content);
if (!safe.empty()) append_safe(safe);
```

`clear()` rebuilds `prefix_` as a fresh `shared_ptr<CommittedPrefix>`
and bumps `prefix_->generation` on the next commit.

**The trigger**

In a multi-sub-turn assistant message (post-tool continuation on the
same `Message`), StreamError does:

```cpp
if (!last->streaming_text.empty()) {
    if (last->text.empty()) last->text = std::move(last->streaming_text);
    else                    last->text += std::move(last->streaming_text);
    std::string{}.swap(last->streaming_text);
}
```

When `last->text` was already non-empty (a prior sub-turn already
committed text), the resulting `last->text` is **prepend(text,
streaming_text)** — but the StreamingMarkdown widget's `source_` was
the *previous* `streaming_text` alone. Next frame's
`set_content(msg.text)`:

- New content does NOT have `source_` as a prefix → replace path.
- `clear()` resets `prefix_->generation`, then re-feeds the full
  body, parses fresh blocks.

**Invariant violated.** The committed-block render of the same bytes
has a different per-block layout than the streaming-tail inline
render that produced them originally. Code fences gain rounded
borders, lists gain markers, tables snap into table layout. **Total
content_rows shifts.** The live frame repaints correctly via the
diff — but rows of the *previous* render that have already overflowed
into native scrollback (immutable) retain the inline-tail rendering,
while the new live frame is built on the committed-block rendering.

**Symptom.** Visible boundary mismatch between scrollback tail and
viewport top: duplicated row, missing row, or border fragments,
depending on how much height shifted. Only triggers on long
assistant turns that overflowed viewport before the error.

**Severity.** Real scrollback corruption. Narrow trigger but
reproducible (transient SSE drop mid-stream during a long markdown
body).

**Possible fix.** Either teach `set_content` to handle prepend-only
(detect that `content.ends_with(source_)` and rebuild without
calling `clear()` mid-frame), or force a Divergent transition on the
StreamError → retry path so the next render is a full repaint
(scrollback wipe acceptable — recoverable error already disturbs the
user's mental flow).

---

## Finding 3 — `salvage_args` runs tools with partial-JSON-parsed truncated string fields [RESOLVED]

**Fix applied:** added `agentty::tools::util::ended_inside_string(raw)`
(`include/agentty/tool/util/partial_json.hpp`,
`src/tool/util/partial_json.cpp`) which mirrors
`close_partial_json`'s state machine to detect whether the buffer
stopped while a string value was still open. `salvage_args` now
refuses to salvage in that case (returns an empty json::object), and
both call sites (the `StreamToolUseEnd` parse-fail branch and
`finalize_turn`'s flush) emit a tailored
`"tool args truncated mid-string"` failure so the model knows the
file body never finished arriving instead of seeing a generic
"invalid args" message.

**Files**

- `src/runtime/app/update/stream.cpp:415-440` (`salvage_args`)
- `src/runtime/app/update/stream.cpp:911-975` (`StreamToolUseEnd` parse-or-salvage)

```cpp
json salvage_args(const ToolUse& tc) {
    if (auto parsed = try_parse_partial(tc.args_streaming)) {
        if (!parsed->empty()) return *parsed;
    }
    // ... fallback per-key sniffers ...
}
```

`try_parse_partial` closes open brackets on `tc.args_streaming`, then
parses. **If the cutoff landed mid-string in a `content` /
`new_string` value, the parsed JSON contains a truncated string** —
syntactically valid (closing `"` was synthesised by the partial-close
heuristic), semantically a half-written file body.

`finalize_turn` then dispatches `run_tool(tc.id, tc.name, tc.args)`
with the salvaged truncated args.

**Severity.** File-corruption risk (the `write` tool writes the
truncated content). Not a rendering bug, but adjacent and arguably
worse — silent on-disk corruption with no surface signal beyond the
later "looks weird" tool result.

**Possible fix.** When `try_parse_partial` succeeds, validate that
string-typed required fields end at a real closing quote in
`args_streaming` (not at the partial-closer's synthesised one). If
not, fail the tool with a clear "args cut off mid-string" message.

---

## Finding 4 — Turn numbering off during compaction window [RESOLVED]

**Fix applied:** the compaction-finalize block in
`src/runtime/app/update/stream.cpp` now counts assistant turns in
the pre-compact prefix (`summarised_assistants`) before the rebuild,
then seeds `thread_view_start_turn` with
`summarised_assistants - preserved_assistants + prior_view_start_turn`.
The first preserved-tail Assistant therefore displays its
pre-compact absolute turn number rather than "turn 1", keeping the
user's mental turn count intact across the boundary.

**Files**

- `src/runtime/view/thread/conversation.cpp:30-48`

```cpp
std::size_t total = m.d.current.messages.size();
if (m.s.compacting
    && m.s.compact_pre_synth_count > 0
    && m.s.compact_pre_synth_count <= total) {
    total = m.s.compact_pre_synth_count;
}
const std::size_t start = static_cast<std::size_t>(
    std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
int turn = 1 + m.ui.thread_view_start_turn;
```

**Invariant violated.** `thread_view_start_turn` is the running count
of Assistant messages already virtualized into scrollback, maintained
by `maybe_virtualize`. It was computed against the un-clipped
`messages.size()`. When `compacting` clips `total` down to
`compact_pre_synth_count`, the loop renders fewer turns than the
running counter implies. The next user turn after compaction gets
numbered against a counter that includes turns the user no longer
sees.

**Symptom.** Visible only if the user mentally tracks "turn N" via
the meta strip. Numbering jumps by ≥1 across the compaction
transition.

**Severity.** Cosmetic. Not corruption of cells.

**Fix.** When clipping `total`, also clamp `turn` by walking the
visible range to recompute the running count, or store the clipped
turn baseline in the model.

---

## Finding 5 — Markdown-snap + maybe_virtualize commit on the same frame: zero-slack invariant

**Files**

- `src/runtime/app/cmd_factory.cpp:295-313` (post-tool sub-turn launch)
- `src/runtime/view/thread/turn/turn.cpp:44-83` (`cached_markdown_for` + `finish()`)
- `maya/src/render/serialize.cpp:17-52` (`commit_prefix`)
- `maya/include/maya/app/app.hpp:390-399` (`commit_inline_overflow`)

After a tool result returns and a new sub-turn begins:

```cpp
auto virt = detail::maybe_virtualize(m);
auto ctx = take_active_ctx(std::move(m.s.phase));
m.s.phase = phase::Streaming{std::move(ctx).value()};
Message placeholder;
placeholder.role = Role::Assistant;
m.d.current.messages.push_back(std::move(placeholder));
if (!virt.is_none()) cmds.push_back(std::move(virt));
cmds.push_back(launch_stream(m));
```

The same frame also sees the just-finalized assistant message
transition `is_turn_resolved → true`, which means
`cached_markdown_for` calls `finish()` on the StreamingMarkdown
widget. `finish()` commits any uncommitted tail bytes as a block,
which can change the rendered height by ±N rows (inline-tail render
vs. committed-block render of the same bytes have different
per-block layouts).

**Two height-mutating events on the same render frame**:

1. The canvas content_rows changes by the markdown-snap delta.
2. `commit_inline_overflow` is queued, which mutates `prev_rows` by
   the strictly-clamped `max(0, prev_rows - term_h)`.

**Current status: safe.** `commit_inline_overflow` uses the
*current* `state.prev_rows` (the state at the moment the Cmd runs,
between reducer and the next render), so it never over-commits. The
`compose_inline_frame` shrink path's bottom-row re-emit + `\x1b[J`
cleans up any residual rows.

**Why this is worth flagging.** The safety depends on
`commit_inline_overflow` clamping against `state.prev_rows` *at Cmd
time*, not the model's view of it. If a future change introduced a
height-mutating Cmd that ran *after* render (e.g. a "commit N rows
based on what view computed" path), the canvas-vs-prev_cells
correspondence would skew. The current invariant has zero slack.

**No code change needed today; documenting the load-bearing
ordering for future readers.**

---

## Finding 6 — Composer placeholder insert assumes cursor is codepoint-aligned

**Files**

- `src/runtime/app/update/composer.cpp:289-291, 567-571, 589-593, 651-654` (paste / chip insert paths)
- `src/runtime/view/helpers.cpp:163+` (`utf8_prev`, `chip_prev`, `chip_next`)

All chip-insert call sites do:

```cpp
m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
m.ui.composer.cursor += static_cast<int>(placeholder.size());
```

The cursor is a byte offset. Every cursor mover in the composer
(`ComposerCursorLeft/Right/WordLeft/WordRight`, backspace, history
restore) routes through `utf8_prev`/`chip_prev`/`chip_next` which
respect codepoint boundaries. As long as no code path lands the
cursor mid-codepoint, the placeholder insert is safe.

**Audit result.** All cursor mutations reviewed go through the
UTF-8-aware helpers or set the cursor to `text.size()` (codepoint-safe
end). No bug today.

**Worth flagging.** Any future direct `cs.cursor = N` mutation that
isn't routed through `utf8_prev`/`utf8_next`/`chip_prev`/`chip_next`
would split a multi-byte glyph. The renderer would emit U+FFFD; no
scrollback corruption (live frame only) but visible garbage.

---

## Finding 7 — Stream-stall watchdog rebases `last_event_at` on slow frames

**Files**

- `src/runtime/app/update/meta.cpp:200-228` (Tick handler)

```cpp
constexpr auto kTickRebaseThreshold = std::chrono::seconds(2);
if (auto* a = active_ctx(m.s.phase);
    a && tick_gap >= kTickRebaseThreshold
    && a->last_event_at.time_since_epoch().count() != 0) {
    a->last_event_at += tick_gap;
}
```

A render pass taking >2 s pushes `last_event_at` forward by the same
gap, regardless of whether the stream actually delivered events
during that gap.

**Symptom.** A genuinely stalled stream that happens to coincide
with a slow render frame gets its 120 s deadline silently extended.
Worst case: a series of slow frames during a real stall delays the
"stream stalled" error by O(sum of tick_gaps).

**Severity.** Not corruption. Reduces watchdog responsiveness on
loaded systems. Documenting because the rebase is conservative on
both sides (false positives prevented, true positives delayed) and
could surface as "agentty hung instead of giving me an error".

---

## Finding 8 — `pending_stream` smoothing pacer is not a corruption source

**Files**

- `src/runtime/app/update/meta.cpp:181-198` (Tick drip)
- `src/runtime/app/update/stream.cpp` StreamError / CancelStream arms

The error / cancel paths drain `pending_stream → streaming_text`
*before* moving `streaming_text → text`. No bytes are lost. The
visible lag (up to ~6 s for a 50 KB burst dripping at 256 B / tick =
~7.7 KB/s) collapses to zero on stream-end because `StreamMessageStop`
also drains. **No bug.**

What is worth noting: at stream end, the canvas content_rows can
jump by tens of rows in a single frame (the full drained tail
appears at once). This is a normal frame the diff handles, but it's
the largest single-frame height delta the renderer sees in steady
state — flagging because any change that makes the renderer assume
"per-frame height delta is small" would break it.

---

## Finding 9 — Subprocess fd hygiene is correct

**Files**

- `src/tool/util/subprocess.cpp:404-422` (POSIX `posix_spawn_file_actions`)
- `src/tool/util/subprocess.cpp:142-165` (Windows pipe + handle inherit)
- `src/provider/anthropic/transport.cpp:105+` (debug log file, not tty)

Children get stdin from `/dev/null`, stdout+stderr dup2'd to a pipe.
No child writes to the parent's fd 1. `AGENTTY_LOG=wire=trace` writes to a
file. No path was found by which a subprocess could corrupt the
terminal mid-frame.

**No bug.** Documenting the absence so future audits don't re-check.

---

## Finding 10 — Width-1 terminal silently no-ops the render

**Files**

- `maya/src/app/app.cpp:245-249`

```cpp
const int w = is_inline()
    ? std::max(1, size_.width.raw() - 1)
    : size_.width.raw();
if (w <= 0) return ok();
```

Inline mode reserves one column for cursor parking, so a 1-column
terminal yields `w = 0` and the render is skipped. `prev_cells` is
left untouched. A subsequent resize triggers Divergent → full clear
& repaint.

**Symptom.** None observable. Worth flagging only as a degenerate
case: in a 1-column terminal, agentty's frame never updates until
the user resizes back.

---

## Finding 11 — Single-threaded reducer; no data races on message state

The provider's stream worker dispatches `Msg` values through a
`BackgroundQueue` to the UI thread. All `m.d.current.messages`
mutations happen on the UI thread inside the reducer. No race.

**No bug.** Documenting to close the loop.

---

## Finding 12 — StreamingMarkdown async-parse adopt races with direct mutators [RESOLVED]

**Fix applied.** `set_content_async`'s worker writes `source_` /
`committed_` / `prefix_` on the foreground thread when its result
lands (`maybe_apply_async_`). The original adopt gate compared only
`async_latest_source_` against the worker's input — it didn't check
that the foreground's `source_` was still at that state. Any direct
`set_content` / `append` / `finish` call between worker spawn and
worker landing would NOT clear `async_latest_source_`, so the worker
result would adopt: source_ rewinds to the worker's input, committed_
snaps back, prefix_ is replaced — silently undoing the host's most
recent mutation.

Fix is three-pronged:

1. `set_content_async`'s sync-delegate branches (both the growth-from-
   prefix path and the below-threshold path) now clear `async_slot_`
   and `async_latest_source_` so any in-flight worker's result is
   discarded on arrival.
2. `finish()` mirrors `clear()` and drops the in-flight slot. Without
   this, finalize-during-async (would-be rare; possible if a long
   thread loads and `cached_markdown_for` calls `finish()` while a
   pending divergent-prefix parse is mid-flight) could see the worker
   land *after* finish committed everything and clobber the finished
   state.
3. `maybe_apply_async_` adds defense-in-depth: even if a future caller
   forgets to clear the sentinel, adoption requires `source_` to be
   byte-equal to `async_latest_source_`. Mismatch → discard.

**Files**

- `maya/src/widget/markdown/streaming.cpp` — `set_content_async`,
  `finish`, `maybe_apply_async_`.

**Reachability in agentty's host flow.** The async path is only
spawned when `source_` is non-empty AND the incoming content
diverges from `source_` as a prefix AND content size ≥ 16 KB. In
agentty's `cached_markdown_for` flow, settled-message bytes are
immutable and live-streaming bytes only ever grow, so the divergent
branch is essentially unreachable. The fix is still worth landing
because (a) the widget's public API contract is now coherent, and
(b) any future host operation that swaps a non-empty widget to a
large unrelated body (thread switch into a cache slot that survived
LRU eviction is the obvious candidate) would otherwise have hit
silent state corruption.

---

## Bottom line — confirmed corruption pathways

| # | Severity | Trigger | Mode | Status |
|---|----------|---------|------|--------|
| 1 | UI desync, easy reproduction | ToggleToolExpanded on resolved turn | live-frame ≠ model | **resolved** (cache already keyed on render_key) |
| 2 | Scrollback corruption, narrow trigger | StreamError mid-stream on multi-sub-turn long markdown body | immutable rows ≠ live frame at boundary | **resolved** (force_redraw batched on prepend path) |
| 3 | File corruption (adjacent to rendering) | Stream cuts off mid-string in `content`/`new_string` field | truncated file write | **resolved** (`ended_inside_string` gate in `salvage_args`) |
| 4 | Cosmetic | During compaction window | turn numbering off-by-N | **resolved** (seed `thread_view_start_turn` with folded count) |
| 5 | Currently safe, zero slack | maybe_virtualize + finish() coincide on same frame | (potential — flag for future readers) | unchanged |
| 12 | Latent widget-API hazard | async parse lands while host did direct set_content/append/finish | source_ / prefix_ rewind to stale snapshot | **resolved** (slot drop on sync delegate + finish; source_ verify in adopt gate) |

The renderer itself (the `compose_inline_frame` diff path,
`commit_prefix` shift math, the `(prev_width, prev_rows)` case
disambiguation, the `try_drain_residue` backpressure guard, the
full-canvas-clear-every-frame discipline) is genuinely careful. The
interesting failures are at the layer above: state mutations that
don't invalidate caches (1), parallel height-changing events that
the renderer reconciles only because the invariants happen to
compose (2, 5), and partial-JSON salvage running tools with bodies
that look syntactically valid but are semantically truncated (3).

## Priority fix order

1. **Finding 1 (ToggleToolExpanded cache invalidation)**. Cheapest,
   most user-visible. Either fold `tc.expanded` into the cache key
   check at `turn.cpp:354-369`, or wire `compute_render_key()` (it
   already exists, just unused) into the cache predicate.
2. **Finding 2 (StreamingMarkdown clear-on-prepend)**. Either make
   `set_content` detect the prepend-only case and rebuild without
   `clear()` mid-frame, or force a Divergent transition on the
   StreamError → retry path so the height delta surfaces as a full
   repaint instead of a silent scrollback-vs-viewport mismatch.
3. **Finding 3 (salvage_args partial parse)**. Validate that
   string-typed required fields end at a real closing quote in the
   wire bytes before dispatching the tool.
4. **Finding 4 (turn numbering during compaction)**. One-line clamp
   when `compact_pre_synth_count` clips `total`.
