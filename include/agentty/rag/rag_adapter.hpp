#pragma once
// agentty::rag — the retrieval adapter.
//
// agentty's RAG engine is the external rag-cpp library (rag::Engine: contextual
// BM25 + optional dense/HNSW retrieval, weighted RRF, MMR rerank, opt-in CRAG/
// HyDE/GraphRAG, and validated .ragdb persistence). This header is the ONLY
// surface the rest of agentty sees: it hides every rag:: type behind a compact,
// stable API so the app never depends on the engine's internals.
//
// The boundary is deliberately tiny — three things the app needs:
//
//   1. Retriever          — build/refresh a docs index from a folder, fuse it
//                           with skills + learned-memory + MCP-resource
//                           knowledge sources, and answer a query with ranked,
//                           compressed passages. Backs the `search_docs` tool
//                           and the pre-turn proactive-retrieval path.
//   2. feedback::note_file_opened  — the learning loop's write side: a `read`
//                           of a file a passage pointed at counts as a "win".
//   3. bench::run          — the `agentty rag-bench` CLI subcommand.
//
// Everything below is std types + POD; no rag:: type leaks. The heavy engine
// lives in the .cpp (src/rag/adapter.cpp).

#include <cstdint>

#include "agentty/rag/embed_backend.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agentty::rag {

// One retrieved passage, flattened for the app. Mirrors mcp::tools::DocPassage
// field-for-field so the backend maps it with a trivial copy.
struct Passage {
    std::string   source;      // provenance: "docs" / "skills" / "memory" / "mcp:<uri>"
    std::string   path;        // file path or virtual uri (skill://…, memory://…)
    int           line_start = 0;
    int           line_end   = 0;
    double        score      = 0.0;
    std::string   text;        // passage body (already compressed)
};

// The result of a retrieval: the ranked passages + a human-readable mode label
// (engine config + confidence) the tool shell renders, + the confidence signal
// so the proactive path can gate on it.
struct Retrieval {
    std::vector<Passage> passages;
    std::string          mode;            // e.g. "hybrid+ctx, reranked, confidence 0.62"
    double               confidence = 0.0;
    std::string          error;           // non-empty ⇒ failure (no knowledge, etc.)
};

// Knobs, all resolved from the environment by default (from_env()). Kept as a
// struct so tests can drive the adapter deterministically. Every rag-cpp
// quality feature agentty drives has a toggle here.
struct Config {
    std::string  docs_root;               // AGENTTY_DOCS_DIR (or ./docs, ./.agentty/knowledge)

    // WHICH embedder, and how to reach it. Was three loose fields hardwired to
    // a local Ollama daemon; now one value the user can configure from the TUI
    // (Ctrl+K -> RAG -> Embeddings). Defaults reproduce the old behaviour
    // exactly: Auto -> Ollama on 127.0.0.1:11434 with nomic-embed-text.
    //
    // NOTE `embed.dim` is DERIVED by embed::probe(), never typed by a user:
    // rag-cpp's HNSW silently drops vectors whose size disagrees with the
    // index, so a guessed dimension yields an empty index and no error.
    embed::EmbedConfig embed;

    bool         skills   = true;         // AGENTTY_RAG_SKILLS
    bool         memory   = true;         // AGENTTY_RAG_MEMORY
    bool         mcp_resources = false;   // AGENTTY_RAG_MCP, explicit opt-in

