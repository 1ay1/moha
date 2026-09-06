#pragma once
// agentty::rag::embed — the embedding BACKEND boundary.
//
// Retrieval used to be hardwired to a local Ollama daemon: `{"type":"ollama"}`
// was spelled literally at four sites in adapter.cpp (docs attach, code attach,
// warm probe, rag-bench), and the only tuning was AGENTTY_OLLAMA_HOST. That is
// an opinionated default worth KEEPING — but it was also the only reachable
// option, which locks out anyone who cannot run a background daemon (locked-down
// corporate machines) or who already has an internal embeddings gateway.
//
// This header makes the backend a VALUE. rag-cpp already registers every
// factory we need (see rag-cpp/src/plugin/builtins.cpp); agentty's job is only
// to name one and hand over its keys. So this file is deliberately thin:
//
//   * `Backend`  — the closed set of backends agentty exposes.
//   * `EmbedConfig` — the user-facing knobs, one struct.
//   * `spec_for()`  — EmbedConfig -> rag-cpp plugin Json. The SINGLE place a
//                     spec is minted; adapter.cpp holds no literal any more.
//   * `identity()`  — a hash of everything that defines the VECTOR SPACE.
//   * `validate()`  — pure precondition check, rendered by the picker.
//
// ── Why `dim` is not here ────────────────────────────────────────────────
// A user-supplied dimension is a loaded footgun. rag-cpp's HNSW index drops
// mismatched vectors SILENTLY:
//
//     void HnswIndex::add(std::uint32_t id, std::span<const float> vec) {
//         if (dim_ == 0) dim_ = vec.size();
//         if (vec.size() != dim_ || dim_ == 0) return;   // ignored!
//
// so a wrong dim yields an empty index, no error, and retrieval quietly
// degrading to BM25 forever. The dimension is therefore DERIVED — probe()
// embeds one string and reads the vector's size. It is a result, never an
// input, and the type system reflects that: there is no `dim` field to set.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace agentty::rag::embed {

// ── The backends agentty exposes ─────────────────────────────────────────
// Deliberately a small closed set, not a registry table. Each maps onto a
// factory rag-cpp already registers. `Auto` is not a backend — it is a
// POLICY (prefer Ollama, degrade to BM25) that spec_for() expands.
enum class Backend : std::uint8_t {
    Auto,        // opinionated default: local Ollama if it answers, else BM25
    Ollama,      // explicit local/remote Ollama daemon
    OpenAI,      // ANY OpenAI-compatible /v1/embeddings host
    LlamaCpp,    // llama.cpp server /embedding (also what a llamafile serves)
    Gguf,        // in-process GGUF — no daemon, no network, no install
    Onnx,        // in-process ONNX Runtime
    Disabled,    // lexical only: BM25/keyword, never embed
};

inline constexpr int kBackendCount = 7;
inline constexpr Backend kBackends[kBackendCount] = {
    Backend::Auto,   Backend::Ollama, Backend::OpenAI, Backend::LlamaCpp,
    Backend::Gguf,   Backend::Onnx,   Backend::Disabled,
};

[[nodiscard]] constexpr std::string_view id_of(Backend b) noexcept {
    switch (b) {
        case Backend::Auto:     return "auto";
        case Backend::Ollama:   return "ollama";
        case Backend::OpenAI:   return "openai";
        case Backend::LlamaCpp: return "llamacpp";
        case Backend::Gguf:     return "gguf";
        case Backend::Onnx:     return "onnx";
        case Backend::Disabled: return "off";
    }
    return "auto";
}

[[nodiscard]] constexpr std::string_view label_of(Backend b) noexcept {
    switch (b) {
        case Backend::Auto:     return "Auto";
        case Backend::Ollama:   return "Ollama";
        case Backend::OpenAI:   return "Custom endpoint";
        case Backend::LlamaCpp: return "llama.cpp server";
        case Backend::Gguf:     return "GGUF (in-process)";
        case Backend::Onnx:     return "ONNX (in-process)";
        case Backend::Disabled: return "Off";
    }
    return "Auto";
}

[[nodiscard]] constexpr std::string_view blurb_of(Backend b) noexcept {
    switch (b) {
        case Backend::Auto:     return "local Ollama if reachable, else keyword-only";
        case Backend::Ollama:   return "Ollama daemon, local or remote";
        case Backend::OpenAI:   return "any OpenAI-compatible /v1/embeddings host";
        case Backend::LlamaCpp: return "llama.cpp / llamafile server endpoint";
        case Backend::Gguf:     return "no daemon — model file loaded in-process";
        case Backend::Onnx:     return "no daemon — ONNX model loaded in-process";
        case Backend::Disabled: return "keyword search only (BM25)";
    }
    return "";
}

[[nodiscard]] constexpr Backend backend_from_id(std::string_view id) noexcept {
    for (Backend b : kBackends)
        if (id_of(b) == id) return b;
    return Backend::Auto;
}

