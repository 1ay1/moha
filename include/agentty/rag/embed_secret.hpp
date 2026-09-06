#pragma once
// agentty::rag::embed — storage for the embeddings API key.
//
// Separate from embed_backend.hpp on purpose: that header is pure config
// (usable from tests, the picker, and the wire path with no IO), while this
// one touches the keystore and the filesystem. Keeping the split means the
// config layer can be reasoned about without dragging in credential IO.
//
// The key is keyed by ENDPOINT rather than globally, so pointing two projects
// at two different gateways cannot leak one's credential into the other's
// request. It is never written to settings.json — see embed_secret.cpp.

#include <string>

#include "agentty/rag/embed_backend.hpp"

namespace agentty::rag::embed {

// The storage key for this config's endpoint, or "" when the backend takes
// no credential (in-process models, Ollama, llama.cpp).
[[nodiscard]] std::string endpoint_key(const EmbedConfig& c);

// Fetch the stored key for an endpoint ("" when none). Never throws.
[[nodiscard]] std::string load_key(const std::string& endpoint);

// Persist (or, with an empty key, erase). Returns false if the secret could
// not be secured — callers must treat that as "not saved" rather than
// silently writing plaintext.
bool store_key(const std::string& endpoint, const std::string& key);

bool erase_key(const std::string& endpoint);

} // namespace agentty::rag::embed
