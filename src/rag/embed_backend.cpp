// embed_backend.cpp — implementation of the embedding backend boundary.
//
// Split from adapter.cpp on purpose: adapter.cpp is the one TU that includes
// <rag/rag.hpp> for the ENGINE, and it is already 2300 lines. This file owns
// only the config->spec mapping, identity hashing, validation and the probe,
// so the picker/reducer/tests can link against the boundary without dragging
// in the whole retrieval engine.
//
// probe() does need rag-cpp (it builds an embedder to measure it), so it is
// compiled out on platforms where rag-cpp is unavailable — the rest of the
// file is pure and always builds.

#include "agentty/rag/embed_backend.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#ifndef AGENTTY_HAS_RAGCPP
#  define AGENTTY_HAS_RAGCPP 1
#endif

#if AGENTTY_HAS_RAGCPP
#  include <rag/rag.hpp>
#endif

namespace agentty::rag::embed {

namespace {

using json = nlohmann::json;

// FNV-1a. Same primitives adapter.cpp uses for its manifest fingerprints, so
// identity hashing reads consistently with the rest of the retrieval layer.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

void hash_text(std::uint64_t& h, std::string_view v) noexcept {
    for (unsigned char c : v) { h ^= c; h *= kFnvPrime; }
    h ^= 0xFFu; h *= kFnvPrime;   // field separator: "ab"+"c" != "a"+"bc"
}
void hash_u64(std::uint64_t& h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFFu; h *= kFnvPrime; }
}

[[nodiscard]] std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

[[nodiscard]] const char* env_or_null(const char* key) {
    const char* v = std::getenv(key);
    return (v && v[0]) ? v : nullptr;
}

// Split "host:port" / "http://host:port" into parts. Tolerant on purpose:
// users paste URLs, and rejecting them for a scheme prefix is hostile.
struct Endpoint { std::string host; std::uint16_t port = 0; bool tls = false; bool have_port = false; };

[[nodiscard]] Endpoint parse_endpoint(std::string raw) {
    Endpoint e;
    raw = trim(std::move(raw));
    if (raw.rfind("https://", 0) == 0) { e.tls = true;  raw.erase(0, 8); }
    else if (raw.rfind("http://", 0) == 0) { e.tls = false; raw.erase(0, 7); }
    if (auto slash = raw.find('/'); slash != std::string::npos) raw.erase(slash);
    if (auto colon = raw.rfind(':'); colon != std::string::npos) {
        const std::string port_s = raw.substr(colon + 1);
        bool digits = !port_s.empty()
            && std::all_of(port_s.begin(), port_s.end(),
                           [](unsigned char c) { return std::isdigit(c); });
        if (digits) {
            try {
                const int p = std::stoi(port_s);
                if (p > 0 && p <= 65535) {
                    e.port = static_cast<std::uint16_t>(p);
                    e.have_port = true;
                    raw.erase(colon);
                }
            } catch (...) { /* keep host intact */ }
        }
    }
    e.host = std::move(raw);
    return e;
}

// What Auto dials before anything has been probed: the historical default.
[[nodiscard]] constexpr Backend concrete(Backend b) noexcept {
    return b == Backend::Auto ? Backend::Ollama : b;
}

} // namespace

// ── Validation ───────────────────────────────────────────────────────────
Validity validate(const EmbedConfig& c) {
    if (c.backend == Backend::Disabled || c.backend == Backend::Auto)
        return std::monostate{};   // nothing to get wrong

    if (needs_endpoint(c.backend)) {
        if (trim(c.host).empty())
            return Invalid{"host is empty"};
        if (c.port == 0)
            return Invalid{"port must be 1-65535"};
        if (c.host.find(' ') != std::string::npos)
            return Invalid{"host contains a space"};
    }
    if (needs_model_name(c.backend) && trim(c.model).empty())
        return Invalid{"model name is required"};
    if (needs_model_path(c.backend)) {
        if (trim(c.model_path).empty())
            return Invalid{"model file path is required"};
    }
    if (c.backend == Backend::OpenAI && !c.path.empty() && c.path.front() != '/')
        return Invalid{"path must start with '/'"};
    return std::monostate{};
}

