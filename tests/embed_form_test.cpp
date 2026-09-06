// The embeddings pane's row model: build_form / config_from_form.
//
// The pane itself now holds nothing but a form::Form — navigation, editing,
// the dropdown and the key map all come from the shared component layer, and
// every glyph belongs to maya::Form. What is left to test here is the only
// genuinely embeddings-specific knowledge: WHICH rows a backend needs, and
// that rows round-trip back onto an EmbedConfig without disturbing the vector
// -space identity.

#include <doctest/doctest.h>

#include "agentty/runtime/rag_settings.hpp"

namespace rs = agentty::rag_settings;
namespace eb = agentty::rag::embed;
namespace ff = agentty::form::field;

TEST_CASE("embed form: rows are derived from the backend") {
    eb::EmbedConfig c;
    c.backend = eb::Backend::Auto;
    auto f = rs::build_form(c, agentty::store::RagMode::On);
    // Auto needs no endpoint — but a pane holding ONE row reads as broken.
    // It states what Auto will actually do, with the rows locked rather than
    // hidden, and still offers the probe.
    CHECK(f.fields.size() > 1);
    // Row 0 is the proactive-retrieval MODE — the pane is one question ("how
    // does retrieval behave here?"), asked as mode + embedder.
    CHECK(f.fields[0].id == rs::kFieldMode);
    CHECK(f.find(rs::kFieldBackend) != nullptr);
    CHECK(f.find(rs::kFieldTest) != nullptr);
    // The endpoint row is display-only under Auto.
    REQUIRE(f.find(rs::kFieldHost) != nullptr);
    CHECK(f.find(rs::kFieldHost)->locked);

    c.backend = eb::Backend::OpenAI;
    f = rs::build_form(c, agentty::store::RagMode::On);
    CHECK(f.find(rs::kFieldHost)      != nullptr);
    CHECK(f.find(rs::kFieldPort)      != nullptr);
    CHECK(f.find(rs::kFieldModel)     != nullptr);
    CHECK(f.find(rs::kFieldApiKey)    != nullptr);
    CHECK(f.find(rs::kFieldTest)      != nullptr);
    CHECK(f.find(rs::kFieldModelPath) == nullptr);   // not in-process
    // Under a real backend the endpoint IS editable.
    CHECK_FALSE(f.find(rs::kFieldHost)->locked);

    // In-process: a model FILE, and no endpoint or credential rows — showing
    // them would be a lie about what the backend uses.
    c.backend = eb::Backend::Gguf;
    f = rs::build_form(c, agentty::store::RagMode::On);
    CHECK(f.find(rs::kFieldModelPath) != nullptr);
    CHECK(f.find(rs::kFieldHost)      == nullptr);
    CHECK(f.find(rs::kFieldPort)      == nullptr);
    CHECK(f.find(rs::kFieldApiKey)    == nullptr);
}

TEST_CASE("embed form: display-only rows never write back into the config") {
    // Under Auto the endpoint row shows a rendered "host:port" SUMMARY. If
    // config_from_form read locked rows, that summary string would land in
    // `host` and corrupt the config on the next keystroke.
    eb::EmbedConfig c;
    c.backend = eb::Backend::Auto;
    c.host    = "127.0.0.1";
    c.port    = 11434;

    const auto back = rs::config_from_form(c, rs::build_form(c, agentty::store::RagMode::On));
    CHECK(back.host == "127.0.0.1");
    CHECK(back.port == 11434);
    CHECK(eb::identity(back) == eb::identity(c));
}

TEST_CASE("embed form: the backend row is a dropdown with per-option hints") {
    // The affordance that answers "which of these can I actually run here?"
    // without leaving the pane.
    eb::EmbedConfig c;
    const auto f = rs::build_form(c, agentty::store::RagMode::On);
    const auto* row = f.find(rs::kFieldBackend);
    REQUIRE(row != nullptr);
    REQUIRE(row->is_choice());
    const auto& ch = std::get<ff::Choice>(row->value);
    CHECK(ch.count() == eb::kBackendCount);
    CHECK(ch.hints.size() == static_cast<std::size_t>(eb::kBackendCount));
    CHECK(ch.id() == "auto");
}