    // Conservative production defaults: each optional stage must earn its
    // latency and output cost on the user's corpus before it is enabled.
    bool     contextual = true;   // index-time situating context
    bool     mmr        = true;   // diversity over the final candidate pool
    float    mmr_lambda = 0.65f;
    bool     stitch     = true;
    // Near-duplicate dedup + relevance autocut — the refinement stages from
    // rag-cpp's Pipeline::best(). dedup folds paraphrase/boilerplate copies so
    // an LLM context window isn't spent re-reading the same passage; autocut
    // trims the low-relevance tail at the score knee. Both are cheap and win
    // for grounded generation, but can shorten the result below k, so on by
    // default yet individually toggleable.
    bool     dedup      = true;   // AGENTTY_RAG_DEDUP
    float    dedup_threshold = 0.92f;
    bool     autocut    = true;   // AGENTTY_RAG_AUTOCUT
    float    autocut_sensitivity = 2.0f;
    bool     prf        = false;  // can drift queries; opt in after benchmarking
    bool     corrective = false; // lexical proxy rejects semantic matches
    bool     graph      = false; // quadratic graph build; explicit power mode
    // LLM query generation improves recall for difficult research questions,
    // but adds one or more model round trips. Keep normal coding turns on the
    // deterministic hybrid path; users can enable either feature explicitly.
    bool     expand     = false;  // AGENTTY_RAG_EXPAND — multi-query / RAG-Fusion
    bool     hyde       = false;  // AGENTTY_RAG_HYDE — HyDE
    // HyDE/multi-query need an LLM. When enabled, agentty uses a SMALL LOCAL
    // model on the SAME Ollama it embeds with (zero cloud cost / tokens).
    // Override the model via AGENTTY_RAG_GEN_MODEL.
    std::string gen_model = "qwen2.5:0.5b";   // tiny, fast, ubiquitous on Ollama
    bool     persist    = true;   // AGENTTY_RAG_PERSIST — .ragdb cache under .agentty/
    bool     learn      = false;  // implicit file-open feedback is opt-in until
                                  // every source type has an attributable signal
    bool     trace      = false;  // AGENTTY_RAG_TRACE — fold per-stage trace into mode
    // Fusion. rag-cpp measures convex (TM2C2) combination as beating RRF on
    // NDCG, so it is agentty's default; the ADAPTIVE variant additionally
    // shifts the per-query weight toward whichever retriever is more confident
    // on THAT query (a sharp, top-heavy score curve). Set AGENTTY_RAG_FUSION=rrf
    // to fall back to weighted reciprocal-rank fusion, which is the only mode
    // that honours the bm25/dense weights below (convex ignores them).
    std::string fusion = "convex";   // AGENTTY_RAG_FUSION: convex | rrf
    bool     adaptive_fusion = true; // AGENTTY_RAG_ADAPTIVE (convex only)
    // Weighted RRF; both public weights directly affect fusion (rrf mode only).
    float    dense_weight = 1.0f;
    float    bm25_weight  = 1.0f;

    // Proactive / "fork" behaviour — the pre-turn active-RAG path (read by the
    // tools backend, not the retrieve funnel). `proactive` gates the whole
    // pre-turn injection; proactive_min_conf is the CRAG bar to clear (a value
    // > 1.0 is a legitimate "never inject" switch); proactive_bytes caps the
    // injected <retrieved-context> block. Defaults mirror the env reads
    // (AGENTTY_RAG_PROACTIVE / _MIN / _BYTES).
    bool     proactive          = true;
    double   proactive_min_conf = 0.35;
    int      proactive_bytes    = 6144;

    [[nodiscard]] static Config from_env();
};

// The retriever. One long-lived instance backs search_docs + proactive
// retrieval (the backend holds a function-local static). Thread-safe: retrieve()
// may be called concurrently; the docs index is guarded internally.
class Retriever {
public:
    Retriever();
    ~Retriever();
    Retriever(const Retriever&) = delete;
    Retriever& operator=(const Retriever&) = delete;

    // Retrieve up to k passages for `query`. `skip_docs` uses the independent
    // skills+memory index and cannot walk, rebuild, or discard the docs corpus.
    [[nodiscard]] Retrieval retrieve(const std::string& query, int k,
                                     bool skip_docs = false);

    // SEMANTIC CODE SEARCH (backs search_code): index source files under the
    // current working directory (code-aware chunking) and answer `query` with
    // ranked passages. Edit-aware: a cheap fingerprint over the walked tree
    // rebuilds on drift. Independent of the docs index. Never throws.
    [[nodiscard]] Retrieval retrieve_code(const std::string& query, int k);

    // OPTIONAL LLM seam for HyDE + multi-query / RAG-Fusion. Given a prompt,
    // return one or more completions. When set (and AGENTTY_RAG_HYDE /
    // AGENTTY_RAG_EXPAND are on), retrieval uses the LLM to close the
    // query–document asymmetry gap and boost recall. Absent → those features
    // degrade gracefully to plain hybrid search (rag-cpp's contract). agentty
    // wires its own provider here so RAG can "think" with the same model.
    using Generator =
        std::function<std::vector<std::string>(const std::string& prompt, int n)>;
    void set_generator(Generator g);

