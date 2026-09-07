#pragma once
// agentty::Msg — every event the runtime can process, as a closed grouped variant.
//
// History:
//   v1: 79 leaf alternatives in one std::variant. sizeof(Msg) = max over all 79
//       leaves; one std::visit call instantiated a 79-arm dispatch table; one
//       update.cpp TU compiled all 79 arms in a single overload{}. Tolerable at
//       first; began to wobble with scale — compile time of update.cpp climbed
//       past 15 s, sizeof(Msg) was pinned by the heaviest alternative no matter
//       which path was active, and any leaf change forced a full recompile of
//       the dispatch site.
//
//   v2 (this file): leaves grouped into 10 domain sub-variants. The top-level
//       Msg is a `std::variant` of those domains. The flat construction syntax
//       still works:
//
//           Msg m1 = ComposerEnter{};       // -> Msg{ComposerMsg{ComposerEnter{}}}
//           dispatch(StreamTextDelta{"x"}); // -> dispatch(Msg{StreamMsg{...}})
//
//       std::variant's converting constructor walks each alternative; only the
//       matching domain accepts a given leaf, so the wrap is unambiguous and
//       implicit. Call sites don't change.
//
//       Per-domain reducers live in update/<domain>.cpp; update.cpp's top-level
//       std::visit is now a 10-arm dispatcher that forwards into them. Each
//       domain's TU compiles independently — touching a composer leaf no
//       longer forces stream/login/diff to recompile.

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/runtime/panel/fork.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/runtime/panel/settings/categories.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/tool/registry.hpp"