// Does this backend reach the network / a daemon at all? Drives the "no
// daemon required" badge in the picker — the single fact KhazAkar's request
// actually turns on.
[[nodiscard]] constexpr bool is_in_process(Backend b) noexcept {
    return b == Backend::Gguf || b == Backend::Onnx;
}
[[nodiscard]] constexpr bool needs_endpoint(Backend b) noexcept {
    return b == Backend::Ollama || b == Backend::OpenAI || b == Backend::LlamaCpp;
}
[[nodiscard]] constexpr bool needs_model_path(Backend b) noexcept {
    return b == Backend::Gguf || b == Backend::Onnx;
}
// Only a remote HTTP API takes a bearer token. Ollama and llama.cpp servers
// are unauthenticated in every deployment we target.
[[nodiscard]] constexpr bool needs_api_key(Backend b) noexcept {
    return b == Backend::OpenAI;
}
[[nodiscard]] constexpr bool needs_model_name(Backend b) noexcept {
    return b == Backend::Ollama || b == Backend::OpenAI;
}

// ── The user-facing configuration ────────────────────────────────────────
// One struct, all optional. Defaults reproduce today's behaviour EXACTLY:
// Backend::Auto + nomic-embed-text on 127.0.0.1:11434, so an existing user
// who never opens the picker observes no change whatsoever.
struct EmbedConfig {
    Backend      backend = Backend::Auto;
    std::string  model   = "nomic-embed-text";

    // Endpoint, split rather than a URL string so the picker can validate the
    // parts independently and so port stays typed. `tls` and `path` only
    // matter for Backend::OpenAI.
    std::string   host = "127.0.0.1";
    std::uint16_t port = 11434;
    bool          tls  = false;
    std::string   path;            // empty ⇒ backend default (/v1/embeddings)

    // In-process backends.
    std::string model_path;        // .gguf / .onnx
    std::string tokenizer_path;    // ONNX only

    // Credential. NEVER persisted to settings.json and never part of
    // identity() — it authenticates, it does not define the vector space.
    std::string api_key;

    // Derived by probe(), never typed by the user. 0 ⇒ unknown; the spec
    // omits the key entirely so rag-cpp uses the model's native dimension.
    std::uint32_t dim = 0;

    [[nodiscard]] bool operator==(const EmbedConfig&) const = default;
};

// ── Validation ───────────────────────────────────────────────────────────
// A value, not a callback: the picker renders `Invalid::why` inline and
// refuses to commit. Pure ⇒ unit-testable with no UI.
struct Invalid { std::string why; };
using Validity = std::variant<std::monostate, Invalid>;

[[nodiscard]] Validity validate(const EmbedConfig& c);
[[nodiscard]] inline bool is_valid(const EmbedConfig& c) {
    return std::holds_alternative<std::monostate>(validate(c));
}

// ── Identity of the vector space ─────────────────────────────────────────
// THE correctness primitive. A persisted .ragdb is only reusable by an
// embedder that produces the SAME geometry. Model NAME alone cannot express
// that once backends are pluggable: "nomic-embed-text" served by Ollama and
// the same weights loaded in-process as GGUF differ in pooling and
// normalization, so their cosines are not comparable. Reopening one index
// with the other embedder yields silently wrong retrieval — no crash, no
// warning, just bad answers.
//
// So the manifest is keyed on this hash of {backend, model, endpoint, paths,
// dim}, and it is also spliced into the .ragdb FILENAME — which upgrades the
// fix into a feature: switching backends SWITCHES between warm indexes
// instead of invalidating one.
//
// Excludes api_key (a credential, not geometry) and every ranking-only knob.
[[nodiscard]] std::uint64_t identity(const EmbedConfig& c);

// Short hex form used in filenames.
[[nodiscard]] std::string identity_tag(const EmbedConfig& c);

// ── Spec minting ─────────────────────────────────────────────────────────
// The ONE place an embedder spec is constructed. Returns nullopt when the
// backend is Disabled (or invalid), which callers read as "lexical only".
//
// `resolved_auto` picks what Auto expands to: callers that have already
// probed pass the concrete backend. Auto with no probe result maps to Ollama
// so the very first probe still dials the historical default.
//
// The return type is a JSON string rather than rag::plugin::Json because this
// header must not pull rag-cpp into every TU that merely reads config;
// adapter.cpp is still the only place that includes <rag/rag.hpp>.
[[nodiscard]] std::optional<std::string> spec_json(const EmbedConfig& c);

// Human-readable one-liner for the status row / diagnostics, e.g.
//   "nomic-embed-text via ollama 127.0.0.1:11434"
//   "bge-small in-process (gguf)"
[[nodiscard]] std::string describe(const EmbedConfig& c);

// ── Probe result ─────────────────────────────────────────────────────────
// What `Test connection` returns, and what populates `dim`.
struct ProbeOk {
    std::uint32_t dim        = 0;
    int           latency_ms = 0;
};
struct ProbeErr { std::string why; };
using ProbeResult = std::variant<ProbeOk, ProbeErr>;

// Blocking single-shot probe: build the embedder, embed one short string,
// measure. Never throws. Runs off the UI thread (the reducer wraps it in a
// Cmd). Returns the DERIVED dimension — the only trustworthy source.
[[nodiscard]] ProbeResult probe(const EmbedConfig& c);

// ── Environment ──────────────────────────────────────────────────────────
// Legacy AGENTTY_OLLAMA_HOST / AGENTTY_EMBED_MODEL keep working; the new
// AGENTTY_EMBED_* names win where both are set. Applied on top of `base`
// so a persisted picker config can be layered over env defaults.
void apply_env(EmbedConfig& c);

} // namespace agentty::rag::embed
