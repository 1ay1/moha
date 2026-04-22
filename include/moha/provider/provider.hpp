#pragma once
// moha::provider — the abstraction over "something that streams a chat
// completion".  A Provider is domain, not infrastructure: the conversation
// speaks to a Provider, and a Provider happens to speak HTTP+SSE to an
// Anthropic endpoint (or, in tests, to a deterministic in-memory script).
//
// The runtime never names a concrete type; anything satisfying the concept
// is accepted.

#include <concepts>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/auth/auth.hpp"
#include "moha/domain/conversation.hpp"
#include "moha/io/http.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::provider {

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    // Anthropic's fine-grained tool streaming flag — see ToolDef in
    // tool/registry.hpp for the full story. Mirrored on the wire as
    // `eager_input_streaming: true` per tool when set.
    bool eager_input_streaming = false;
};

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    // Single source of truth for the per-request output cap. 16384 matches
    // Claude Code v2.1.113's main-loop config (the binary's docs explicitly
    // warn that `max_tokens > ~16000` puts traffic on a slower path that
    // risks SDK HTTP timeouts). Earlier 64000 default was correlated with
    // 20-30 s mid-stream pauses on long write/edit calls.
    //
    // Trade-off: a single tool_use whose `content` field exceeds ~12-13k
    // tokens of file body will hit the cap and arrive truncated (model burns
    // its budget streaming input_json_delta, then stop_reason=max_tokens).
    // For most edits/writes this is fine; bump per-request for known-huge
    // generations.
    int max_tokens = 16384;

    std::string auth_header;
    auth::Style auth_style = auth::Style::ApiKey;

    // Optional cancellation handle. Tripping the token from the UI thread
    // tears down the in-flight stream within ~200 ms. Null means uncancellable.
    http::CancelTokenPtr cancel;
};

using EventSink = std::function<void(Msg)>;

// ── The contract every provider satisfies ──────────────────────────────────
template <class P>
concept Provider = requires(P& p, Request req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<void>;
};

} // namespace moha::provider
