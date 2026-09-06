#pragma once
// mcp_tools_backends — the agentty-side HostServices backends for the
// host-coupled tool SHELLS that mcp-cpp's toolset owns (remember/forget/
// wipe, todo, skill, search_docs, task).
//
//   mcp-cpp owns each tool's protocol surface; agentty supplies the work via
//   injected backends. install_host_backends() constructs the adapters
//   (MemoryStore over the JSONL store, SkillResolver over the Agent-Skills
//   engine, DocRetriever over the RAG pipeline, SubagentRunner over the
//   subagent loop) and installs them into the HostServices the bridge passes
//   to make_provider(). A tool whose backend is null isn't advertised.

#include <mcp/tools/host.hpp>

#include <optional>
#include <string>

namespace agentty::store { struct RagConfig; }

namespace agentty::tools {

// Populate svc.memory / svc.skills / svc.retriever / svc.subagent with the
// agentty backends. Leaves svc.todo null (the mcp todo shell needs no host
// state) and svc.http untouched (the bridge installs the HttpClient).
void install_host_backends(::mcp::tools::HostServices& svc);

// Headless one-shot: run `prompt` through the SAME agent loop the `task`
// tool uses (full toolset, doom-loop breaker, retry/backoff, kMaxTurns
// bound) and return the final report text. `agent_type` picks the system
// prompt / model-routing role ("general" default). Requires the subagent
// seam to be installed (main wires it before subcommand dispatch). On
// failure returns the actionable error text and sets is_error.
[[nodiscard]] std::string run_one_shot(const std::string& prompt,
                                       const std::string& agent_type,
                                       bool& is_error);

// Live-apply user RAG configuration (the RAG settings picker's commit path)
// to the process-wide retriever. Rebuilds indexes lazily. Never throws.
void rag_apply_settings(const store::RagConfig& cfg);

// Live dense-embedder status for the RAG picker's status row and
// `agentty diagnostics`. When the embedder is unavailable, `reason` says why
// — the point being that a misconfigured endpoint is visible rather than
// retrieval silently degrading to keyword-only forever.
struct RagEmbedStatus {
    enum class State : std::uint8_t { Unprobed, Ready, Unavailable };
    State         state      = State::Unprobed;
    std::uint32_t dim        = 0;
    int           latency_ms = 0;
    std::string   reason;
    std::string   describe;
};
[[nodiscard]] RagEmbedStatus rag_embed_status();

// Probe an ARBITRARY embed configuration without disturbing the live
// retriever — what the picker's "Test connection" row runs on a worker
// thread. Returns the MEASURED dimension, the only trustworthy source for it.
struct RagProbeOutcome {
    bool          ok = false;
    std::uint32_t dim = 0;
    int           latency_ms = 0;
    std::string   error;
};
[[nodiscard]] RagProbeOutcome rag_probe_embedder(const store::RagConfig& cfg,
                                                 const std::string& api_key);

// Stop the process-wide retriever's in-flight background warm and reclaim its
// worker. Call EARLY in teardown (before static destruction) so ^C is prompt:
// otherwise the retriever's warm jthread is joined at static-dtor time, after
// main() returns, and blocks the exit on a multi-second embed pass. Idempotent.
void rag_shutdown();

// Whether the proactive pre-turn injection is enabled, per the live config
// (persisted RAG picker), with AGENTTY_RAG_PROACTIVE as a shell override.
[[nodiscard]] bool proactive_enabled();

// Whether the GLOBAL RAG mode is "first turn only" (proactive injection only
// on a thread's first turn). Consulted by the per-turn gate when a thread has
// no per-thread override.
[[nodiscard]] bool proactive_first_turn_only();

// Smart Mode Innovation 3 (SPECULATIVE): kick a detached, best-effort code/
// docs retrieval warm-up for `query` so the workspace index is hot and the
// grounding is pre-fetched by the time the orchestrator delegates — real
// speculative execution overlapping the lead's thinking, with zero wasted-
// token risk (retrieval is local). Returns immediately; never throws.


// ── Proactive retrieval (explicit opt-in) ────────────────────────────
// Run the RAG pipeline outside the model's tool loop. The app invokes the
// blocking form on an isolated worker and launches the model only after it
// settles, so each turn performs at most one retrieval. Automatic proactive
// injection is disabled unless AGENTTY_RAG_PROACTIVE=1.
struct ProactiveHit {
    std::string block;        // fenced <retrieved-context> text for the wire
    double      confidence;   // [0,1] retrieval confidence that cleared the bar
    int         passages;     // how many passages the block carries
    // Cross-turn dedup keys (source:path:line) for the passages this block
    // ACTUALLY carries. proactive_retrieve builds the block on a worker that
    // may be abandoned on a latency-budget overrun, so the keys are only
    // COMMITTED to the dedup FIFO once the hit is really returned to the
    // caller — an abandoned worker never suppresses a passage it didn't show.
    std::vector<std::string> dedup_keys;
};
[[nodiscard]] std::optional<ProactiveHit>
proactive_retrieve(const std::string& query, int k = 3);

// Same single-execution funnel used by the app's isolated worker.
// the caller always injects/stages this result. Never throws.
[[nodiscard]] std::optional<ProactiveHit>
proactive_retrieve_blocking(const std::string& query, int k = 3);

} // namespace agentty::tools
