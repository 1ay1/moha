#pragma once
// agentty::tool — Tool concept + DynamicDispatch adapter.
//
// The Tool concept describes the static interface every tool module
// exposes: a typed (Args, Result, Effects) bundle plus identity
// (name, description, schema). The runtime dispatcher uses the
// untyped registry edge (ToolDef) to look up + invoke; individual
// tools live behind `util::adapt<Args>(parse, run)` which lifts a
// typed pair into the JSON-typed signature ToolDef holds.

#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/tool/effects.hpp"
#include "agentty/tool/policy.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/spec.hpp"

namespace agentty::tool {

using tools::ToolOutput;
using tools::ToolError;
using tools::ExecResult;
using tools::EffectSet;

namespace detail {

// UTF-8-safe right-trim: never split a multi-byte sequence at the cap
// boundary. UTF-8 leading bytes have the top bits 0xxxxxxx (ASCII) or
// 11xxxxxx; continuation bytes are 10xxxxxx. Walk back from `n` until
// the byte at `n-1` is a leading byte, so the slice ends on a complete
// code point. Bounded to a 4-byte rewind because no UTF-8 character
// occupies more than 4 bytes.
[[nodiscard]] inline std::size_t utf8_safe_floor(const std::string& s,
                                                 std::size_t n) noexcept {
    if (n >= s.size()) return s.size();
    for (std::size_t back = 0; back < 4 && n > 0; ++back, --n) {
        unsigned char c = static_cast<unsigned char>(s[n]);
        if ((c & 0xC0) != 0x80) return n;   // not a continuation byte → safe
    }
    return n;
}

// Same idea but ceiling — first leading byte at or after `n`. Used by
// the Tail strategy so the kept slice STARTS on a complete code point
// rather than a continuation byte.
[[nodiscard]] inline std::size_t utf8_safe_ceil(const std::string& s,
                                                std::size_t n) noexcept {
    if (n >= s.size()) return s.size();
    for (std::size_t fwd = 0; fwd < 4 && n < s.size(); ++fwd, ++n) {
        unsigned char c = static_cast<unsigned char>(s[n]);
        if ((c & 0xC0) != 0x80) return n;
    }
    return n;
}

// Apply a per-tool output budget. budget == 0 means "no cap".
// Returns text untouched if it already fits; otherwise truncates per
// `strategy` and appends a one-line marker the model can read to know
// some output was elided. The marker uses ASCII so it survives any
// downstream encoding pass without further escaping. Strategies:
//
//   Head     — keep first `budget` chars, append "[... N chars elided
//              ...]". Right for tools whose answer is "the requested
//              chunk, in order" (Read, Edit, Write).
//   Tail     — prepend "[... N chars elided ...]\n", keep last `budget`
//              chars. Right for log-stream tools (Bash, Diagnostics).
//   HeadTail — keep first 70% of budget, marker, keep last 30% of
//              budget. Right for tools where both ends carry signal
//              (Grep, Web*, Git diff/log/status).
[[nodiscard]] inline std::string
apply_output_budget(std::string text, int budget,
                    tools::OutputTruncation strategy) {
    if (budget <= 0) return text;
    auto bsz = static_cast<std::size_t>(budget);
    if (text.size() <= bsz) return text;

    using Strat = tools::OutputTruncation;
    switch (strategy) {
        case Strat::Head: {
            auto cut = utf8_safe_floor(text, bsz);
            std::string out = text.substr(0, cut);
            out += std::format("\n\n[... {} chars elided — output exceeded "
                               "tool's budget; refine your request to see "
                               "more ...]",
                               text.size() - cut);
            return out;
        }
        case Strat::Tail: {
            auto cut = utf8_safe_ceil(text, text.size() - bsz);
            std::string out = std::format("[... {} chars elided from start — "
                                          "showing tail of output ...]\n\n",
                                          cut);
            out.append(text, cut, std::string::npos);
            return out;
        }
        case Strat::HeadTail: {
            // 70 / 30 split, both UTF-8-safe.
            auto head_n = (bsz * 7) / 10;
            auto tail_n = bsz - head_n;
            auto head_cut = utf8_safe_floor(text, head_n);
            auto tail_start = utf8_safe_ceil(text, text.size() - tail_n);
            // If the head and tail overlap (small budget on huge text
            // shouldn't happen given catalog values, but be defensive),
            // collapse to plain Head.
            if (tail_start <= head_cut) {
                std::string out = text.substr(0, head_cut);
                out += std::format("\n\n[... {} chars elided ...]",
                                   text.size() - head_cut);
                return out;
            }
            std::string out = text.substr(0, head_cut);
            out += std::format("\n\n[... {} chars elided from middle ...]\n\n",
                               tail_start - head_cut);
            out.append(text, tail_start, std::string::npos);
            return out;
        }
    }
    return text;   // unreachable
}

} // namespace detail

// A Tool is a static-type bundle of identity + schema + effects +
// behavior. The `Args` and `Result` types are exposed as nested
// typedefs so the surface is fully typed at compile time; only the
// dispatcher boundary speaks JSON.
template <class T>
concept Tool = requires {
    typename T::Args;
    typename T::Result;
    { T::name() }         -> std::convertible_to<std::string_view>;
    { T::description() }  -> std::convertible_to<std::string_view>;
    { T::input_schema() } -> std::convertible_to<nlohmann::json>;
    { T::effects() }      -> std::convertible_to<EffectSet>;
} && requires(const nlohmann::json& args) {
    { T::execute(args) }  -> std::convertible_to<ExecResult>;
};

struct DynamicDispatch {
    [[nodiscard]] static const tools::ToolDef* find(std::string_view name) noexcept {
        return tools::find(name);
    }

