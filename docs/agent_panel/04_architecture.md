# 04 — Architecture: Model, Msg, update, view, streaming

This doc specifies the **shape** the moha agent panel should have:
how state is organized, what messages flow through `update`, and how
the streaming SSE feed reaches the UI without blocking.

It documents the target shape, calling out where moha's current
implementation matches and where it diverges. Use the rebuild playbook
(`13_rebuild_playbook.md`) to sequence the migration.

## 1. The Elm core

maya is built around the Elm pattern:

```cpp
struct App {
    using Model = …;
    using Msg   = std::variant<…>;

    static Model init();
    static auto update(Model, Msg) -> std::pair<Model, Cmd<Msg>>;
    static Element view(const Model&);
    static auto subscribe(const Model&) -> Sub<Msg>;
};
```

- **Model** is a plain value, no `shared_ptr`. Cloned per-frame.
- **Msg** is an enum-of-variants — every event the app can react to.
- **update** is a pure function that produces a new Model and a `Cmd`
  describing side effects to perform (HTTP, clipboard, timers, etc.).
- **view** is a pure function from Model to UI.
- **subscribe** declares which external events the app currently wants
  (key events, ticks, paste, resize). The set can change with state.

The whole agent panel must respect this discipline. Side effects only
escape via `Cmd::task` (and only those that need to). `view` is pure.

## 2. Model — the target shape

Lives in `include/moha/model.hpp`. The current model exists; the gap
list is at the bottom.

```cpp
namespace moha {

enum class Phase {
    Idle,
    Streaming,             // assistant text/tool deltas arriving
    AwaitingPermission,    // a tool needs Y/N from the user
    ExecutingTool,         // tool is running locally (not yet returning)
};

enum class Profile {
    Write,    // any tool, prompts only on first use of dangerous ones
    Ask,      // every tool prompts
    Minimal,  // read-only tools auto-allowed
};

enum class Route {
    Conversation,  // the agent panel main view
    History,       // thread list
    Settings,      // model/profile config
};

struct PendingPermission {
    std::string tool_call_id;
    std::string tool_name;
    std::string reason;
    std::vector<std::string> always_choices;  // ["only this time", "always for X", ...]
    int selected_choice = 0;
    bool dropdown_open = false;
};

struct ToolUse {
    enum class Status { Pending, Approved, Running, Done, Error, Rejected, Cancelled };

    std::string id;
    std::string name;
    nlohmann::json args;             // parsed
    std::string args_streaming;      // partial JSON during stream
    std::string output;
    Status status = Status::Pending;
    bool   expanded = true;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point ended_at{};

    [[nodiscard]] float elapsed_seconds() const;
};

struct Message {
    Role role;
    std::string text;                       // finalized
    std::string streaming_text;             // partial during stream
    std::vector<ToolUse> tool_calls;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> checkpoint_id;
    bool editing = false;                    // user message in edit mode
};

struct Thread {
    std::string id;
    std::string title;
    std::vector<Message> messages;
    std::chrono::system_clock::time_point created_at, updated_at;
};

struct Composer {
    std::string text;
    int cursor = 0;                          // byte offset
    bool expanded = false;                   // Shift+Alt+Esc → 80% height
    std::vector<MentionChip> mentions;       // resolved @file/@symbol/@thread
    bool fast_mode = false;
    bool thinking = false;
    int  thinking_effort = 1;                // 0=Low 1=Med 2=High
};

struct ScrollState {
    int  offset = 0;
    bool at_bottom = true;                   // follow-tail flag
};

struct Selectors {
    bool model_open = false;
    int  model_index = 0;
    bool profile_open = false;
    int  profile_index = 0;
    bool mode_open = false;
    int  mode_index = 0;
    bool agent_open = false;                 // agent picker (if multiple agents)
};

struct Model {
    Route route = Route::Conversation;

    Thread current;
    std::vector<Thread> all_threads;          // for History route

    Phase phase = Phase::Idle;
    Profile profile = Profile::Write;
    std::string model_id = "claude-opus-4-5";
    std::vector<std::string> available_models;
    std::vector<std::string> favorite_models;

    Composer composer;
    ScrollState scroll;
    Selectors selectors;

    std::optional<PendingPermission> pending_permission;
    std::optional<std::string> stream_error;

    // Streaming bookkeeping
    bool stream_active = false;
    std::chrono::steady_clock::time_point stream_started_at{};
    int spinner_phase = 0;
    std::vector<std::string> queued_user_messages;  // sent while streaming

    // Token tracking
    int tokens_in = 0;
    int tokens_out = 0;
    int context_max = 200000;

    // Diff review state (when reviewing batched edits)
    std::optional<DiffReviewState> diff_review;
};

} // namespace moha
```

