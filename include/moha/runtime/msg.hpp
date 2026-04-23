#pragma once
// moha::Msg — every event the runtime can process, as a closed variant.

#include <expected>
#include <string>
#include <variant>

#include "moha/runtime/model.hpp"
#include "moha/tool/registry.hpp"

namespace moha {

// ── Composer ─────────────────────────────────────────────────────────────
struct ComposerCharInput { char32_t ch; };
struct ComposerBackspace {};
struct ComposerEnter {};
struct ComposerNewline {};
struct ComposerSubmit {};
struct ComposerToggleExpand {};
struct ComposerCursorLeft {};
struct ComposerCursorRight {};
struct ComposerCursorHome {};
struct ComposerCursorEnd {};
struct ComposerPaste { std::string text; };

// ── Streaming from provider ──────────────────────────────────────────────
struct StreamStarted {};
struct StreamTextDelta { std::string text; };
struct StreamToolUseStart { ToolCallId id; ToolName name; };
struct StreamToolUseDelta { std::string partial_json; };
struct StreamToolUseEnd {};
// Mirrors Anthropic's message.usage shape. cache_* fields are non-zero only
// when the request hit a cache_control breakpoint. Fields default to 0 so
// callers that only care about input/output keep working.
struct StreamUsage {
    int input_tokens               = 0;
    int output_tokens              = 0;
    int cache_creation_input_tokens = 0;
    int cache_read_input_tokens    = 0;
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
struct StreamError { std::string message; };
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
};
// Live progress snapshot from a running tool (e.g. bash stdout+stderr so far).
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

// ── Permission ───────────────────────────────────────────────────────────
struct PermissionApprove {};
struct PermissionReject {};
struct PermissionApproveAlways {};

// ── Navigation / modals ──────────────────────────────────────────────────
struct OpenModelPicker {};
struct CloseModelPicker {};
struct ModelPickerMove { int delta; };
struct ModelPickerSelect {};
struct ModelPickerToggleFavorite {};
struct ModelsLoaded { std::vector<ModelInfo> models; };

struct OpenThreadList {};
struct CloseThreadList {};
struct ThreadListMove { int delta; };
struct ThreadListSelect {};
struct NewThread {};

struct OpenCommandPalette {};
struct CloseCommandPalette {};
struct CommandPaletteInput { char32_t ch; };
struct CommandPaletteBackspace {};
struct CommandPaletteMove { int delta; };
struct CommandPaletteSelect {};

// ── Todo modal ──────────────────────────────────────────────────────────
struct OpenTodoModal {};
struct CloseTodoModal {};
struct UpdateTodos { std::vector<TodoItem> items; };

// ── Profile / mode ───────────────────────────────────────────────────────
struct CycleProfile {};

// ── Diff review ──────────────────────────────────────────────────────────
struct OpenDiffReview {};
struct CloseDiffReview {};
struct DiffReviewMove { int delta; };
struct DiffReviewNextFile {};
struct DiffReviewPrevFile {};
struct AcceptHunk {};
struct RejectHunk {};
struct AcceptAllChanges {};
struct RejectAllChanges {};

// ── Checkpoint ───────────────────────────────────────────────────────────
struct RestoreCheckpoint { CheckpointId id; };

// ── Thread / misc ────────────────────────────────────────────────────────
struct ScrollThread { int delta; };
struct ToggleToolExpanded { ToolCallId id; };

// ── Tick / meta ──────────────────────────────────────────────────────────
struct Tick {};
struct Quit {};
struct NoOp {};
// Delayed sentinel that clears `m.s.status` iff it hasn't been
// overwritten since the toast was scheduled. `stamp` is the value
// `m.s.status_until` had at schedule time; if the reducer has since
// written a newer status, stamps won't match and this Msg is a no-op.
struct ClearStatus { std::chrono::steady_clock::time_point stamp; };

using Msg = std::variant<
    ComposerCharInput, ComposerBackspace, ComposerEnter, ComposerNewline,
    ComposerSubmit, ComposerToggleExpand,
    ComposerCursorLeft, ComposerCursorRight, ComposerCursorHome, ComposerCursorEnd,
    ComposerPaste,
    StreamStarted, StreamTextDelta,
    StreamToolUseStart, StreamToolUseDelta, StreamToolUseEnd,
    StreamUsage, StreamFinished, StreamError, CancelStream, RetryStream,
    ToolExecOutput, ToolExecProgress, ToolTimeoutCheck,
    PermissionApprove, PermissionReject, PermissionApproveAlways,
    OpenModelPicker, CloseModelPicker, ModelPickerMove, ModelPickerSelect, ModelPickerToggleFavorite, ModelsLoaded,
    OpenThreadList, CloseThreadList, ThreadListMove, ThreadListSelect, NewThread,
    OpenCommandPalette, CloseCommandPalette, CommandPaletteInput,
    CommandPaletteBackspace, CommandPaletteMove, CommandPaletteSelect,
    OpenTodoModal, CloseTodoModal, UpdateTodos,
    CycleProfile,
    OpenDiffReview, CloseDiffReview, DiffReviewMove,
    DiffReviewNextFile, DiffReviewPrevFile,
    AcceptHunk, RejectHunk, AcceptAllChanges, RejectAllChanges,
    RestoreCheckpoint,
    ScrollThread, ToggleToolExpanded,
    Tick, Quit, NoOp, ClearStatus
>;

} // namespace moha