// ── Identity ─────────────────────────────────────────────────────────────
std::uint64_t identity(const EmbedConfig& c) {
    std::uint64_t h = kFnvOffset;
    // The backend id is part of the geometry: the same weights served by
    // Ollama vs loaded in-process differ in pooling/normalization.
    hash_text(h, id_of(concrete(c.backend)));
    if (needs_model_name(concrete(c.backend))) hash_text(h, c.model);
    if (needs_endpoint(concrete(c.backend))) {
        hash_text(h, c.host);
        hash_u64(h, c.port);
        hash_u64(h, c.tls ? 1u : 0u);
        hash_text(h, c.path);
    }
    if (needs_model_path(concrete(c.backend))) {
        hash_text(h, c.model_path);
        hash_text(h, c.tokenizer_path);
    }
    hash_u64(h, c.dim);
    // Deliberately NOT hashed: api_key. Rotating a credential must not throw
    // away a perfectly valid index.
    return h;
}

std::string identity_tag(const EmbedConfig& c) {
    static constexpr char kHex[] = "0123456789abcdef";
    const std::uint64_t h = identity(c);
    std::string out(8, '0');
    for (int i = 0; i < 8; ++i) out[7 - i] = kHex[(h >> (i * 4)) & 0xFu];
    return out;
}

// ── Spec minting ─────────────────────────────────────────────────────────
std::optional<std::string> spec_json(const EmbedConfig& c) {
    if (c.backend == Backend::Disabled) return std::nullopt;
    if (!is_valid(c)) return std::nullopt;

    const Backend b = concrete(c.backend);
    json j;
    j["type"] = std::string{id_of(b)};

    switch (b) {
        case Backend::Ollama:
            j["model"] = c.model;
            j["host"]  = c.host;
            j["port"]  = c.port;
            j["timeout_ms"] = 1200;
            break;
        case Backend::OpenAI:
            j["model"] = c.model;
            j["host"]  = c.host;
            j["port"]  = c.port;
            j["tls"]   = c.tls;
            if (!c.path.empty())    j["path"]    = c.path;
            if (!c.api_key.empty()) j["api_key"] = c.api_key;
            j["timeout_ms"] = 8000;   // remote: allow for WAN latency
            break;
        case Backend::LlamaCpp:
            j["host"] = c.host;
            j["port"] = c.port;
            if (!c.path.empty()) j["path"] = c.path;
            j["timeout_ms"] = 4000;
            break;
        case Backend::Gguf:
            j["model_path"] = c.model_path;
            j["normalize"]  = true;
            break;
        case Backend::Onnx:
            j["model_path"] = c.model_path;
            if (!c.tokenizer_path.empty()) j["tokenizer_path"] = c.tokenizer_path;
            j["normalize"] = true;
            break;
        case Backend::Auto:
        case Backend::Disabled:
            break;   // unreachable: concrete() mapped Auto, Disabled returned
    }
    // Only pin the dimension once it has been MEASURED. Never guessed: a
    // wrong dim makes rag-cpp's HNSW silently drop every vector.
    if (c.dim > 0) j["dim"] = c.dim;
    return j.dump();
}

std::string describe(const EmbedConfig& c) {
    if (c.backend == Backend::Disabled) return "keyword only (BM25)";
    const Backend b = concrete(c.backend);
    std::string out;
    if (needs_model_name(b)) out += c.model.empty() ? "(no model)" : c.model;
    else if (needs_model_path(b)) {
        const auto slash = c.model_path.find_last_of("/\\");
        out += c.model_path.empty()
             ? "(no model file)"
             : c.model_path.substr(slash == std::string::npos ? 0 : slash + 1);
    } else out += "llama.cpp";

    if (is_in_process(b)) {
        out += " in-process (";
        out += id_of(b);
        out += ")";
    } else {
        out += " via ";
        out += id_of(b);
        out += ' ';
        out += c.host;
        out += ':';
        out += std::to_string(c.port);
    }
    return out;
}

