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
};

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    int max_tokens = 32000;

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
