#pragma once
// moha::session — per-turn state for a single in-flight LLM request.
// See docs/design/streaming.md for the full design rationale.

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <maya/widget/spinner.hpp>

namespace moha::http { class CancelToken; }

namespace moha {

namespace phase {
struct Idle               {};
struct Streaming          {};
struct AwaitingPermission {};
struct ExecutingTool      {};
} // namespace phase
using Phase = std::variant<phase::Idle, phase::Streaming,
                           phase::AwaitingPermission, phase::ExecutingTool>;

// ── Retry / stall watchdog state machine ──────────────────────
// Used to be two parallel bools (`stall_dispatched`, `retry_pending`)
// that together encoded a 3-state machine; the invariants were comment-
// only and hand-maintained across ~6 sites in the reducer. Now a proper
// sum type:
//
//   Fresh        — stream alive, watchdog armed.
//   StallFired   — watchdog tripped the cancel token, synthetic
//                  StreamError is in flight. The worker thread's late
//                  StreamError("cancelled") must be re-classified as
//                  Transient, not user-cancel.
//   Scheduled    — StreamError handler scheduled a RetryStream via
//                  Cmd::after. A second StreamError arriving during the
//                  wait must NOT schedule another retry.
//
// Transitions:
//   Fresh       → StallFired  (Tick watchdog detects dead stream)
//   Fresh       → Scheduled   (StreamError fires directly, non-stall)
//   StallFired  → Scheduled   (the synthetic StreamError schedules retry)
//   Scheduled   → Fresh       (RetryStream fires, fresh stream begins)
//   any         → Fresh       (CancelStream, StreamStarted reset)
namespace retry {
struct Fresh      {};
struct StallFired {};
struct Scheduled  {};
} // namespace retry
using RetryState = std::variant<retry::Fresh, retry::StallFired, retry::Scheduled>;

[[nodiscard]] constexpr std::string_view to_string(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return "idle";
        else if constexpr (std::same_as<T, phase::Streaming>)          return "streaming";
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return "permission";
        else                                                           return "working";
    }, p);
}

struct StreamState {
    Phase phase = phase::Idle{};
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_tick{};
    int tokens_in   = 0;
    int tokens_out  = 0;
    int context_max = 200000;
    std::string status;
    // Optional expiry for `status`. When set, the status bar hides the
    // banner once now() passes this point and the reducer treats the
    // field as empty. Used for toast-style transient messages
    // (retrying, cancelled, checkpoint-restore-not-implemented, …) so
    // they don't linger forever. A default-constructed time_point
    // (epoch=0) means "no expiry" — the status stays until something
    // else writes over it.
    std::chrono::steady_clock::time_point status_until{};

    // True iff `status` is set AND either has no expiry or hasn't expired yet.
    [[nodiscard]] bool status_active(std::chrono::steady_clock::time_point now) const noexcept {
        if (status.empty()) return false;
        if (status_until.time_since_epoch().count() == 0) return true;
        return now < status_until;
    }
    maya::Spinner<maya::SpinnerStyle::Dots> spinner{};
    int truncation_retries = 0;
    // Transient-error retry counter (overloaded / 429 / 5xx / network
    // drop). Reset at the start of each user turn. Independent of
    // truncation_retries so a turn that hits both retries fairly.
    int transient_retries = 0;

    // Live tok/s speedometer — bytes of text/json delta, not the rare
    // usage field. first_delta_at excludes TTFT from the rate divisor.
    std::size_t live_delta_bytes = 0;
    std::chrono::steady_clock::time_point first_delta_at{};

    // Sparkline ring buffer for the status-bar trend glyphs.
    static constexpr std::size_t kRateSamples = 12;
    std::array<float, kRateSamples> rate_history{};
    std::size_t rate_history_pos = 0;
    bool rate_history_full = false;
    std::chrono::steady_clock::time_point rate_last_sample_at{};
    std::size_t rate_last_sample_bytes = 0;

    std::shared_ptr<moha::http::CancelToken> cancel;

    // ── Stream-stall watchdog ─────────────────────────────────────
    // Bumped on every SSE event the reducer processes (StreamStarted,
    // TextDelta, ToolUseStart/Delta/End, Usage). The Tick handler reads
    // it and force-fires a Transient error if no event has arrived for
    // longer than the stall threshold. Catches the case the http idle
    // timeout misses: server keeps the TCP/h2 connection alive (PING
    // ACKs reset `last_rx`) but stops sending application-level events.
    std::chrono::steady_clock::time_point last_event_at{};

    // Retry/stall machine state. See the RetryState definition above
    // for the full transition diagram. Replaces the legacy bool pair
    // (stall_dispatched + retry_pending) which encoded the same 3-state
    // progression with no help from the type system.
    RetryState retry_state = retry::Fresh{};

    // Predicates — read sites stay terse and self-documenting:
    //   if (m.s.in_stall_fired()) reclassify_cancel_as_transient();
    //   if (m.s.in_scheduled())   skip_double_retry();
    [[nodiscard]] bool in_fresh()       const noexcept { return std::holds_alternative<retry::Fresh>(retry_state); }
    [[nodiscard]] bool in_stall_fired() const noexcept { return std::holds_alternative<retry::StallFired>(retry_state); }
    [[nodiscard]] bool in_scheduled()   const noexcept { return std::holds_alternative<retry::Scheduled>(retry_state); }

    // ── Phase predicates ─────────────────────────────────────────────────
    [[nodiscard]] bool is_idle()                const noexcept { return std::holds_alternative<phase::Idle>(phase); }
    [[nodiscard]] bool is_streaming()           const noexcept { return std::holds_alternative<phase::Streaming>(phase); }
    [[nodiscard]] bool is_awaiting_permission() const noexcept { return std::holds_alternative<phase::AwaitingPermission>(phase); }
    [[nodiscard]] bool is_executing_tool()      const noexcept { return std::holds_alternative<phase::ExecutingTool>(phase); }

    // Derived: "is anything actively in flight?" — true whenever the
    // session is in any non-Idle phase. This used to be a parallel
    // `bool active` field that callers had to keep in lock-step with
    // `phase`; deriving it eliminates the invariant ("active is true
    // iff phase != Idle") that the type system couldn't enforce.
    // Spinner ticks, watchdogs, and the live-elapsed indicators all
    // gate on this — there is no longer any case where one wants
    // `active == true && phase == Idle` or vice versa.
    [[nodiscard]] bool active() const noexcept { return !is_idle(); }
};

} // namespace moha