### Key invariants

- `phase == Idle` ⟹ `pending_permission == nullopt && stream_active == false`
- `phase == Streaming` ⟹ `stream_active == true`
- `pending_permission.has_value()` ⟹ `phase == AwaitingPermission`
- `selectors.*_open` are mutually exclusive (only one popover at a time)
- `composer.text.empty()` ⟹ Send button disabled

Enforce these in `update()` after every transition. They aren't enforced
by types because that would balloon the type complexity; they're
enforced by code.

## 3. Msg — the event taxonomy

Group messages by **what they're about**, not by widget. Roughly:

```cpp
namespace moha {

// ── Composer ──────────────────────────────────────────────────────
struct ComposerCharInput { char32_t cp; };
struct ComposerBackspace {};
struct ComposerEnter {};               // submit if not multiline-shift
struct ComposerNewline {};             // alt+enter / shift+enter
struct ComposerPaste { std::string text; };
struct ComposerExpand {};              // shift+alt+esc
struct ComposerToggleFastMode {};
struct ComposerToggleThinking {};
struct ComposerSetThinkingEffort { int level; };
struct ComposerCursorMove { int delta; };       // arrows
struct ComposerCursorJump { int target; };
struct ComposerSubmit {};
struct ComposerStop {};                // stop button → cancel stream

// ── Mentions ──────────────────────────────────────────────────────
struct ComposerOpenMentionMenu {};     // typed '@' or clicked +
struct ComposerSelectMention { MentionRef ref; };
struct ComposerCancelMention {};

// ── Streaming (from anthropic SSE) ────────────────────────────────
struct StreamStarted {};
struct StreamTextDelta { std::string text; };
struct StreamToolUseStart { std::string id, name; };
struct StreamToolUseDelta { std::string partial_json; };
struct StreamToolUseEnd {};
struct StreamUsage { int in_tokens, out_tokens; };
struct StreamFinished {};
struct StreamError { std::string message; ErrorKind kind; };

// ── Local tool execution ──────────────────────────────────────────
struct ToolExecOutput { std::string id; std::string output; bool ok; };

// ── Permission ────────────────────────────────────────────────────
struct PermissionApprove {};
struct PermissionReject {};
struct PermissionToggleDropdown {};
struct PermissionSelectChoice { int idx; };  // includes "always for X"

// ── Tool card affordances ─────────────────────────────────────────
struct ToolCardToggleExpand { std::string id; };
struct ToolCardCancel { std::string id; };

// ── Conversation navigation ───────────────────────────────────────
struct ScrollLineUp {};
struct ScrollLineDown {};
struct ScrollPageUp {};
struct ScrollPageDown {};
struct ScrollHome {};
struct ScrollEnd {};
struct JumpPrevMessage {};
struct JumpNextMessage {};

// ── Selectors ─────────────────────────────────────────────────────
struct OpenModelSelector {}; struct CloseModelSelector {};
struct ModelSelectorMove { int delta; };
struct ModelSelectorChoose {};
// (mirror for profile + mode + agent)

// ── Routes ────────────────────────────────────────────────────────
struct OpenHistory {}; struct CloseHistory {};
struct OpenSettings {}; struct CloseSettings {};
struct NewThread {};
struct OpenThread { std::string id; };
struct DeleteThread { std::string id; };

// ── Editing ───────────────────────────────────────────────────────
struct EditUserMessage { std::string message_id; };
struct CancelEdit {};
struct RegenerateFrom { std::string message_id; };
struct RestoreCheckpoint { std::string checkpoint_id; };

// ── Diff review ───────────────────────────────────────────────────
struct OpenDiffReview {}; struct CloseDiffReview {};
struct DiffReviewMove { int delta; };
struct DiffAcceptHunk {}; struct DiffRejectHunk {};
struct DiffAcceptAll {}; struct DiffRejectAll {};

// ── Meta ──────────────────────────────────────────────────────────
struct Tick {};                         // every 60ms while streaming
struct Resize { int w, h; };
struct Quit {};
struct NoOp {};

using Msg = std::variant<
    ComposerCharInput, ComposerBackspace, ComposerEnter, ComposerNewline,
    ComposerPaste, ComposerExpand, ComposerToggleFastMode, ComposerToggleThinking,
    ComposerSetThinkingEffort, ComposerCursorMove, ComposerCursorJump,
    ComposerSubmit, ComposerStop, ComposerOpenMentionMenu,
    ComposerSelectMention, ComposerCancelMention,
    StreamStarted, StreamTextDelta, StreamToolUseStart, StreamToolUseDelta,
    StreamToolUseEnd, StreamUsage, StreamFinished, StreamError,
    ToolExecOutput,
    PermissionApprove, PermissionReject, PermissionToggleDropdown,
    PermissionSelectChoice,
    ToolCardToggleExpand, ToolCardCancel,
    ScrollLineUp, ScrollLineDown, ScrollPageUp, ScrollPageDown, ScrollHome, ScrollEnd,
    JumpPrevMessage, JumpNextMessage,
    OpenModelSelector, CloseModelSelector, ModelSelectorMove, ModelSelectorChoose,
    OpenProfileSelector, CloseProfileSelector, ProfileSelectorMove, ProfileSelectorChoose,
    OpenModeSelector, CloseModeSelector, ModeSelectorMove, ModeSelectorChoose,
    OpenAgentSelector, CloseAgentSelector, AgentSelectorMove, AgentSelectorChoose,
    OpenHistory, CloseHistory, OpenSettings, CloseSettings,
    NewThread, OpenThread, DeleteThread,
    EditUserMessage, CancelEdit, RegenerateFrom, RestoreCheckpoint,
    OpenDiffReview, CloseDiffReview, DiffReviewMove,
    DiffAcceptHunk, DiffRejectHunk, DiffAcceptAll, DiffRejectAll,
    Tick, Resize, Quit, NoOp
>;

} // namespace moha
```

