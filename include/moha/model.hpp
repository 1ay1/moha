#pragma once
// moha domain model — strong types, decomposed state, enum reflection.

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <maya/widget/spinner.hpp>

// Forward declarations — storing these as shared_ptr keeps model.hpp from
// having to pull in the full maya element/streaming headers (which would
// transitively drag layout/render types across every translation unit).
namespace maya {
    struct Element;
    class  StreamingMarkdown;
}

namespace moha {

// ============================================================================
// Strong ID types — compile-time distinct, zero-overhead
// ============================================================================
// Prevents accidental interchange of ThreadId / ToolCallId / ModelId.
// Provides == with string_view for pattern matching against known values.

template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) : value(std::move(s)) {}

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

    [[nodiscard]] bool operator==(std::string_view sv) const { return value == sv; }

    friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
    friend void from_json(const nlohmann::json& j, Id& id) { j.get_to(id.value); }
};

struct ThreadIdTag {};
struct ToolCallIdTag {};
struct ModelIdTag {};
struct CheckpointIdTag {};
struct ToolNameTag {};

using ThreadId     = Id<ThreadIdTag>;
using ToolCallId   = Id<ToolCallIdTag>;
using ModelId      = Id<ModelIdTag>;
using CheckpointId = Id<CheckpointIdTag>;
using ToolName     = Id<ToolNameTag>;

// ============================================================================
// Enums with constexpr reflection
// ============================================================================

enum class Role : uint8_t { User, Assistant, System };
enum class Profile : uint8_t { Write, Ask, Minimal };
enum class Phase : uint8_t { Idle, Streaming, AwaitingPermission, ExecutingTool };