    // Resolve a tool to its (immortal, since registry.cpp's WireCache keeps
    // every snapshot alive) ToolDef pointer ONCE, so a caller that gates and
    // then executes does both against the SAME definition. This closes the
    // TOCTOU where an `mcp/tools/list_changed` between needs_permission() and
    // execute() could resolve the tool name to two different ToolDefs with
    // different effect sets (gate against benign annotations, run with full
    // effects, or vice-versa). Callers that gate-then-execute should resolve
    // once and pass the pointer to the *_with overloads below.
    [[nodiscard]] static const tools::ToolDef* resolve(std::string_view name) noexcept {
        return tools::find(name);
    }

    // Execute against a pre-resolved ToolDef (see resolve()). Same body as
    // execute(name, args) but skips the second name lookup.
    [[nodiscard]] static ExecResult execute_with(const tools::ToolDef* td,
                                                 std::string_view name,
                                                 const nlohmann::json& args) noexcept {
        if (!td) return std::unexpected(ToolError::not_found(tools::unknown_tool_error(name)));
        static const nlohmann::json kEmpty = nlohmann::json::object();
        const nlohmann::json& safe_args = args.is_object() ? args : kEmpty;
        ExecResult result;
        try {
            result = td->execute(safe_args);
        } catch (const std::exception& e) {
            return std::unexpected(ToolError::unknown(std::string{"tool crashed: "} + e.what()));
        } catch (...) {
            return std::unexpected(ToolError::unknown("tool crashed with a non-standard exception"));
        }
        if (result && td->max_output_chars > 0) {
            result->text = detail::apply_output_budget(
                std::move(result->text), td->max_output_chars,
                td->output_truncation);
        }
        return result;
    }

    // Permission decision against a pre-resolved ToolDef (see resolve()).
    [[nodiscard]] static bool needs_permission_with(const tools::ToolDef* td,
                                                    Profile profile) noexcept {
        if (!td) return true;   // unknown tool fails closed
        return tools::policy::permission(td->effects, profile)
               == tools::policy::Decision::Prompt;
    }

    [[nodiscard]] static ExecResult execute(std::string_view name,
                                            const nlohmann::json& args) noexcept {
        const auto* td = tools::find(name);
        if (!td) return std::unexpected(ToolError::not_found(tools::unknown_tool_error(name)));
        // Avoid copying `args` on the hot path: tools receive an empty object
        // only when the model emitted a non-object (rare). Use a process-
        // lifetime empty json so the reference stays valid either way.
        static const nlohmann::json kEmpty = nlohmann::json::object();
        const nlohmann::json& safe_args = args.is_object() ? args : kEmpty;
        ExecResult result;
        try {
            result = td->execute(safe_args);
        } catch (const std::exception& e) {
            return std::unexpected(ToolError::unknown(std::string{"tool crashed: "} + e.what()));
        } catch (...) {
            return std::unexpected(ToolError::unknown("tool crashed with a non-standard exception"));
        }
        // Per-tool output budget. The catalog declares both a char cap
        // and a truncation strategy; this is the dispatcher-level
        // safety net that catches any tool whose internal limiting
        // failed on pathological input (a Read of a 2000-line file with
        // 5KB lines, a Bash dump of a multi-MB log, a Grep on a runaway
        // pattern). Tools whose `max_output_chars == 0` (todo,
        // git_commit) bypass — their output shape is bounded by
        // construction. Errors aren't truncated: typed ToolError detail
        // strings are short by design and the model needs the full text
        // to recover.
        if (result && td->max_output_chars > 0) {
            result->text = detail::apply_output_budget(
                std::move(result->text), td->max_output_chars,
                td->output_truncation);
        }
        return result;
    }

    // Single source of truth for whether a tool gates on the user.
    // Reads the tool's declared effects, asks the policy. Unknown
    // tools default to "needs permission" (fail closed).
    [[nodiscard]] static bool needs_permission(std::string_view name,
                                               Profile profile) noexcept {
        const auto* td = tools::find(name);
        if (!td) return true;
        return tools::policy::permission(td->effects, profile)
               == tools::policy::Decision::Prompt;
    }

    // Friendly reason text for the permission card. Reads the tool's
    // effects + the active profile to explain why permission is needed.
    [[nodiscard]] static std::string_view permission_reason(
        std::string_view name, Profile profile) noexcept {
        const auto* td = tools::find(name);
        if (!td) return "unknown tool";
        return tools::policy::reason(td->effects, profile);
    }
};

} // namespace agentty::tool