That's roughly 70 message types — same order of magnitude as moha's
current count. The current taxonomy is in
`include/moha/msg.hpp:87-108`; some renames and additions needed but
no architectural shift.

## 4. update — the dispatch table

The body of `update` is one giant `std::visit` over `Msg`. Each arm
does:

1. Mutate a copy of `m` based on the message
2. Re-establish invariants
3. Return `(m, cmd)` where `cmd` describes any new effect to start

### Anatomy of a key arm

```cpp
[&](ComposerSubmit) -> std::pair<Model, Cmd<Msg>> {
    if (m.composer.text.empty()) return {m, Cmd<Msg>::none()};
    if (m.phase != Phase::Idle) {
        // Queue and wait
        m.queued_user_messages.push_back(std::move(m.composer.text));
        m.composer.text.clear();
        m.composer.cursor = 0;
        return {m, Cmd<Msg>::none()};
    }

    // 1) Append user message to thread
    Message um;
    um.role = Role::User;
    um.text = std::move(m.composer.text);
    um.timestamp = std::chrono::system_clock::now();
    m.current.messages.push_back(std::move(um));
    m.composer.text.clear();
    m.composer.cursor = 0;

    // 2) Append placeholder assistant message
    Message am;
    am.role = Role::Assistant;
    am.timestamp = std::chrono::system_clock::now();
    m.current.messages.push_back(std::move(am));

    // 3) Begin streaming
    m.phase = Phase::Streaming;
    m.stream_active = true;
    m.stream_started_at = std::chrono::steady_clock::now();
    m.scroll.at_bottom = true;
    return {m, launch_stream_cmd(m)};
},
```

