// rag_settings_form.cpp — the embeddings pane's row model.
//
// build_form / config_from_form are a round-trip pair: build_form projects an
// EmbedConfig onto form rows, config_from_form reads them back. Keeping both
// here (and pure) means the reducer never hand-maps a row index to a config
// field, and the pair is property-testable without a terminal.
//
// Rows are DERIVED from the backend, so the form only ever shows fields that
// can affect the selected backend — an API-key row on an in-process model
// would be a lie about what the config does.
//
// Everything else — navigation, editing, the dropdown, the key map, every
// glyph — comes from the shared form layer and maya::Form. This file holds
// only the knowledge that is genuinely about embeddings.

#include "agentty/runtime/rag_settings.hpp"

#include <algorithm>

namespace agentty::rag_settings {

namespace eb = agentty::rag::embed;

namespace {

[[nodiscard]] std::string text_of(const form::Form& f, const char* id) {
    const auto* row = f.find(id);
    if (!row) return {};
    if (const auto* t = std::get_if<form::field::Text>(&row->value))   return t->value;
    if (const auto* s = std::get_if<form::field::Secret>(&row->value)) return s->value;
    if (const auto* p = std::get_if<form::field::Path>(&row->value))   return p->value;
    return {};
}

[[nodiscard]] bool bool_of(const form::Form& f, const char* id, bool fallback) {
    const auto* row = f.find(id);
    if (!row) return fallback;
    const auto* t = std::get_if<form::field::Toggle>(&row->value);
    return t ? t->on : fallback;
}

// Append every registry row, grouped, in table order. NOTHING here names an
// individual setting — the table is the source of truth and this is a walk.
// That is what makes "adding a knob is adding a row" true rather than
// aspirational.
//
// Generic over the OWNING config struct so one walk serves both halves of the
// table — the RAG rows read from a RagConfig, the Smart Mode routing rows from
// a store::Settings. `owner` selects which rows this pass is responsible for;
// the rest belong to the other call and are skipped.
template <class C>
void add_registry_rows(form::Builder& b, const C& cfg,
                       settings::registry::Owner owner, bool advanced,
                       bool& first, settings::registry::Group& last_group) {
    namespace reg = settings::registry;

    for (const auto& d : reg::kSettings) {
        if (d.owner() != owner) continue;

        // Advanced rows are hidden, not disabled: a knob whose effect a user
        // cannot judge is noise on the main screen, but it still has to be
        // reachable (^A) rather than env-only.
        if (d.tier == reg::Tier::Advanced && !advanced) continue;

        // A group header separates sections. It is a real field kind, not a
        // locked text row: faking it leaked a placeholder into the value
        // column and let the cursor land on a label that does nothing.
        if (first || d.group != last_group) {
            b.header(std::string{reg::label_of(d.group)});
            last_group = d.group;
            first = false;
        }

        const std::string id{d.id};
        const std::string label{d.label};
        const std::string help{d.help};

        switch (d.type) {
            case reg::Type::Bool: {
                const bool on = reg::get(cfg, d) == "true";
                b.toggle(id, label, on, help);
                break;
            }
            case reg::Type::Real: {
                double v = 0.0;
                try { v = std::stod(reg::get(cfg, d)); } catch (...) { v = d.min; }
                b.slider(id, label, v, d.min, d.max, d.step, help);
                break;
            }
            case reg::Type::Int: {
                long long v = 0;
                try { v = std::stoll(reg::get(cfg, d)); } catch (...) { v = 0; }
                b.number(id, label, v, static_cast<std::int64_t>(d.min),
                         static_cast<std::int64_t>(d.max), help);
                break;
            }
            case reg::Type::Enum: {
                std::vector<std::string> opts;
                std::string_view rest = d.options;
                while (!rest.empty()) {
                    const auto bar = rest.find('|');
                    opts.emplace_back(rest.substr(0, bar));
                    if (bar == std::string_view::npos) break;
                    rest.remove_prefix(bar + 1);
                }
                b.choice(id, label, opts, {}, reg::get(cfg, d), help);
                break;
            }
        }

        // An env var overriding this row makes it READ-ONLY and names the
        // variable. The row would otherwise look editable and silently lose
        // the edit on the next read — layered config's worst failure, and the
        // reason `origin` exists at all.
        if (const std::string env = reg::env_override(d); !env.empty()) {
            b.origin("env: " + env);
            b.lock("env: " + env);
        }
        // Provenance: a row still on its shipped value says so, which is the
        // difference between "I never touched this" and "I set it to that".
        else if (reg::is_default(cfg, d)) b.origin("default");
    }
}

// Both halves of the table, in table order, with the group headers running
// continuously across the boundary.
//
// The Settings-owned rows are the SMART MODE routing policy, and they render
// in the Smart Mode pane (^S → ^A) next to the switch and slots they govern —
// not here. A knob shown in the wrong pane is findable only by accident, which
// is the same failure as leaving it in an env var. This pane is Retrieval, so
// it walks the Retrieval rows.
void add_all_registry_rows(form::Builder& b, const store::Settings& settings,
                           bool advanced) {
    namespace reg = agentty::settings::registry;
    auto last_group = reg::Group::Sources;
    bool first = true;
    add_registry_rows(b, settings.rag, reg::Owner::Rag, advanced, first, last_group);
}

} // namespace

form::Form build_form(const eb::EmbedConfig& c, store::RagMode mode,
                      const store::Settings& settings, bool advanced) {
    const store::RagConfig& rag = settings.rag;
    form::Builder b{" Retrieval "};
    b.subtitle(eb::describe(c));

    // Row 0: WHEN retrieval runs. A genuine enum (three named policies), so a
    // dropdown is the right control. It used to be a whole separate overlay
    // whose only content was these three rows — which left no obvious place
    // for the embedder settings to live, and made them undiscoverable.
    {
        std::vector<std::string> labels, ids, hints;
        for (store::RagMode mm : kModes) {
            labels.emplace_back(store::to_string(mm));
            ids.emplace_back(store::to_string(mm));
            hints.emplace_back(store::describe(mm));
        }
        b.choice(kFieldMode, "Proactive", std::move(labels), std::move(ids),
                 store::to_string(mode),
                 "when to retrieve context before a turn", std::move(hints));
    }

    // Row 1: WHAT computes the vectors. Presented as a dropdown with
    // per-option blurbs, so "which of these can I actually run here?" is
    // answerable without leaving the pane.
    {
        std::vector<std::string> labels, ids, hints;
        for (eb::Backend bk : eb::kBackends) {
            labels.emplace_back(eb::label_of(bk));
            ids.emplace_back(eb::id_of(bk));
            hints.emplace_back(eb::blurb_of(bk));
        }
        b.choice(kFieldBackend, "Embeddings", std::move(labels), std::move(ids),
                 eb::id_of(c.backend),
                 "what computes the embedding vectors", std::move(hints));
    }

    // Auto and Off need no endpoint and no model file — but a pane holding a
    // single row reads as broken ("why is Backend the only thing here?").
    // Auto is not "nothing configured", it is a POLICY, so say what it will
    // actually do and show the one knob that still applies. The rows are
    // locked rather than hidden: a visible, explained row beats an empty pane.
    if (c.backend == eb::Backend::Auto) {
        b.text(kFieldModel, "Model", c.model,
               "used when Auto finds a local Ollama");
        b.text(kFieldHost, "Endpoint", c.host + ":" + std::to_string(c.port));
        b.lock("auto-detected");
        b.action(kFieldTest, "Test connection",
                 "check whether Auto can reach a local Ollama right now");
        add_all_registry_rows(b, settings, advanced);
        return b.build();
    }
    if (c.backend == eb::Backend::Disabled) {
        b.text(kFieldModel, "Retrieval", "keyword only (BM25)");
        b.lock("embeddings off");
        add_all_registry_rows(b, settings, advanced);
        return b.build();
    }

    if (eb::needs_model_name(c.backend))
        b.text(kFieldModel, "Model", c.model,
               "embedding model name as the endpoint knows it");

    if (eb::needs_endpoint(c.backend)) {
        b.text(kFieldHost, "Host", c.host, "hostname or IP (no scheme)");
        b.number(kFieldPort, "Port", static_cast<std::int64_t>(c.port), 1, 65535);
    }

    if (c.backend == eb::Backend::OpenAI) {
        b.toggle(kFieldTls, "HTTPS", c.tls, "use TLS for the endpoint");
        b.text(kFieldPath, "Path", c.path, "request path; blank = /v1/embeddings");
        b.secret(kFieldApiKey, "API key", c.api_key,
                 "stored in the OS keystore, never in settings.json");
    }

    if (c.backend == eb::Backend::LlamaCpp)
        b.text(kFieldPath, "Path", c.path, "request path; blank = /embedding");

    if (eb::needs_model_path(c.backend)) {
        // A Path field, not Text: the widget badges exists/missing while you
        // type, so a typo'd model path is visible immediately instead of
        // surfacing later as an opaque "backend could not be constructed".
        b.path(kFieldModelPath, "Model file", c.model_path,
               c.backend == eb::Backend::Gguf
                   ? "absolute path to a .gguf embedding model"
                   : "absolute path to a .onnx model");
        if (c.backend == eb::Backend::Onnx)
            b.path(kFieldTokenizer, "Tokenizer", c.tokenizer_path,
                   "tokenizer.json path (blank = alongside the model)");
    }

    // The probe row. An Action, not a value: it is also the ONLY way `dim` is
    // ever set, because a user-supplied dimension is silently dropped by
    // rag-cpp's HNSW on mismatch.
    b.action(kFieldTest, "Test connection",
             "embed a probe string and measure the vector");

    add_all_registry_rows(b, settings, advanced);
    return b.build();
}

eb::EmbedConfig config_from_form(const eb::EmbedConfig& base, const form::Form& f) {
    eb::EmbedConfig c = base;

    if (const auto* row = f.find(kFieldBackend))
        if (const auto* ch = std::get_if<form::field::Choice>(&row->value))
            c.backend = eb::backend_from_id(ch->id());

    // A locked row is DISPLAY-ONLY: under Auto the endpoint row shows a
    // rendered "host:port" summary and the model row shows what Auto would
    // use. Reading those back would write the summary string into `host` and
    // corrupt the config. `editable()` is the same predicate the reducer and
    // the key router use, so "can the user change this" has one owner.
    const auto readable = [&](const char* id) -> const form::Field* {
        const auto* row = f.find(id);
        return (row && row->editable()) ? row : nullptr;
    };

    if (readable(kFieldModel))     c.model          = text_of(f, kFieldModel);
    if (readable(kFieldHost))      c.host           = text_of(f, kFieldHost);
    if (readable(kFieldPath))      c.path           = text_of(f, kFieldPath);
    if (readable(kFieldModelPath)) c.model_path     = text_of(f, kFieldModelPath);
    if (readable(kFieldTokenizer)) c.tokenizer_path = text_of(f, kFieldTokenizer);
    if (readable(kFieldApiKey))    c.api_key        = text_of(f, kFieldApiKey);
    if (readable(kFieldTls))       c.tls            = bool_of(f, kFieldTls, c.tls);

    if (const auto* row = readable(kFieldPort))
        if (const auto* n = std::get_if<form::field::Number>(&row->value))
            c.port = static_cast<std::uint16_t>(std::clamp<std::int64_t>(n->value, 1, 65535));

    return c;
}

store::RagMode mode_from_form(const form::Form& f, store::RagMode fallback) {
    const auto* row = f.find(kFieldMode);
    if (!row) return fallback;
    const auto* ch = std::get_if<form::field::Choice>(&row->value);
    if (!ch) return fallback;
    const auto id = ch->id();
    for (store::RagMode mm : kModes)
        if (store::to_string(mm) == id) return mm;
    return fallback;
}

void apply_form_to_settings(const form::Form& f, store::Settings& settings) {
    namespace reg = settings::registry;

    // Walks the SAME table build_form walked, so a row cannot be written that
    // was never read, and a knob added to the table is picked up by both
    // directions at once. Each row is written through its OWN config struct;
    // a locked row (an env override) is skipped, so a shell export is never
    // silently overwritten by the value the UI happened to be showing.
    const auto write = [&](const auto& d, auto& target) {
        const auto* row = f.find(d.id);
        if (!row || row->locked) return;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, form::field::Toggle>)
                (void)reg::set(target, d, v.on ? "true" : "false");
            else if constexpr (std::is_same_v<T, form::field::Slider>)
                (void)reg::set(target, d, std::to_string(v.value));
            else if constexpr (std::is_same_v<T, form::field::Number>)
                (void)reg::set(target, d, std::to_string(v.value));
            else if constexpr (std::is_same_v<T, form::field::Choice>)
                (void)reg::set(target, d, std::string{v.id()});
        }, row->value);
    };

    for (const auto& d : reg::kSettings) {
        if (d.owner() == reg::Owner::Rag) write(d, settings.rag);
        else                              write(d, settings);
    }
}

} // namespace agentty::rag_settings
