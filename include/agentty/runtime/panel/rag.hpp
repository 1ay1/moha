#pragma once
// RAG mode picker — one decision: how proactive (pre-turn) retrieval behaves.
//
// Open it from the command palette (Ctrl+K → "RAG"). Three modes:
//
//   On               inject retrieved context before EVERY turn
//   First turn only  inject only on a thread's first turn (grounding, then off)
//   Off              no proactive injection (search_docs/search_code still work)
//
// ↑↓ move, Enter/Space/← → select, Esc closes. The choice persists to
// settings.json and applies live. The advanced retrieval knobs are no longer
// in the UI — they stay at their defaults (env-tunable for power users).
//
// UI-state only; reducer in update/rag_settings.cpp, view in view/pickers.cpp.

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentty/rag/embed_backend.hpp"
#include "agentty/runtime/panel/form.hpp"
#include "agentty/runtime/settings_registry.hpp"
#include "agentty/store/store.hpp"   // store::RagMode

namespace agentty {

namespace rag_settings {

// ── The embeddings sub-form ────────────────────────────────────────
// Retrieval used to be hardwired to a local Ollama daemon, which locks out
// anyone who cannot run one (locked-down work machines) or who already has an
// internal embeddings gateway. The backend is now user-chosen, and — per the
// requirement that everything be configurable from the TUI — chosen HERE
// rather than only through environment variables.
//
// The pane holds a form::Form and nothing else: navigation, editing, the
// dropdown and the key map all come from the shared component layer, and
// maya::Form owns every glyph. What remains here is the only genuinely
// embeddings-specific knowledge — WHICH rows a backend needs, and how rows
// map back onto an EmbedConfig.
//
// Rows are DERIVED from the selected backend (see rows_for): a custom endpoint
// shows host/port/model/key, an in-process backend shows a model-file path
// instead. The form never displays a row that cannot affect the chosen
// backend, so there is no "which of these actually applies?" ambiguity.
//
// The probe is the load-bearing row. Embedding dimension is DERIVED from it,
// never typed: rag-cpp's HNSW silently drops vectors whose size disagrees with
// the index, so a guessed dimension yields an empty index with no error at
// all. Test connection is therefore on the path to a valid config.
struct EmbedForm {
    rag::embed::EmbedConfig cfg;

    // Rows + cursor + focus + dirty, all from the shared model.
    form::Form form;

    // Probe lifecycle. Stays a pane concern rather than a form concern: it is
    // the ONE thing a generic form cannot express, because its result feeds
    // back into the config (the measured dimension).
    struct Idle {};
    struct Testing {};
    struct Ok   { std::uint32_t dim; int latency_ms; };
    struct Failed { std::string why; };
    using Probe = std::variant<Idle, Testing, Ok, Failed>;
    Probe probe{Idle{}};

    [[nodiscard]] bool dirty() const noexcept { return form.dirty; }
};

// Field ids. Named constants because the reducer addresses rows by id (a row
// index changes meaning when the backend changes the row set).
inline constexpr const char* kFieldMode      = "mode";
inline constexpr const char* kFieldBackend   = "backend";
inline constexpr const char* kFieldModel     = "model";
inline constexpr const char* kFieldHost      = "host";
inline constexpr const char* kFieldPort      = "port";
inline constexpr const char* kFieldTls       = "tls";
inline constexpr const char* kFieldPath      = "path";
inline constexpr const char* kFieldModelPath = "model_path";
inline constexpr const char* kFieldTokenizer = "tokenizer";
inline constexpr const char* kFieldApiKey    = "api_key";
inline constexpr const char* kFieldTest      = "test";

// Build the form for a config's backend. Pure — unit-testable without UI.
// `mode` is the proactive-retrieval policy, rendered as the first row: it and
// the embedder are the two halves of one question ("how does retrieval
// behave here?"), and splitting them across two overlays made the second one
// undiscoverable.
//
// `rag` supplies the pipeline knobs, which are GENERATED from
// settings_registry::kSettings rather than listed here — adding a knob is
// adding a table row, and this pane picks it up with no edit.
// `advanced` reveals the rows whose effect a user cannot easily judge.
[[nodiscard]] form::Form build_form(const rag::embed::EmbedConfig& c,
                                    store::RagMode mode,
                                    const store::Settings& settings = {},
                                    bool advanced = false);

// Read the rows back into a config (the inverse of build_form).
[[nodiscard]] rag::embed::EmbedConfig config_from_form(
    const rag::embed::EmbedConfig& base, const form::Form& f);

// Read the mode row back.
[[nodiscard]] store::RagMode mode_from_form(const form::Form& f,
                                            store::RagMode fallback);

// Read the registry-generated rows back into a RagConfig. Walks the same
// table build_form walked, so a row cannot be written that was never read.
void apply_form_to_settings(const form::Form& f, store::Settings& settings);

struct Closed {};
struct Open {
    // The cursor is the MODE itself, not an index into kModes. Storing an
    // index means every read is `kModes[i]` — an unchecked array subscript
    // whose meaning silently changes if the list is reordered or resized.
    store::RagMode cursor = store::RagMode::On;
    store::RagMode active = store::RagMode::On;   // persisted mode (row marker)

    // The pane's form. NOT optional: a form-backed overlay with no form owns
    // the keyboard and can answer nothing, which reads on screen as the app
    // freezing. That state was reachable while `embed` was an optional filled
    // in by a deferred Cmd; it is now built at construction, so "open but
    // inert" is not representable.
    EmbedForm embed;

    // Show the Tier::Advanced rows — the knobs whose effect a user cannot judge
    // at a glance (numeric tuning, the routing policy). Hidden by DEFAULT, not
    // absent: burying them in an env var is what made them undiscoverable in
    // the first place, and a settings screen that lists thirty sliders is its
    // own kind of unusable. Toggled with ^A; the pane rebuilds its form.
    bool advanced = false;
};

// The rows, in display order — 1:1 with store::RagMode. The ONE place the
// layout is written down: the view walks it and the cursor moves through it.
inline constexpr store::RagMode kModes[] = {
    store::RagMode::On, store::RagMode::FirstTurnOnly, store::RagMode::Off,
};
inline constexpr int kModeCount =
    static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));

// Cursor movement closed over the enumeration — no call site owns a modulus.
[[nodiscard]] constexpr store::RagMode next_mode(store::RagMode mmode,
                                                 int delta) noexcept {
    int i = 0;
    for (int k = 0; k < kModeCount; ++k)
        if (kModes[k] == mmode) { i = k; break; }
    const int n = kModeCount;
    return kModes[((i + delta) % n + n) % n];
}

} // namespace rag_settings

using RagSettingsState =
    std::variant<rag_settings::Closed, rag_settings::Open>;

[[nodiscard]] inline bool rag_settings_is_open(const RagSettingsState& s) noexcept {
    return std::holds_alternative<rag_settings::Open>(s);
}
[[nodiscard]] inline rag_settings::Open*
rag_settings_opened(RagSettingsState& s) noexcept {
    return std::get_if<rag_settings::Open>(&s);
}
[[nodiscard]] inline const rag_settings::Open*
rag_settings_opened(const RagSettingsState& s) noexcept {
    return std::get_if<rag_settings::Open>(&s);
}

} // namespace agentty