Key practices:

- **Always return `Cmd::none()` if there's nothing to do** — never
  spawn a no-op task.
- **Dispatch one command at a time** — use `Cmd::batch` only if you
  truly need multiple effects in parallel.
- **Don't do work inline that could be a `Cmd`** — file IO, network,
  timers all belong in `Cmd`s.

## 5. view — pure function to Element tree

Layout target (TUI):

```
┌──────────────────────────────────────────┐
│  CHROME  [agent ▾]   [⤢] [history] [⋯]   │  height: 1 row
├──────────────────────────────────────────┤
│                                          │
│  STREAM  (scrollable, follows tail)      │  grow: 1
│                                          │
├──────────────────────────────────────────┤
│  (callout if pending changes ≥ N)        │  variable
├──────────────────────────────────────────┤
│  COMPOSER (3–N rows tall)                │  fixed-min, expand on demand
├──────────────────────────────────────────┤
│  STATUS BAR  ready · 4.2k/200k · keys    │  height: 1 row
└──────────────────────────────────────────┘
```

Skeleton:

```cpp
Element MohaApp::view(const Model& m) {
    using namespace maya::dsl;

    auto chrome    = views::chrome(m);
    auto stream    = views::message_stream(m);
    auto callouts  = views::pending_changes_callout(m);
    auto composer  = views::composer(m);
    auto status    = views::status_bar(m);

    auto base = (v(
        chrome,
        stream | grow_<1>,
        callouts,
        composer,
        status
    ) | padding(0, 1, 0, 1)).build();

    // Selectors and routes layered on top:
    if (m.selectors.model_open)   return overlay_popup(base, views::model_popup(m));
    if (m.selectors.profile_open) return overlay_popup(base, views::profile_popup(m));
    if (m.selectors.mode_open)    return overlay_popup(base, views::mode_popup(m));
    if (m.route == Route::History) return views::history_view(m);     // route swap
    if (m.route == Route::Settings) return views::settings_view(m);

    return base;
}
```

The route swap is **content replacement** — when you press Cmd+Shift+H,
the conversation pane swaps out for the history list. Going back swaps
it back. No modal involved.

The popup overlays are local — see `11_navigation.md` for how to render
them at a fixed position over the chrome.

## 6. subscribe — input policy

```cpp
Sub<Msg> MohaApp::subscribe(const Model& m) {
    // Always-on subscriptions
    auto keys = Sub<Msg>::on_key([m](const KeyEvent& k) -> std::optional<Msg> {
        return route_key_event(m, k);
    });
    auto resize = Sub<Msg>::on_resize([](Size s){ return Resize{s.w, s.h}; });
    auto paste = Sub<Msg>::on_paste([](std::string s){ return ComposerPaste{std::move(s)}; });

    auto subs = Sub<Msg>::batch({keys, resize, paste});

    // Tick only while streaming (drives spinner)
    if (m.stream_active) {
        subs = Sub<Msg>::batch({subs, Sub<Msg>::every(60ms, Tick{})});
    }

    return subs;
}
```

`route_key_event` is the keymap dispatch. It's a single function rather
than a `key_map` literal because the **target Msg depends on which
selector / route is currently open** (e.g., `Up` means "scroll line"
in the conversation but "previous menu entry" in an open selector).

Pseudo:

