// agentty::provider::anthropic — wire body builder.
//
// Extracted verbatim from transport.cpp (byte-identity guarded by
// wire_golden_test). This is the request-body serializer: the streaming
// string-backed message-array writer (json_write_* helpers, the CachePin
// breakpoint model, the per-block writers) and messages_json_string() /
// build_messages(). Pure serialization — no SSE, no network, no StreamCtx.
//
// Kept as its own TU so the largest chunk of transport.cpp's hot path lives
// in a focused module, matching the ChatGPT provider's split.

#include "agentty/provider/anthropic/transport.hpp"

#include "agentty/provider/wire.hpp"            // wire::scrub_utf8 / cap_tool_result_aged
#include "agentty/provider/msg_shared.hpp"      // wire::is_assistant_with_results
#include "agentty/provider/wire_supersede.hpp"  // wire::superseded_read_ids / kSupersededReadPointer
#include "agentty/runtime/composer_attachment.hpp"  // attachment::expand
#include "agentty/util/base64.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::provider::anthropic {

using nlohmann::json;
using wire::scrub_utf8;

// ── Streaming string-backed message-array writer ─────────────────────────
//
// Building the messages array via nlohmann::json was the largest hidden
// allocation in the request hot path. The `{"input", tc.args.is_object()
// ? tc.args : json::object()}` initializer-list deep-copies tc.args; for
// a `write` tool whose `content` field is a 1 MiB file body, that's a
// 1 MiB recursive json clone followed by another 1+ MiB allocation when
// `body.dump()` re-serializes it. Two big copies per request, paid again
// on every retry.
//
// `messages_json_string` writes the messages array directly into a
// std::string buffer, JSON-escaping inline. The win lands on tc.args:
// tc.args_dump() already caches the serialized form (used by the view
// for permission cards), so we splice those bytes verbatim into the
// "input" field. No clone, no re-parse, no re-dump. For unrecoverable
// edge cases (an args object that somehow lost its dump cache) we fall
// back to an on-demand dump rather than copying through json.
//
// Cache-breakpoint pinning (the `pin_last_block` helper that mutates
// the last content block of the last + second-to-last messages) is now
// done inline during write — we count messages first, then know in
// advance which are the pin-eligible ones.
namespace {

// True whenever an assistant message carries ANY tool_calls. Anthropic
// requires that every `tool_use` block be followed by a matching
// `tool_result` in the next message — sending the tool_use without its
// pair returns HTTP 400 ("`tool_use` ids were found without
// `tool_result` blocks immediately after") and, because the broken
// transcript is replayed on every subsequent turn, the session
// becomes wedged. We therefore emit the follow-up user turn whenever
// there's a tool_use to pair, even if some of them are still in a
// non-terminal state (Pending / Approved / Running). The non-terminal
// branches get a synthesized placeholder result downstream so the wire
// stays valid; the in-memory ToolUse status is left untouched.
[[nodiscard]] inline bool is_assistant_with_results(const Message& m) noexcept {
    return wire::is_assistant_with_results(m);
}

// True iff the message carries at least one sendable image. Delegates to the
// shared SSOT (wire::has_wire_message_image) so the empty-bytes skip, the size
// ceiling and the role gate live in ONE place across all dialects. `max_side`
// is the request-wide ceiling (see wire::wire_image_count).
[[nodiscard]] inline bool has_wire_image(const Message& m, unsigned max_side) noexcept {
    return wire::has_wire_message_image(m, max_side);
}

void json_write_escaped_string(std::string& out, std::string_view s) {
    out.push_back('"');
    out.reserve(out.size() + s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out.append("\\\"", 2); break;
            case '\\': out.append("\\\\", 2); break;
            case '\b': out.append("\\b",  2); break;
            case '\f': out.append("\\f",  2); break;
            case '\n': out.append("\\n",  2); break;
            case '\r': out.append("\\r",  2); break;
            case '\t': out.append("\\t",  2); break;
            default:
                if (c < 0x20) {
                    // \u00XX for control bytes.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out.append(buf, 6);
                } else {
                    // Printable + UTF-8 multibyte: passthrough. We
                    // assume the caller already scrub_utf8'd inputs
                    // (text bodies, tool outputs, args), so multi-byte
                    // sequences here are well-formed.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void json_write_field(std::string& out, std::string_view key,
                      std::string_view value, bool& first) {
    if (!first) out.push_back(',');
    first = false;
    json_write_escaped_string(out, key);
    out.push_back(':');
    json_write_escaped_string(out, value);
}

// Splice raw pre-serialized JSON into a value slot (no escaping).
void json_write_raw_field(std::string& out, std::string_view key,
                          std::string_view raw_value, bool& first) {
    if (!first) out.push_back(',');
    first = false;
    json_write_escaped_string(out, key);
    out.push_back(':');
    out.append(raw_value);
}

void json_write_bool_field(std::string& out, std::string_view key,
                           bool v, bool& first) {
    json_write_raw_field(out, key, v ? "true" : "false", first);
}

// Cache-control markers for prompt caching. Compile-time strings so we
// don't pay re-serialization on every breakpoint. Two TTLs:
//   • kCacheCtlJsonRaw    — default 5-minute ephemeral. Used on the ROLLING
//     breakpoint (the newest message), which changes every turn anyway.
//   • kCacheCtl1hJsonRaw  — 1-hour ephemeral (needs beta_extended_cache_ttl).
//     Used on STABLE breakpoints (the conversation-prefix anchor) so a long
//     idle window doesn't force a full-price re-cache of the whole prefix.
constexpr std::string_view kCacheCtlJsonRaw   = R"({"type":"ephemeral"})";
constexpr std::string_view kCacheCtl1hJsonRaw = R"({"type":"ephemeral","ttl":"1h"})";

// Cache TTL choice for a breakpoint. NotPinned = no cache_control at all.
enum class CachePin : std::uint8_t { NotPinned, Ttl5m, Ttl1h };

[[nodiscard]] constexpr std::string_view cache_ctl_for(CachePin p) noexcept {
    switch (p) {
        case CachePin::Ttl1h: return kCacheCtl1hJsonRaw;
        case CachePin::Ttl5m: return kCacheCtlJsonRaw;
        default:              return {};
    }
}

void json_write_cache_control(std::string& out, CachePin pin, bool& first) {
    if (pin == CachePin::NotPinned) return;
    json_write_raw_field(out, "cache_control", cache_ctl_for(pin), first);
}

void write_text_block(std::string& out, std::string_view text, CachePin pin) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "text", first);
    json_write_field(out, "text", text, first);
    json_write_cache_control(out, pin, first);
    out.push_back('}');
}

// Anthropic image content block:
//   {"type":"image","source":{"type":"base64",
//                              "media_type":"image/png","data":"..."}}
// `data` is standard RFC-4648 base64 (NOT base64url). We encode the
// bytes once at write time — keeping them raw in `ImageContent.bytes`
// avoids the +33% memory overhead in the running model state.
void write_image_block(std::string& out, const ImageContent& img,
                       CachePin pin) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "image", first);
    if (!first) out.push_back(',');
    first = false;
    out.append(R"("source":{"type":"base64",)");
    out.append(R"("media_type":)");
    json_write_escaped_string(out, wire::wire_media_type(img));
    out.append(R"(,"data":)");
    json_write_escaped_string(out, util::base64_encode(img.bytes));
    out.push_back('}');
    json_write_cache_control(out, pin, first);
    out.push_back('}');
}

