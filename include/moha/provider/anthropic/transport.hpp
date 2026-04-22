#pragma once
// moha::provider::anthropic — the wire layer that talks to api.anthropic.com.
// Impersonates the Claude Code CLI on the wire so OAuth tokens are accepted;
// see memory/project_claude_code_wire.md for the full header/body contract.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/auth/auth.hpp"
#include "moha/domain/catalog.hpp"
#include "moha/domain/conversation.hpp"
#include "moha/io/http.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::provider::anthropic {

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    bool eager_input_streaming = false;
};

struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    // Always set by AnthropicProvider from provider::Request (which owns the
    // default). No initializer here — keeping one risks the same silent-
    // override bug that masked the cap on long write/edit calls.
    int max_tokens;

    std::string auth_header;                 // "Bearer <t>" or raw API key
    auth::Style auth_style = auth::Style::ApiKey;
};

using EventSink = std::function<void(Msg)>;

// Runs a streaming request synchronously on the calling thread. Each SSE event
// is dispatched through `sink` as a Msg. Returns when the stream closes.
// `cancel` is polled at frame boundaries; tripping it sends RST_STREAM and
// returns within ~200 ms with a StreamError("cancelled") if no cleaner
// terminal event has arrived first.
void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel = {});

// Build the Anthropic-shaped messages array from our Thread.
[[nodiscard]] nlohmann::json build_messages(const Thread& t);

// Standard system prompt with env info.
[[nodiscard]] std::string default_system_prompt();

// Tool specs corresponding to our local tool implementations.
[[nodiscard]] std::vector<ToolSpec> default_tools();

// Fetch available models from Anthropic API.
[[nodiscard]] std::vector<ModelInfo> list_models(const std::string& auth_header,
                                                 auth::Style auth_style);

} // namespace moha::provider::anthropic