```cpp
std::optional<Msg> route_key_event(const Model& m, const KeyEvent& k) {
    // 1) Open popovers eat keys first
    if (m.selectors.model_open)   return model_popup_keys(m, k);
    if (m.selectors.profile_open) return profile_popup_keys(m, k);
    if (m.selectors.mode_open)    return mode_popup_keys(m, k);

    // 2) Pending permission handles Y/N/A and Esc
    if (m.pending_permission && permission_focused(m)) {
        if (key_is(k, 'y') || key_is(k, 'Y')) return PermissionApprove{};
        if (key_is(k, 'n') || key_is(k, 'N')) return PermissionReject{};
        if (key_is(k, 'a') || key_is(k, 'A')) return PermissionToggleDropdown{};
        if (key_is(k, SpecialKey::Escape))    return PermissionReject{};
    }

    // 3) Route-level keys
    if (m.route == Route::History) return history_keys(m, k);
    if (m.route == Route::Settings) return settings_keys(m, k);

    // 4) Conversation route
    if (composer_focused(m)) return composer_keys(m, k);
    if (key_is(k, SpecialKey::PageUp))   return ScrollPageUp{};
    if (key_is(k, SpecialKey::PageDown)) return ScrollPageDown{};
    // etc.

    // 5) Global agent shortcuts
    if (ctrl_is(k, 'h'))    return OpenHistory{};
    if (ctrl_is(k, '/'))    return OpenModelSelector{};
    // etc.

    return std::nullopt;
}
```

Full keymap in `12_keymap.md`.

## 7. Streaming integration

The streaming pipeline is the biggest non-trivial piece. SSE frames
arrive on a libcurl thread; they need to land in `update()` on the UI
thread without blocking either.

### Cmd::task is the bridge

```cpp
Cmd<Msg> launch_stream_cmd(const Model& m) {
    anthropic::Request req;
    req.model = m.model_id;
    req.system_prompt = anthropic::default_system_prompt();
    req.messages = build_messages_for_request(m.current);
    req.tools = anthropic::default_tools();
    req.auth_header = g_creds.header_value();
    req.auth_style  = g_creds.style();
    req.max_tokens = 8192;

    return Cmd<Msg>::task(
        [req = std::move(req)](std::function<void(Msg)> dispatch) mutable {
            anthropic::run_stream_sync(std::move(req),
                [dispatch](Msg msg) {
                    // dispatch is thread-safe — it routes to the UI
                    // thread via maya's BackgroundQueue
                    dispatch(std::move(msg));
                }
            );
        }
    );
}
```

`run_stream_sync` (`src/anthropic.cpp:195`) blocks the worker thread
on libcurl's perform loop. Each SSE event is parsed and immediately
turned into a `Msg` (see `04_architecture.md` § 3 above for the Msg
list). The `dispatch` callback is just a plain `std::function<void(Msg)>`
— maya's runtime ensures it's safe to call from any thread, and the
Msg is queued for the next frame's `update()`.

### SSE → Msg mapping

In `src/anthropic.cpp:37-82`:

| SSE event | Msg |
|---|---|
| `message_start` | `StreamStarted{}` |
| `content_block_start` (text) | (none — implied) |
| `content_block_start` (tool_use) | `StreamToolUseStart{id, name}` |
| `content_block_delta` (text_delta) | `StreamTextDelta{text}` |
| `content_block_delta` (input_json_delta) | `StreamToolUseDelta{partial_json}` |
| `content_block_stop` | `StreamToolUseEnd{}` |
| `message_delta` (with usage) | `StreamUsage{in, out}` |
| `message_stop` | `StreamFinished{}` |
| 4xx body / curl error | `StreamError{msg, kind}` |

### Fine-grained tool streaming (CRITICAL — `eager_input_streaming`)

Anthropic's API does **not** stream `input_json_delta` events eagerly by
default. Without an explicit opt-in, the edge buffers tool input
server-side and trickles it down in coarse chunks. For prose this is
invisible — `text_delta` events stream at the model's emission rate
(~60 tok/s on opus). For tool input it is catastrophic: a multi-KB
`write` `content` body or a multi-edit `edits[].new_text` array drops
the visible cadence to ~0–1 tok/s while the model is generating at
full speed. The user sees a frozen card and assumes the network is
broken; the wire is healthy and bytes simply aren't being released.

The opt-in is **per-tool**, on the request body's tool definition:

