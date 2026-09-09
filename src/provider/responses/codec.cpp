// agentty::provider::responses — the OpenAI Responses-API dialect codec.
//
// This file was EXTRACTED from src/provider/chatgpt/responses.cpp (see the
// git history through the rename): the conversation encoder, the tools
// encoder and the SSE state machine turned out to be entirely host-neutral,
// while only the HTTP envelope differed per backend. It is now shared by
// every host that speaks this dialect — ChatGPT/Codex and GitHub Copilot —
// with per-host differences supplied through a `responses::Site` descriptor.
// See include/agentty/provider/responses/responses.hpp for the contract and
// the measured table of what actually varies between hosts.
#include "agentty/provider/responses/responses.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <format>

#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/stream_scaffold.hpp"
#include "agentty/provider/usage.hpp"
#include "agentty/provider/msg_shared.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/provider/debug.hpp"
#include "agentty/provider/wire/streamed.hpp"
#include "agentty/provider/wire_supersede.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/util/base64.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::responses {
namespace {
using json = nlohmann::json;

// UTF-8 scrub — a pasted blob / tool output can carry invalid UTF-8 that
// nlohmann::dump() would throw on. Replace malformed bytes with U+FFFD.
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* p   = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++p; continue; }
        int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
        if (extra < 0 || p + extra >= end) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k)
            if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        out.append(reinterpret_cast<const char*>(p), extra + 1);
        p += extra + 1;
    }
    return out;
}

} // namespace  (host-neutral helpers above are TU-local)

