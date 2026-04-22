#pragma once
// moha::session — the state machine for a single in-flight LLM request.
//
// This is pure domain (no sockets, no curl), but it's strictly more volatile
// than the conversation types: `Phase` flips on every delta, `StreamState`
// owns a cancel handle that tears down the live HTTP/2 stream. It lives in
// its own header so editors and view-layer code that only need Thread /
// Message don't transitively pull in the http:: forward-decl.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <maya/widget/spinner.hpp>

namespace moha::http { class CancelToken; }

namespace moha {

// Phase is a sum type — exactly one of the four states is true at a time.
// Modeling it as a variant of empty alternatives (rather than an enum) lets
// us express that intent in the type system and use `std::visit` for
// exhaustiveness rather than relying on the compiler's switch warning.
namespace phase {
struct Idle               {};
struct Streaming          {};
struct AwaitingPermission {};
struct ExecutingTool      {};
} // namespace phase
using Phase = std::variant<phase::Idle, phase::Streaming,
                           phase::AwaitingPermission, phase::ExecutingTool>;

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
    Phase phase  = phase::Idle{};
    bool  active = false;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_tick{};
    int tokens_in   = 0;
    int tokens_out  = 0;
    int context_max = 200000;
    std::string status;
    maya::Spinner<maya::SpinnerStyle::Dots> spinner{};
    // How many times the current user turn has been transparently retried
    // because the upstream stream cut off mid-tool-input. Reset on every
    // fresh user submit. Capped (see kMaxTruncationRetries in update.cpp)
    // so a persistently broken upstream surfaces as a real error eventually
    // instead of looping forever.
    int truncation_retries = 0;
    // ── Live tok/s speedometer ──────────────────────────────────────────
    // Anthropic only emits `message_delta.usage.output_tokens` rarely (often
    // just once before message_stop), so the official `tokens_out` is stale
    // for nearly the whole stream. We instead accumulate the byte length of
    // every text/json delta as it arrives, divide by ~4 (Claude tokenizer
    // averages ~3.5–4 bytes/token), and use that as the live rate source.
    // `first_delta_at` is stamped on the first non-empty delta so the
    // divisor excludes time-to-first-token (TTFT). Both reset on every
    // StreamStarted so each sub-turn after a tool exec measures cleanly.
    std::size_t live_delta_bytes = 0;
    std::chrono::steady_clock::time_point first_delta_at{};
    // Cancel handle for the in-flight HTTP/2 stream. Set when launch_stream
    // dispatches the worker; nulled when the terminal Msg lands. Tripping it
    // from the UI thread (Msg::CancelStream) tears the stream down within a
    // few hundred ms.
    std::shared_ptr<moha::http::CancelToken> cancel;

    // ── Phase predicates ─────────────────────────────────────────────────
    [[nodiscard]] bool is_idle()                const noexcept { return std::holds_alternative<phase::Idle>(phase); }
    [[nodiscard]] bool is_streaming()           const noexcept { return std::holds_alternative<phase::Streaming>(phase); }
    [[nodiscard]] bool is_awaiting_permission() const noexcept { return std::holds_alternative<phase::AwaitingPermission>(phase); }
    [[nodiscard]] bool is_executing_tool()      const noexcept { return std::holds_alternative<phase::ExecutingTool>(phase); }
};

} // namespace moha