```json
{
  "name": "write",
  "description": "...",
  "input_schema": { ... },
  "eager_input_streaming": true
}
```

GA on Claude 4.6 (sonnet-4-6, opus-4-7). For older models — including
haiku-4-5 — the API requires the beta header
`fine-grained-tool-streaming-2025-05-14` in the `anthropic-beta`
cocktail. Sending the header on 4.6+ is a no-op, so we always include
it whenever any tool in the request opts in.

In moha:

- `ToolDef::eager_input_streaming` (registry.hpp) — set `true` on tools
  whose input field can be large enough that batching would be
  visible. Currently `tool_write()` and `tool_edit()`.
- `ToolSpec::eager_input_streaming` (provider.hpp + anthropic
  transport.hpp) — mirrored through the abstract → concrete request
  translation (`AnthropicProvider::stream`).
- `tool_spec_to_json()` (transport.cpp) — emits
  `"eager_input_streaming": true` only when set, so cache-key shape for
  non-streaming tools stays identical to Claude Code's wire layout.
- `select_betas(model, is_oauth, any_eager_streaming)` — appends
  `fine-grained-tool-streaming-2025-05-14` to the beta cocktail when
  any tool in the request opts in. The call site in `run_stream_sync`
  computes `any_eager` via a single `std::ranges::any_of` over
  `req.tools`.

### Cross-references

This is what Zed and Claude Code both do; verifying against either is
the fastest way to check moha's wire shape if streaming feels off:

- **Zed**: `crates/agent/src/tools/streaming_edit_file_tool.rs` — the
  `StreamingEditFileTool` opts in via the trait method
  `fn supports_input_streaming() -> bool { true }`. The Anthropic
  adapter at `crates/anthropic/src/completion.rs:196` maps that to
  `eager_input_streaming` on each `Tool` it serializes.
- **Claude Code (v2.1.113)**: the deobfuscated `do$()` tool serializer
  sets `_.eager_input_streaming = true` when the auth context is
  `firstParty` and either the `tengu_fgts` statsig flag or the
  `CLAUDE_CODE_ENABLE_FINE_GRAINED_TOOL_STREAMING` env var is on.
  The serialized output spreads it conditionally:
  `..._.eager_input_streaming && {eager_input_streaming:!0}`.

### Why this matters for the UI

Eager streaming is what enables the **incremental edit/write card**:
the model emits `partial_json` deltas containing the file body or each
`edits[i].old_text`/`new_text` chunk-by-chunk, and the reducer can
update the card preview as bytes arrive. Without FGTS the card sits
empty until the entire tool input has been buffered server-side, and
no amount of client-side throttling or partial-JSON parsing can make
the card feel alive — the bytes simply aren't there yet.

This is also why moha's edit card renders **all** edits during
streaming, not just the first: with FGTS on, `edits[1].old_text` is
already arriving on the wire while `edits[0].new_text` is still
filling in. The streaming reducer mirrors the entire `edits[]` array
into `tc.args["edits"]` and the EditTool widget paints one diff
section per entry, so every hunk lands live.

### update reactions

```cpp
[&](StreamTextDelta d) -> std::pair<Model, Cmd<Msg>> {
    if (m.current.messages.empty()) return {m, Cmd<Msg>::none()};
    auto& last = m.current.messages.back();
    if (last.role != Role::Assistant) return {m, Cmd<Msg>::none()};
    last.streaming_text += d.text;
    if (m.scroll.at_bottom) m.scroll.offset = scroll::END_SENTINEL;
    return {m, Cmd<Msg>::none()};
},

[&](StreamToolUseStart s) -> std::pair<Model, Cmd<Msg>> {
    if (m.current.messages.empty()) return {m, Cmd<Msg>::none()};
    auto& last = m.current.messages.back();
    ToolUse tc;
    tc.id = s.id; tc.name = s.name;
    tc.status = ToolUse::Status::Pending;
    tc.started_at = std::chrono::steady_clock::now();
    last.tool_calls.push_back(std::move(tc));
    return {m, Cmd<Msg>::none()};
},

[&](StreamToolUseDelta d) -> std::pair<Model, Cmd<Msg>> {
    auto* tc = current_streaming_tool(m);
    if (!tc) return {m, Cmd<Msg>::none()};
    tc->args_streaming += d.partial_json;
    // try parse opportunistically — for live arg display
    try {
        tc->args = nlohmann::json::parse(tc->args_streaming);
    } catch (...) { /* not yet complete */ }
    return {m, Cmd<Msg>::none()};
},

[&](StreamFinished) -> std::pair<Model, Cmd<Msg>> {
    return finalize_turn(std::move(m));
},
```