    // Non-blocking: is the docs index built & fresh for the current root
    // (or is there no docs root, in which case retrieval is always warm)?
    [[nodiscard]] bool warm() const;

    // Non-blocking: can retrieve_code() answer WITHOUT a cold index build?
    // True when the code index is live in memory, or a persisted one exists
    // on disk (loading it is cheap; only a from-scratch build is expensive).
    // Gates OPPORTUNISTIC retrieval — e.g. search_structural's zero-hit
    // semantic leads — so a side-effect query never triggers a cold build.
    [[nodiscard]] bool code_warm() const;

    // Kick a detached background index build so a future turn is warm.
    // Single-flight; returns immediately.
    void warm_async();

    // Stop any in-flight background warm PROMPTLY and join it. Call this early
    // in process teardown (before static destruction) so ^C doesn't stall on
    // a multi-second embed pass. Idempotent; safe if no warm is running.
    void shutdown();

    // Replace the live configuration. Sources/pipeline/fusion changes that
    // alter the corpus or persisted-index identity force a rebuild on the next
    // retrieve(); pure ranking toggles take effect immediately. Thread-safe;
    // never throws. Backs the RAG settings picker's "apply" path.
    void apply_config(const Config& cfg);

    // The currently-live config (for the picker's initial state / round-trip).
    [[nodiscard]] Config snapshot_config() const;

    // Live dense-embedder status, for the RAG picker's status row and
    // `agentty diagnostics`. Mirrors the internal DenseState: when the
    // embedder is unavailable this carries the REASON, so a misconfigured
    // endpoint is a visible message rather than retrieval quietly falling
    // back to BM25 forever.
    //
    // NEVER BLOCKS. Safe to call from the UI thread on any frame: if the
    // retriever is mid-reconfiguration (apply_config re-probes the embedder
    // and rebuilds three engines while holding the lock) this reports
    // Unprobed instead of waiting. A status readout that can stall the render
    // loop is worse than a status readout that is briefly vague.
    struct EmbedStatus {
        enum class State : std::uint8_t { Unprobed, Ready, Unavailable };
        State         state      = State::Unprobed;
        std::uint32_t dim        = 0;   // MEASURED, never user-supplied
        int           latency_ms = 0;
        std::string   reason;           // populated when Unavailable
        std::string   describe;         // "nomic-embed-text via ollama 127.0.0.1:11434"
    };
    [[nodiscard]] EmbedStatus embed_status() const;

    // Re-run the availability probe against the CURRENT config and adopt the
    // measured dimension. Blocking; call off the UI thread. This is what the
    // picker's "Test connection" row drives.
    EmbedStatus reprobe_embedder();

private:
    struct Impl;
    // Owned. unique_ptr with the destructor defined in adapter.cpp (where
    // Impl is complete) — the type system carries the ownership instead of
    // a comment; the header still pulls in no rag:: type.
    std::unique_ptr<Impl> impl_;
};

// ── Learning loop (write side) ─────────────────────────────────────────
// A closed feedback loop with two halves:
//   note_surfaced(paths) — search_docs calls this with the file paths it just
//                          surfaced. Each is recorded as a "use" (denominator)
//                          and remembered as a recently-surfaced candidate.
//   note_file_opened(path) — the tool seam calls this when the agent `read`s a
//                          file. It counts as a "win" (numerator) ONLY when that
//                          path was recently surfaced by retrieval — i.e. the
//                          passage pointed somewhere worth acting on. The
//                          Beta-smoothed win/use rate then nudges that path's
//                          future ranking (read side lives in Retriever).
// Both are best-effort and never throw. AGENTTY_RAG_LEARN=0 disables the loop.
namespace feedback {
void note_surfaced(const std::vector<std::string>& paths);
void note_file_opened(const std::string& path);
}

// ── CLI: `agentty rag-bench <root>` ────────────────────────────────────
namespace bench {
int run(const std::string& root);
}

} // namespace agentty::rag