namespace agentty {

// ============================================================================
// Leaf types — the actual events. Grouped by domain in source order; the
// runtime classification is enforced by the domain variants further down.
// ============================================================================

// ── Composer ─────────────────────────────────────────────────────────────
struct ComposerCharInput { char32_t ch; };
struct ComposerBackspace {};
struct ComposerEnter {};
struct ComposerNewline {};
struct ComposerSubmit {};
struct ComposerToggleExpand {};
// ^B — arm/disarm LOOP mode: re-send the armed message automatically after
// every completed turn until toggled off. Arming snapshots the composer's
// current text+attachments (see ComposerState::loop_text) and submits it
// immediately, so ^B reads as "send this, and keep sending it".
struct ComposerToggleLoop {};
struct ComposerCursorLeft {};
struct ComposerCursorRight {};
struct ComposerCursorHome {};
struct ComposerCursorEnd {};
struct ComposerPaste { std::string text; };
// Fired ~1.2s after an escape-based clipboard read (OSC 5522 / OSC 52) was
// emitted. If no paste reply arrived by then, the reducer surfaces an
// ACTIONABLE diagnosis (which terminal, tmux, mosh, the exact env-var fix)
// instead of the old behaviour — the "reading clipboard…" toast silently
// lapsing and teaching the user nothing. `seq` matches the query that armed
// it so a stale timeout never fires after a successful paste.
struct ClipboardQueryTimeout { std::uint64_t seq = 0; };
// Recall queued messages back into the composer for editing. Bound to
// Up-arrow when the composer is empty and queued messages exist.
// Mirrors Claude Code's `Lc_` (binary offset 76303220): drains every
// editable queued item into the composer in queue order joined by
// "\n", with the cursor landing at the boundary between recalled text
// and any pre-existing composer text. Destructive on the queue — the
// items only exist in the composer buffer afterwards, so the user
// must resubmit to re-queue. If the user clears the composer after
// recall, the items are gone (same as Claude Code's behaviour).
struct ComposerRecallQueued {};

// Per-item queue editor. The bigger sibling of ComposerRecallQueued:
// rather than drain-all, these let the user cycle THROUGH the queued
// items, load one at a time into the composer, edit it, and submit
// (which re-queues at the tail — the existing flow). Bound to Alt+↑ /
// Alt+↓ / Alt+Backspace.
//
//   Alt+↑  ComposerQueuePeekPrev  — load the previous queued item into
//                                    the composer for editing. On the
//                                    first press, the live draft is
//                                    snapshotted into composer.draft_save
//                                    so the round-trip back is non-
//                                    destructive (same idea as history
//                                    walk). The item stays in the queue
//                                    while being peeked; submitting
//                                    removes it from the queue and re-
//                                    queues the edited version at the
//                                    tail.
//   Alt+↓  ComposerQueuePeekNext  — load the next queued item; at the
//                                    end of the queue, restore the
//                                    live draft and exit peek mode.
//   Alt+Backspace (on empty composer with no peek active)
//          ComposerQueuePopLast   — pop the most recently queued item
//                                    off the tail entirely. Undo for an
//                                    accidental queue submit.
struct ComposerQueuePeekPrev {};
struct ComposerQueuePeekNext {};
struct ComposerQueuePopLast {};

// Word-wise cursor jumps (Ctrl+Left / Ctrl+Right). Word boundaries
// are whitespace runs; chip placeholders count as a single word.
struct ComposerCursorWordLeft {};
struct ComposerCursorWordRight {};
// Kill-line family (Ctrl+K / Ctrl+U). Kill-to-end deletes from the
// cursor to the next '\n' (or end-of-buffer); kill-to-beginning
// deletes from the previous '\n' (or start-of-buffer) to the cursor.
struct ComposerKillToEndOfLine {};
struct ComposerKillToBeginningOfLine {};
// Word-wise delete (Ctrl+W / Alt+D). Back deletes from the previous
// word boundary to the cursor (readline `unix-word-rubout`); forward
// deletes from the cursor to the next word boundary (`kill-word`).
// Both reuse the same chip-aware word_left/word_right boundaries the
// cursor jumps use, so a delete-word over a chip removes the whole
// attachment token in one stroke.
struct ComposerDeleteWordBack {};
struct ComposerDeleteWordForward {};
// Undo / redo (Ctrl+Z / Ctrl+Y). Each mutating composer op snapshots
// the prior state into a per-composer stack; new edits clear redo.
struct ComposerUndo {};
struct ComposerRedo {};
// History walking — ↑/↓ over previous user messages in the active
// thread. Prev steps further into the past; Next walks back toward
// the live draft (which was snapshotted on the first Prev).
struct ComposerHistoryPrev {};
struct ComposerHistoryNext {};
// Explicit "paste image from system clipboard" (Ctrl+V). Bracketed
// paste (Ctrl+Shift+V) only carries UTF-8 text — for an image-on-
// clipboard path the reducer shells out to wl-paste / xclip /
// pngpaste / PowerShell to capture the raw PNG bytes. See
// io/clipboard.{hpp,cpp}.
struct ComposerImagePasteFromClipboard {};

// ── Streaming from provider ──────────────────────────────────────────────
struct StreamStarted {};
struct StreamTextDelta { std::string text; };
// Emitted when the wire closes a TEXT content block (Anthropic
// content_block_stop for a non-tool block). Always precedes the
// tool_use content_block_start, so it is the earliest authoritative
// "model finished typing prose" signal — the reducer flips the
// in-flight message's text_block_closed so the view drains the reveal
// cursor to the edge before any tool card is pushed (kills the reveal
// burst at the text→tool seam). No-op if no text block was open.
struct StreamTextBlockClosed {};
struct StreamToolUseStart { ToolCallId id; ToolName name; };
// Append-only wire fragment, always addressed by the stable tool-call id.
// Anthropic, Codex Responses, and OpenAI Chat all expose an id when the call
// opens; carrying it on every update makes overlapping/parallel streams safe
// and removes the reducer's provider-ordering fallback.
struct StreamToolUseDelta { ToolCallId id; std::string partial_json; };
// Full current argument snapshot. ACP tool_call_update.rawInput has replace
// semantics (it is not a textual delta), so keeping this distinct prevents
// repeated snapshots from producing concatenated, invalid JSON.
struct StreamToolUseSnapshot { ToolCallId id; std::string json; };
// Completion is also explicitly addressed. An empty/default id is not a valid
// wire event: adapters must retain the id they received at call start.
struct StreamToolUseEnd { ToolCallId id; };
// Terminal result for a tool executed by a delegated agent (external ACP).
// It follows StreamToolUseEnd so the shared argument assembler still parses
// the final input, then marks the card terminal without scheduling the host's
// tool executor a second time.
struct StreamObservedToolResult {
    ToolCallId id;
    bool failed = false;
    std::string output;
};
// A chunk of the assistant's thinking block (adaptive thinking). `text` is
// the visible reasoning delta (empty under display:omitted); `signature` is
// the opaque per-block signature that arrives once, near the block's end.
// The reducer accumulates both onto the in-flight assistant Message so the
// block can be replayed verbatim on the next turn (Anthropic requires it
// when the turn also carries tool_use). Doubles as a liveness heartbeat —
// the handler bumps last_event_at like StreamHeartbeat does.
//
// `block_boundary` marks the START of a NEW reasoning block/paragraph:
// Anthropic emits it on each thinking content_block_start (interleaved
// thinking produces several signed blocks per response — merging them
// corrupts the signature replay), and Codex/Responses emits it on each new
// reasoning summary part / reasoning item (paragraph boundaries that would
// otherwise concatenate into run-together prose). The reducer seals the
// previous block and inserts a display separator.
struct StreamThinkingDelta {
    std::string text;
    std::string signature;
    bool block_boundary = false;
    // Non-empty = a complete REDACTED thinking block (Anthropic safety
    // encryption; opaque). The reducer stores it as its own ThinkingBlock so
    // the wire replays it verbatim — required before tool_use. Not rendered.
    std::string redacted_data;
};
// Codex/Responses reasoning-item capture. Unlike StreamThinkingDelta (which
// carries visible summary text for the thinking block), this carries the
// OPAQUE `encrypted_content` blob from a completed reasoning output item.
// The reducer stashes it on the in-flight assistant Message so the follow-up
// turn can replay it in `input[]` and preserve chain-of-thought across tool
// rounds under store:false. Never rendered; wire-only.
struct StreamReasoning { std::string encrypted; };
// Mirrors Anthropic's message.usage shape. cache_* fields are non-zero only
// when the request hit a cache_control breakpoint. Fields default to 0 so
// callers that only care about input/output keep working.
struct StreamUsage {
    int input_tokens               = 0;
    int output_tokens              = 0;
    int cache_creation_input_tokens = 0;
    int cache_read_input_tokens    = 0;
    // Reasoning/thinking tokens billed WITHIN output_tokens (OpenAI counts
    // them under completion_tokens_details.reasoning_tokens /
    // output_tokens_details.reasoning_tokens; Anthropic doesn't break them
    // out). Purely informational — do NOT add to output_tokens for cost/
    // context math, they are already included there. 0 when the wire omits
    // the detail (non-reasoning model, or a provider that doesn't report it).
    int reasoning_output_tokens    = 0;
};
// Why the stream ended. Maps Anthropic's wire string
// (`message_delta.delta.stop_reason`: "end_turn" | "tool_use" |
// "max_tokens" | "stop_sequence" | absent) into a closed enum so the
// reducer can `switch` on it without string compare. Anything the wire
// doesn't recognise (forward-compat: a future Anthropic stop reason)
// becomes `Unspecified` — handled identically to a missing field, which
// means "treat as a clean stream end."
enum class StopReason : std::uint8_t {
    EndTurn,        // model finished naturally
    ToolUse,        // model wants tool results before continuing
    MaxTokens,      // hit the output token cap mid-stream
    StopSequence,   // matched a configured stop_sequence
    Unspecified,    // wire absent, empty, or unknown
};

[[nodiscard]] constexpr std::string_view to_string(StopReason r) noexcept {
    switch (r) {
        case StopReason::EndTurn:      return "end_turn";
        case StopReason::ToolUse:      return "tool_use";
        case StopReason::MaxTokens:    return "max_tokens";
        case StopReason::StopSequence: return "stop_sequence";
        case StopReason::Unspecified:  return "";
    }
    return "";
}

// Inverse: parse a wire string into the typed enum. Used at the
// dynamism boundary in `provider/anthropic/transport.cpp`. Unrecognised
// values become `Unspecified` so a future Anthropic addition doesn't
// crash the reducer.
[[nodiscard]] constexpr StopReason parse_stop_reason(std::string_view s) noexcept {
    if (s == "end_turn")      return StopReason::EndTurn;
    if (s == "tool_use")      return StopReason::ToolUse;
    if (s == "max_tokens")    return StopReason::MaxTokens;
    if (s == "stop_sequence") return StopReason::StopSequence;
    return StopReason::Unspecified;
}

struct StreamFinished { StopReason stop_reason = StopReason::Unspecified; };

// Non-fatal provider advisory, surfaced as a transient status toast while
// the stream keeps running. The first honest use: Copilot's Auto session
// SUBSTITUTES a concrete model server-side — the user picked X, the server
// streams Y. Silently accepting that is exactly the "model switching is not
// working" experience; a toast ("copilot auto → gpt-4o") makes the
// substitution visible without interrupting the turn.
struct StreamNotice { std::string text; };

// Stream-level failure. `message` is human-readable (used for both the
// status banner and `provider::classify(string)` fallback). `retry_after`
// is the server's Retry-After hint when present — Anthropic sets it on
// 429 (rate_limit_error) and 529 (overloaded_error); the runtime prefers
// this over its hardcoded backoff schedule because the server knows
// better than we do how long the brown-out will last (see Zed's
// `parse_retry_after`, anthropic.rs:574-580). std::chrono::seconds
// because Anthropic always emits whole seconds; clamped at the use site
// so a buggy proxy can't pin us for an hour.
struct StreamError {
    std::string message;
    std::optional<std::chrono::seconds> retry_after;
    // The observed HTTP response status when the failure was an HTTP-status
    // error (0 when it wasn't one — a transport/socket failure, a synthetic
    // stall, or an SSE `event: error` body where no status is available). The
    // transport already KNOWS this precisely; carrying it lets the retry
    // reducer classify via the typed, compile-time-proven
    // provider::classify(HttpError) path instead of substring-sniffing the
    // human `message`. Non-zero ⇒ classify by status; zero ⇒ fall back to the
    // string sniff. This is how the StreamResult's precision reaches the
    // reducer without changing the Msg-driven runtime seam.
    int http_status = 0;
    // The server accepted a non-idempotent request before transport failure;
    // retrying could duplicate model work/cost.
    bool non_replayable = false;
    // True only for the SYNTHETIC StreamError the stall watchdog dispatches
    // after it trips the cancel token (see meta.cpp Tick). The handler must
    // treat a `from_stall` error as a recoverable upstream stall (reclassify
    // a resulting "cancelled" worker unwind as Transient) — but it must NOT
    // infer that intent from the volatile `in_stall_fired()` phase state,
    // because a later turn can re-enter StallFired and a genuinely
    // user-cancelled error coinciding in that window would be wrongly
    // reclassified. Carrying the intent ON the message ties it to the
    // specific stall that produced it, immune to phase churn from other
    // turns/retries that ran in between.
    bool from_stall = false;
};
// Wire-alive heartbeat. Model/SSE heartbeats reset transient retry history;
// transport-only activity (for example HTTP/2 PING ACK bytes) merely prevents
// the 120-second UI watchdog from misclassifying a buffering corporate
// gateway as dead.
struct StreamHeartbeat {
    bool transport_only = false;
};
// A live intermediary is withholding response DATA. Unlike a generic
// heartbeat this is user-visible: the reducer keeps the bounded request alive
// and explains why output may arrive in one burst instead of incrementally.
struct StreamBufferedWait {};
// User-driven cancel of the in-flight stream (Esc while streaming). The
// reducer trips the StreamState cancel token; the http layer notices within
// ~200 ms and the worker thread eventually emits a StreamError("cancelled").
struct CancelStream {};
// Scheduled re-launch of the in-flight stream after a transient-error
// backoff (Overloaded / 429 / 5xx / network blip). The reducer issues
// `Cmd::after(delay, RetryStream{})` from the StreamError handler;
// when this Msg fires, the stream is re-launched on the same context.
// The user can intercept with Esc → CancelStream during the wait.
struct RetryStream {};

// ── Tool execution (local) ───────────────────────────────────────────────
// Tool finished executing. `result` is `expected<output_text, ToolError>`
// — the success/failure distinction is the type, not a parallel `bool error`
// flag. Reducer dispatches via `std::visit` (or the `if (e.result)` short
// form for the common case); the typed `ToolError::kind` flows all the way
// to the view, where it could drive different rendering per category.
struct ToolExecOutput {
    ToolCallId id;
    std::expected<std::string, tools::ToolError> result;
    // Structured filesystem mutation a file-touching tool (edit / write /
    // apply_patch / replace) returns alongside its text; std::nullopt for
    // every other tool. The reducer appends it to m.d.pending_changes so the
    // diff-review pane (Ctrl+R) can walk / accept / reject the hunks before
    // they're kept. Decoded from the tool's FileChange meta in the mcp bridge
    // (decode_result), which rebuilds structured hunks via diff::compute.
    std::optional<FileChange> change;
    // Multi-file edits (replace) — one entry per written file. When non-empty
    // it supersedes `change` for queuing; `change` remains its first element
    // for single-file compatibility.
    std::vector<FileChange> changes;
    // Images a tool surfaced for a vision model (read on an image file). The
    // reducer stores them on ToolUse::Done so the wire renders them as image
    // blocks in this call's tool_result. Empty for text-only tools.
    std::vector<ImageContent> images;
};
// Contains the FULL accumulated output, not a delta — the update handler can
// assign unconditionally without maintaining append state. Coalesced at the
// subprocess boundary (~100 ms) so a chatty command doesn't flood the event
// queue with micro-updates.
struct ToolExecProgress { ToolCallId id; std::string snapshot; };
// Wall-clock watchdog for tool execution. Scheduled by kick_pending_tools
// via Cmd::after when a non-subprocess tool transitions to Running. If the
// tool has reached a terminal state by the time the check fires, this is
// a no-op; otherwise the tool is force-failed so the UI doesn't sit on a
// hung filesystem call / blocked syscall forever. The worker thread that
// owns the tool may keep running — its eventual ToolExecOutput is silently
// discarded by apply_tool_output's idempotent guard.
struct ToolTimeoutCheck { ToolCallId id; };

// Permission-prompt resolution from the user. Tied to ToolMsg because a
// permission prompt is always about a specific pending tool call and the
// resolution feeds straight back into the tool state machine.
struct PermissionApprove {};
struct PermissionReject {};
struct PermissionApproveAlways {};

// ── Model catalog ────────────────────────────────────────────────────────
// Result of a background model-catalog fetch. `provider_id` is the canonical
// id of the provider the fetch was FOR (captured when the fetch launched):
// the reducer drops the payload if the active provider has changed since —
// without it, an in-flight fetch for provider A landing after a switch to B
// installs A's catalog under B (picking from it then streams B with an
// A-model id: the "model changed, provider didn't" bug).
struct ModelsLoaded {
    std::vector<ModelInfo> models;
    std::string            provider_id;
    // Non-empty ⇒ the fetch FAILED and this is the human-readable reason.
    // Carried here (not as a StreamError) because StreamError feeds the
    // LIVE TURN's retry state machine: a catalog failure dispatched as
    // StreamError while a stream is active would pop the assistant message
    // receiving deltas, schedule RetryStream (racing a second worker into
    // the session), or latch the healthy turn terminal — all for an error
    // that has nothing to do with the stream. The reducer surfaces this as
    // a transient status toast instead.
    std::string            error;
};

// ── Fused cross-provider model picker ────────────────────────────────────
// The unified list spanning EVERY authenticated provider (docs/design/
// unified-model-picker.md). Each row is a concrete (provider, model) the
// user switches to atomically, or a "sign in to X" offer that routes to
// login. Opened with `^/` and the `/model` slash command.
//
// This is the ONE model surface. It also serves Smart Mode role→model
// assignment: when Model::ui.smart_assign_slot >= 0 the list is scoped to
// the active provider and Select writes the slot instead of switching.
struct OpenFusedPicker {};
// Deferred lazy refresh: after the ACTIVE provider's fast refetch is issued on
// open, this fires (via a short After delay) to background-refresh every OTHER
// authed provider's live catalog. Keeping them off the initial batch means the
// active provider's result isn't queued behind slower providers on the bounded
// worker pool — the selected models refresh first, the rest trickle in.
struct FusedRefreshOthers {};
// ^L — force a full live refresh: mark EVERY authed provider's catalog stale
// and refetch (active immediately, others deferred). The manual escape hatch
// when a catalog is stale/failed and the user doesn't want to wait for the TTL
// or reopen. Shows the loading hint while fetches are in flight.
struct FusedPickerRefresh {};
struct CloseFusedPicker {};
struct FusedPickerMove { int delta; };
struct FusedPickerJump { enum class Where { Home, End, PageUp, PageDown }; Where where; };
struct FusedPickerSelect {};        // atomic switch to the highlighted row
struct FusedPickerToggleFavorite {};
// ←/→ cycles the reasoning-effort tier of the highlighted model; ^E toggles
// thinking on/off. The fused picker is the COMPLETE "pick + tune your model"
// surface — there is no second picker.
struct FusedPickerCycleEffort { int delta; };
struct FusedPickerToggleReasoning {};
// Toggle whether the model's reasoning/thinking is SHOWN (^R). Flips the
// persisted Settings.show_reasoning / Model.show_reasoning: renders the
// reasoning block in the transcript for every provider AND asks Anthropic
// for visible thinking (interleaved-thinking beta). Distinct from
// FusedPickerToggleReasoning above, which flips a single model's effort
// CAPABILITY override.
struct FusedPickerToggleShowReasoning {};
// ^/ — scope the browse/filter list to ONLY the provider of the currently
// highlighted row (so you can drill into "just this provider's models").
// Pressing it again when already scoped clears the scope (back to all
// providers). A no-op when the highlighted row has no provider (offers).
struct FusedPickerScopeProvider {};
struct FusedPickerFilterInput { char32_t ch; };
struct FusedPickerFilterBackspace {};
// One authed provider's catalog resolved (async, one per provider on open).
// `provider_id` guards against a provider signed out mid-fetch; `ok=false`
// marks the group Failed. Merges into Model::d.provider_catalogs in place.
struct FusedCatalogLoaded {
    std::string            provider_id;
    std::vector<ModelInfo> models;
    bool                   ok = true;
};
// ^Tab MRU cycle: walk the recent (provider,model) ring to progressively
// older models (A → B → C → D → A), no overlay. Reuses the atomic-switch
// resolution but does NOT reorder the MRU, so repeated presses cycle the
// whole ring instead of toggling the last two.
struct SwitchToPreviousModel {};
// ── Provider picker ──────────────────────────────────────────────────────
// Mirrors the model picker. Selecting a provider live-switches the active
// backend (provider::select + a deps() seam swap), persists the choice, and
// kicks a fresh model fetch so the model list reflects the new backend.
struct OpenProviderPicker {};
struct CloseProviderPicker {};
struct ProviderPickerMove { int delta; };
struct ProviderPickerJump  { enum class Where { Home, End, PageUp, PageDown }; Where where; };
struct ProviderPickerSelect {};
// Live search-filter over the provider list (mirrors the model picker): a
// typed character appends to the query, Backspace trims it. The row list
// narrows to fuzzy/substring matches on the id + label + blurb.
struct ProviderPickerFilterInput { char32_t codepoint; };
struct ProviderPickerFilterBackspace {};
// ^D on a row — two-press delete (confirm_remove): removes a SAVED CUSTOM
// HOST from Settings entirely, OR signs out of a PRESET that has a saved key
// (clears the key; the built-in preset stays). Mirrors ThreadListDelete /
// AccountRemove. No-op on presets with no saved key, ACP, and the sentinel.
struct ProviderPickerDelete {};

// ── Thread list ──────────────────────────────────────────────────────────
struct OpenThreadList {};
struct CloseThreadList {};
struct ThreadListMove { int delta; };
// Absolute-jump nav for long thread histories. See FusedPickerJump.
struct ThreadListJump  { enum class Where { Home, End, PageUp, PageDown }; Where where; };
struct ThreadListSelect {};
// `d` / `D` in the thread picker — two-press delete with confirm_remove.
// Mirrors the established SettingsListRemove / AccountRemove pattern:
// first press marks the focused thread as pending-delete (⚠ badge),
// second press on the SAME row commits via persistence::delete_thread().
// Any move/select/new/close disarms the pending state.
struct ThreadListDelete {};
// Quick-cycle: switch to the adjacent thread (by recency order, the
// same order the ^J picker shows) WITHOUT opening the picker. delta is
// applied to the thread-list index: +1 = older, -1 = newer; wraps at
// both ends. Bound to Alt+←/→ globally, and to Ctrl+←/→ when the
// composer is empty and no agent turn is active (otherwise Ctrl+arrows
// stay jump-by-word). The reducer surfaces a "thread k/N · title"
// toast so the user always knows where they landed.
struct ThreadCycle { int delta; };
struct NewThread {};
// Result of the background thread-history load kicked off from
// `AgenttyApp::init()`. The on-disk thread JSON walk used to run
// synchronously on startup; with hundreds of multi-MB files (real-world
// usage) it was the dominant startup cost (~1.7 s for 643 threads at
// 376 MB total). Now `init()` returns immediately with an empty
// `m.d.threads` and a `Cmd::task` that does the directory walk +
// JSON parse off the UI thread; this Msg lands when it's done.
struct ThreadsLoaded    { std::vector<Thread> threads; };
// Result of a background single-thread load kicked off when the user
// hits Enter on a row in the thread picker. The synchronous load was
// ~30ms of JSON parse + ~3ms of rehydrate on real threads (786+ msgs)
// — small individually, but applied directly between the keypress and
// the next paint it shows up as visible "click and wait". The picker
// now closes immediately, the old thread stays on screen, and this
// Msg lands when the new thread's bytes are parsed; the reducer then
// swaps `m.d.current`, rehydrates the frozen prefix, and commits the
// scrollback-overflow seam in one go.
struct ThreadLoaded     { Thread thread; };

// ── Command palette ──────────────────────────────────────────────────────
struct OpenCommandPalette {};
struct CloseCommandPalette {};
struct CommandPaletteInput { char32_t ch; };
struct CommandPaletteBackspace {};
struct CommandPaletteMove { int delta; };
struct CommandPaletteSelect {};

// ── @file mention picker ────────────────────────────────────────────────
struct OpenMentionPalette {};
struct CloseMentionPalette {};
struct MentionPaletteInput { char32_t ch; };
struct MentionPaletteBackspace {};
struct MentionPaletteMove { int delta; };
struct MentionPaletteSelect {};

// ── #symbol picker (parallel to @file) ────────────────────────────────────
struct OpenSymbolPalette {};
struct CloseSymbolPalette {};
struct SymbolPaletteInput { char32_t ch; };
struct SymbolPaletteBackspace {};
struct SymbolPaletteMove { int delta; };
struct SymbolPaletteSelect {};

// ── Code-block picker (Ctrl+G — run AI-suggested commands) ──────────
// Open scans the newest assistant reply for fenced ``` blocks; the modal
// lists them and the user picks an action per block:
//   Select (Enter / digit) → run the block through /bin/sh, async — the
//     command + output land in the thread so the model sees the result
//     next turn. `index` ≥ 0 targets a specific row (digit shortcut);
//     -1 means "the cursor row".
//   Edit → stage the cleaned body into the composer (tweak, then Enter
//     submits as a normal message — or the user deletes it, whatever).
//   Copy → cleaned body to the system clipboard.
// RunFinished is the run completion carrying the captured output; it
// opens the RESULT card (exit code + tail preview) where the user
// decides: a = attach to composer as an Output chip, y = copy clean,
// Esc = discard (the live transcript already sits in scrollback).
struct OpenCodeBlockPicker {};
struct CloseCodeBlockPicker {};
struct CodeBlockPickerMove { int delta; };
// Select a code block. `index` absent = act on the row under the cursor
// (Enter); engaged = a direct 1-9 number key naming a row. It was
// `int = -1`, so "no override" and "row -1" were the same value and every
// reader had to remember which one it meant.
struct CodeBlockPickerSelect { std::optional<int> index; };
struct CodeBlockPickerEdit {};
struct CodeBlockPickerCopy {};
struct CodeBlockRunFinished {
    std::string command;
    std::string output;
    int         exit_code = 0;
    bool        timed_out = false;
};
struct CodeBlockResultAttach {};
struct CodeBlockResultCopy {};
struct CodeBlockResultDiscard {};

// ── Tool-output viewer ───────────────────────────────────────────────────────
// Ctrl+O scrollable overlay for reading the FULL stored output of any
// settled tool call — the timeline cards elide long bodies (head/tail
// windows) and expanding them in place would rewrite committed scrollback
// rows, so inspection happens in an overlay that paints strictly over the
// live viewport. Open snapshots the entries; Move drives the list cursor
// (or the body scroll in the viewing stage); Select enters the body
// stage; Close = Esc (body → back to list, list → closed); Copy = y.
struct OpenToolOutputViewer {};
struct CloseToolOutputViewer {};
struct ToolViewerMove { int delta; };
struct ToolViewerSelect {};
struct ToolViewerCopy {};
// ←/→: in the body stage, jump straight to the previous/next entry's
// output without bouncing through the list; in the list stage ←/→ are
// unbound (the viewer swallows all keys while open, so they simply no-op).
struct ToolViewerStep { int delta; };

// ── Todo modal ───────────────────────────────────────────────────────────
struct OpenTodoModal {};
struct CloseTodoModal {};
struct UpdateTodos { std::vector<TodoItem> items; };

// ── Smart Mode overlay ───────────────────────────────────────────────────
// A dedicated config overlay for role-based routing (docs/design/smart-mode.md).
// A picker-style modal: row 0 is the master Enabled toggle, rows 1-3 are the
// Strategic / Implementation / Utility slots. Enter on row 0 flips enabled;
// Enter on a slot row opens the model picker in slot-assign mode. All changes
// persist to settings.json.
//
// ONE key message, like the Retrieval pane: navigation, the master toggle and
// the picker hand-off are all resolved by the shared form layer, so this pane
// cannot drift from that one.
struct OpenSmartMode {};
struct CloseSmartMode {};
struct SmartModeKey { form::keys::Action action; };
// Reveal/hide the advanced routing-policy rows (^A). View state on the
// overlay; the pane rebuilds. Not persisted.
struct SmartModeAdvanced {};
struct SmartModeClearSlot {};     // 'x' on a slot row: reset it to auto

// ── In-app login modal ───────────────────────────────────────────────────
// Shown when the user starts agentty with no valid credentials, OR
// triggered explicitly in-app to sign in or add an account.
// Same state-machine flavor as the other modals: closed → picking →
// {oauth_code | api_key_input} → done. The async OAuth exchange runs
// on a worker thread (Cmd::task) and reports back via LoginExchanged.
struct OpenLogin {};
struct CloseLogin {};
// Esc in a login sub-modal: pop ONE level of the flow (key prompt → host
// input → provider picker → closed) instead of collapsing everything.
// The reducer reads the sub-state's `back` origin field.
struct LoginBack {};
// Async custom-host probe result (worker → reducer). `ok` means a model
// list answered somewhere; `models_path`/`native_api` carry the DETECTED
// dialect so the commit adopts what the server actually speaks. On failure
// `error` is the human reason ("nothing listening", "HTTP 401", …).
// attempt_id matches HostProbing so a stale result is dropped.
struct HostProbed {
    std::uint64_t attempt_id = 0;
    std::string   spec;
    bool          ok = false;
    std::string   models_path;   // the path that answered
    bool          native_api = false;
    int           model_count = 0;
    long          latency_ms = 0;
    std::string   error;
};
// Sign out of the ACTIVE provider: clear its on-disk credentials (Anthropic
// credentials.json, or the Codex/ChatGPT token store), zero the live auth
// header via update_auth, and re-open the sign-in modal so the user lands
// exactly where they'd start a fresh login. Triggered from the command
// palette ("Sign out"). Lets a user switch OAuth ↔ API key / accounts without
// dropping to the CLI `agentty logout`.
struct SignOut {};
// Open the in-app account switcher for the ACTIVE provider. Entering the
// active Anthropic or ChatGPT row in the provider picker opens this list, so
// provider and account selection stay in one provider-centric flow. It lists
// every saved account so the user can switch who they're signed in as without
// leaving agentty. On first open with only a legacy single login, that login is
// auto-registered as "default" so it shows up as a switchable row.
// Open the account manager. Empty `provider` = the ACTIVE provider (the
// classic drill-down); a non-empty id opens accounts for THAT provider without
// switching to it first — so Enter on any provider row shows its accounts, not
// the model picker.
struct OpenAccounts { std::string provider; };
// Move the highlight in the account switcher (wraps at both ends; the last
// row is always "+ Add another account…").
struct AccountMove { int delta; };
// Activate the highlighted account switcher row. On a saved account: swap its
// stored credential into the live store + re-install the auth header. On the
// trailing add-new row: drop into the normal Picking flow, tagged so the
// resulting login is captured under a new name.
struct AccountSelect {};
// Remove the highlighted saved account (its stored credential snapshot). If it
// was the active one, the newest remaining account for the provider becomes
// active; if it was the last, the switcher falls back to Picking.
struct AccountRemove {};
struct LoginPickMethod  { char32_t key; };          // '1' = ApiKey, '2' = OAuth, '3' = ChatGPT
struct LoginCharInput   { char32_t ch; };
struct LoginBackspace   {};
struct LoginPaste       { std::string text; };
struct LoginCursorLeft  {};
struct LoginCursorRight {};
struct LoginSubmit      {};
// User pressed the "copy URL to clipboard" key while the OAuthCode
// modal is up. The reducer issues a Cmd<Msg>::write_clipboard with
// the active authorize URL, then surfaces a brief status toast so
// the user has visual confirmation the keystroke registered.
struct LoginCopyAuthUrl {};
// User pressed the copy-CODE key in a device-flow modal: copies the one-time
// user_code (the thing typed into the browser) rather than the URL. Distinct
// from LoginCopyAuthUrl so device modals can offer BOTH shortcuts.
struct LoginCopyCode {};
// User pressed the "open browser again" key. Re-issues the same
// xdg-open / `open` invocation that fired when OAuth was first
// selected, in case the original launch was missed (alt-tabbed away
// before the browser surfaced, or the OS swallowed the first open
// silently). Idempotent — reuses the URL already in OAuthCode state.
struct LoginOpenBrowserAgain {};
// Result of the async OAuth code-exchange. Carries the typed
// `auth::TokenResult` so the reducer can distinguish ApiError /
// Network / MissingToken without parsing strings.
struct LoginExchanged   { agentty::auth::TokenResult result; };
// The SSH/device flow dispatches this as soon as OpenAI allocates the
// one-time code, while the same worker continues polling for approval.
struct CodexDeviceCodeReady {
    std::uint64_t attempt_id = 0;
    std::string verification_url;
    std::string user_code;
};

// Final result of the native ChatGPT OAuth login. The flow is either the
// local loopback callback or SSH-friendly device authorization; both return
// the same credential shape. The reducer persists it only after matching the
// active attempt id.
struct CodexLoginDone {
    std::uint64_t attempt_id = 0;
    std::expected<agentty::provider::chatgpt::CodexCredentials,
                  agentty::auth::OAuthError> result;
};

// Native OAuth **device-flow** login (GitHub Copilot, Kimi, or any future
// device-flow provider) — the sibling of the Codex pair, but PROVIDER-GENERIC.
// The worker dispatches DeviceCodeReady as soon as the provider issues the
// one-time code (so the modal can show it) and DeviceLoginDone when the user
// approves (or the flow fails/times out). `provider` is the canonical registry
// id ("copilot", "kimi"). The worker persists the token itself before
// dispatching DeviceLoginDone, so the reducer only needs success-or-error —
// hence `error` (nullopt = success) rather than a provider-specific token type.
struct DeviceCodeReady {
    std::string   provider;
    std::uint64_t attempt_id = 0;
    std::string   verification_url;   // BARE url (shown in the panel, has a code field)
    std::string   browser_url;         // pre-filled url (auto-opened; copied by `u`)
    std::string   user_code;
};
struct DeviceLoginDone {
    std::string                 provider;
    std::string                 provider_label;   // "GitHub Copilot" | "Kimi"
    std::uint64_t               attempt_id = 0;
    std::optional<std::string>  error;             // nullopt = signed in OK
};
// Result of the background OAuth refresh kicked off from init() when
// `auth::resolve()` returned an expired token paired with a refresh
// token. Same TokenResult shape as LoginExchanged, handled in
// update/login.cpp::token_refreshed: success installs the new creds via
// `update_auth` + saves to disk + drains any queued composer text;
// failure surfaces an `error: token refresh failed: ...` toast and
// leaves the queue intact so the user can retry through the in-app
// login modal.
struct TokenRefreshed   { agentty::auth::TokenResult result; };

// Proactive pre-turn retrieval landed off-thread. Proactive RAG runs on the
// submit path under a small wall-clock hedge so Enter never freezes (see
// proactive_retrieve). When the funnel overruns the hedge on a large/slow
// corpus, the worker keeps running detached and dispatches THIS when the
// grounding is ready. `block` is the fenced <retrieved-context> text; empty
// means the late retrieval cleared no confidence bar (nothing to inject).
// The reducer STAGES it (m.d.staged_proactive_context) so the next user
// submit flushes it into the transcript at a safe boundary — a slow first
// turn's grounding is deferred by one turn instead of being dropped.
struct ProactiveContextReady { std::string block; double confidence = -1.0; };

// ── RAG mode picker ────────────────────────────────────────────────
// One decision: how proactive (pre-turn) retrieval behaves — On / First turn
// only / Off. Selecting commits the mode (persist + live-apply) and closes.
struct OpenRagSettings {};
struct CloseRagSettings {};
struct RagSettingsMove   { int delta; };   // move the row cursor
struct RagSettingsAdjust {};               // select the highlighted mode
struct RagSettingsReset  {};               // back to default (On)
// Reveal/hide the Tier::Advanced rows (^A). The pane rebuilds its form; the
// flag is view state, not config, so it is not persisted.
struct RagSettingsAdvanced {};

// ── Embeddings sub-form (RAG picker → Embeddings) ──────────────────
// Which embedder retrieval uses, configured entirely in the TUI.
//
// There is ONE key message, not fifteen. Navigation, editing and the dropdown
// are owned by the shared form layer (runtime/panel/form_keys.hpp), which turns a
// KeyEvent into a pane-agnostic intent; the reducer applies it and handles
// only what is genuinely embeddings-specific. A pane that spelled out
// Char/Backspace/CursorLeft/… messages would be re-deriving that key map, and
// two panes doing so is how they drift apart.
struct RagEmbedOpen  {};                   // enter the embeddings pane
struct RagEmbedClose {};                   // leave one level (menu/field/pane)
struct RagEmbedKey   { form::keys::Action action; };
struct RagEmbedTest  {};                   // run the probe (worker thread)
// Probe result. Carries the MEASURED dimension — the only trustworthy source
// for it, since a wrong dim makes rag-cpp's HNSW silently drop every vector.
struct RagEmbedTestDone {
    bool          ok = false;
    std::uint32_t dim = 0;
    int           latency_ms = 0;
    std::string   error;
};
struct RagEmbedSave  {};                    // persist + live-apply
struct RagEmbedRevert{};                    // discard edits, reload from config

// ── Settings pickers (Ctrl+K → Plugins/Commands/Agents/Hooks) ──────
// One shared list modal, parameterised by the config concern. Opening
// carries which concern; Move scrolls; Activate acts on the focused row.
struct OpenSettingsList  { settings::Category concern; };
struct CloseSettingsList {};
struct SettingsListMove  { int delta; };
struct SettingsListActivate {};
// Inline add-mode.
struct SettingsListAddStart   {};              // `a` — enter the add prompt
struct SettingsListRemove     {};              // `d` — delete the highlighted plugin (deliberate)
struct SettingsListChar       { char32_t ch; };// typed codepoint
struct SettingsListPaste      { std::string text; }; // bracketed paste into the add-mode input
// The MCP connection snapshot the Plugins panel renders. Dispatched by the
// load_plugins_async() Cmd when a connect/reload finishes on a worker. The
// reducer stores it in m.ui.plugins — the SINGLE source the view reads, so
// the panel is a pure function of the Model (no plugin_model() at render
// time). Mirrors the ThreadsLoaded{vec} pattern.
struct PluginsUpdated         { mcp::PluginModel model; };
struct SettingsListBackspace  {};
struct SettingsListSubmitInput{};              // Enter in add-mode → create
struct SettingsListCancelInput{};              // Esc in add-mode → back to list

// ── Fork picker ──────────────────────────────────────────────
// Branch the current thread into a FRESH one (near-zero context) and choose
// how proactive RAG behaves in the fork. The parent's transcript is written
// to disk and the fork carries a read-on-demand pointer to it — no copy, no
// summary. ForkThread does the branch. See docs/FORK.md.
struct OpenForkPicker  {};
struct CloseForkPicker {};
struct ForkPickerMove  { int delta; };
// Fork with the CURRENTLY-highlighted choice (the reducer reads the picker's
// cursor). A leaf with no payload keeps the key handler from needing Model.
struct ForkThread      {};

// ── Diff review ──────────────────────────────────────────────────────────
struct OpenDiffReview {};
struct CloseDiffReview {};
struct DiffReviewMove { int delta; };
// Page the FOCUSED hunk's body window by `delta` diff lines (^D/^U,
// PgDn/PgUp). Clamped in the reducer against the hunk's real line count.
struct DiffReviewScroll { int delta; };
struct DiffReviewNextFile {};
struct DiffReviewPrevFile {};
struct AcceptHunk {};
struct RejectHunk {};
struct AcceptAllChanges {};
struct RejectAllChanges {};

// ── Meta / session-level ─────────────────────────────────────────────────
// CompactContext, Tick, Quit, NoOp, ClearStatus, CycleProfile,
// RestoreCheckpoint, ScrollThread — all events that
// are conceptually "above" any single domain (the session itself, the
// tick clock, profile mode, etc.).

// Auto / manual conversation compaction. Mirrors Claude Code 2.1.119's
// `BetaToolRunner.compactionControl` (binary near offset 134600). When
// the running input-token total approaches the model's context window,
// or the user invokes "Compact context" from the palette, the runtime
// appends a synthetic User message asking the model to summarise the
// conversation per a structured schema; the resulting assistant text is
// then promoted to a single User message that REPLACES the entire
// conversation history. The next turn proceeds against the compacted
// prefix as if the summary were the only prior context. m.s.compacting
// is the in-flight flag — set on dispatch, cleared on the StreamFinished
// that lands the summary.
struct CompactContext {};

struct CycleProfile {};
struct RestoreCheckpoint { CheckpointId id; };
// Completion of the async git-restore kicked off by RestoreCheckpoint.
// `ok=false` carries a human-readable reason in `error`. The transcript
// truncation + composer refill happen HERE (not at dispatch) so the
// worktree is already byte-identical to the snapshot when the user sees
// their old prompt reappear in the composer.
struct CheckpointRestored { CheckpointId id; bool ok = false; std::string error; };

// ── Checkpoint picker ────────────────────────────────────────────────────
// Rewind picker over ALL checkpointed turns in the thread (not just the
// newest). Open builds the entry list from messages carrying a
// checkpoint_id and kicks an async diff-summary per entry; Select maps the
// highlighted entry onto the existing RestoreCheckpoint flow.
struct OpenCheckpointPicker {};
struct CloseCheckpointPicker {};
struct CheckpointPickerMove { int delta; };
struct CheckpointPickerSelect {};
// One entry's async "what changed since here" summary landed. `index` is
// the entry's position in Open::entries at dispatch time; the reducer
// re-validates it against the current list (an open/close race can't
// corrupt a stale index).
struct CheckpointDiffLoaded {
    int  index          = 0;
    bool ok             = false;
    int  files_changed  = 0;
    int  insertions     = 0;
    int  deletions      = 0;
};
struct ScrollThread { int delta; };
// Terminal window focus changed (?1004 CSI I/O via maya Sub::on_focus).
// Gates the hardware caret: an unfocused agentty parks + hides the real
// cursor instead of leaving a blinking bar in an inactive pane.
struct TerminalFocus { bool focused = true; };
// Ctrl+U on the transcript — flip the newest retrieved-context card between
// its compact one-line-per-source form and a full-passage-text expansion.
// Carries the target message id so the reducer mutates exactly that card
// (id-addressed mutation + render-key bump).
struct ToggleRetrievedExpanded { MessageId id; };
// Ctrl+T on the transcript — fold/unfold the newest assistant turn's reasoning
// ("Thinking") block. Carries the target message id so the reducer flips
// exactly that turn's Message::reasoning_expanded and bumps its render key
// (mirrors ToggleRetrievedExpanded). Provider-agnostic: the block is shown
// for any turn whose unified reasoning text is non-empty.
struct ToggleReasoning { MessageId id; };

struct Tick {};
struct Quit {};
struct NoOp {};
// User-triggered "drop the renderer's cell cache and repaint from
// scratch". Bound to Ctrl-L (universal terminal redraw convention).
// Dispatches `Cmd::force_redraw()`, which mirrors the SIGWINCH
// coherence-collapse — no scrollback wipe, just an in-place rebuild
// of `prev_cells` from the current canvas. Doubles as a debug hatch
// when something visibly desyncs.
struct RedrawScreen {};
// Delayed sentinel that clears `m.s.status` iff it hasn't been
// overwritten since the toast was scheduled. `stamp` is the value
// `m.s.status_until` had at schedule time; if the reducer has since
// written a newer status, stamps won't match and this Msg is a no-op.
struct ClearStatus { std::chrono::steady_clock::time_point stamp; };

// ── Self-update ───────────────────────────────────────────
// Background release check finished (worker thread → UI). Empty `latest`
// or update_available=false = nothing to announce; the reducer stores the
// state so the status-bar chip + palette entry appear. Check errors are
// swallowed silently — an update NOTICE must never surface as an error.
struct UpdateCheckDone {
    bool        update_available = false;
    std::string latest;   // "0.3.1"
    std::string url;      // release page (used in the toast)
};
// In-TUI update finished (worker → UI). Success shows the restart toast;
// failure shows the error + a hint to run `agentty update` from a shell.
struct UpdateApplied {
    bool        ok = false;
    std::string detail;   // version on success, error text on failure
};

// ============================================================================
// Domain variants — one per orthogonal slice of the runtime. Each is a
// `std::variant` over its leaves; per-domain reducers visit on these.
// ============================================================================
namespace msg {

using ComposerMsg = std::variant<
    ComposerCharInput, ComposerBackspace, ComposerEnter, ComposerNewline,
    ComposerSubmit, ComposerToggleExpand, ComposerToggleLoop,
    ComposerCursorLeft, ComposerCursorRight, ComposerCursorHome, ComposerCursorEnd,
    ComposerCursorWordLeft, ComposerCursorWordRight,
    ComposerKillToEndOfLine, ComposerKillToBeginningOfLine,
    ComposerDeleteWordBack, ComposerDeleteWordForward,
    ComposerUndo, ComposerRedo,
    ComposerHistoryPrev, ComposerHistoryNext,
    ComposerImagePasteFromClipboard,
    ComposerPaste, ClipboardQueryTimeout, ComposerRecallQueued,
    ComposerQueuePeekPrev, ComposerQueuePeekNext, ComposerQueuePopLast>;

using StreamMsg = std::variant<
    StreamStarted, StreamTextDelta, StreamTextBlockClosed,
    StreamToolUseStart, StreamToolUseDelta, StreamToolUseSnapshot,
    StreamToolUseEnd, StreamObservedToolResult,
    StreamThinkingDelta,
    StreamReasoning,
    StreamUsage, StreamFinished, StreamError, StreamHeartbeat,
    StreamBufferedWait, CancelStream, RetryStream, ProactiveContextReady,
    StreamNotice>;

using ToolMsg = std::variant<
    ToolExecOutput, ToolExecProgress, ToolTimeoutCheck,
    PermissionApprove, PermissionReject, PermissionApproveAlways,
    OpenToolOutputViewer, CloseToolOutputViewer,
    ToolViewerMove, ToolViewerSelect, ToolViewerCopy, ToolViewerStep>;

using ProviderPickerMsg = std::variant<
    OpenProviderPicker, CloseProviderPicker, ProviderPickerMove,
    ProviderPickerJump, ProviderPickerSelect,
    ProviderPickerFilterInput, ProviderPickerFilterBackspace,
    ProviderPickerDelete>;

// The ONE model surface: browse/filter every provider's models, tune effort,
// and (in Smart Mode slot-assign mode) pin a role→model. `ModelsLoaded` is
// the active provider's catalog fetch; `FusedCatalogLoaded` is a background
// per-provider one.
using FusedPickerMsg = std::variant<
    OpenFusedPicker, CloseFusedPicker, FusedPickerMove, FusedPickerJump,
    FusedPickerSelect, FusedPickerToggleFavorite,
    FusedPickerCycleEffort, FusedPickerToggleReasoning,
    FusedPickerToggleShowReasoning,
    FusedPickerScopeProvider,
    FusedPickerFilterInput, FusedPickerFilterBackspace,
    ModelsLoaded, FusedCatalogLoaded, SwitchToPreviousModel, FusedRefreshOthers,
    FusedPickerRefresh>;

using ThreadListMsg = std::variant<
    OpenThreadList, CloseThreadList, ThreadListMove, ThreadListJump,
    ThreadListSelect, ThreadListDelete, ThreadCycle, NewThread, ThreadsLoaded, ThreadLoaded>;

using CommandPaletteMsg = std::variant<
    OpenCommandPalette, CloseCommandPalette, CommandPaletteInput,
    CommandPaletteBackspace, CommandPaletteMove, CommandPaletteSelect>;

using MentionPaletteMsg = std::variant<
    OpenMentionPalette, CloseMentionPalette, MentionPaletteInput,
    MentionPaletteBackspace, MentionPaletteMove, MentionPaletteSelect>;

using SymbolPaletteMsg = std::variant<
    OpenSymbolPalette, CloseSymbolPalette, SymbolPaletteInput,
    SymbolPaletteBackspace, SymbolPaletteMove, SymbolPaletteSelect>;

using CodeBlockMsg = std::variant<
    OpenCodeBlockPicker, CloseCodeBlockPicker, CodeBlockPickerMove,
    CodeBlockPickerSelect, CodeBlockPickerEdit, CodeBlockPickerCopy,
    CodeBlockRunFinished,
    CodeBlockResultAttach, CodeBlockResultCopy, CodeBlockResultDiscard>;

using CheckpointMsg = std::variant<
    OpenCheckpointPicker, CloseCheckpointPicker, CheckpointPickerMove,
    CheckpointPickerSelect, CheckpointDiffLoaded>;

using RagSettingsMsg = std::variant<
    OpenRagSettings, CloseRagSettings, RagSettingsMove,
    RagSettingsAdjust, RagSettingsReset, RagSettingsAdvanced,
    RagEmbedOpen, RagEmbedClose, RagEmbedKey,
    RagEmbedTest, RagEmbedTestDone, RagEmbedSave, RagEmbedRevert>;

using SettingsListMsg = std::variant<
    OpenSettingsList, CloseSettingsList, SettingsListMove,
    SettingsListActivate, SettingsListAddStart, SettingsListRemove,
    SettingsListChar, SettingsListPaste, PluginsUpdated, SettingsListBackspace,
    SettingsListSubmitInput, SettingsListCancelInput>;

using ForkMsg = std::variant<
    OpenForkPicker, CloseForkPicker, ForkPickerMove, ForkThread>;

using TodoMsg = std::variant<
    OpenTodoModal, CloseTodoModal, UpdateTodos>;

using LoginMsg = std::variant<
    OpenLogin, CloseLogin, LoginBack, SignOut,
    OpenAccounts, AccountMove, AccountSelect, AccountRemove,
    LoginPickMethod, LoginCharInput, LoginBackspace,
    LoginPaste, LoginCursorLeft, LoginCursorRight, LoginSubmit,
    LoginCopyAuthUrl, LoginCopyCode, LoginOpenBrowserAgain,
    LoginExchanged, CodexDeviceCodeReady, CodexLoginDone,
    DeviceCodeReady, DeviceLoginDone, TokenRefreshed, HostProbed>;

using DiffReviewMsg = std::variant<
    OpenDiffReview, CloseDiffReview, DiffReviewMove, DiffReviewScroll,
    DiffReviewNextFile, DiffReviewPrevFile,
    AcceptHunk, RejectHunk, AcceptAllChanges, RejectAllChanges>;

using MetaMsg = std::variant<
    CompactContext, CycleProfile, RestoreCheckpoint, CheckpointRestored,
    ScrollThread, ToggleRetrievedExpanded,
    TerminalFocus,
    OpenSmartMode, CloseSmartMode, SmartModeKey, SmartModeAdvanced,
    SmartModeClearSlot,
    Tick, Quit, NoOp, ClearStatus, RedrawScreen,
    UpdateCheckDone, UpdateApplied>;

} // namespace msg

// ============================================================================
// Msg — top-level grouped variant. Construction from any leaf works because
// std::variant's converting constructor walks each alternative; only the
// matching domain accepts a given leaf, so the wrap is unambiguous and
// implicit:
//
//   Msg m = ComposerEnter{};       //  -> Msg{ComposerMsg{ComposerEnter{}}}
//   Cmd<Msg>::after(d, RetryStream{}); // same path
//
// std::visit on a Msg dispatches on domain, not leaf — see update.cpp.
// ============================================================================
using Msg = std::variant<
    msg::ComposerMsg,
    msg::StreamMsg,
    msg::ToolMsg,
    msg::ProviderPickerMsg,
    msg::FusedPickerMsg,
    msg::ThreadListMsg,
    msg::CommandPaletteMsg,
    msg::MentionPaletteMsg,
    msg::SymbolPaletteMsg,
    msg::CodeBlockMsg,
    msg::CheckpointMsg,
    msg::RagSettingsMsg,
    msg::SettingsListMsg,
    msg::ForkMsg,
    msg::TodoMsg,
    msg::LoginMsg,
    msg::DiffReviewMsg,
    msg::MetaMsg
>;

// ── Msg-domain proofs ─────────────────────────────────────────
// The Msg variant relies on `std::variant`'s converting constructor to
// route a leaf (e.g. `ComposerEnter{}`) into the right domain arm by
// finding the UNIQUE domain whose variant accepts it. "Unique" is the
// load-bearing word: if a leaf appears in two domain variants, the
// converting constructor is ambiguous and the build fails — but only
// at the call site that tries to construct the Msg. The proofs below
// surface that property in one place so the failure is at THIS line,
// not at every dispatch in the codebase.
namespace msg_proofs {

// True if leaf type L is one of the alternatives of variant V.
template <class L, class V>
struct in_variant : std::false_type {};
template <class L, class... Ts>
struct in_variant<L, std::variant<Ts...>>
    : std::bool_constant<(std::is_same_v<L, Ts> || ...)> {};
template <class L, class V>
inline constexpr bool in_variant_v = in_variant<L, V>::value;

// Count how many domain variants contain leaf L. Should be exactly 1
// for every leaf the runtime actually uses.
template <class L>
consteval int leaf_domain_count() {
    return int{in_variant_v<L, msg::ComposerMsg>}
         + int{in_variant_v<L, msg::StreamMsg>}
         + int{in_variant_v<L, msg::ToolMsg>}
         + int{in_variant_v<L, msg::ProviderPickerMsg>}
         + int{in_variant_v<L, msg::FusedPickerMsg>}
         + int{in_variant_v<L, msg::ThreadListMsg>}
         + int{in_variant_v<L, msg::CommandPaletteMsg>}
         + int{in_variant_v<L, msg::MentionPaletteMsg>}
         + int{in_variant_v<L, msg::SymbolPaletteMsg>}
         + int{in_variant_v<L, msg::CodeBlockMsg>}
         + int{in_variant_v<L, msg::CheckpointMsg>}
         + int{in_variant_v<L, msg::RagSettingsMsg>}
         + int{in_variant_v<L, msg::ForkMsg>}
         + int{in_variant_v<L, msg::TodoMsg>}
         + int{in_variant_v<L, msg::LoginMsg>}
         + int{in_variant_v<L, msg::DiffReviewMsg>}
         + int{in_variant_v<L, msg::MetaMsg>};
}

// Sample of representative leaves across every domain. If any of these
// lands in 0 domains, the variant member needs an arm; if any lands in
// 2+, the converting constructor for Msg becomes ambiguous and call
// sites stop compiling. We hand-pick one leaf per domain rather than
// trying to enumerate all 79+ leaves — the proof only needs to catch
// the case where SOMEONE adds a leaf in two domains by accident; one
// witness per domain is enough to keep the discipline visible here.
static_assert(leaf_domain_count<ComposerCharInput>()         == 1,
              "ComposerCharInput must belong to exactly one Msg domain");
static_assert(leaf_domain_count<StreamTextDelta>()           == 1,
              "StreamTextDelta must belong to exactly one Msg domain");
static_assert(leaf_domain_count<ToolExecOutput>()            == 1,
              "ToolExecOutput must belong to exactly one Msg domain");
static_assert(leaf_domain_count<OpenFusedPicker>()            == 1,
              "OpenFusedPicker must belong to exactly one Msg domain");
static_assert(leaf_domain_count<ModelsLoaded>()               == 1,
              "ModelsLoaded must belong to exactly one Msg domain");
static_assert(leaf_domain_count<OpenProviderPicker>()        == 1,
              "OpenProviderPicker must belong to exactly one Msg domain");
static_assert(leaf_domain_count<NewThread>()                 == 1,
              "NewThread must belong to exactly one Msg domain");
static_assert(leaf_domain_count<CommandPaletteSelect>()      == 1,
              "CommandPaletteSelect must belong to exactly one Msg domain");
static_assert(leaf_domain_count<MentionPaletteSelect>()      == 1,
              "MentionPaletteSelect must belong to exactly one Msg domain");
static_assert(leaf_domain_count<SymbolPaletteSelect>()       == 1,
              "SymbolPaletteSelect must belong to exactly one Msg domain");
static_assert(leaf_domain_count<CodeBlockPickerSelect>()     == 1,
              "CodeBlockPickerSelect must belong to exactly one Msg domain");
static_assert(leaf_domain_count<CheckpointPickerSelect>()    == 1,
              "CheckpointPickerSelect must belong to exactly one Msg domain");
static_assert(leaf_domain_count<OpenRagSettings>()           == 1,
              "OpenRagSettings must belong to exactly one Msg domain");
static_assert(leaf_domain_count<ForkThread>()                == 1,
              "ForkThread must belong to exactly one Msg domain");
static_assert(leaf_domain_count<UpdateTodos>()               == 1,
              "UpdateTodos must belong to exactly one Msg domain");
static_assert(leaf_domain_count<LoginSubmit>()               == 1,
              "LoginSubmit must belong to exactly one Msg domain");
static_assert(leaf_domain_count<AcceptAllChanges>()          == 1,
              "AcceptAllChanges must belong to exactly one Msg domain");
static_assert(leaf_domain_count<Tick>()                      == 1,
              "Tick must belong to exactly one Msg domain");

// Pin the top-level Msg domain count too — if someone adds a new domain
// they must also update the kDomains array used by the dispatcher in
// update.cpp, which currently exhausts on 12 arms. Mismatch → dispatch
// switch loses a domain silently.
static_assert(std::variant_size_v<Msg> == 18,
              "Msg domain count changed — update the dispatcher in "
              "src/runtime/app/update.cpp and this proof to match");

// Spot-check the converting-constructor is unambiguous for a few
// representative leaves. If a leaf appeared in two domain variants the
// line `Msg{X{}}` would fail to compile here — surfacing the problem
// at the proof site instead of every dispatch call site.
static_assert([] {
    Msg m1 = ComposerEnter{};       (void)m1;
    Msg m2 = StreamFinished{};      (void)m2;
    Msg m3 = Tick{};                (void)m3;
    Msg m4 = NewThread{};           (void)m4;
    Msg m5 = OpenLogin{};           (void)m5;
    return true;
}(), "Msg leaf construction must be unambiguous — if this fires, some "
     "leaf appears in two domain variants");

} // namespace msg_proofs

} // namespace agentty