### The "tool kick" loop

After `StreamFinished`, `finalize_turn` flushes streaming text into
finalized text and walks the last message's tool calls:

```cpp
std::pair<Model, Cmd<Msg>> finalize_turn(Model m) {
    if (!m.current.messages.empty()) {
        auto& last = m.current.messages.back();
        if (!last.streaming_text.empty()) {
            last.text = std::move(last.streaming_text);
            last.streaming_text.clear();
        }
    }
    return kick_pending_tools(std::move(m));
}

std::pair<Model, Cmd<Msg>> kick_pending_tools(Model m) {
    if (m.current.messages.empty()) {
        m.phase = Phase::Idle;
        return {m, flush_queued(m)};
    }
    auto& last = m.current.messages.back();
    if (last.role != Role::Assistant) {
        m.phase = Phase::Idle;
        return {m, flush_queued(m)};
    }

    for (auto& tc : last.tool_calls) {
        if (tc.status != ToolUse::Status::Pending) continue;

        if (tools::needs_permission(tc.name, m.profile)) {
            if (!m.pending_permission) {
                m.pending_permission = make_permission_request(tc, m.profile);
                m.phase = Phase::AwaitingPermission;
                return {m, Cmd<Msg>::none()};
            }
            return {m, Cmd<Msg>::none()};   // already waiting
        }

        // Execute inline
        tc.status = ToolUse::Status::Running;
        m.phase = Phase::ExecutingTool;
        return {m, run_tool_cmd(tc)};
    }

    // All tools have results → re-stream to model
    if (any_tool_results_unflushed(last)) {
        m.phase = Phase::Streaming;
        m.stream_active = true;
        // Append a fresh assistant placeholder
        Message am;
        am.role = Role::Assistant;
        am.timestamp = std::chrono::system_clock::now();
        m.current.messages.push_back(std::move(am));
        return {m, launch_stream_cmd(m)};
    }

    m.phase = Phase::Idle;
    m.stream_active = false;
    return {m, flush_queued(m)};
}
```

This loop is the core of the agent state machine. It's almost identical
to what moha already has — main difference is the additional
`Approved → Running` distinct status for clearer UI feedback.

### run_tool_cmd

```cpp
Cmd<Msg> run_tool_cmd(const ToolUse& tc) {
    return Cmd<Msg>::task([tc](std::function<void(Msg)> dispatch) {
        auto result = tools::execute(tc.name, tc.args);
        dispatch(ToolExecOutput{
            .id = tc.id,
            .output = std::move(result.output),
            .ok = result.ok,
        });
    });
}
```

### Updating ToolExecOutput

```cpp
[&](ToolExecOutput out) -> std::pair<Model, Cmd<Msg>> {
    if (m.current.messages.empty()) return {m, Cmd<Msg>::none()};
    auto& last = m.current.messages.back();
    for (auto& tc : last.tool_calls) {
        if (tc.id != out.id) continue;
        tc.output = std::move(out.output);
        tc.status = out.ok ? ToolUse::Status::Done : ToolUse::Status::Error;
        tc.ended_at = std::chrono::steady_clock::now();
        break;
    }
    return kick_pending_tools(std::move(m));
},
```

### Errors and cancellation

