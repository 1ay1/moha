#pragma once
// moha::provider::anthropic::AnthropicProvider — the concrete adapter that
// satisfies the `provider::Provider` concept by translating the abstract
// request into an anthropic::transport call.

#include <utility>

#include "moha/provider/provider.hpp"
#include "moha/provider/anthropic/transport.hpp"

namespace moha::provider::anthropic {

class AnthropicProvider {
public:
    void stream(provider::Request req, provider::EventSink sink) {
        Request areq;
        areq.model         = std::move(req.model);
        areq.system_prompt = std::move(req.system_prompt);
        areq.messages      = std::move(req.messages);
        areq.max_tokens    = req.max_tokens;
        areq.auth_header   = std::move(req.auth_header);
        areq.auth_style    = req.auth_style;
        areq.tools.reserve(req.tools.size());
        for (auto& t : req.tools)
            areq.tools.push_back({std::move(t.name),
                                  std::move(t.description),
                                  std::move(t.input_schema)});
        run_stream_sync(std::move(areq), std::move(sink), std::move(req.cancel));
    }
};

static_assert(provider::Provider<AnthropicProvider>);

} // namespace moha::provider::anthropic