[[nodiscard]] constexpr std::string_view to_string(Role r) noexcept {
    switch (r) {
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::System:    return "system";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(Profile p) noexcept {
    switch (p) {
        case Profile::Write:   return "Write";
        case Profile::Ask:     return "Ask";
        case Profile::Minimal: return "Minimal";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:               return "idle";
        case Phase::Streaming:          return "streaming";
        case Phase::AwaitingPermission: return "permission";
        case Phase::ExecutingTool:      return "working";
    }
    return "?";
}

// ============================================================================
// Domain value objects
// ============================================================================

struct ToolUse {
    enum class Status : uint8_t { Pending, Approved, Running, Done, Error, Rejected };
    ToolCallId     id;
    ToolName       name;
    nlohmann::json args;
    std::string    args_streaming;
    std::string    output;
    // Live stdout+stderr snapshot for a running tool. Shown in the card while
    // status == Running so the user sees progress immediately instead of
    // waiting until the whole command finishes. Replaced by the formatted
    // `output` once the terminal ToolExecOutput arrives.
    std::string    progress_text;
    Status         status   = Status::Pending;
    bool           expanded = true;
    // Wall-clock stamps for progress reporting. `started_at` is set when the
    // tool transitions from Pending/Approved to Running so the view can show
    // live "3.2s" elapsed on the card; `finished_at` is set when terminal.
    // steady_clock (not system_clock) so a user changing the system clock
    // mid-execution doesn't produce negative elapsed times.
    std::chrono::steady_clock::time_point started_at {};
    std::chrono::steady_clock::time_point finished_at {};

    // Lazy cache of args.dump() for the view. args.dump() is O(args) per call
    // and showed up in per-frame views (thread/permission cards) for tools
    // without a bespoke renderer. Invalidate via mark_args_dirty() whenever
    // `args` is mutated.
    mutable std::string args_dump_cache;
    mutable bool        args_dump_valid = false;

    void mark_args_dirty() { args_dump_valid = false; args_dump_cache.clear(); }
    const std::string& args_dump() const {
        if (!args_dump_valid) {
            args_dump_cache = args.dump();
            args_dump_valid = true;
        }
        return args_dump_cache;
    }
};

struct Message {
    Role        role = Role::User;
    std::string text;
    std::string streaming_text;
    std::vector<ToolUse> tool_calls;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::optional<CheckpointId> checkpoint_id;

    // ── Per-message render cache (not persisted, not semantic) ──────────
    // A finalized message's text is immutable in this codebase (only
    // finalize_turn / StreamError append to it), so we parse once and
    // reuse the Element forever.  Mutators must reset cached_md_element
    // explicitly.  The streaming tail gets its own StreamingMarkdown —
    // block-boundary cached → O(new_chars) per delta.
    // shared_ptr keeps Message trivially copyable (Model is value-type).
    mutable std::shared_ptr<maya::Element>            cached_md_element;
    mutable std::shared_ptr<maya::StreamingMarkdown>  stream_md;
};

struct Thread {
    ThreadId    id;
    std::string title;
    std::vector<Message> messages;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updated_at = std::chrono::system_clock::now();
};

struct Hunk {
    enum class Status : uint8_t { Pending, Accepted, Rejected };
    int old_start = 0, old_len = 0, new_start = 0, new_len = 0;
    std::string patch;
    Status status = Status::Pending;
};

struct FileChange {
    std::string path;
    int added   = 0;
    int removed = 0;
    std::vector<Hunk> hunks;
    std::string original_contents;
    std::string new_contents;
};

struct PendingPermission {
    ToolCallId  id;
    ToolName    tool_name;
    std::string reason;
};

struct ModelInfo {
    ModelId     id;
    std::string display_name;
    std::string provider;
    int  context_window = 200000;
    bool favorite       = false;
};

// ============================================================================
// Model sub-states — each owns a single concern
// ============================================================================

struct ComposerState {
    std::string text;
    int  cursor   = 0;
    bool expanded = false;
    std::vector<std::string> queued;
};

struct StreamState {
    Phase phase  = Phase::Idle;
    bool  active = false;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_tick{};
    int tokens_in  = 0;
    int tokens_out = 0;
    int context_max = 200000;
    std::string status;
    maya::Spinner<maya::SpinnerStyle::Dots> spinner{};
};

struct ModelPickerState {
    bool open  = false;
    int  index = 0;
};

struct ThreadListState {
    bool open  = false;
    int  index = 0;
};

// ── Command palette — enum-driven, no magic indices ──────────────────────

enum class Command : uint8_t {
    NewThread, ReviewChanges, AcceptAll, RejectAll,
    CycleProfile, OpenModels, OpenThreads, OpenPlan, Quit,
};

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
};

inline constexpr std::array kCommands = std::array{
    CommandDef{Command::NewThread,      "New thread",         "Start a fresh conversation"},
    CommandDef{Command::ReviewChanges,  "Review changes",     "Open diff review pane"},
    CommandDef{Command::AcceptAll,      "Accept all changes", "Apply every pending hunk"},
    CommandDef{Command::RejectAll,      "Reject all changes", "Discard every pending hunk"},
    CommandDef{Command::CycleProfile,   "Cycle profile",      "Write \u2192 Ask \u2192 Minimal"},
    CommandDef{Command::OpenModels,     "Open model picker",  ""},
    CommandDef{Command::OpenThreads,    "Open threads",       ""},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress"},
    CommandDef{Command::Quit,          "Quit",               "Exit moha"},
};

struct CommandPaletteState {
    bool open = false;
    std::string query;
    int index = 0;
};

struct DiffReviewState {
    bool open       = false;
    int  file_index = 0;
    int  hunk_index = 0;
};

enum class TodoStatus : uint8_t { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    TodoStatus  status = TodoStatus::Pending;
};

struct TodoState {
    bool open = false;
    std::vector<TodoItem> items;
};

// ============================================================================
// Model — application state, decomposed into semantic sub-states
// ============================================================================

struct Model {
    // ── Domain ───────────────────────────────────────────────────────
    Thread              current;
    std::vector<Thread> threads;
    Profile             profile = Profile::Write;

    std::vector<ModelInfo> available_models;
    ModelId                model_id{std::string{"claude-opus-4-5"}};

    std::vector<FileChange>          pending_changes;
    std::optional<PendingPermission> pending_permission;

    // ── UI sub-states ────────────────────────────────────────────────
    ComposerState       composer;
    StreamState         stream;
    ModelPickerState    model_picker;
    ThreadListState     thread_list;
    CommandPaletteState command_palette;
    DiffReviewState     diff_review;
    TodoState           todo;
    int                 thread_scroll = 0;

    // ── View virtualization ──────────────────────────────────────────
    // Index of the first message the view should render.  Messages before
    // this point are considered committed to the terminal's native
    // scrollback (maya's InlineFrameState::commit_prefix was called for
    // their rows).  Advancing this counter + returning Cmd::commit_scrollback
    // keeps the Yoga/paint cost bounded to the window, not the full
    // transcript.
    int thread_view_start = 0;
};

} // namespace moha
