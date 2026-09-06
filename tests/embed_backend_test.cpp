// Tests for the embedding backend boundary — the layer that let retrieval stop
// being hardwired to a local Ollama daemon.
//
// Two properties here are worth more than the rest combined, because both fail
// SILENTLY in production:
//
//  1. identity() must separate vector spaces. A persisted .ragdb is only
//     reusable by an embedder with the same geometry, and rag-cpp's HNSW
//     DROPS mismatched vectors without an error (hnsw.cpp: `if (vec.size() !=
//     dim_) return;`). So a stale index reopened under a different backend
//     yields empty results or garbage cosines with nothing in any log.
//
//  2. api_key must NOT participate in identity. Rotating a credential is not
//     a change of geometry; if it were hashed, every key rotation would
//     silently throw away a perfectly good index.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "agentty/rag/embed_backend.hpp"

using namespace agentty::rag::embed;

namespace {

EmbedConfig ollama_cfg() {
    EmbedConfig c;
    c.backend = Backend::Ollama;
    c.model   = "nomic-embed-text";
    c.host    = "127.0.0.1";
    c.port    = 11434;
    return c;
}

EmbedConfig openai_cfg() {
    EmbedConfig c;
    c.backend = Backend::OpenAI;
    c.model   = "nomic-embed-text";
    c.host    = "gateway.internal";
    c.port    = 8080;
    return c;
}

} // namespace

TEST_CASE("embed: defaults reproduce the historical Ollama behaviour") {
    // An upgrading user who never opens the picker must observe NO change.
    EmbedConfig c;
    CHECK(c.backend == Backend::Auto);
    CHECK(c.model == "nomic-embed-text");
    CHECK(c.host == "127.0.0.1");
    CHECK(c.port == 11434);

    // Auto dials Ollama until something says otherwise.
    const auto spec = spec_json(c);
    REQUIRE(spec.has_value());
    const auto j = nlohmann::json::parse(*spec);
    CHECK(j.at("type") == "ollama");
    CHECK(j.at("model") == "nomic-embed-text");
    CHECK(j.at("port") == 11434);
}

TEST_CASE("embed: identity separates vector spaces") {
    // Same model NAME, different backend ⇒ different geometry. This is the
    // case the old manifest (which stored `embed_model`) could not express,
    // and the reason a name is not an identity once backends are pluggable.
    auto a = ollama_cfg();
    auto b = openai_cfg();
    CHECK(identity(a) != identity(b));

    // Endpoint is part of the space: two hosts may serve different weights
    // under the same label.
    auto c = openai_cfg();
    c.host = "other.internal";
    CHECK(identity(b) != identity(c));

    auto d = openai_cfg();
    d.port = 9090;
    CHECK(identity(b) != identity(d));

    // A measured dimension distinguishes too (truncated-embedding endpoints).
    auto e = openai_cfg();
    e.dim = 768;
    auto f = openai_cfg();
    f.dim = 1536;
    CHECK(identity(e) != identity(f));

    // Model name still matters within one backend.
    auto g = ollama_cfg();
    g.model = "mxbai-embed-large";
    CHECK(identity(a) != identity(g));
}

TEST_CASE("embed: identity ignores the credential") {
    // Rotating an API key must not invalidate a warm index — it authenticates,
    // it does not define the geometry.
    auto a = openai_cfg();
    auto b = openai_cfg();
    a.api_key = "sk-first";
    b.api_key = "sk-second-rotated";
    CHECK(identity(a) == identity(b));
    CHECK(identity_tag(a) == identity_tag(b));
}