void write_tool_use_block(std::string& out, const ToolUse& tc, CachePin pin) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "tool_use", first);
    json_write_field(out, "id",   tc.id.value, first);
    json_write_field(out, "name", tc.name.value, first);
    // Splice the cached args dump verbatim. args_dump() guarantees a
    // valid JSON-object string ("{}" minimum, never empty); fall back
    // to a fresh dump if for any reason the cache is in an unexpected
    // shape (defensive — shouldn't fire in practice).
    std::string_view dump = tc.args_dump();
    std::string fallback;
    if (dump.empty() || dump.front() != '{') {
        fallback = tc.args.is_object() ? tc.args.dump() : std::string{"{}"};
        dump = fallback;
    }
    json_write_raw_field(out, "input", dump, first);
    json_write_cache_control(out, pin, first);
    out.push_back('}');
}

// Age-tiered tool-result wire budget (Anthropic "tool result clearing").
// The full transcript is immutable; only the WIRE copy of each tool_result
// is sized by how RECENT the call is. The policy — budgets, the recency
// window, the errors-never-fade / short-ships-verbatim invariants, and the
// head+tail cap itself — lives once in wire::cap_tool_result_aged (shared by
// all three transports). `recency_rank` is 0 for the newest terminal tool
// result in the thread and grows toward the oldest.
void write_tool_result_block(std::string& out, const ToolUse& tc,
                             CachePin pin, int recency_rank,
                             bool superseded, unsigned max_side) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "tool_result", first);
    json_write_field(out, "tool_use_id", tc.id.value, first);
    // Non-terminal tools (Pending / Approved / Running) carry no
    // output yet. We still MUST emit a tool_result for them — see
    // is_assistant_with_results above for the wire-shape rationale —
    // so synthesize an `is_error: true` placeholder. Marking it as an
    // error tells the model the call didn't actually produce a result,
    // which is the truthful read of "the previous turn died before
    // this tool finished." Empty Done output stays as the historical
    // "(no output)" placeholder (not an error) for tools that
    // legitimately produced nothing.
    auto raw_output = tc.output();
    std::string scrubbed;
    const bool non_terminal = !tc.is_terminal();
    const bool is_error = non_terminal || tc.is_failed() || tc.is_rejected();
    if (non_terminal) {
        json_write_field(out, "content",
            "(tool call did not complete \u2014 previous turn ended before this tool produced a result)",
            first);
    } else if (raw_output.empty()) {
        json_write_field(out, "content", "(no output)", first);
    } else if (superseded) {
        // A LATER turn read/edited/wrote this same file, so this earlier
        // read's body is stale — the model already has fresher state for it.
        // Collapse it to a deterministic one-line pointer NOW instead of
        // waiting for age-fading (kFullResultWindow turns) to shrink it. In a
        // read-heavy coding loop the same files get touched repeatedly, so
        // this reclaims the single largest source of dead wire weight. The
        // pointer text is FIXED (no byte counts / positions) so a given
        // superseded read always serialises identically — no cache churn.
        json_write_field(out, "content",
            std::string{wire::kSupersededReadPointer}, first);
    } else if (!wire::wire_tool_result_images(tc).empty()) {
        // Vision tool_result: Anthropic accepts an ARRAY of content blocks for a
        // tool_result, so emit the text (aged-budgeted) followed by one image
        // block per surfaced image. The model reads them in order — text note
        // first, then the picture(s). Image bytes never fade (they're the
        // point of the call), only the text does.
        std::string capped = wire::cap_tool_result_aged(raw_output, recency_rank, is_error);
        scrubbed = scrub_utf8(capped);
        if (!first) out.push_back(',');
        first = false;
        out.append("\"content\":[");
        bool block_first = true;
        // Text block (skip if the tool produced no text at all).
        if (!scrubbed.empty()) {
            out.push_back('{');
            bool tf = true;
            json_write_field(out, "type", "text", tf);
            json_write_field(out, "text", scrubbed, tf);
            out.push_back('}');
            block_first = false;
        }
        for (const auto* imgp : wire::wire_tool_result_images(tc, max_side)) {
            if (!block_first) out.push_back(',');
            block_first = false;
            write_image_block(out, *imgp, CachePin::NotPinned);
        }
        out.push_back(']');
    } else {
        // Pick the wire budget from recency. Recent results (and ALL error
        // results, at any age) keep the full budget so the model can act on
        // them; stale successful results fade to a tight head+tail so a
        // 60 KiB read from 30 calls ago stops replaying in full every turn.
        std::string capped = wire::cap_tool_result_aged(raw_output, recency_rank, is_error);
        scrubbed = scrub_utf8(capped);
        json_write_field(out, "content", scrubbed, first);
    }
    json_write_bool_field(out, "is_error", is_error, first);
    json_write_cache_control(out, pin, first);
    out.push_back('}');
}

} // namespace

