#pragma once
// moha conversation domain — the pure value types that describe a chat.
//
// No I/O, no UI, no streaming state machine.  A `Thread` is what gets
// persisted, sent to the provider, and displayed.  `Message` and `ToolUse`
// are its building blocks.

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/domain/id.hpp"

// Forward declarations — storing maya types as shared_ptr keeps this header
// from having to pull in the full maya element/streaming headers, which
// would transitively drag layout/render types into every TU.
namespace maya {
    struct Element;
    class  StreamingMarkdown;
}

namespace moha {

enum class Role : std::uint8_t { User, Assistant, System };

[[nodiscard]] constexpr std::string_view to_string(Role r) noexcept {
    switch (r) {
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::System:    return "system";
    }
    return "?";
}

// `ToolUse::Status` is a sum type. Each alternative owns the data that is
// actually meaningful in that state — `Running` holds the live progress
// buffer, `Done`/`Failed` hold the final output, terminal states hold the
// finish time, etc. Storing those fields inline on `ToolUse` (the previous
// design) meant every reader had to remember which fields were valid in
// which state — variant alternatives make that invariant unbreakable.
//
// Wall-clock stamps use steady_clock (not system_clock) so a user changing
// the system clock mid-execution doesn't produce negative elapsed times.
struct ToolUse {
    // Pending carries a started_at because the card shows a live elapsed
    // counter during the args-streaming window too — Anthropic streams the
    // tool input as deltas and a long `content` field can take seconds, so
    // freezing the timer until execution begins reads as "stuck". The
    // timestamp survives the Pending → Running transition (kick_pending_tools
    // reads it via started_at()).
    struct Pending  { std::chrono::steady_clock::time_point started_at{}; };
    struct Approved { std::chrono::steady_clock::time_point started_at{}; };
    struct Running  {
        std::chrono::steady_clock::time_point started_at{};
        // Live stdout+stderr snapshot for a running tool. Shown in the card
        // while status is Running so the user sees progress immediately
        // instead of waiting until the whole command finishes.
        std::string progress_text;
    };
    struct Done {
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point finished_at{};
        std::string output;
    };
    struct Failed {
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point finished_at{};
        std::string output;
    };
    struct Rejected {
        std::chrono::steady_clock::time_point finished_at{};
    };
    using Status = std::variant<Pending, Approved, Running, Done, Failed, Rejected>;

    ToolCallId     id;
    ToolName       name;
    nlohmann::json args;
    std::string    args_streaming;
    // Throttle for the live preview re-parse during input_json_delta. The
    // preview path closes the partial JSON and runs `nlohmann::json::parse`
    // on the entire growing buffer to extract long fields (write `content`,
    // edit `edits[*].old_text`/`new_text`); doing that on every tiny delta
    // is O(n²) and was the dominant CPU cost on a multi-KB write — visible
    // as the UI "hanging" while the wire is healthy. Reducer skips the
    // preview re-parse if less than ~250 ms has passed since the last one.
    std::chrono::steady_clock::time_point last_preview_at{};
    Status         status   = Pending{};
    bool           expanded = true;

    // ── State predicates ─────────────────────────────────────────────────
    [[nodiscard]] bool is_pending()  const noexcept { return std::holds_alternative<Pending>(status);  }
    [[nodiscard]] bool is_approved() const noexcept { return std::holds_alternative<Approved>(status); }
    [[nodiscard]] bool is_running()  const noexcept { return std::holds_alternative<Running>(status);  }
    [[nodiscard]] bool is_done()     const noexcept { return std::holds_alternative<Done>(status);     }
    [[nodiscard]] bool is_failed()   const noexcept { return std::holds_alternative<Failed>(status);   }
    [[nodiscard]] bool is_rejected() const noexcept { return std::holds_alternative<Rejected>(status); }
    [[nodiscard]] bool is_terminal() const noexcept { return is_done() || is_failed() || is_rejected(); }

    // ── State-safe accessors ─────────────────────────────────────────────
    // Return the relevant field for the current state, or an empty/default
    // when the alternative doesn't carry one. Views can rely on these
    // without first checking the discriminator. Returning by const-ref
    // keeps existing call sites that did `.empty()` / `.substr(…)` on the
    // old field unchanged.
    [[nodiscard]] const std::string& output() const noexcept {
        static const std::string empty;
        if (auto* d = std::get_if<Done>(&status))   return d->output;
        if (auto* f = std::get_if<Failed>(&status)) return f->output;
        return empty;
    }
    [[nodiscard]] const std::string& progress_text() const noexcept {
        static const std::string empty;
        if (auto* r = std::get_if<Running>(&status)) return r->progress_text;
        return empty;
    }
    [[nodiscard]] std::chrono::steady_clock::time_point started_at() const noexcept {
        return std::visit([](const auto& s) -> std::chrono::steady_clock::time_point {
            if constexpr (requires { s.started_at; }) return s.started_at;
            else return {};
        }, status);
    }
    [[nodiscard]] std::chrono::steady_clock::time_point finished_at() const noexcept {
        return std::visit([](const auto& s) -> std::chrono::steady_clock::time_point {
            if constexpr (requires { s.finished_at; }) return s.finished_at;
            else return {};
        }, status);
    }

    // String tag for serialization / logging. Stable across versions; the
    // reverse direction lives in persistence.cpp.
    [[nodiscard]] std::string_view status_name() const noexcept {
        return std::visit([](const auto& s) -> std::string_view {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::same_as<T, Pending>)       return "pending";
            else if constexpr (std::same_as<T, Approved>) return "approved";
            else if constexpr (std::same_as<T, Running>)  return "running";
            else if constexpr (std::same_as<T, Done>)     return "done";
            else if constexpr (std::same_as<T, Failed>)   return "failed";
            else                                          return "rejected";
        }, status);
    }

    // Lazy cache of args.dump() for the view. args.dump() is O(args) per call
    // and showed up in per-frame views (thread/permission cards) for tools
    // without a bespoke renderer. Invalidate via mark_args_dirty() whenever
    // `args` is mutated.
    mutable std::string args_dump_cache;
    mutable bool        args_dump_valid = false;

    void mark_args_dirty() {
        args_dump_valid = false;
        args_dump_cache.clear();
        render_cache.reset();
    }
    const std::string& args_dump() const {
        if (!args_dump_valid) {
            args_dump_cache = args.dump();
            args_dump_valid = true;
        }
        return args_dump_cache;
    }

    // Frame-rate render cache. render_tool_call() rebuilds the bordered card
    // (Yoga layout + paint) every frame; for terminal-state tools that's
    // pure waste — the card never changes again.
    mutable std::shared_ptr<maya::Element> render_cache;
    mutable std::uint64_t render_cache_key = 0;

    [[nodiscard]] std::uint64_t compute_render_key() const {
        std::uint64_t k = 1469598103934665603ULL;  // FNV offset basis
        auto mix = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ULL; };
        mix(output().size());
        mix(static_cast<std::uint64_t>(status.index()));
        mix(expanded ? 1ULL : 0ULL);
        return k;
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

struct PendingPermission {
    ToolCallId  id;
    ToolName    tool_name;
    std::string reason;
};

} // namespace moha