// ── Environment ──────────────────────────────────────────────────────────
void apply_env(EmbedConfig& c) {
    // Legacy names first so the new ones win when both are present.
    if (const char* m = env_or_null("AGENTTY_EMBED_MODEL")) c.model = m;
    if (const char* h = env_or_null("AGENTTY_OLLAMA_HOST")) {
        const auto e = parse_endpoint(h);
        if (!e.host.empty()) c.host = e.host;
        if (e.have_port)     c.port = e.port;
    }

    if (const char* b = env_or_null("AGENTTY_EMBED_BACKEND")) {
        std::string id{b};
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        // Accept a couple of friendly aliases for the same row.
        if (id == "none" || id == "disabled" || id == "bm25") id = "off";
        if (id == "llamafile" || id == "llama.cpp")           id = "llamacpp";
        if (id == "openai-compatible" || id == "custom")      id = "openai";
        c.backend = backend_from_id(id);
    }
    if (const char* e = env_or_null("AGENTTY_EMBED_ENDPOINT")) {
        const auto ep = parse_endpoint(e);
        if (!ep.host.empty()) c.host = ep.host;
        if (ep.have_port)     c.port = ep.port;
        c.tls = ep.tls;
        // Default the port from the scheme when the URL omitted one.
        if (!ep.have_port && ep.tls) c.port = 443;
    }
    if (const char* p = env_or_null("AGENTTY_EMBED_PATH"))       c.path           = p;
    if (const char* p = env_or_null("AGENTTY_EMBED_MODEL_PATH")) c.model_path     = p;
    if (const char* p = env_or_null("AGENTTY_EMBED_TOKENIZER"))  c.tokenizer_path = p;
    if (const char* k = env_or_null("AGENTTY_EMBED_API_KEY"))    c.api_key        = k;
}

// ── Probe ────────────────────────────────────────────────────────────────
#if AGENTTY_HAS_RAGCPP

ProbeResult probe(const EmbedConfig& c) {
    if (c.backend == Backend::Disabled)
        return ProbeErr{"embeddings are disabled"};
    if (auto v = validate(c); auto* bad = std::get_if<Invalid>(&v))
        return ProbeErr{bad->why};

    // Probe with the dimension UNPINNED: we are here to learn it, and pinning
    // a stale value would mask the very mismatch we want to detect.
    EmbedConfig probe_cfg = c;
    probe_cfg.dim = 0;
    const auto spec = spec_json(probe_cfg);
    if (!spec) return ProbeErr{"no embedder for this configuration"};

    try {
        const auto t0 = std::chrono::steady_clock::now();
        ::rag::Engine engine;
        if (!engine.with_embedder_spec(::rag::plugin::Json::parse(*spec)))
            return ProbeErr{"backend '" + std::string{id_of(concrete(c.backend))}
                            + "' could not be constructed"};
        auto vector = engine.corpus().embed_text("agentty retrieval availability probe");
        const auto t1 = std::chrono::steady_clock::now();
        if (!vector)
            return ProbeErr{"embed failed — endpoint unreachable or model missing"};
        if (vector->empty())
            return ProbeErr{"backend returned an empty vector"};

        ProbeOk ok;
        ok.dim = static_cast<std::uint32_t>(vector->size());
        ok.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return ok;
    } catch (const std::exception& e) {
        return ProbeErr{e.what()};
    } catch (...) {
        return ProbeErr{"unknown error while probing the embedder"};
    }
}

#else

ProbeResult probe(const EmbedConfig&) {
    return ProbeErr{"retrieval engine (rag-cpp) is not built on this platform"};
}

#endif

} // namespace agentty::rag::embed