```cpp
[&](StreamError e) -> std::pair<Model, Cmd<Msg>> {
    m.phase = Phase::Idle;
    m.stream_active = false;
    m.stream_error = std::move(e.message);
    return {m, Cmd<Msg>::none()};
},

[&](ComposerStop) -> std::pair<Model, Cmd<Msg>> {
    // Cancel current stream / tool — sent if user presses Stop
    if (m.phase == Phase::Streaming) {
        // tell the task to cancel — see "task cancellation" below
        cancel_in_flight_stream();
    }
    if (m.phase == Phase::ExecutingTool) {
        cancel_running_tool();
    }
    m.phase = Phase::Idle;
    m.stream_active = false;
    return {m, Cmd<Msg>::none()};
},
```

### Task cancellation (subtle)

`Cmd::task` doesn't currently have a built-in cancellation token. To
support stop:

1. Stash a `std::shared_ptr<std::atomic_bool>` cancellation flag in the
   Model when launching the task.
2. The task closure captures the flag and checks it at every SSE
   boundary (cheap — ~dozens per second).
3. `ComposerStop` sets the flag.
4. The task returns early, dispatching nothing more.

Alternatively, `anthropic::run_stream_sync` could accept a cancellation
callback in its `Request` struct. Today neither exists — implementing
this is part of the rebuild work.

## 8. Persistence

`src/persistence.cpp` already implements:

- `~/.moha/threads/<id>.json` — one file per thread, JSON-serialized
  `Thread`
- `~/.moha/settings.json` — model id, profile, favorite models

**Hooks during update**:

- After every `StreamFinished`: save the current thread
- On `NewThread`: save the previous thread, init a fresh one
- On `OpenThread`: save the current, load the requested
- On `DeleteThread`: remove the file
- On settings change: save settings

Saving is a `Cmd::task` to keep `update` pure:

```cpp
Cmd<Msg> save_thread_cmd(Thread t) {
    return Cmd<Msg>::task([t](auto dispatch){
        persistence::save_thread(t);
        // No msg dispatched on success
    });
}
```

If the save fails, it could dispatch a `Toast{ "couldn't save" }` — but
the current persistence layer doesn't surface errors, so this is
optional.

## 9. Where current moha matches & diverges

Match (good — keep these):

- Elm flow: `init / update / view / subscribe`
- Streaming via `Cmd::task` → SSE → Msgs ✓
- `kick_pending_tools` loop ✓
- `pending_permission` field on Model ✓
- Per-thread JSON persistence ✓
- Tool dispatch by name to specialized widgets ✓

Diverge (needs change):

1. **No `Approved` status distinct from `Running`** — add it
2. **No `Cancelled` status** — add it
3. **No `editing` flag on `Message`** — needed for inline edit
4. **No `Selectors` struct** — currently scattered booleans on Model
5. **No `Route` enum** — current implementation is "modal flag" model;
   should be route-based
6. **`scroll` is just a counter** — needs `at_bottom` follow-tail logic
7. **Composer is bare** — no `MentionChip`, `fast_mode`, `thinking`,
   `thinking_effort` fields
8. **No `stream_error` field** — currently surfaced inline in the
   message text (works but hard to clear)
9. **No cancellation flag** — `ComposerStop` can't actually stop
   in-flight requests
10. **No `started_at` / `ended_at` on `ToolUse`** — needed for elapsed
    time display

The `13_rebuild_playbook.md` sequences these in order of dependency.

## 10. The contract this architecture enforces

If `update`, `view`, and `subscribe` follow the rules:

- **Replay** is possible: capturing a stream of `Msg`s lets you replay
  the entire UI history.
- **Snapshot/restore** is trivial: serialize Model, deserialize later.
- **Tests** become pure-function tests: hand a Model + Msg, get a
  Model + Cmd back.
- **Threading is invisible** to UI code: only `Cmd::task` knows
  threads exist.
- **Crashes during streaming** can resume cleanly: persisted Model
  contains all the in-progress state.

The current moha implementation respects this contract about 80% of the
way. The rebuild is mostly about closing the remaining 20% — and using
the resulting clean state to land the visual rebuild.