TEST_CASE("embed: identity is stable and short") {
    const auto c = ollama_cfg();
    CHECK(identity(c) == identity(c));          // deterministic
    CHECK(identity_tag(c).size() == 8);         // filename-safe
    for (char ch : identity_tag(c))
        CHECK(((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')));
}

TEST_CASE("embed: validation catches what the picker must refuse") {
    // Auto/Off are complete on their own.
    EmbedConfig auto_cfg;
    CHECK(is_valid(auto_cfg));
    EmbedConfig off;
    off.backend = Backend::Disabled;
    CHECK(is_valid(off));

    auto bad_host = openai_cfg();
    bad_host.host.clear();
    CHECK_FALSE(is_valid(bad_host));

    auto bad_port = openai_cfg();
    bad_port.port = 0;
    CHECK_FALSE(is_valid(bad_port));

    auto no_model = ollama_cfg();
    no_model.model.clear();
    CHECK_FALSE(is_valid(no_model));

    // In-process backends need a file, not an endpoint.
    EmbedConfig gguf;
    gguf.backend = Backend::Gguf;
    CHECK_FALSE(is_valid(gguf));
    gguf.model_path = "/models/nomic.gguf";
    CHECK(is_valid(gguf));

    // A path must look like one.
    auto bad_path = openai_cfg();
    bad_path.path = "v1/embeddings";   // missing leading slash
    CHECK_FALSE(is_valid(bad_path));
    bad_path.path = "/v1/embeddings";
    CHECK(is_valid(bad_path));
}

TEST_CASE("embed: an invalid config mints no spec") {
    // spec_json is the only place a spec is built, so refusing here is what
    // stops a half-typed endpoint reaching rag-cpp.
    auto bad = openai_cfg();
    bad.host.clear();
    CHECK_FALSE(spec_json(bad).has_value());

    EmbedConfig off;
    off.backend = Backend::Disabled;
    CHECK_FALSE(spec_json(off).has_value());   // lexical-only is not an embedder
}

TEST_CASE("embed: dimension is only pinned once measured") {
    // A GUESSED dim is a silent index-corruption footgun: rag-cpp's HNSW
    // ignores vectors that disagree with it. So the spec omits the key
    // entirely until a probe has measured one.
    auto c = openai_cfg();
    CHECK(c.dim == 0);
    auto j = nlohmann::json::parse(*spec_json(c));
    CHECK_FALSE(j.contains("dim"));

    c.dim = 768;
    j = nlohmann::json::parse(*spec_json(c));
    REQUIRE(j.contains("dim"));
    CHECK(j.at("dim") == 768);
}

TEST_CASE("embed: specs carry each backend's own keys") {
    // OpenAI-compatible: the case KhazAkar actually asked for — an arbitrary
    // endpoint with a user-provided model.
    auto oa = openai_cfg();
    oa.tls     = true;
    oa.path    = "/v1/embeddings";
    oa.api_key = "sk-test";
    const auto j = nlohmann::json::parse(*spec_json(oa));
    CHECK(j.at("type") == "openai");
    CHECK(j.at("host") == "gateway.internal");
    CHECK(j.at("tls") == true);
    CHECK(j.at("path") == "/v1/embeddings");
    CHECK(j.at("api_key") == "sk-test");

    // In-process: no host/port at all — this is the "cannot run a daemon at
    // work" answer, and a stray endpoint key would be a lie about that.
    EmbedConfig g;
    g.backend    = Backend::Gguf;
    g.model_path = "/models/nomic.gguf";
    const auto gj = nlohmann::json::parse(*spec_json(g));
    CHECK(gj.at("type") == "gguf");
    CHECK(gj.at("model_path") == "/models/nomic.gguf");
    CHECK_FALSE(gj.contains("host"));
    CHECK_FALSE(gj.contains("port"));
    CHECK_FALSE(gj.contains("api_key"));
}

TEST_CASE("embed: backend ids round-trip") {
    for (Backend b : kBackends) {
        CHECK(backend_from_id(id_of(b)) == b);
        CHECK_FALSE(label_of(b).empty());
        CHECK_FALSE(id_of(b).empty());
    }
    // Unknown ids fall back to the safe default rather than throwing.
    CHECK(backend_from_id("nonsense") == Backend::Auto);
    CHECK(backend_from_id("") == Backend::Auto);
}

TEST_CASE("embed: capability predicates match the spec keys") {
    // These drive which rows the picker shows; a mismatch means a field the
    // user can type into that the spec then ignores.
    CHECK(is_in_process(Backend::Gguf));
    CHECK(is_in_process(Backend::Onnx));
    CHECK_FALSE(is_in_process(Backend::Ollama));

    CHECK(needs_endpoint(Backend::Ollama));
    CHECK(needs_endpoint(Backend::OpenAI));
    CHECK(needs_endpoint(Backend::LlamaCpp));
    CHECK_FALSE(needs_endpoint(Backend::Gguf));

    // Only the remote HTTP API takes a bearer token — Ollama and llama.cpp
    // servers are unauthenticated in every deployment we target.
    CHECK(needs_api_key(Backend::OpenAI));
    CHECK_FALSE(needs_api_key(Backend::Ollama));
    CHECK_FALSE(needs_api_key(Backend::LlamaCpp));
    CHECK_FALSE(needs_api_key(Backend::Gguf));

    CHECK(needs_model_path(Backend::Gguf));
    CHECK_FALSE(needs_model_path(Backend::OpenAI));
}

TEST_CASE("embed: describe names the transport honestly") {
    CHECK(describe(ollama_cfg()).find("ollama") != std::string::npos);
    CHECK(describe(ollama_cfg()).find("11434") != std::string::npos);

    EmbedConfig g;
    g.backend    = Backend::Gguf;
    g.model_path = "/models/nomic.gguf";
    const auto d = describe(g);
    CHECK(d.find("in-process") != std::string::npos);
    CHECK(d.find("nomic.gguf") != std::string::npos);   // basename, not full path

    EmbedConfig off;
    off.backend = Backend::Disabled;
    CHECK(describe(off).find("BM25") != std::string::npos);
}