[[nodiscard]] std::string messages_json_string(const Thread& t,
                                               bool include_thinking) {
    // Read-collapse analysis: earlier reads whose file a later turn touched
    // again are stale on the wire and get a one-line pointer instead of their
    // full body (see wire::superseded_read_ids). Deterministic, so it never
    // churns the prompt cache.
    const auto superseded = wire::superseded_read_ids(t);

    // The many-image ceiling is a property of the REQUEST, not of any single
    // picture, so it is computed once here over the whole thread and passed
    // down to every gate. Counting per-message instead would let a 20-image
    // session apply the loose cap to each turn individually and still ship a
    // request the provider rejects.
    const unsigned max_side = util::wire_max_side(wire::wire_image_count(t.messages));

    // First pass: figure out where the cache breakpoints land. cli.js
    // pins BOTH the last and second-to-last *emitted* messages' last
    // content blocks (rolling cache reuse — turn N's last becomes turn
    // N+1's second-to-last). A "message" here is whatever lands in the
    // output array, so an Assistant turn with terminal tool_calls
    // contributes TWO messages (assistant + tool_results follow-up).
    int total_msgs = 0;
    // Also count how many terminal tool results the thread carries so we can
    // assign each a recency rank (0 = newest) for the age-tiered wire budget
    // in write_tool_result_block. Only terminal results with non-empty
    // output participate in fading; the count is a ceiling, ranks are
    // assigned as we emit below.
    int total_tool_results = 0;
    for (const auto& m : t.messages) {
        const bool has_images = (m.role == Role::User && has_wire_image(m, max_side));
        if (!m.text.empty()
         || has_images
         || (m.role == Role::Assistant && !m.tool_calls.empty())) {
            ++total_msgs;
        }
        if (is_assistant_with_results(m)) {
            ++total_msgs;
            total_tool_results += static_cast<int>(m.tool_calls.size());
        }
    }
    const int pin_last       = total_msgs - 1;
    const int pin_second_last = total_msgs - 2;

    // ── Stable 1-hour anchor breakpoint ─────────────────────────────────
    // The last / second-to-last pins ROLL every turn: turn N's last block
    // becomes turn N+1's second-to-last, so those two breakpoints only ever
    // cache the two newest messages. Everything BEFORE them is a cache HIT
    // only if some earlier turn's breakpoint still covers it — and under the
    // 5-minute default TTL that coverage evaporates the moment the user
    // pauses. The result: a long session re-pays full-price cache-creation
    // for the entire history tail after every idle gap.
    //
    // Fix (Claude Code's `Dt6` shape): plant a THIRD breakpoint at a STABLE
    // position deep in history and give it the 1-HOUR ttl. Because it barely
    // moves, the whole prefix up to it stays a cache hit across turns AND
    // across long idle windows. To keep it from moving every turn (which
    // would defeat the purpose), we QUANTIZE its position to a multiple of
    // kAnchorStep messages — it only advances once every kAnchorStep new
    // messages, and each advance re-caches at most kAnchorStep messages once.
    //
    // The anchor must land strictly before pin_second_last so the three
    // breakpoints are distinct (Anthropic allows up to 4 cache_control blocks
    // total: system + tools + these); when history is too short to fit a
    // distinct anchor we simply don't emit one (the rolling pair already
    // covers everything).
    constexpr int kAnchorStep = 20;
    const int pin_anchor = [&]() -> int {
        if (total_msgs < 2 * kAnchorStep) return -1;   // too short: no distinct anchor
        // Anchor at the largest multiple of kAnchorStep that leaves at least a
        // full step of headroom before the rolling pair. Keying off
        // (total_msgs - kAnchorStep) rather than (total_msgs - 2) means the
        // floored value only advances once per FULL step of growth — appending
        // one turn never moves it, so the cached prefix up to the anchor holds
        // across those turns. When it does advance, it re-caches one step once.
        int a = ((total_msgs - kAnchorStep) / kAnchorStep) * kAnchorStep;
        if (a <= 0 || a >= pin_second_last) return -1;
        return a;
    }();

    std::string out;
    // Conservative reserve: typical sessions are ~64 KiB; a write turn
    // can push past 1 MiB. Either way, let the std::string growth
    // strategy take it from here without an early reallocation.
    out.reserve(64 * 1024);
    out.push_back('[');

    int emitted = 0;
    // Running count of tool results emitted so far (oldest first). The
    // recency rank of the next result is total_tool_results-1-emitted, so
    // the LAST-emitted (newest) result gets rank 0.
    int tool_results_emitted = 0;
    auto emit_msg_open = [&] {
        if (emitted > 0) out.push_back(',');
        ++emitted;
    };
    // Anthropic allows a MAXIMUM of 4 cache_control breakpoints per request;
    // extras are silently dropped from the FRONT (oldest first), which would
    // sacrifice our most valuable pin — the system prompt. The full body
    // already spends 2 of the 4 slots on the stable prefix (system + tools),
    // leaving 2 for the messages array. So:
    //   • anchor present  → anchor (1h) + last (5m)   [2 slots — drop the
    //     second-to-last rolling pin; the anchor already covers everything
    //     up to it, so that pin bought almost nothing]
    //   • no anchor       → last + second-to-last (5m) [the classic rolling
    //     pair, still 2 slots]
    // Either branch keeps the messages array at ≤ 2 breakpoints → ≤ 4 total.
    const bool have_anchor = (pin_anchor >= 0);
    auto pinning_for = [&](int idx) -> CachePin {
        // The anchor is the STABLE, long-lived breakpoint → 1-hour TTL.
        if (idx == pin_anchor)  return CachePin::Ttl1h;
        // Newest message: always a rolling 5-minute pin.
        if (idx == pin_last)    return CachePin::Ttl5m;
        // Second-to-last rolling pin only when there is NO anchor (otherwise
        // it would push the request to 5 breakpoints and evict the system
        // prompt from the cache).
        if (!have_anchor && idx == pin_second_last) return CachePin::Ttl5m;
        return CachePin::NotPinned;
    };
    // A block that is NOT the last block of a pinned message must stay
    // unpinned; helper folds the "only the message's last block carries the
    // marker" rule together with the per-message TTL choice.
    auto pin_if_last = [](CachePin msg_pin, bool last_block) -> CachePin {
        return last_block ? msg_pin : CachePin::NotPinned;
    };

    for (const auto& m : t.messages) {
        // ── Primary message (text + tool_use blocks if Assistant) ──
        const bool has_text   = !m.text.empty();
        const bool has_images = (m.role == Role::User && has_wire_image(m, max_side));
        const bool has_tools  = (m.role == Role::Assistant && !m.tool_calls.empty());
        // Replay captured thinking block(s) on assistant turns that also
        // carry real content (text or tool_use). Anthropic requires the
        // blocks be present and verbatim on the turn whose tool_use they
        // precede, or the request 400s. Interleaved thinking can produce
        // SEVERAL signed blocks per message — `thinking_blocks` is the
        // authoritative in-order capture; the legacy (thinking,
        // thinking_signature) pair covers threads persisted before the
        // multi-block split. Only SIGNED blocks are replayed (an unsigned
        // thinking block is rejected), gated on the request enabling
        // thinking (include_thinking).
        int signed_blocks = 0;
        if (include_thinking && m.role == Role::Assistant) {
            for (const auto& tb : m.thinking_blocks)
                if (!tb.signature.empty() || !tb.redacted_data.empty())
                    ++signed_blocks;
            if (signed_blocks == 0 && !m.thinking_signature.empty())
                signed_blocks = 1;  // legacy single-pair fallback
        }
        const bool has_thinking = signed_blocks > 0 && (has_text || has_tools);
        if (has_text || has_images || has_tools) {
            const int my_idx   = emitted;
            const CachePin do_pin = pinning_for(my_idx);
            emit_msg_open();
            out.push_back('{');
            out.append(R"("role":)");
            out.append(m.role == Role::User ? R"("user")" : R"("assistant")");
            out.append(R"(,"content":[)");
            // Anthropic accepts mixed content arrays. Emit images
            // FIRST so the prose that references them ("describe this
            // screenshot") follows in the same content array — the
            // model reads in array order and benefits from having the
            // visual context loaded before the prompt text. Then the
            // text block, then any tool_use blocks (Assistant turns).
            // EMPTY-bytes images are skipped entirely: a stray
            // empty ImageContent (e.g. a draft attachment whose bytes
            // were already drained) would serialize an empty base64
            // "data" field and 400 the whole request.
            // Count the images that will ACTUALLY be emitted, using the very
            // gate the emission loop below uses. These two must agree exactly:
            // `blocks` decides which block is last, and the last block is
            // where the cache breakpoint gets pinned. If the count includes an
            // image the emitter drops (oversize), `block_emitted` never reaches
            // `blocks`, no block is pinned, and the request silently loses its
            // cache breakpoint. Previously this counted non-empty bytes while
            // the emitter applied the size ceiling too — they could diverge.
            const int wire_images =
                has_images
                    ? static_cast<int>(wire::wire_message_images(m, max_side).size())
                    : 0;
            int blocks = (has_thinking ? signed_blocks : 0)
                       + wire_images
                       + (has_text ? 1 : 0)
                       + (has_tools ? static_cast<int>(m.tool_calls.size()) : 0);
            int block_emitted = 0;
            // Thinking blocks go FIRST — the model emits them before its
            // text/tool_use, and the replay order must match. They are never
            // the cache pin (content always follows them). json(...).dump()
            // JSON-encodes the (possibly empty) thinking text + opaque
            // signature; no cache_control on a thinking block.
            if (has_thinking) {
                auto write_thinking = [&](const std::string& txt,
                                          const std::string& sig) {
                    if (block_emitted++ > 0) out.push_back(',');
                    out.append(R"({"type":"thinking","thinking":)");
                    out.append(json(scrub_utf8(txt)).dump());
                    out.append(R"(,"signature":)");
                    out.append(json(sig).dump());
                    out.push_back('}');
                };
                bool any_vec = false;
                for (const auto& tb : m.thinking_blocks) {
                    if (!tb.redacted_data.empty()) {
                        // Safety-redacted block: opaque, replayed verbatim as
                        // its own content type (thinking-shaped replay 400s).
                        if (block_emitted++ > 0) out.push_back(',');
                        out.append(R"({"type":"redacted_thinking","data":)");
                        out.append(json(tb.redacted_data).dump());
                        out.push_back('}');
                        any_vec = true;
                    } else if (!tb.signature.empty()) {
                        write_thinking(tb.text, tb.signature);
                        any_vec = true;
                    }
                }
                if (!any_vec)  // legacy single-pair fallback
                    write_thinking(m.thinking, m.thinking_signature);
            }
            if (has_images) {
                for (const auto* imgp : wire::wire_message_images(m, max_side)) {
                    const auto& img = *imgp;
                    if (block_emitted++ > 0) out.push_back(',');
                    const bool last_block = (block_emitted == blocks);
                    write_image_block(out, img, pin_if_last(do_pin, last_block));
                }
            }
            if (has_text) {
                if (block_emitted++ > 0) out.push_back(',');
                const bool last_block = (block_emitted == blocks);
                // Expand chip placeholders (\x01ATT:N\x01) into
                // their attachment bodies so the model sees the
                // literal pasted text / file contents. The
                // transcript renderer keeps the chip form for the
                // user; only the wire payload sees the full bytes.
                // No-op when m.attachments is empty (no expansion
                // needed and no allocation either).
                std::string wire_text = m.attachments.empty()
                    ? m.text
                    : attachment::expand(m.text, m.attachments);
                write_text_block(out, scrub_utf8(wire_text), pin_if_last(do_pin, last_block));
            }
            if (has_tools) {
                for (const auto& tc : m.tool_calls) {
                    if (block_emitted++ > 0) out.push_back(',');
                    const bool last_block = (block_emitted == blocks);
                    write_tool_use_block(out, tc, pin_if_last(do_pin, last_block));
                }
            }
            out.append("]}");
        }

        // ── Tool-result follow-up (synthetic User turn) ──
        // Emit one tool_result per tool_use, terminal or not. The
        // wire shape Anthropic enforces is pairwise (every tool_use
        // id must appear as a tool_use_id in the next message), so
        // we cannot selectively drop the non-terminal ones — that's
        // exactly what triggered the HTTP 400 loop.
        if (is_assistant_with_results(m)) {
            const int my_idx   = emitted;
            const CachePin do_pin = pinning_for(my_idx);
            emit_msg_open();
            out.append(R"({"role":"user","content":[)");
            const int total_results = static_cast<int>(m.tool_calls.size());
            int result_emitted = 0;
            for (const auto& tc : m.tool_calls) {
                if (result_emitted++ > 0) out.push_back(',');
                const bool last_block = (result_emitted == total_results);
                const int recency_rank =
                    total_tool_results - 1 - tool_results_emitted;
                ++tool_results_emitted;
                const bool is_superseded = superseded.count(tc.id.value) != 0;
                write_tool_result_block(out, tc, pin_if_last(do_pin, last_block),
                                        recency_rank, is_superseded, max_side);
            }
            out.append("]}");
        }
    }

    out.push_back(']');
    return out;
}

// Compatibility shim: the public signature still returns json (callers
// outside transport.cpp's hot path may depend on it). The hot path uses
// `messages_json_string` directly.
json build_messages(const Thread& t) {
    return json::parse(messages_json_string(t, /*include_thinking=*/false));
}

} // namespace agentty::provider::anthropic