TEST_CASE("embed form: the API key row is a Secret") {
    // If this regressed to Text the key would render in the clear — the exact
    // leak the type split exists to prevent.
    eb::EmbedConfig c;
    c.backend = eb::Backend::OpenAI;
    c.api_key = "sk-live-key";
    const auto f = rs::build_form(c, agentty::store::RagMode::On);
    const auto* row = f.find(rs::kFieldApiKey);
    REQUIRE(row != nullptr);
    CHECK(std::holds_alternative<ff::Secret>(row->value));
}

TEST_CASE("embed form: a model file is a Path, so a typo is visible") {
    eb::EmbedConfig c;
    c.backend    = eb::Backend::Gguf;
    c.model_path = "/models/nomic.gguf";
    const auto f = rs::build_form(c, agentty::store::RagMode::On);
    const auto* row = f.find(rs::kFieldModelPath);
    REQUIRE(row != nullptr);
    CHECK(std::holds_alternative<ff::Path>(row->value));
}

TEST_CASE("embed form: build_form / config_from_form round-trips") {
    eb::EmbedConfig c;
    c.backend = eb::Backend::OpenAI;
    c.model   = "bge-m3";
    c.host    = "gateway.internal";
    c.port    = 8443;
    c.tls     = true;
    c.path    = "/v1/embeddings";
    c.api_key = "sk-round-trip";

    const auto back = rs::config_from_form(c, rs::build_form(c, agentty::store::RagMode::On));
    CHECK(back.backend == c.backend);
    CHECK(back.model   == c.model);
    CHECK(back.host    == c.host);
    CHECK(back.port    == c.port);
    CHECK(back.tls     == c.tls);
    CHECK(back.path    == c.path);
    CHECK(back.api_key == c.api_key);
    // A round trip must not disturb the vector-space identity.
    CHECK(eb::identity(back) == eb::identity(c));
}

TEST_CASE("embed form: editing a row is reflected in the config") {
    eb::EmbedConfig c;
    c.backend = eb::Backend::OpenAI;
    c.host    = "old.internal";
    auto f = rs::build_form(c, agentty::store::RagMode::On);

    auto* row = f.find(rs::kFieldHost);
    REQUIRE(row != nullptr);
    agentty::form::clear(row->value);
    agentty::form::paste(row->value, "new.internal");

    const auto back = rs::config_from_form(c, f);
    CHECK(back.host == "new.internal");
    // Changing the endpoint MUST change the identity: it may serve different
    // weights under the same model name.
    CHECK(eb::identity(back) != eb::identity(c));
}

TEST_CASE("embed form: selecting a backend by dropdown rebuilds the rows") {
    // The interaction that makes the pane feel coherent: pick "Custom
    // endpoint" and the endpoint rows appear.
    eb::EmbedConfig c;
    auto f = rs::build_form(c, agentty::store::RagMode::On);
    auto* row = f.find(rs::kFieldBackend);
    REQUIRE(row != nullptr);
    std::get<ff::Choice>(row->value).select_id("openai");

    const auto next = rs::config_from_form(c, f);
    CHECK(next.backend == eb::Backend::OpenAI);
    const auto rebuilt = rs::build_form(next, agentty::store::RagMode::On);
    CHECK(rebuilt.find(rs::kFieldHost)   != nullptr);
    CHECK(rebuilt.find(rs::kFieldApiKey) != nullptr);
}

TEST_CASE("embed form: the in-process backends need no daemon") {
    // The property that answers "I can't run ollama at work".
    for (auto b : {eb::Backend::Gguf, eb::Backend::Onnx}) {
        eb::EmbedConfig c;
        c.backend    = b;
        c.model_path = "/models/m.bin";
        CHECK(eb::is_in_process(c.backend));
        CHECK(eb::is_valid(c));
        const auto f = rs::build_form(c, agentty::store::RagMode::On);
        CHECK(f.find(rs::kFieldHost) == nullptr);
    }
}

TEST_CASE("embed form: the probe row is an Action, not a value") {
    // It runs something and reports; it holds no configuration. Being a real
    // alternative is what lets the reducer dispatch on the type instead of
    // string-matching a row id.
    eb::EmbedConfig c;
    c.backend = eb::Backend::Ollama;
    const auto f = rs::build_form(c, agentty::store::RagMode::On);
    const auto* row = f.find(rs::kFieldTest);
    REQUIRE(row != nullptr);
    CHECK(row->is_action());
    CHECK_FALSE(row->editable());   // Enter fires it; it never enters edit mode
}