// ── agentty conversation → Responses `input[]` array ───────────────────────
//
// Rebuilds the whole turn history each call (the Codex backend runs
// store:false, so state is client-side). Every assistant tool_call becomes a
// `function_call` item immediately followed by its `function_call_output`.
json build_input(const provider::Request& req) {
    json input = json::array();
    // Count terminal-carrying tool results so each can be assigned a recency
    // rank (0 = newest) for the shared age-tiered wire budget. Without this a
    // 500 KiB grep or a big-file `read` replays VERBATIM on every subsequent
    // turn, bloating the prompt long before compaction — the same fix every
    // other transport applies via wire::cap_tool_result_aged. Errors never
    // fade (the model may need the full failure to recover), and results
    // already under budget ship as-is.
    int total_tool_results = 0;
    for (const auto& m : req.messages)
        for (const auto& tc : m.tool_calls)
            if (tc.is_terminal()) ++total_tool_results;
    int seen_tool_results = 0;

    // Per-side image ceiling for THIS request (loose by default, tightened
    // once the request crosses the provider's many-image threshold). Computed
    // over the whole message list because the limit counts blocks in the
    // request, not per turn.
    const unsigned max_side =
        util::wire_max_side(wire::wire_image_count(req.messages));
    const auto superseded = wire::superseded_read_ids(req.messages);
    for (const auto& m : req.messages) {
        if (m.role == Role::System) continue;   // folded into `instructions`

        const std::string text = m.attachments.empty()
            ? m.text
            : attachment::expand(m.text, m.attachments);

        if (m.role == Role::User) {
            json content = json::array();
            if (!text.empty())
                content.push_back({
                    {"type", "input_text"}, {"text", scrub_utf8(text)},
                });
            for (const auto* imgp : wire::wire_message_images(m, max_side)) {
                const auto& img = *imgp;
                content.push_back({
                    {"type", "input_image"},
                    {"image_url", "data:" + std::string{wire::wire_media_type(img)}
                                    + ";base64," + util::base64_encode(img.bytes)},
                });
            }
            if (content.empty()) continue;
            input.push_back({
                {"type", "message"}, {"role", "user"},
                {"content", std::move(content)},
            });
            continue;
        }

        // Assistant turn: emit its prose (if any) as an output_text message,
        // then each tool call as function_call + function_call_output.
        //
        // FIRST replay any captured reasoning items. Responses requires the
        // reasoning item to precede the message / function_call items it
        // produced (item-pairing invariant). Under store:false we send only
        // `encrypted_content` — NOT the server `id` (echoing a rs_… id makes
        // the backend do a lookup that 404s on a non-persisted response).
        if (!m.reasoning_encrypted.empty()) {
            std::size_t start = 0;
            while (start <= m.reasoning_encrypted.size()) {
                std::size_t nl = m.reasoning_encrypted.find('\n', start);
                std::string blob = m.reasoning_encrypted.substr(
                    start, nl == std::string::npos ? std::string::npos : nl - start);
                if (!blob.empty())
                    input.push_back({
                        {"type", "reasoning"},
                        {"summary", json::array()},
                        {"encrypted_content", std::move(blob)},
                    });
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }

        if (!text.empty()) {
            input.push_back({
                {"type", "message"}, {"role", "assistant"},
                {"content", json::array({
                    json{{"type", "output_text"}, {"text", scrub_utf8(text)}}})},
            });
        }
        for (const auto& tc : m.tool_calls) {
            std::string args = tc.args.is_null() ? "{}" : tc.args.dump();
            input.push_back({
                {"type", "function_call"},
                {"call_id", tc.id.value},
                {"name", tc.name.value},
                {"arguments", scrub_utf8(args)},
            });
            // The result the host produced for this call (may be pending if
            // the turn is still in flight — then we skip, the model re-requests).
            if (tc.is_terminal()) {
                // Age-tiered wire budget (shared with every other transport):
                // newest results keep the full budget, stale successes fade to
                // a tight head+tail so a big dump stops replaying every turn.
                const int recency_rank =
                    total_tool_results - 1 - seen_tool_results;
                ++seen_tool_results;
                const bool is_error = tc.is_failed() || tc.is_rejected();
                std::string out = (!is_error && superseded.count(tc.id.value))
                    ? std::string{wire::kSupersededReadPointer}
                    : wire::cap_tool_result_aged(
                          tc.output(), recency_rank, is_error);
                // Vision tool_result: when the tool surfaced images (read on an
                // image file), the Responses API accepts `output` as an ARRAY
                // of content parts — the text followed by input_image parts —
                // instead of a plain string. Same SSOT selector as every other
                // dialect; only the JSON shape here is OpenAI-specific.
                auto imgs = wire::wire_tool_result_images(tc, max_side);
                if (imgs.empty()) {
                    input.push_back({
                        {"type", "function_call_output"},
                        {"call_id", tc.id.value},
                        {"output", scrub_utf8(out)},
                    });
                } else {
                    json parts = json::array();
                    if (!out.empty())
                        parts.push_back({{"type", "output_text"},
                                         {"text", scrub_utf8(out)}});
                    for (const auto* imgp : imgs) {
                        const auto& img = *imgp;
                        parts.push_back({
                            {"type", "input_image"},
                            {"image_url",
                             "data:" + std::string{wire::wire_media_type(img)}
                                 + ";base64," + util::base64_encode(img.bytes)},
                        });
                    }
                    input.push_back({
                        {"type", "function_call_output"},
                        {"call_id", tc.id.value},
                        {"output", std::move(parts)},
                    });
                }
            }
        }
    }
    return input;
}

json build_tools(const provider::Request& req) {
    json tools = json::array();
    for (const auto& t : req.tools) {
        // Responses API function tool is FLAT (name/description/parameters at
        // top level), unlike Chat Completions' nested {function:{...}}. The
        // null-schema guard is shared with the Chat/Ollama encoder (SSOT).
        tools.push_back({
            {"type", "function"},
            {"name", t.name},
            {"description", t.description},
            {"parameters", wire::tool_schema_or_empty(t.input_schema)},
        });
    }
    return tools;
}

// The NEUTRAL Responses request body. Everything here is true of the
// dialect itself; anything true of only ONE backend belongs in that host's
// Site::decorate_body instead (ChatGPT's store:false + encrypted-reasoning
// include[], Copilot's reasoning.summary mode, …).
//
// `model` is deliberately NOT defaulted here: hosts can rewrite the slug
// (Copilot's Auto session picks a server-blessed model), so the caller's
// Target::model is authoritative and stream() stamps it after decoration.
json build_body(const provider::Request& req) {
    json body{
        {"model", req.model},
        {"instructions", scrub_utf8(req.system_prompt)},
        {"input", build_input(req)},
        {"tool_choice", "auto"},
        {"parallel_tool_calls", true},
        {"stream", true},
    };
    if (auto tools = build_tools(req); !tools.empty()) body["tools"] = tools;
    // Reasoning ladder. `summary: auto` is what makes a reasoning model
    // return human-readable summary text (response.reasoning_summary_text.*)
    // rather than silently burning thinking tokens — measured on both
    // ChatGPT and Copilot.
    if (!req.effort.empty())
        body["reasoning"] = json{{"effort", req.effort}, {"summary", "auto"}};
    else
        body["reasoning"] = json{{"summary", "auto"}};
    return body;
}

// ── SSE dispatch state ─────────────────────────────────────────────────────
// One open function_call item. `args` accumulates from all four carriers the
// Responses dialect may use — `arguments` on output_item.added, the .delta
// fragments, the .done snapshot, and the item snapshot on output_item.done.
// unseen() makes the redundant ones no-ops, so we route every carrier and
// none can be the one we forgot. (Copilot sends ONLY .done; we used to drop
// it and dispatch every tool call with `{}`.)
struct ToolSlot {
    std::string call_id;   // the id a tool_result must echo
    std::string args;
};

struct StreamCtx {
    EventSink sink;
    wire::SseFramer sse;
    // item.id (fc_…) → the open call's slot. Keyed by item_id because that is
    // what argument events carry; the slot holds the call_id they must be
    // forwarded under.
    std::unordered_map<std::string, ToolSlot> tools;
    std::unordered_set<std::string> open_tool_items;
    std::string latest_tool_item;   // fallback for older events without item_id
    bool text_block_open = false;
    bool saw_function_call = false;
    bool terminated = false;
    StopReason stop = StopReason::EndTurn;
    // Diagnostic: how many reasoning-bearing events arrived this turn.
    // Lets a "reasoning is not showing" report be split into its two REAL
    // causes from the log alone: 0 → the server never sent summary text
    // (model/account tier doesn't emit it), >0 → it arrived and the loss is
    // client-side (reducer/render). Surfaced in the end-of-turn Debug line.
    int thinking_deltas = 0;
};

// Route an argument payload from ANY carrier into the slot and forward only
// what is newly known. `total` = the server's complete view, not a fragment.
void feed_tool_args(StreamCtx& ctx, const std::string& item_id,
                    std::string_view payload, bool total) {
    const auto it = ctx.tools.find(item_id);
    if (it == ctx.tools.end()) {
        // An argument payload we can't route is a tool call about to fail
        // as "[invalid args]" — the model DID send the arguments, we just
        // couldn't attach them (item never opened / id mismatch / a carrier
        // event shape we don't know). Exactly the `.done` bug's failure
        // shape; must never be silent.
        AGT_LOG(Wire, Warn, "responses.tool_args_unroutable",
                "item_id={} known_items={} bytes={}",
                item_id, ctx.tools.size(), payload.size());
        return;
    }
    const auto fresh = wire::unseen(it->second.args, payload, total);
    if (!fresh.empty())
        ctx.sink(StreamToolUseDelta{ToolCallId{it->second.call_id},
                                    std::string{fresh}});
}

void close_tool(StreamCtx& ctx, const std::string& item_id) {
    if (item_id.empty() || !ctx.open_tool_items.erase(item_id)) return;
    if (const auto it = ctx.tools.find(item_id); it != ctx.tools.end())
        ctx.sink(StreamToolUseEnd{ToolCallId{it->second.call_id}});
    if (ctx.latest_tool_item == item_id) {
        ctx.latest_tool_item = ctx.open_tool_items.empty()
            ? std::string{} : *ctx.open_tool_items.begin();
    }
}

void close_all_tools(StreamCtx& ctx) {
    std::vector<std::string> ids(ctx.open_tool_items.begin(),
                                 ctx.open_tool_items.end());
    for (const auto& id : ids) close_tool(ctx, id);
}

void emit_usage(StreamCtx& ctx, const json& usage) {
    // Shared extractor — see usage::from_responses (single source of truth for
    // the Responses/Codex usage shape).
    if (auto su = usage::from_responses(usage)) ctx.sink(*su);
}

void dispatch(StreamCtx& ctx, std::string_view data) {
    if (data.empty() || data == "[DONE]") return;
    json j;
    try { j = json::parse(data); } catch (const std::exception& e) {
        // A frame we could not even parse is a DROPPED wire event — the
        // single most expensive thing to lose silently (a dropped
        // output_text/function_call frame reads as "model went mute" or
        // "invalid args", never as what it is). Warn: on by default in
        // release, and lands in the crash-ring flight recorder.
        AGT_LOG(Wire, Warn, "responses.frame_unparseable",
                "err={} bytes={} head={}", e.what(), data.size(),
                data.substr(0, 256));
        return;
    }

    const auto type = j.value("type", std::string{});

    if (type == "response.output_text.delta") {
        if (!ctx.text_block_open) ctx.text_block_open = true;
        ctx.sink(StreamTextDelta{j.value("delta", std::string{})});
        return;
    }
    if (type == "response.reasoning_summary_text.delta"
        || type == "response.reasoning_text.delta") {
        ++ctx.thinking_deltas;
        ctx.sink(StreamThinkingDelta{j.value("delta", std::string{}), {}});
        return;
    }
    // Summary PART boundary: the Responses API splits a reasoning summary
    // into parts (one per paragraph) and emits several reasoning ITEMS per
    // response (one before each tool call). Their text deltas would
    // otherwise concatenate with no separator ("…thought one.Start of
    // thought two…"). Emit a block boundary so the reducer inserts the
    // paragraph break — same event Anthropic uses for a new thinking block.
    if (type == "response.reasoning_summary_part.added") {
        ctx.sink(StreamThinkingDelta{{}, {}, /*block_boundary=*/true});
        return;
    }
    if (type == "response.output_item.added") {
        const auto& item = j.value("item", json::object());
        const auto itype  = item.value("type", std::string{});
        if (itype == "reasoning") {
            // New reasoning item — paragraph boundary for its summary text
            // (see above). Harmless if the item produces no visible text.
            ctx.sink(StreamThinkingDelta{{}, {}, /*block_boundary=*/true});
        }
        if (itype == "function_call") {
            // A new tool call opens. Close any prior text block first so the
            // reveal cursor snaps before the card (matches Anthropic seam).
            if (ctx.text_block_open) {
                ctx.text_block_open = false;
                ctx.sink(StreamTextBlockClosed{});
            }
            const std::string item_id = item.value("id", std::string{});
            const std::string call_id = item.value("call_id", item_id);
            const std::string name    = item.value("name", std::string{});
            ctx.tools[item_id] = ToolSlot{call_id, {}};
            ctx.open_tool_items.insert(item_id);
            ctx.latest_tool_item = item_id;
            ctx.saw_function_call = true;
            ctx.sink(StreamToolUseStart{ToolCallId{call_id}, ToolName{name}});
            // Carrier (1): some backends deliver the whole args string
            // up-front on `added`. A snapshot, so route it as a total.
            if (const auto a = item.value("arguments", std::string{}); !a.empty())
                feed_tool_args(ctx, item_id, a, /*total=*/true);
        }
        return;
    }
    // Carrier (2): incremental fragments. What Codex sends.
    if (type == "response.function_call_arguments.delta") {
        feed_tool_args(ctx, j.value("item_id", ctx.latest_tool_item),
                       j.value("delta", std::string{}), /*total=*/false);
        return;
    }
    // Carrier (3): the authoritative snapshot that terminates the argument
    // stream. GitHub Copilot's proxy coalesces and sends ONLY this, so a
    // codec that ignores it dispatches every tool call with `{}`. For a
    // server that also streamed fragments this is a no-op, because
    // observe_total() returns just the unseen suffix (usually nothing).
    if (type == "response.function_call_arguments.done") {
        feed_tool_args(ctx, j.value("item_id", ctx.latest_tool_item),
                       j.value("arguments", std::string{}), /*total=*/true);
        return;
    }
    if (type == "response.output_item.done") {
        const auto& item = j.value("item", json::object());
        const auto itype = item.value("type", std::string{});
        if (itype == "function_call") {
            const std::string item_id = item.value("id", std::string{});
            // Carrier (4): the completed item may restate the full arguments.
            // Last chance to learn them before the call is dispatched — a
            // no-op when an earlier carrier already supplied them.
            if (const auto a = item.value("arguments", std::string{}); !a.empty())
                feed_tool_args(ctx, item_id, a, /*total=*/true);
            close_tool(ctx, item_id);
        }
        else if (itype == "reasoning") {
            // A reasoning item completed. Capture its opaque encrypted_content
            // so the reducer can stash it on the assistant message and replay
            // it next turn (chain-of-thought continuity across tool rounds
            // under store:false). The visible summary already streamed via
            // reasoning_summary_text.delta → StreamThinkingDelta.
            if (auto enc = item.value("encrypted_content", std::string{});
                !enc.empty())
                ctx.sink(StreamReasoning{std::move(enc)});
            if (ctx.text_block_open) {
                ctx.text_block_open = false;
                ctx.sink(StreamTextBlockClosed{});
            }
        }
        else if (ctx.text_block_open) {
            ctx.text_block_open = false;
            ctx.sink(StreamTextBlockClosed{});
        }
        return;
    }
    if (type == "response.completed") {
        close_all_tools(ctx);
        const auto& resp = j.value("response", json::object());
        emit_usage(ctx, resp.value("usage", json::object()));
        // No finish_reason on the wire — a function_call in the output means
        // the model wants tool results before continuing.
        ctx.stop = ctx.saw_function_call ? StopReason::ToolUse : StopReason::EndTurn;
        AGT_LOG(Wire, Debug, "responses.completed",
                "stop={} tool_calls={} thinking_deltas={}",
                ctx.saw_function_call ? "tool_use" : "end_turn",
                ctx.tools.size(), ctx.thinking_deltas);
        ctx.sink(StreamFinished{ctx.stop});
        ctx.terminated = true;
        return;
    }
    if (type == "response.incomplete") {
        close_all_tools(ctx);
        const auto& resp = j.value("response", json::object());
        emit_usage(ctx, resp.value("usage", json::object()));
        const auto reason = resp.value("incomplete_details", json::object())
                                .value("reason", std::string{});
        // Warn: an incomplete response is a truncated turn the user WILL
        // notice; the wire reason ("max_output_tokens", "content_filter"…)
        // is the answer to their "why did it stop mid-sentence?" report.
        AGT_LOG(Wire, Warn, "responses.incomplete", "reason={}", reason);
        ctx.stop = reason == "max_output_tokens" ? StopReason::MaxTokens
                                                  : StopReason::EndTurn;
        ctx.sink(StreamFinished{ctx.stop});
        ctx.terminated = true;
        return;
    }
    if (type == "response.failed" || type == "error") {
        close_all_tools(ctx);
        std::string msg;
        // Surface the error TYPE/CODE alongside the message. The runtime's
        // classify(string_view) sniffs this text to decide retryability
        // (e.g. "rate_limit", "429", "overloaded", "server_error") — dropping
        // the type would misclassify a transient overload as terminal and
        // skip the auto-retry that Anthropic/OpenAI get. Mirror the wire's
        // in-band error shape: `{type|code}: {message}`.
        auto compose = [](const json& err) {
            std::string m = err.value("message", std::string{});
            std::string tag = err.value("type", err.value("code", std::string{}));
            if (!tag.empty()) return m.empty() ? tag : tag + ": " + m;
            return m;
        };
        if (type == "error") {
            // A top-level `error` event: its own `type` field is the SSE event
            // discriminator ("error"), so the meaningful classifier token is
            // `code` (e.g. "rate_limit_exceeded", "server_error"). Some
            // variants nest the detail under `error`; handle both.
            const json& err = j.contains("error") && j["error"].is_object()
                                  ? j["error"] : j;
            std::string m   = err.value("message", j.value("message", std::string{}));
            std::string tag = err.value("code", err.value("type", std::string{}));
            if (tag == "error") tag.clear();   // never the event discriminator
            msg = tag.empty() ? m : (m.empty() ? tag : tag + ": " + m);
            if (msg.empty()) msg = "stream error";
        } else {
            const auto& err = j.value("response", json::object())
                                  .value("error", json::object());
            msg = compose(err);
            if (msg.empty()) msg = "Codex request failed";
        }
        // The full wire error event, not just the composed one-liner: the
        // event may carry fields compose() doesn't surface (param, request
        // id) that decide a provider-side ticket vs a client bug.
        AGT_LOG(Wire, Warn, "responses.error_event", "msg={} raw={}",
                msg, data.substr(0, 2048));
        ctx.sink(StreamError{msg, std::nullopt});
        ctx.terminated = true;
        return;
    }
    // response.created / in_progress / content_part.* / *_summary_part.* etc.
    // are structural — nothing to render. Bump liveness so the stall watchdog
    // knows the wire is healthy during a long reasoning pass.
    //
    // Anything we don't recognise lands here too, and silence is what made the
    // Copilot bug so expensive: `.done` carried the tool arguments, we ignored
    // it, and the only symptom was "[invalid args] pattern required" — which
    // reads as a bad model, not a dropped event. Falling through is still the
    // right BEHAVIOUR (an unknown event is usually structural); it just must
    // not be invisible. One debug line per event type, deduped, so a new
    // dialect quirk names itself instead of being blamed on the model.
    if (!type.empty() && !type.starts_with("response.created")
        && !type.starts_with("response.in_progress")
        && !type.starts_with("response.content_part")
        && !type.starts_with("response.output_text.done")
        && !type.contains("_summary_part")) {
        static std::mutex           seen_mu;
        static std::set<std::string> seen;
        std::lock_guard lk(seen_mu);
        if (seen.insert(type).second)
            util::dbglog("responses.unhandled_event", type);
    }
    ctx.sink(StreamHeartbeat{});
}

// ── The shared transport ──────────────────────────────────────────────────
//
// Everything above this line is host-neutral. Everything a HOST differs on
// arrives through `site`: where to POST, with what credentials, what extra
// body fields, and how to phrase an HTTP error. The loop itself — SSE
// framing, error-body capping, retry-after capture, watchdog timeouts and
// the shared epilogue — is identical for every Responses backend, which is
// the whole reason this module exists.
provider::StreamResult stream(const Site& site, provider::Request req,
                              provider::EventSink sink) {
    sink(StreamStarted{});

    // Host resolves credentials + destination (may block on a token refresh,
    // and may rewrite req.model — e.g. Copilot's Auto session picking a
    // server-blessed slug). Auth prose is the host's: this layer never
    // invents remediation text it can't be sure of.
    auto target = site.authorize(req);
    if (!target) {
        sink(StreamError{target.error()});
        return provider::StreamResult::failed(
            std::string{site.id} + ": " + target.error());
    }

    http::Request hr;
    hr.method  = http::HttpMethod::Post;
    hr.host    = target->host;
    hr.port    = target->port;
    hr.path    = target->path;
    hr.headers = target->headers;
    // The SSE anti-buffering trio every streaming transport in agentty sends.
    http::append_sse_no_buffer(hr.headers);

    try {
        json body = build_body(req);
        // Host-specific fields layer on top of the neutral body.
        if (site.decorate_body) site.decorate_body(body, req);
        // The host's model choice is authoritative (see build_body).
        if (!target->model.empty()) body["model"] = target->model;
        hr.body = body.dump();
    } catch (const std::exception& e) {
        sink(StreamError{std::string{"could not encode request: "} + e.what()});
        return provider::StreamResult::failed("could not encode request");
    }

    StreamCtx ctx;
    ctx.sink = sink;
    // Shared scaffold: status/Retry-After capture, heartbeats, buffered-wait,
    // wire dump, capped error body. Our feed stops the read once dispatch()
    // fired the terminal event (deliberate post-`response.completed` abort —
    // a latency win the epilogue understands as AlreadyTerminated).
    provider::StreamScaffold sc;
    sc.dialect = "openai-responses";
    sc.sink    = sink;
    sc.feed    = [&](std::string_view chunk) {
        ctx.sse.feed(chunk.data(), chunk.size(),
            [&](std::string_view, std::string_view payload, char*) {
                dispatch(ctx, payload);
            });
        return !ctx.terminated;
    };

    http::Timeouts tos = provider::stream_timeouts();

    auto result = http::default_client().stream(hr, sc.handler(), tos, req.cancel);

    // Uniform end-of-turn pair (Debug summary + Warn raw error body) with the
    // dialect-specific counters appended.
    sc.log_result(bool(result),
                  result ? std::string_view{} : result.error().render(),
                  std::format("terminated={} thinking_deltas={}",
                              ctx.terminated ? 1 : 0, ctx.thinking_deltas));

    // End the turn through the SHARED epilogue so every Responses host
    // finishes identically to Anthropic/OpenAI/Ollama. The critical case is
    // AlreadyTerminated: when a `response.completed` frame fired StreamFinished
    // inside dispatch(), on_chunk returned false to stop reading (a deliberate
    // latency win), which the HTTP layer reports as an aborted / "cancelled"
    // transfer. finish_stream treats that as EXPECTED (emits nothing), avoiding
    // the spurious StreamError{"cancelled"} that used to show after clean turns.
    return provider::finish_stream({
        .terminated  = ctx.terminated,
        .sink        = sink,
        .result_ok   = bool(result),
        .http_status = sc.http_status,
        .non_replayable = !result && result.error().non_replayable,
        .cancel      = req.cancel,
        .stop        = ctx.stop,
        .http_error_message = [&]() -> std::string {
            return site.explain_http_error(sc.http_status, sc.error_body);
        },
        .retry_after = sc.retry_after_hint,
        .transport_error_message = [&]() -> std::string {
            return result.error().render();
        },
    });
}

// ── Codec accessors (shared by hosts and tests) ───────────────────────────
std::vector<Msg> parse_sse_for_test(const std::vector<std::string>& sse_data_lines) {
    std::vector<Msg> out;
    StreamCtx ctx;
    ctx.sink = [&](Msg m) { out.push_back(std::move(m)); };
    for (const auto& line : sse_data_lines) dispatch(ctx, line);
    return out;
}

} // namespace agentty::provider::responses
