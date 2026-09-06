// mcp_tools_backends.cpp — the agentty-side HostServices backends the
// mcp-cpp toolset's host-coupled SHELLS dispatch into.
//
//   mcp-cpp owns the protocol surface for remember/forget/wipe, todo,
//   skill, search_docs, and task — names, schemas, arg parsing, scope
//   validation, dedup/dry-run messaging, formatting. But the DATA and the
//   WORK for those tools live in agentty: the JSONL memory store, the
//   Agent-Skills engine, the RAG pipeline, the subagent loop. This file is
//   the inversion-of-control seam: each backend is a small class deriving
//   an `mcp::tools::*` interface and delegating to agentty's existing
//   subsystem, with the EXACT arg→backend mapping, scope vocabulary, and
//   output formatting the native tool bodies had.
//
//   Built + installed by build_mcp_tool_defs() (mcp_tools_bridge.cpp).

#include "agentty/util/user_root.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"
#include "agentty/util/home_dir.hpp"

#include "agentty/scope/scope.hpp"
#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/registry.hpp"   // tools::progress::emit
#include "agentty/tool/tool.hpp"       // tool::DynamicDispatch, ToolUse, Message …
#include "agentty/tool/util/partial_json.hpp"   // args salvage for truncated tool JSON

#include "agentty/provider/anthropic/provider.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/error_class.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/wire.hpp"

#include "agentty/rag/rag_adapter.hpp"
#include "agentty/rag/embed_secret.hpp"
#include <thread>
#include "agentty/io/persistence.hpp"
#include "agentty/store/store.hpp"

#include "agentty/mcp/client.hpp"   // mcp_resources / mcp_read_resource seams
#include "agentty/util/dbglog.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/util/isolated_thread.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>

#include "mcp/tools/util/fs_helpers.hpp"   // util::ReadContextScope
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

namespace {

namespace mt  = ::mcp::tools;
namespace fs  = std::filesystem;
using json    = nlohmann::json;

// AGENTTY_TRACE_TOOLS=1 — emit a `TOOL <name> <ok|error>` line to stderr for
// each tool the headless agent loop executes. Read once (the env can't change
// mid-process). Used by run_one_shot's runner and the Tier-2 agentic evals.
[[nodiscard]] bool trace_tools_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AGENTTY_TRACE_TOOLS");
        return v && v[0] && v[0] != '0' && v[0] != 'f' && v[0] != 'F'
                 && v[0] != 'n' && v[0] != 'N';
    }();
    return on;
}

// ── MemoryStore ────────────────────────────────────────────────────────
//   Backs remember / forget / wipe. The shell owns the schema + dedup/pin/
//   tag/supersede surface; this maps its requests onto agentty::tools::
//   memory free functions. Scope vocabulary is ["project","user"] so the
//   shell defaults to project (the safer default, matching the native
//   remember tool) and accepts "user" for cross-project facts.
// Drift guard for the parallel result structs bridged in append() below.
// agentty::tools::memory::AppendResult and mcp::tools::MemoryAppendResult carry
// identical fields in identical order; the bridge copies them one by one. If a
// field is added/removed on either side their sizes diverge and this trips at
// compile time — a reminder to update BOTH structs and the mapping. (The
// designated-init mapping below separately guards renames/removals.)
static_assert(sizeof(memory::AppendResult) == sizeof(mt::MemoryAppendResult),
              "AppendResult / MemoryAppendResult drifted — update both structs "
              "and the field mapping in AgenttyMemoryStore::append()");

class AgenttyMemoryStore final : public mt::MemoryStore {
public:
    // scopes()[0] is the DEFAULT scope the shell uses when the model omits
    // `scope`. We only advertise "project" (and only make it the default)
    // when project storage is actually WRITABLE here. In a workspace whose
    // root is "/" or otherwise unwritable, path_for(Project) is empty and a
    // project append would fail — so offering "project" as the default made
    // the model's very first remember() call fail every time, forcing a
    // retry with scope="user". Dropping the unavailable scope from the
    // vocabulary makes the default "user", which always succeeds, WITHOUT
    // silently promoting an explicit project fact to user scope (append()
    // still refuses an explicit unavailable-project write — no cross-
    // workspace memory bleed).
    std::vector<std::string> scopes() const override {
        if (!memory::path_for(memory::Scope::Project).empty())
            return {"project", "user"};   // project writable → normal order
        return {"user"};                   // project unavailable → user is default
    }

    mt::MemoryAppendResult append(const mt::MemoryAppendRequest& req) override {
        auto scope = memory::parse_scope(req.scope);
        if (!scope)
            return mt::MemoryAppendResult{.error = "unknown scope '" + req.scope + "'"};

        // Smart scope: the shell defaults an omitted `scope` to scopes()[0]
        // ("project" when writable), so a fact that's plainly about the HUMAN
        // — "my name is…", "I use fish" — lands in project by default and then
        // bleeds into every repo. suggest_scope() reads the fact text; on a
        // CONFIDENT user signal against a project write we correct it and say
        // so in the note. We only ever redirect project→user this way (never
        // the reverse, and never when project was genuinely intended for a
        // codebase fact): the risk is one-directional — a personal fact
        // escaping into shared project memory is the harm; a project fact in
        // user memory is merely narrow. An UNAVAILABLE user scope is left
        // alone (append's own guard handles reachability).
        std::string smart_note;
        if (*scope == memory::Scope::Project) {
            const auto hint = memory::suggest_scope(req.text);
            if (hint.confident() && hint.scope == memory::Scope::User
                && !memory::path_for(memory::Scope::User).empty()) {
                *scope = memory::Scope::User;
                smart_note = "scope→user (" + hint.reason + ")";
            }
        }

        memory::AppendOptions opts;
        opts.pinned        = req.pinned;
        opts.tags          = req.tags;
        opts.supersedes_id = req.supersedes_id;
        const auto r = memory::append(*scope, req.text, opts);

        // Bridge the agentty result onto the mcp result. Named designated
        // initialisers so a renamed/removed field is a compile error rather
        // than a silent miscopy; the static_assert above catches size drift.
        std::string note = r.note;
        if (!smart_note.empty())
            note = note.empty() ? smart_note : (smart_note + "; " + note);
        return mt::MemoryAppendResult{
            .id      = r.id,
            .error   = r.error,
            .note    = note,
            .rolled  = r.rolled,
            .deduped = r.deduped,
        };
    }

    std::size_t forget_by_id(const std::string& id) override {
        return memory::forget_by_id(id);
    }
    std::size_t forget_by_substring(const std::string& needle) override {
        return memory::forget_by_substring(needle);
    }
    std::vector<mt::MemoryRecord> preview_forget(const std::string& needle) override {
        std::vector<mt::MemoryRecord> out;
        for (const auto& r : memory::preview_forget_by_substring(needle)) {
            mt::MemoryRecord m;
            m.id     = r.id;
            m.text   = r.text;
            m.scope  = std::string{memory::to_string(r.scope)};
            m.pinned = r.pinned;
            m.tags   = r.tags;
            m.ts     = r.ts;
            m.hits   = r.hits;
            out.push_back(std::move(m));
        }
        return out;
    }

    std::optional<std::size_t> preview_wipe(const std::string& scope) override {
        auto s = memory::parse_scope(scope);
        if (!s || memory::path_for(*s).empty()) return std::nullopt;
        return memory::load_all(*s).size();
    }

    std::optional<std::size_t> wipe(const std::string& scope) override {
        auto s = memory::parse_scope(scope);
        if (!s) return std::nullopt;
        return memory::wipe(*s);
    }
};

// ── SkillResolver ──────────────────────────────────────────────────────
//   Backs the skill tool. The shell returns whatever string load() yields
//   verbatim, so load() returns the FULL activation payload (body wrapped
//   in <skill_content>, the absolute skill dir, the <skill_resources>
//   listing) — identical to the native tool — and applies the same
//   re-activation dedup (spec §5: don't re-inject a body already in
//   context). On an unknown name it leaves the body empty and fills `err`
//   with the available-skills recovery hint.
class AgenttySkillResolver final : public mt::SkillResolver {
public:
    std::optional<std::string> load(const std::string& name, std::string& err) override {
        const auto* s = skills::find(name);
        if (!s) {
            std::ostringstream avail;
            bool first = true;
            for (const auto& sk : skills::all()) {
                avail << (first ? "" : ", ") << sk.name;
                first = false;
            }
            err = "no skill named '" + name + "'";
            if (!first) err += " — available: " + avail.str();
            else        err += " — no skills are installed in this workspace";
            return std::nullopt;
        }
        if (!skills::note_activated(s->name)) {
            return "Skill '" + s->name + "' is already active in this "
                   "session — its instructions are in an earlier tool_result. "
                   "Refer to that instead of re-loading.";
        }
        return skills::activation_payload(*s);
    }
};

// ── DocRetriever ───────────────────────────────────────────────
//   Backs search_docs. Runs agentty's full RAG pipeline (rag-cpp) and returns
//   flat passages. The funnel, as actually wired in src/rag/adapter.cpp:
//
//     sources:  docs folder + skills + memory, with MCP resources opt-in
//     retrieve: BM25 plus probed Ollama dense retrieval, weighted RRF fusion
//     optional: PRF, GraphRAG, CRAG, HyDE, and multi-query are explicit modes
//     rerank:   deterministic feature rerank + MMR + adjacent-hit stitch
//     compress: query-focused spans under one aggregate output budget
//     persist:  validated manifest + incremental docs/code refresh
//
//   The `mode` string carries the rich provenance (root path, mode,
//   reranked, +N variants, confidence) so no signal is lost when the
//   shell renders the result.
// Map a persisted store::RagConfig onto a live rag::Config, preserving the
// infra fields the picker never exposes (docs root) from the given base
// (env-derived) config.
static ::agentty::rag::Config rag_config_from_settings(
        const store::RagConfig& s, ::agentty::rag::Config base) {
    base.skills          = s.skills;
    base.memory          = s.memory;
    base.mcp_resources   = s.mcp_resources;
    base.contextual      = s.contextual;
    base.dedup           = s.dedup;
    base.mmr             = s.mmr;
    base.stitch          = s.stitch;
    base.autocut         = s.autocut;
    base.mmr_lambda          = s.mmr_lambda;
    base.dedup_threshold     = s.dedup_threshold;
    base.autocut_sensitivity = s.autocut_sensitivity;
    base.dense_weight        = s.dense_weight;
    base.bm25_weight         = s.bm25_weight;
    base.prf             = s.prf;
    base.corrective      = s.corrective;
    base.graph           = s.graph;
    base.expand          = s.expand;
    base.hyde            = s.hyde;
    base.fusion          = s.fusion;
    base.adaptive_fusion = s.adaptive_fusion;
    base.persist         = s.persist;
    base.learn           = s.learn;
    base.trace           = s.trace;
    base.proactive          = s.proactive;
    base.proactive_min_conf = s.proactive_min_conf;
    base.proactive_bytes    = s.proactive_bytes;

    // Embeddings. An empty backend id means the user never opened the pane,
    // so the env-derived embed config stands untouched.
    if (!s.embed_backend.empty()) {
        namespace eb = ::agentty::rag::embed;
        auto& e = base.embed;
        e.backend = eb::backend_from_id(s.embed_backend);
        if (!s.embed_model.empty())      e.model          = s.embed_model;
        if (!s.embed_host.empty())       e.host           = s.embed_host;
        if (s.embed_port != 0)           e.port           = s.embed_port;
        e.tls            = s.embed_tls;
        e.path           = s.embed_path;
        e.model_path     = s.embed_model_path;
        e.tokenizer_path = s.embed_tokenizer_path;
        e.dim            = s.embed_dim;
        // The credential never round-trips through settings.json; it is
        // fetched from the keystore/sealed store keyed by endpoint.
        if (eb::needs_api_key(e.backend)) {
            if (auto key = eb::load_key(eb::endpoint_key(e)); !key.empty())
                e.api_key = std::move(key);
        }
    }
    return base;
}

// A single process-wide Retriever backs both the search_docs tool and the
// proactive pre-turn path. Function-local static ⇒ constructed on first use
// (after the app has set cwd / env), destroyed at exit. On first construction
// we fold in any persisted RAG settings (the RAG picker) so the user's saved
// config wins over the env-derived defaults; if the user has never touched the
// picker (configured=false) the env config stands.
static ::agentty::rag::Retriever& shared_retriever() {
    static ::agentty::rag::Retriever r;
    // Fold in persisted RAG picker settings exactly once, on first use.
    static const bool configured_once = [] {
        try {
            auto s = persistence::load_settings();
            if (s.rag.configured)
                r.apply_config(rag_config_from_settings(s.rag, r.snapshot_config()));
        } catch (...) { /* best-effort; env config stands */ }
        return true;
    }();
    (void)configured_once;
    return r;
}

} // namespace (anonymous)

// Live-apply a RagConfig from the running app (the RAG settings picker's
// commit path). Thread-safe; rebuilds indexes lazily on the next retrieve.
void rag_apply_settings(const store::RagConfig& s) {
    // NEVER BLOCKS. apply_config() takes the retriever's mutex, re-probes the
    // embedder when the vector space changed (a network dial with a
    // multi-second timeout) and cold-rebuilds three engines.
    //
    // This is called from REDUCERS — the UI thread, between two frames — so
    // doing that work synchronously froze the render loop, animations
    // included. Making every caller remember to wrap it in a Cmd is a rule
    // that will be forgotten; making the function itself async is a property
    // that cannot be.
    //
    // Detached rather than queued: applying settings is idempotent and
    // last-writer-wins, so a superseded apply doing redundant work is
    // harmless, and the retriever's own mutex serialises them.
    try {
        std::thread([s] {
            try {
                auto& r = shared_retriever();
                r.apply_config(rag_config_from_settings(s, r.snapshot_config()));
            } catch (...) { /* best-effort */ }
        }).detach();
    } catch (...) { /* thread creation failed: skip the apply, never block */ }
}

RagEmbedStatus rag_embed_status() {
    RagEmbedStatus out;
    try {
        const auto s = shared_retriever().embed_status();
        using S = ::agentty::rag::Retriever::EmbedStatus::State;
        out.state = s.state == S::Ready       ? RagEmbedStatus::State::Ready
                  : s.state == S::Unavailable ? RagEmbedStatus::State::Unavailable
                                              : RagEmbedStatus::State::Unprobed;
        out.dim        = s.dim;
        out.latency_ms = s.latency_ms;
        out.reason     = s.reason;
        out.describe   = s.describe;
    } catch (...) {
        out.state  = RagEmbedStatus::State::Unavailable;
        out.reason = "retriever unavailable";
    }
    return out;
}

RagProbeOutcome rag_probe_embedder(const store::RagConfig& s, const std::string& api_key) {
    namespace eb = ::agentty::rag::embed;
    RagProbeOutcome out;
    try {
        // Build the candidate config WITHOUT touching the live retriever: the
        // user is testing a config they have not committed, and a failed probe
        // must not degrade the retrieval they currently have working.
        ::agentty::rag::Config base;
        eb::apply_env(base.embed);
        auto cfg = rag_config_from_settings(s, base);
        if (!api_key.empty()) cfg.embed.api_key = api_key;
        // Never trust a carried-over dimension: measuring it is the point.
        cfg.embed.dim = 0;

        auto r = eb::probe(cfg.embed);
        if (const auto* ok = std::get_if<eb::ProbeOk>(&r)) {
            out.ok         = true;
            out.dim        = ok->dim;
            out.latency_ms = ok->latency_ms;
        } else {
            out.error = std::get<eb::ProbeErr>(r).why;
        }
    } catch (const std::exception& e) {
        out.error = e.what();
    } catch (...) {
        out.error = "unknown error while probing";
    }
    return out;
}

// Prompt teardown of the retriever's background warm. Without this the warm
// jthread is only joined when the function-local `static Retriever` is
// destroyed at process exit (after main returns), which blocked ^C for 4–10 s
// while an in-flight embed pass finished. Called early in main()'s teardown.
void rag_shutdown() {
    try { shared_retriever().shutdown(); } catch (...) { /* best-effort */ }
}

bool proactive_enabled() {
    // Shell override wins for one-off runs; otherwise the live config (which
    // folds in the persisted RAG picker) is the source of truth. Default off
    // at the app level — proactive injection is an explicit opt-in.
    if (const char* v = std::getenv("AGENTTY_RAG_PROACTIVE"); v && v[0]) {
        std::string s{v};
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return s == "1" || s == "true" || s == "on" || s == "yes";
    }
    try { return shared_retriever().snapshot_config().proactive; }
    catch (...) { return false; }
}

bool proactive_first_turn_only() {
    try {
        auto s = persistence::load_settings();
        return s.rag.configured && s.rag.mode == store::RagMode::FirstTurnOnly;
    } catch (...) { return false; }
}

namespace {

class AgenttyDocRetriever final : public mt::DocRetriever {
public:
    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        return run_(q, mode, err);
    }

private:
    static std::vector<mt::DocPassage>
    run_(const mt::DocQuery& q, std::string& mode, std::string& err) {
        std::vector<mt::DocPassage> out;
        auto res = shared_retriever().retrieve(q.query, q.k);
        if (!res.error.empty()) { err = res.error; return out; }
        mode = res.mode;
        // A successful search that matched NOTHING is not a failure — but a
        // bare empty result makes "corpus is irrelevant" indistinguishable
        // from "index empty / Ollama down / no docs configured". Fold a short
        // reason into the MODE label (which the shell renders as context, not
        // as an error) so the model/user can tell why. err is left empty: a
        // genuine failure already comes back via res.error above.
        if (res.passages.empty()) {
            mode = res.mode.empty()
                 ? "no matches (no docs indexed — set AGENTTY_DOCS_DIR, or none "
                   "of your skills/memory matched)"
                 : ("no matches (" + res.mode + ")");
            return out;
        }
        out.reserve(res.passages.size());
        for (auto& p : res.passages) {
            mt::DocPassage d;
            d.source     = std::move(p.source);
            d.path       = std::move(p.path);
            d.line_start = p.line_start;
            d.line_end   = p.line_end;
            d.score      = p.score;
            d.text       = std::move(p.text);
            out.push_back(std::move(d));
        }
        return out;
    }
};


// ── CodeRetriever ──────────────────────────────────────────
//   definition-aware source chunks; BM25 is always available and a bounded
//   Ollama probe enables dense retrieval. A pruned 4,000-file manifest detects
//   drift, and small edit sets update only changed files.
class AgenttyCodeRetriever final : public mt::DocRetriever {
public:
    // Opportunistic queries (structural zero-hit leads, over-budget ordering)
    // must never trigger a cold code-index build — only explicit search_code
    // calls pay that. Warm ⇔ index live in memory or persisted on disk.
    bool warm() const override { return shared_retriever().code_warm(); }

    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        std::vector<mt::DocPassage> out;
        auto res = shared_retriever().retrieve_code(q.query, q.k);
        if (!res.error.empty()) { err = res.error; return out; }
        mode = res.mode;
        out.reserve(res.passages.size());
        for (auto& p : res.passages) {
            mt::DocPassage d;
            d.source     = std::move(p.source);
            d.path       = std::move(p.path);
            d.line_start = p.line_start;
            d.line_end   = p.line_end;
            d.score      = p.score;
            d.text       = std::move(p.text);
            out.push_back(std::move(d));
        }
        return out;
    }
};

// ── SubagentRunner ─────────────────────────────────────────────────────
//   Backs the task tool. The ENTIRE isolated agent loop — agent-type role
//   prompts, the per-completion stream reassembly, local tool dispatch, the
//   activity feed pumped to the parent card via progress::emit, the report
//   harvest — lives here, lifted verbatim from the native task tool. The
//   shell owns only the schema + the availability gate; run() does the work.
//
//   The native task tool's `display_description` arg is UI-only and stays in
//   the shell's schema; run() never needs it.

// Where an agent persona came from — the provenance the task card surfaces
// so a repo-injected agent is never invisible (transparency, not a gate).
enum class AgentOrigin : std::uint8_t { Builtin, User, Project };

struct AgentType {
    std::string_view              name;
    bool                          read_only;
    std::string_view              role;
    std::vector<std::string_view> allow;   // empty ⇒ all (minus task)
    smart::ModelRole              model_role = smart::ModelRole::Utility;
    // Where this persona came from. Built-ins and the user's OWN
    // ~/.agentty/agents are trusted-by-authorship; a PROJECT-scoped agent
    // rode in on the workspace (a clone could inject its role prompt), so the
    // task card surfaces "project agent" — not a block, just so an injected
    // persona is never invisible. Mirrors the plugins picker's scope badge.
    AgentOrigin                   origin = AgentOrigin::Builtin;
};

// ── User-defined agents (.agentty/agents/*.md) ─────────────────────
// A user agent is a markdown file whose BODY becomes the role prompt and
// whose frontmatter configures the sandbox:
//
//     ---
//     description: Reviews SQL migrations for safety.   # for the catalog
//     tools: read grep glob list_dir                     # allowlist (opt)
//     read-only: true                                    # effect gate (opt)
//     ---
//     Your role: MIGRATION REVIEWER. Check every migration for …
//
// Roots (project first, then user; first name wins — mirrors commands/
// skills): .agentty/agents, .agents/agents, .claude/agents, then the same
// three under ~. Built-in types always win over a user agent of the same
// name (a user file can't silently replace `general`).
//
// Storage note: AgentType's fields are string_views, so the parsed user
// agents live in a process-lifetime cache; entries are appended once per
// (mtime-signature) scan and NEVER erased or reordered in place — a
// returned reference stays valid for the life of a subagent run.
struct UserAgentStore {
    std::vector<std::unique_ptr<AgentType>> types;   // stable addresses
    std::deque<std::string>                 owned;   // element-stable backing
    // Prior generations, kept alive for the process lifetime: a subagent
    // holds a `const AgentType&` across its whole (minutes-long) run, so a
    // cache refresh mid-run must not destroy the referenced storage.
    // Bounded in practice by how often the agent files change on disk.
    std::vector<std::vector<std::unique_ptr<AgentType>>> retired_types;
    std::vector<std::deque<std::string>>                 retired_owned;
    std::string                             sig = "\x01uninit";
    std::mutex                              mu;
};

UserAgentStore& user_agents() {
    static UserAgentStore s;
    return s;
}

// Frontmatter subset parser (same lenient rules as commands/skills).
void parse_user_agent(const std::string& raw, const std::string& slug,
                      AgentOrigin origin, UserAgentStore& store) {
    auto trim = [](std::string_view v) -> std::string {
        std::size_t b = 0, e = v.size();
        while (b < e && std::isspace(static_cast<unsigned char>(v[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(v[e-1]))) --e;
        return std::string{v.substr(b, e - b)};
    };

    std::string body = raw, tools_line;
    bool read_only = false;
    std::istringstream in(raw);
    std::string line;
    if (std::getline(in, line) && trim(line) == "---") {
        std::streampos body_start{-1};
        while (std::getline(in, line)) {
            if (trim(line) == "---") { body_start = in.tellg(); break; }
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = trim(line.substr(0, colon));
            std::string v = trim(line.substr(colon + 1));
            if      (k == "tools")     tools_line = v;
            else if (k == "read-only") read_only = (v == "true" || v == "1" || v == "yes");
            // description consumed by extra_agent_types' caller via the
            // task-tool description; not needed in the AgentType itself.
        }
        if (body_start != std::streampos(-1))
            body = trim(raw.substr(static_cast<std::size_t>(body_start)));
    }
    body = trim(body);
    if (body.empty()) return;

    auto at = std::make_unique<AgentType>();
    store.owned.push_back(slug);
    at->name = store.owned.back();
    store.owned.push_back(std::move(body));
    at->role = store.owned.back();
    at->read_only = read_only;
    at->model_role = read_only ? smart::ModelRole::Utility
                               : smart::ModelRole::Implementation;
    at->origin = origin;
    // Whitespace-split tools allowlist — each token stored owned.
    std::istringstream ts(tools_line);
    std::string tok;
    while (ts >> tok) {
        store.owned.push_back(tok);
        at->allow.push_back(store.owned.back());
    }
    store.types.push_back(std::move(at));
}

// Scan the six roots; refresh the cache when the mtime signature moves.
// Returns under the store lock — callers copy names or hold refs to the
// stable unique_ptr targets.
void refresh_user_agents_locked(UserAgentStore& store) {
    namespace fs = std::filesystem;
    std::string sig;
    auto scan_root = [&](const fs::path& root) {
        std::error_code ec;
        if (!fs::is_directory(root, ec) || ec) return;
        auto mt = fs::last_write_time(root, ec);
        if (!ec)
            sig += std::to_string(static_cast<long long>(
                       mt.time_since_epoch().count())) + ";";
        std::vector<fs::path> files;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec))
            if (it->path().extension() == ".md") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            std::error_code fec;
            auto fmt = fs::last_write_time(p, fec);
            if (!fec)
                sig += std::to_string(static_cast<long long>(
                           fmt.time_since_epoch().count())) + ";";
        }
    };
    auto home = agentty::util::home_dir_or_empty();
    // Root ladder from scope::plan (Locus-major, Dialect-minor): project
    // .agentty ▷ .agents ▷ .claude ▷ the same three under ~. Same order the
    // hand-written array had; project stays cwd-relative. Built-ins still win
    // over any user agent of the same name (enforced in the load loop below).
    // Each root carries its scope origin so the task card can surface a
    // "project agent" tag (a repo-shipped persona is never invisible).
    std::vector<std::pair<fs::path, AgentOrigin>> roots;
    {
        scope::Env env;
        env.home             = home;
        env.user_native_base = ::agentty::util::user_root();
        env.project_root     = fs::path{"."};
        env.project_writable = true;
        const scope::Layout layout{.leaf = "agents"};
        for (const scope::Source& src : scope::plan(layout, env)) {
            const AgentOrigin org = src.locus == scope::Locus::User
                ? AgentOrigin::User : AgentOrigin::Project;
            roots.emplace_back(src.base / layout.leaf, org);
        }
    }
    for (const auto& [r, org] : roots) if (!r.empty()) scan_root(r);

    if (sig == store.sig) return;
    store.sig = sig;
    // Retire (never destroy) the previous generation — see UserAgentStore.
    if (!store.types.empty()) {
        store.retired_types.push_back(std::move(store.types));
        store.retired_owned.push_back(std::move(store.owned));
    }
    store.types.clear();
    store.owned.clear();
    // std::deque never moves existing elements on push_back, so the
    // string_views taken into owned.back() stay valid even for SSO-small
    // strings (a vector would move the inline bytes on reallocation).
    constexpr std::size_t kMaxUserAgents = 32;
    constexpr std::size_t kMaxAgentBytes = 32 * 1024;
    static const char* kBuiltins[] = {"explorer","reviewer","tester",
                                      "coder","general"};
    for (const auto& [r, org] : roots) {
        if (r.empty()) continue;
        std::error_code ec;
        if (!fs::is_directory(r, ec) || ec) continue;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(r, ec), end; !ec && it != end;
             it.increment(ec))
            if (it->path().extension() == ".md") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            if (store.types.size() >= kMaxUserAgents) return;
            const std::string slug = p.stem().string();
            if (slug.empty() || slug[0] == '.') continue;
            bool taken = false;
            for (auto* b : kBuiltins) if (slug == b) { taken = true; break; }
            for (const auto& t : store.types)
                if (t->name == slug) { taken = true; break; }
            if (taken) continue;
            std::error_code sec;
            auto sz = fs::file_size(p, sec);
            if (sec || sz == 0 || sz > kMaxAgentBytes) continue;
            std::ifstream f(p, std::ios::binary);
            if (!f) continue;
            std::string raw(static_cast<std::size_t>(sz), '\0');
            f.read(raw.data(), static_cast<std::streamsize>(sz));
            raw.resize(static_cast<std::size_t>(f.gcount()));
            parse_user_agent(raw, slug, org, store);
        }
    }
}

// Names of the discovered user agents (for the task tool's enum).
std::vector<std::string> user_agent_names() {
    auto& store = user_agents();
    std::lock_guard lk(store.mu);
    refresh_user_agents_locked(store);
    std::vector<std::string> out;
    out.reserve(store.types.size());
    for (const auto& t : store.types) out.emplace_back(t->name);
    return out;
}

const AgentType& resolve_agent_type(std::string_view t) {
    using MR = smart::ModelRole;
    static const std::vector<AgentType> kTypes = {
        {"explorer", true,
         "Your role: EXPLORER. Map and explain the codebase region the task "
         "names. Read widely, trace call sites and definitions, and return a "
         "precise map: the key files, the functions/types involved, how they "
         "connect, and any gotchas. Cite exact file paths and line numbers. "
         "You are READ-ONLY \xe2\x80\x94 never modify anything.",
         {"read", "grep", "glob", "list_dir", "find_definition", "repo_map",
          "web_search", "web_fetch"}, MR::Utility},
        {"reviewer", true,
         "Your role: REVIEWER. Critically review the code or change the task "
         "names. Look for bugs, edge cases, race conditions, security issues, "
         "and deviations from the surrounding conventions. Return findings as "
         "a prioritised list (blocker / major / minor / nit), each with the "
         "exact file:line and a concrete fix suggestion. You are READ-ONLY.",
         {"read", "grep", "glob", "list_dir", "find_definition", "repo_map",
          "git_diff", "git_log", "git_status"}, MR::Strategic},
        {"tester", false,
         "Your role: TESTER. Reproduce, run, and diagnose. Build/run the "
         "relevant tests or commands the task names, read the failures, and "
         "report the root cause with the exact failing assertion and the "
         "file:line that produced it. Prefer running over guessing. Do NOT "
         "rewrite production code \xe2\x80\x94 only run, read, and diagnose.",
         {"read", "grep", "glob", "list_dir", "find_definition", "repo_map",
          "shell", "diagnostics", "git_diff", "git_status"}, MR::Implementation},
        {"coder", false,
         "Your role: CODER. Implement the change the task names end-to-end: "
         "read the relevant code first, make the edits, and verify they build/"
         "compile if a build command is obvious. Follow the surrounding "
         "conventions exactly. Report what you changed (files + a one-line "
         "summary each) and whether it built.",
         {}, MR::Implementation},
        {"general", false,
         "Your role: GENERAL. Complete the delegated task end-to-end using "
         "whatever tools fit, then report the outcome.",
         {}, MR::Implementation},
    };
    for (const auto& a : kTypes)
        if (a.name == t) return a;
    // User-defined agents (.agentty/agents/*.md): consulted after the
    // built-ins so a user file can never shadow `general` etc. The
    // returned reference points into process-lifetime storage (see
    // UserAgentStore) so it outlives the whole subagent run.
    {
        auto& store = user_agents();
        std::lock_guard lk(store.mu);
        refresh_user_agents_locked(store);
        for (const auto& ua : store.types)
            if (ua->name == t) return *ua;
    }
    return kTypes.back();
}

// Did the subagent's OWN report say it failed?
//
// The exit path can't always tell. An agent that burns its turn budget, or
// decides mid-way that it can't proceed, often still returns a well-formed
// final message — and that message says, in plain words, that it did not do
// the work. Reporting that as a success is worse than useless: the caller
// (a model or a human skimming a ✓) acts on an outcome that never happened.
// This was observed exactly once and was enough: a coder subagent reported
// "Task NOT completed … made zero edits", and the UI rendered ✓ DONE.
//
// Deliberately CONSERVATIVE — a false positive downgrades a good run to a
// warning, so we only match unambiguous self-declarations, anchored near the
// start of the report where a verdict lives. Prose like "the previous
// approach did not work, so I fixed it" must NOT trip this, which is why
// there is no bare "did not work" / "failed" pattern.
[[nodiscard]] bool subagent_self_reported_failure(const std::string& report) {
    // Only inspect the opening of the report: a verdict is stated up front
    // (an "## OUTCOME" header, a first line). Scanning the whole body would
    // match narration of intermediate failures that were later fixed.
    constexpr std::size_t kVerdictWindow = 400;
    std::string head = report.substr(0, std::min(report.size(), kVerdictWindow));
    for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Strip markdown emphasis so "**Task NOT completed**" matches too.
    std::string flat;
    flat.reserve(head.size());
    for (char c : head)
        if (c != '*' && c != '_' && c != '`') flat.push_back(c);

    static constexpr std::string_view kVerdicts[] = {
        "task not completed",
        "task was not completed",
        "not completed",
        "task incomplete",
        "i was unable to complete",
        "unable to complete the task",
        "i could not complete",
        "could not complete the task",
        "no files were created or modified",
        "made zero edits",
        "zero edits",
        "i made no changes",
        "no changes were made",
        "i did not make any changes",
    };
    for (auto v : kVerdicts)
        if (flat.find(v) != std::string_view::npos) return true;
    return false;
}

std::string subagent_system_prompt(const AgentType& type) {
    std::string base = provider::default_system_prompt(/*lean=*/true);
    base += "\n\n<subagent>\n";
    base += std::string{type.role};
    base +=
        "\n\nYou are a SUBAGENT spawned to complete ONE delegated task in "
        "isolation. You do NOT see the parent conversation and cannot ask it "
        "questions \xe2\x80\x94 work fully autonomously from the task prompt alone. "
        "Use your tools to investigate and act, then STOP calling tools and "
        "write your final report as plain text.\n\n"
        "DECIDE, DON'T STALL: if a detail is ambiguous, make the most "
        "reasonable assumption, proceed, and NOTE the assumption in your report "
        "\xe2\x80\x94 do not abort a whole delegation over a minor gap. Only stop early if "
        "the task is genuinely impossible or self-contradictory.\n\n"
        "Your final message is the ONLY thing the parent receives \xe2\x80\x94 not your "
        "transcript, not your tool output. So the report must stand alone and be "
        "TIGHT \xe2\x80\x94 the parent is paying context for every line. No preamble, no "
        "recap of what you were asked, no narration of your steps. Structure it "
        "as:\n"
        "  \xe2\x80\xa2 A one-line OUTCOME (what you found / did).\n"
        "  \xe2\x80\xa2 Only the details the parent needs to ACT, with exact file:line "
        "references where relevant \xe2\x80\x94 evidence, not prose.\n"
        "  \xe2\x80\xa2 Any assumption you made, and anything you could NOT determine, "
        "stated plainly.\n"
        "Cite evidence (paths, line numbers, command output); never invent a "
        "path or line you didn't see. Do not pad or hedge. If the task is "
        "impossible or underspecified beyond a reasonable assumption, say so and "
        "explain exactly what's missing rather than guessing.";
    if (type.read_only)
        base += "\n\nYou are READ-ONLY: you have no tools that modify files, "
                "run commands, or reach the network. Investigate and report only.";
    base += "\n</subagent>";
    return base;
}

std::string summarize_call(const ToolUse& tc) {
    std::string s = tc.name.value;
    if (tc.args.is_object()) {
        for (const char* key : {"path", "file_path", "pattern", "command",
                                "url", "query", "symbol", "prompt"}) {
            auto it = tc.args.find(key);
            if (it != tc.args.end() && it->is_string()) {
                std::string v = it->get<std::string>();
                if (v.size() > 80) { v.resize(77); v += "..."; }
                for (auto& ch : v) if (ch == '\n' || ch == '\r') ch = ' ';
                s += "  ";
                s += v;
                break;
            }
        }
    }
    return s;
}

provider::StreamResult run_one_completion(Thread& thread,
                              const subagent::Config& cfg,
                              const AgentType& type,
                              std::string& log) {
    // Provider-agnostic request — the generic shape every transport accepts.
    // fresh_auth_header refreshes the ANTHROPIC OAuth token from disk; on any
    // other backend it would CLOBBER the provider's key with Anthropic
    // credentials, so gate it on the active provider kind.
    provider::Request req;
    // Model routing: read-only roles (explorer/reviewer) do grunt work —
    // read/grep/map/summarise — a small model handles as well as a flagship
    // for a fraction of the cost, so route them to the cheapest capable model
    // the ACTIVE provider offers. Write-capable roles (coder/general) keep the
    // parent model — their edits must match the parent's quality. The router
    // never routes up and never crosses providers, so a single-model or
    // Opus-only provider sees no change (returns cfg.model unchanged).
    // Model routing: read-only roles (explorer/reviewer) do grunt work —
    // route them to the cheapest capable model the ACTIVE provider offers.
    // Write-capable roles (coder/tester/general) keep the parent model.
    // Layer 3b: when Smart Mode subagent routing is on, resolve each worker's
    // model by its ROLE (explorer→Utility, reviewer→Strategic, coder/tester/
    // general→Implementation) through the shared resolver instead — honouring
    // the user's pinned slots. Off ⇒ exactly the existing tier auto-router.
    if (cfg.smart.subagent_routing()) {
        // resolve_subagent_role hard-clamps effort to the parent's (Effort::None
        // here, so effort stays off — a worker never thinks harder than the
        // turn that spawned it). Model routing is unchanged: explorer→Utility,
        // reviewer→Strategic, coder/tester/general→Implementation.
        smart::RoleProfile role_prof =
            smart::resolve_subagent_role(type.model_role, cfg.model,
                                         Effort::None, cfg.candidates,
                                         cfg.smart, cfg.provider);
        req.model  = role_prof.model;
        // provider::Request.effort is the WIRE string; convert the clamped
        // Effort through the resolved model's capabilities (None → "", i.e.
        // effort off — which is the case today since parent_effort is None).
        req.effort = std::string(effort_wire_for(
            role_prof.effort, resolved_caps(role_prof.model)));
    } else {
        req.model = type.read_only
                      ? agentty::cheapest_capable_model(cfg.model, cfg.candidates)
                      : cfg.model;
    }
    // A subagent NEVER needs the 1M/2M extended-context window: it does a
    // bounded burst (8k output, tool results capped to 8 KiB, up to 24 turns)
    // that comfortably fits the base 200K window. Carrying the parent's
    // picker-only `[1m]`/`[2m]` marker here would make the transport send the
    // entitlement-gated `context-1m-2025-08-07` beta, which 400s with
    // "long context beta is not yet available for this ..." on accounts
    // without the entitlement (or when the flagship parent's cheaper subagent
    // model isn't 1M-eligible) — killing the whole fan-out. Strip the marker
    // unconditionally: robust (never trips the beta) and economical (subagents
    // pay for the window they actually use). cheapest_capable_model already
    // returns a clean id when it finds a cheaper model; this also covers the
    // "kept the parent" fallback and the write-role (cfg.model) path.
    req.model         = agentty::wire_model_id(req.model);
    req.system_prompt = subagent_system_prompt(type);
    // Smart-channel telemetry, the delegation half of the trace. Without this
    // a debug log showed ONLY the Strategic turn: subagents dispatch on a
    // worker thread through the same transport, so their requests appeared
    // (if at all) as anonymous wire traffic with no role, no parent, and no
    // indication they were delegations at all. One line per worker launch,
    // naming the role, the model the router actually chose, and whether
    // Layer 3b or the tier auto-router chose it.
    AGT_LOG(Smart, Debug, "route.subagent",
            "agent={} role_routing={} read_only={} parent_model={} model={}",
            type.name,
            cfg.smart.subagent_routing() ? 1 : 0,
            type.read_only ? 1 : 0,
            cfg.model,
            req.model);
    // Resolve auth LIVE from the ACTIVE provider through the central
    // credential layer — the same discipline as launch_stream's
    // auth_snapshot(). cfg.auth is a snapshot taken at the last
    // update_auth/switch_provider; a background OAuth refresh or an
    // in-picker account switch between then and this subagent's launch
    // would ship a stale (or the WRONG provider's) credential — the exact
    // 401 class fixed on the main turn path. cfg.auth remains the fallback
    // for oauth_native/local providers whose transports own their tokens
    // (resolve returns empty there).
    {
        const auto sel = provider::active();
        const std::string pid =
            sel.kind == provider::Kind::OpenAI
                ? sel.openai_endpoint.label
                : std::string{provider::default_provider_id()};
        auth::AuthHeader live = provider::credentials::resolve(pid);
        const bool live_real = !auth::bearer_token(live).empty()
            || std::holds_alternative<auth::BearerHeader>(live);
        req.auth = live_real
                 ? (sel.kind == provider::Kind::Anthropic
                        ? auth::fresh_auth_header(live)
                        : std::move(live))
                 : (sel.kind == provider::Kind::Anthropic
                        ? auth::fresh_auth_header(cfg.auth)
                        : cfg.auth);
    }
    // A subagent's job is to investigate and return a CONCISE standalone
    // report — not to emit a 32k-token essay. The parent's default is 16k;
    // 8k is ample for a report yet caps the per-turn output cost of a
    // fan-out of parallel subagents (each turn otherwise billed at the full
    // ceiling). The wrap-up nudge already forces a tight final report.
    req.max_tokens    = 8192;
    req.messages      = thread.messages;
    req.cancel        = std::make_shared<http::CancelToken>();
    // Stable per-subagent conversation identity so the shared prefix
    // (heavy system prompt + tool schemas + accumulated tool results) is
    // PROMPT-CACHED across this subagent's whole run instead of being
    // re-encoded from scratch every turn. Keyed on the agent role + the task
    // prompt so each spawned subagent gets its own stable cache lane and a
    // fan-out of parallel explorers doesn't collide. Without this the single
    // biggest subagent cost — re-sending the same large prefix every turn —
    // pays full price on every one of those turns.
    {
        std::string key = "subagent:";
        key += type.name;
        key += ':';
        // A short stable digest of the task prompt (FNV-1a) keeps the key
        // bounded and distinct per delegated task.
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : thread.messages.empty()
                                   ? std::string_view{}
                                   : std::string_view{thread.messages.front().text}) {
            h ^= c; h *= 0x00000100000001B3ULL;
        }
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(h));
        key += buf;
        req.session_key = std::move(key);
    }

    auto allowed = [&](const tools::ToolDef& t) -> bool {
        if (t.name.value == "task") return false;
        if (!type.allow.empty()) {
            bool listed = false;
            for (auto n : type.allow)
                if (n == t.name.value) { listed = true; break; }
            if (!listed) return false;
        }
        if (type.read_only) {
            tools::EffectSet eff = t.effects;
            if (const auto* sp = tools::spec::lookup(t.name.value)) eff = sp->effects;
            using tools::Effect;
            if (eff.has(Effect::WriteFs) || eff.has(Effect::Exec) || eff.has(Effect::Net))
                return false;
        }
        return true;
    };
    std::string_view newest_user;
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
        if (it->role == Role::User && !it->is_proactive_context()) {
            newest_user = it->text;
            break;
        }
    }
    for (const auto* tool : tools::select_wire_tools(newest_user)) {
        const auto& t = *tool;
        if (!allowed(t)) continue;
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});
    }

    Message asst;
    asst.role = Role::Assistant;
    StopReason stop = StopReason::Unspecified;
    std::unordered_map<std::string, std::string> tool_json;

    auto find_tool = [&](const ToolCallId& id) -> ToolUse* {
        auto it = std::find_if(asst.tool_calls.begin(), asst.tool_calls.end(),
            [&](const ToolUse& tc) { return tc.id == id; });
        return it == asst.tool_calls.end() ? nullptr : &*it;
    };

    // Throttled feed pump: a fast model streams hundreds of text deltas
    // per second, and every progress::emit crosses a thread boundary as a
    // ToolExecProgress Msg. ~12 fps is indistinguishable on the card and
    // keeps a parallel fan-out of subagents from flooding the UI queue.
    auto last_pump = std::chrono::steady_clock::now()
                   - std::chrono::milliseconds(100);
    auto pump = [&](bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_pump < std::chrono::milliseconds(80)) return;
        last_pump = now;
        std::string snap = log;
        if (!asst.text.empty()) {
            snap += "\n  \xe2\x96\xb8 ";
            snap += asst.text;
        }
        progress::emit(snap);
    };

    auto sink = [&](Msg m) {
            auto* sm = std::get_if<msg::StreamMsg>(&m);
            if (!sm) return;
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamTextDelta>) {
                    asst.text += e.text;
                    pump();
                } else if constexpr (std::same_as<T, StreamToolUseStart>) {
                    ToolUse tc;
                    tc.id     = e.id;
                    tc.name   = e.name;
                    tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                    asst.tool_calls.push_back(std::move(tc));
                    tool_json[e.id.value].clear();
                } else if constexpr (std::same_as<T, StreamToolUseDelta>) {
                    tool_json[e.id.value] += e.partial_json;
                } else if constexpr (std::same_as<T, StreamToolUseEnd>) {
                    ToolUse* tc = find_tool(e.id);
                    auto json_it = tool_json.find(e.id.value);
                    const std::string partial = json_it == tool_json.end()
                                              ? std::string{} : json_it->second;
                    if (tc && !partial.empty()) {
                        try {
                            tc->args = json::parse(partial);
                        } catch (...) {
                            // Truncated/unbalanced args JSON (stream cut, weak
                            // model). Salvage by synthesising the missing
                            // closers — but NEVER when the cut landed inside a
                            // string VALUE: the repaired JSON would parse fine
                            // and silently run a tool with a half-written body.
                            if (!util::ended_inside_string(partial)) {
                                try {
                                    tc->args = json::parse(
                                        util::close_partial_json(partial));
                                    ::agentty::util::dbglog("subagent.tool_args.repaired",
                                                 partial);
                                } catch (...) {
                                    ::agentty::util::dbglog("subagent.tool_args.parse",
                                                 partial);
                                }
                            } else {
                                ::agentty::util::dbglog("subagent.tool_args.mid_string",
                                             partial);
                            }
                        }
                    }
                    tool_json.erase(e.id.value);
                    if (tc) {
                        log += "\n  ⚙ ";
                        log += summarize_call(*tc);
                        pump(/*force=*/true);
                    }
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    stop = e.stop_reason;
                } else if constexpr (std::same_as<T, StreamError>) {
                    // StreamResult is authoritative. The event remains useful
                    // for legacy/scripted seams that cannot stamp a result.
                }
            }, *sm);
        };

    // Route through the SAME provider dispatch the parent uses (installed
    // at startup); fall back to the Anthropic transport when no seam is
    // wired (tests that install only auth+model).
    provider::StreamResult result;
    auto cancel = req.cancel;
    auto parent_cancel = cancellation::current();
    std::jthread cancel_bridge;
    if (parent_cancel) {
        cancel_bridge = std::jthread([parent_cancel, cancel](std::stop_token st) {
            while (!st.stop_requested() && !cancel->is_cancelled()) {
                if (parent_cancel()) { cancel->cancel(); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    if (cancellation::requested()) cancel->cancel();
    if (cfg.stream) {
        result = cfg.stream(std::move(req), sink);
    } else {
        provider::anthropic::AnthropicProvider p;
        result = p.stream(std::move(req), sink);
    }
    cancel_bridge.request_stop();
    pump(/*force=*/true);   // flush the throttled tail

    // An empty successful close is a transient transport failure. A max-token
    // turn is also incomplete: partial prose/tool JSON is not a final report.
    if (result.ok() && asst.text.empty() && asst.tool_calls.empty())
        result = provider::StreamResult::failed(
            "provider returned an empty completion");
    if (result.ok() && (result.stop == StopReason::MaxTokens
                        || stop == StopReason::MaxTokens)) {
        result = provider::StreamResult::failed(
            "provider hit the max-token limit before completing the subagent turn");
        result.stop = StopReason::MaxTokens;
    }

    thread.messages.push_back(std::move(asst));
    return result;
}

class AgenttySubagentRunner final : public mt::SubagentRunner {
public:
    std::vector<std::string> extra_agent_types() const override {
        // User-authored .agentty/agents/*.md — discoverable in the task
        // tool's agent_type enum. Names only; bodies load at resolve time.
        return user_agent_names();
    }

    std::string unavailable_reason() const override {
        auto cfg = subagent::current();
        if (!cfg.installed)
            return "subagent runtime was not installed; restart agentty with the current executable";
        if (cfg.model.empty())
            return "no model is selected for the active provider";
        // Runtime dispatch is credential-aware itself: ChatGPT resolves its
        // native OAuth store per request and local providers need no auth at
        // all. Only the legacy direct-Anthropic fallback requires a non-empty
        // header here.
        if (!cfg.stream && auth::is_empty(cfg.auth))
            return "no provider stream or fallback Anthropic credential is configured";
        if (subagent::current_depth() >= subagent::kMaxDepth)
            return "subagent nesting depth limit reached (maximum "
                 + std::to_string(subagent::kMaxDepth) + ")";
        if (cancellation::requested())
            return "cancelled before the subagent started";
        return {};
    }

    std::string run(const mt::SubagentRequest& sreq, bool& is_error) override {
        is_error = false;
        auto cfg = subagent::current();
        if (!cfg.installed || cfg.model.empty()
            || (!cfg.stream && auth::is_empty(cfg.auth))) {
            is_error = true;
            return "subagents are unavailable in this context (no model/stream wired)";
        }
        if (subagent::current_depth() >= subagent::kMaxDepth) {
            is_error = true;
            return "subagent depth limit reached — a subagent cannot spawn "
                   "further subagents at this nesting level";
        }

        subagent::push_depth();
        struct DepthGuard { ~DepthGuard() { subagent::pop_depth(); } } depth_guard;

        const AgentType& type = resolve_agent_type(sreq.agent_type);

        Thread thread;
        {
            Message user;
            user.role = Role::User;
            user.text = sreq.prompt;
            thread.messages.push_back(std::move(user));
        }

        int turns = 0;
        std::string log = "\xe2\x97\x86 " + std::string{type.name} + " agent";
        std::string last_error;
        // Give this subagent its OWN read-dedup context for the whole run.
        //
        // `read` answers a repeat of the same (path, range) with "refer to
        // the earlier tool_result" — true only for the context that received
        // those bytes. The cache is process-global, so without this scope a
        // subagent inherits the PARENT's entries and is refused files it has
        // never seen. That is not theoretical: a coder subagent spent all 23
        // of its turns re-requesting one file, got the sentinel every time,
        // and made zero edits. RAII-restored so nesting can't leak the id.
        ::mcp::tools::util::ReadContextScope read_scope{
            "subagent:" + std::string{type.name} + ":"
            + std::to_string(reinterpret_cast<std::uintptr_t>(&thread))};
        // Transient stream failures (429/529 brown-out, TLS reset, transport
        // hiccup) are RETRIED with backoff instead of aborting the whole
        // subagent — "task fails a lot" was mostly one flaky completion
        // killing an otherwise healthy loop. Consecutive counter: any
        // successful completion resets it.
        constexpr int kMaxStreamRetries = 3;
        int stream_failures = 0;
        // Repeat-failure breaker (same rule as the parent's doom-loop
        // breaker): the identical tool call failing 3× means the loop is
        // stuck — stop burning turns and report what we have.
        std::unordered_map<std::string, int> failed_calls;
        bool doomed = false;
        // Set once we've told the model it's out of budget and must write its
        // final report NOW. Prevents a thorough agent from exploring straight
        // into the turn cap and never emitting a report (the loop below then
        // salvages a stale early narration line instead of a real answer).
        bool wrapup_nudged = false;

        // Turn budget for THIS role. A read-only sweep converges quickly; an
        // implementation loop (edit → build → read errors → fix → re-run) is
        // structurally longer, and starving it produces the "ran out of
        // turns, made zero edits" report rather than the work.
        const int max_turns = subagent::max_turns_for(type.read_only);

        while (turns < max_turns && !doomed) {
            ++turns;

            // FINAL-TURN NUDGE: when the remaining budget is nearly spent,
            // inject a synthetic user message ordering the model to stop
            // running tools and write its report. Without this the agent
            // spends its last completion on yet another tool call, hits the
            // cap mid-investigation, and returns no final text at all.
            //
            // The lead time scales with the budget: one turn's warning is
            // enough for a short read sweep, but a write role deep in an
            // edit→build→fix cycle needs room to land what it started (finish
            // the current edit, re-run the build) before summarising. Too
            // early wastes budget; too late produces the very "ran out of
            // turns with nothing to show" report this exists to prevent.
            const int wrapup_lead = max_turns >= 48 ? 3 : 1;
            if (!wrapup_nudged && turns >= max_turns - wrapup_lead
                && !thread.messages.empty()
                && thread.messages.back().role != Role::User) {
                Message nudge;
                nudge.role = Role::User;
                nudge.text =
                    "You are almost out of turn budget. Finish or abandon the "
                    "step you are on — do not start anything new — then write "
                    "your FINAL report as a plain text message: a complete, "
                    "self-contained answer to the task using everything you "
                    "have gathered so far. If you did not complete the task, "
                    "say so plainly and state exactly what remains.";
                thread.messages.push_back(std::move(nudge));
                wrapup_nudged = true;
            }

            provider::StreamResult stream_result =
                run_one_completion(thread, cfg, type, log);

            if (!stream_result.ok()) {
                const std::string err = stream_result.error.value_or("stream failed");
                last_error = err;
                const auto error_class = stream_result.cancelled()
                    ? provider::ErrorClass::Cancelled
                    : provider::classify_stream_error(err,
                                                       stream_result.http_status);
                const bool empty_completion =
                    err == "provider returned an empty completion";
                const bool retryable = !stream_result.non_replayable
                    && (empty_completion
                        || error_class == provider::ErrorClass::Transient
                        || error_class == provider::ErrorClass::RateLimit);
                // A partial assistant message can contain unpaired tool calls
                // and must never be replayed on retry or reported as complete.
                if (!thread.messages.empty()
                    && thread.messages.back().role == Role::Assistant)
                    thread.messages.pop_back();
                if (!retryable || error_class == provider::ErrorClass::Cancelled
                    || cancellation::requested()) {
                    is_error = true;
                    if (error_class == provider::ErrorClass::Cancelled
                        || cancellation::requested())
                        last_error = "subagent cancelled: " + err;
                    break;
                }
                ++stream_failures;
                if (stream_failures > kMaxStreamRetries) {
                    log += "\n  ⚠ stream failed "
                         + std::to_string(stream_failures) + "× — giving up: "
                         + err;
                    progress::emit(log);
                    break;
                }
                // Honor server guidance, but cap it so a hostile/mistaken
                // Retry-After cannot pin a task worker indefinitely.
                constexpr auto kMaxRetryAfter = std::chrono::seconds{30};
                auto wait = stream_result.retry_after
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::min(*stream_result.retry_after, kMaxRetryAfter))
                    : provider::backoff_with_jitter(error_class,
                                                    stream_failures - 1);
                log += "\n  ↻ retry " + std::to_string(stream_failures)
                     + "/" + std::to_string(kMaxStreamRetries)
                     + " in " + std::to_string(wait.count()) + "ms (" + err + ")";
                progress::emit(log);
                const auto retry_until = std::chrono::steady_clock::now() + wait;
                while (std::chrono::steady_clock::now() < retry_until) {
                    if (cancellation::requested()) {
                        is_error = true;
                        return "subagent cancelled while waiting to retry: " + err;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                --turns;
                continue;
            }
            stream_failures = 0;
            last_error.clear();

            Message& asst = thread.messages.back();
            bool ran_a_tool = false;
            if (!asst.tool_calls.empty()) {
                const auto now = std::chrono::steady_clock::now();
                for (auto& tc : asst.tool_calls) {
                    if (cancellation::requested()) {
                        is_error = true;
                        last_error = "subagent cancelled before local tool execution";
                        doomed = true;
                        break;
                    }
                    if (tc.args.is_null()) {
                        tc.status = ToolUse::Failed{now, now,
                            "tool args failed to parse \xe2\x80\x94 re-emit the call "
                            "with complete, valid JSON arguments"};
                        log += "\n    \xe2\x9c\x97 " + tc.name.value + ": bad args";
                        progress::emit(log);
                        // A parse failure still counts as a tool ROUND-TRIP:
                        // the model sees the error tool_result and can
                        // re-emit. Without this the loop broke out on the
                        // first bad-args call and the whole task died.
                        ran_a_tool = true;
                        continue;
                    }
                    ran_a_tool = true;
                    const auto t_start = std::chrono::steady_clock::now();
                    auto res = tool::DynamicDispatch::execute(tc.name.value, tc.args);
                    // Same tool.exec record the main loop emits, so a headless
                    // `agentty run` and a subagent turn are as diagnosable as
                    // an interactive one. A failing tool is Warn (and therefore
                    // in the crash flight recorder) with the args that caused
                    // it — the evidence that was missing while Copilot's tool
                    // calls were arriving empty.
                    AGT_LOGL(Tool, res ? ::agentty::logx::Level::Debug
                                       : ::agentty::logx::Level::Warn,
                             "tool.exec", "name={} ms={} ok={} err={} args={}",
                             tc.name.value,
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t_start).count(),
                             res ? 1 : 0,
                             res ? std::string{"-"}
                                 : std::string{tools::to_string(res.error().kind)}
                                       + ": " + res.error().detail,
                             tc.args.dump());
                    // AGENTTY_TRACE_TOOLS=1 emits one machine-parseable line per
                    // executed tool to STDERR (run/acp/mcp-serve keep stderr as
                    // their diagnostic channel, so the stdout report stays
                    // clean). Independently useful for debugging/scripting a
                    // headless `agentty run`, and the observation hook the
                    // Tier-2 agentic evals need to assert on tool SELECTION.
                    if (trace_tools_enabled())
                        std::fprintf(stderr, "TOOL %s %s\n", tc.name.value.c_str(),
                                     res ? "ok" : "error");
                    if (res) {
                        // ECONOMY: a subagent is a focused, tool-heavy burst
                        // (read/grep/repo_map outputs run to tens of KiB
                        // each). Those results accumulate in the subagent's
                        // OWN thread and replay on EVERY subsequent turn, so
                        // the cost is quadratic in turns — and write roles now
                        // get a budget measured in dozens of turns, which is
                        // exactly when that bites. The parent's 64 KiB
                        // newest-result budget is tuned for a long interactive
                        // chat; for a subagent it's the dominant cost.
                        // Cap each result to a tight head+tail the instant we
                        // store it, so the working set the model reasons over
                        // stays lean without losing the WHAT of any result.
                        // (Transport aging still applies on top for old ones.)
                        constexpr std::size_t kSubagentToolBudget = 8u * 1024u;
                        std::string capped = provider::wire::cap_tool_result(
                            res->text, kSubagentToolBudget);
                        tc.status = ToolUse::Done{now, now, std::move(capped)};
                        log += "\n    \xe2\x9c\x93 " + summarize_call(tc);
                    } else {
                        tc.status = ToolUse::Failed{now, now, res.error().render()};
                        log += "\n    \xe2\x9c\x97 " + summarize_call(tc) + "  \xe2\x80\x94 "
                             + res.error().render();
                        // Identical failing call 3× → the loop is stuck.
                        std::string key = tc.name.value + '\0'
                            + (tc.args.is_null() ? std::string{} : tc.args.dump());
                        if (++failed_calls[key] >= 3) {
                            doomed = true;
                            log += "\n  \xe2\x9a\xa0 same call failed 3\xc3\x97 \xe2\x80\x94 stopping";
                        }
                    }
                    progress::emit(log);
                }
            }

            if (!ran_a_tool) break;   // final text answer (or nothing left to do)
        }

        // Extract the report. The AUTHORITATIVE report is the text of the
        // FINAL assistant turn — the message the model wrote last, after all
        // its tool work. We do NOT walk backward to the first non-empty text:
        // an agent that explored straight into the turn cap has its last few
        // turns as pure tool calls (empty .text), and a naive reverse-walk
        // would resurrect its turn-1 narration ("I'll start by mapping...")
        // and pass it off as a finished report. That was the real bug: a
        // 24-turn run returning its opening sentence instead of an answer.
        std::string report;
        bool salvaged_stale = false;
        if (!thread.messages.empty()
            && thread.messages.back().role == Role::Assistant
            && !thread.messages.back().text.empty()) {
            // Clean finish: the model's last act was to write prose.
            report = thread.messages.back().text;
        } else {
            // No final text (ran out of budget mid-tool, or last turn was
            // tool-only). Salvage the most recent EARLIER assistant text as a
            // partial, but mark it stale so the banner below tells the caller
            // this is not a proper final report.
            for (auto it = thread.messages.rbegin(); it != thread.messages.rend(); ++it) {
                if (it->role == Role::Assistant && !it->text.empty()) {
                    report = it->text;
                    salvaged_stale = true;
                    break;
                }
            }
        }

        // Budget exhaustion is a FAILURE MODE even when the last turn
        // happened to produce clean prose. The old gate here was
        // `report.empty() || salvaged_stale`, so an agent that spent every
        // turn and then wrote a tidy "## OUTCOME — Task NOT completed, I made
        // zero edits" fell through all of it: no banner, is_error never set,
        // and the UI rendered a green ✓ DONE over a self-declared failure.
        // Observed exactly that. Treat hitting the cap as a reportable
        // condition in its own right.
        const bool budget_exhausted = turns >= max_turns;

        if (report.empty() || salvaged_stale) {
            std::string why;
            if (!last_error.empty())
                why = "[subagent failed: " + last_error + "]";
            else if (doomed)
                why = "[subagent stopped: the same tool call failed 3\xc3\x97 "
                      "in a row without converging]";
            else if (budget_exhausted)
                why = "[subagent hit its turn budget without producing a final "
                      "report \xe2\x80\x94 the summary below is incomplete]";
            else
                why = "[subagent finished without a final text report]";
            if (salvaged_stale) {
                // We have partial prose from an earlier turn: surface the
                // banner, then the salvaged text, then the activity log.
                report = why + "\n\nLast text the subagent produced (may be an "
                             "early, incomplete note):\n" + report
                       + (log.empty() ? std::string{}
                                      : "\n\nActivity:\n" + log);
            } else {
                report = log.empty() ? why : (why + "\n\nActivity:\n" + log);
            }
            // A bare error (no salvageable report) propagates as an error so
            // the shell tags the tool_result is_error.
            if (!last_error.empty()) is_error = true;
            // Neither a doom-loop abort nor an exhausted budget produced the
            // work that was asked for. Both are failures; say so.
            if (doomed || budget_exhausted) is_error = true;
        } else if (budget_exhausted) {
            // Ran out of turns but signed off cleanly. The prose is worth
            // keeping — it is the agent's own account of how far it got — but
            // it must not be mistaken for success by the caller or the UI.
            report = "[subagent hit its turn budget \xe2\x80\x94 the task may be "
                     "incomplete; verify before relying on it]\n\n" + report;
            is_error = true;
        }

        // A subagent that NARRATES its own failure must not be reported as a
        // success. The model is the authority on whether it did the work, and
        // when it says it didn't, believing the exit path over the text is how
        // a "zero edits made" report ends up rendered as ✓ DONE.
        if (!is_error && subagent_self_reported_failure(report)) {
            is_error = true;
        }

        std::ostringstream out;
        out << "Subagent report (" << type.name << ", " << turns << " turn"
            << (turns == 1 ? "" : "s") << "):\n\n" << report;
        return out.str();
    }
};

} // namespace

void install_host_backends(::mcp::tools::HostServices& svc) {
    svc.memory    = std::make_shared<AgenttyMemoryStore>();
    svc.skills    = std::make_shared<AgenttySkillResolver>();
    svc.retriever = std::make_shared<AgenttyDocRetriever>();
    svc.code_retriever = std::make_shared<AgenttyCodeRetriever>();
    svc.subagent  = std::make_shared<AgenttySubagentRunner>();
    // svc.todo intentionally left null: the mcp todo shell renders identical
    // text to the native tool with no host state needed, and agentty's TUI
    // parses the rendered text — there is no structured sink to feed.
}

std::string run_one_shot(const std::string& prompt,
                         const std::string& agent_type,
                         bool& is_error) {
    // The one-shot CLI is exactly one subagent run driven from main()
    // instead of from the `task` tool: same loop, same toolset, same
    // bounds. Instantiate the runner directly — no HostServices needed.
    AgenttySubagentRunner runner;
    if (auto why = runner.unavailable_reason(); !why.empty()) {
        is_error = true;
        return why;
    }
    mt::SubagentRequest sreq;
    sreq.prompt     = prompt;
    sreq.agent_type = agent_type.empty() ? "general" : agent_type;
    return runner.run(sreq, is_error);
}

// ── Proactive retrieval (SOTA active-RAG / FLARE / Self-RAG) ────────────
namespace {

// TRUE when the memory record behind `mem_path` ("memory://<scope>/<id>")
// is ALREADY rendered in the system prompt's <learned-memory> block —
// injecting it again via <retrieved-context> would spend context tokens on
// bytes the model can already see. Mirrors the transport's selection
// exactly (load_recent_* → select_for_prompt) and caches the id set,
// invalidated by the same (size,mtime) stat the RAG epoch uses.
bool memory_fact_in_prompt_(const std::string& mem_path) {
    auto slash = mem_path.rfind('/');
    if (slash == std::string::npos || slash + 1 >= mem_path.size()) return false;
    const std::string id = mem_path.substr(slash + 1);

    static std::mutex mu;
    static std::unordered_set<std::string> prompt_ids;
    static std::uint64_t stamp = ~0ULL;

    std::uint64_t now_stamp = 1469598103934665603ULL;
    auto mix = [&now_stamp](std::uint64_t v) {
        now_stamp = (now_stamp ^ v) * 1099511628211ULL;
    };
    for (auto scope : {memory::Scope::User, memory::Scope::Project}) {
        std::error_code ec;
        auto p = memory::path_for(scope);
        if (p.empty()) continue;
        mix(static_cast<std::uint64_t>(
            fs::exists(p, ec) ? fs::file_size(p, ec) : 0));
        auto t = fs::last_write_time(p, ec);
        mix(ec ? 0ULL
               : static_cast<std::uint64_t>(t.time_since_epoch().count()));
    }

    std::lock_guard<std::mutex> lock(mu);
    if (stamp != now_stamp) {
        stamp = now_stamp;
        prompt_ids.clear();
        for (auto load : {&memory::load_recent_user, &memory::load_recent_project}) {
            auto picked = memory::select_for_prompt(load());
            for (const auto& r : picked.records) prompt_ids.insert(r.id);
        }
    }
    return prompt_ids.count(id) > 0;
}

// Cross-turn de-duplication for PROACTIVE injection. Without this, a stable
// high-confidence corpus re-injects the SAME passages into <retrieved-context>
// on every single turn of a thread — pure context-window spend for zero new
// information (the model already saw them last turn). We remember the keys
// (source:path:line) of recently-injected passages in a bounded FIFO and skip
// any we've surfaced before, so proactive injection only ever spends tokens on
// passages the model hasn't been shown yet this session.
//
// Bounded (kMax) so a long thread can't grow this without limit; once a key
// ages out of the window it MAY be re-injected, which is the correct behaviour
// (it's relevant again and long-since scrolled out of the model's attention).
//
// SPLIT into peek (proactive_seen_) and commit (proactive_mark_injected_):
// proactive_retrieve builds candidate blocks on a WORKER that the caller may
// ABANDON when it blows the latency budget. If the funnel both checked AND
// recorded keys, an abandoned worker would mark passages "injected" that the
// user never saw — permanently suppressing them. So the funnel only PEEKS,
// and the caller commits the surviving keys ONLY when it actually returns the
// hit to the wire. Both share one mutex/FIFO.
namespace {
std::mutex& proactive_dedup_mu_() { static std::mutex mu; return mu; }
std::unordered_set<std::string>& proactive_dedup_seen_() {
    static std::unordered_set<std::string> seen; return seen;
}
std::deque<std::string>& proactive_dedup_fifo_() {
    static std::deque<std::string> fifo; return fifo;
}
constexpr std::size_t kProactiveDedupMax = 256;
}

// PEEK: true if `key` was already injected this session. Does NOT record it.
bool proactive_seen_(const std::string& key) {
    std::lock_guard<std::mutex> lock(proactive_dedup_mu_());
    return proactive_dedup_seen_().count(key) > 0;
}

// COMMIT: record `key` as injected (bounded FIFO eviction). Idempotent.
void proactive_mark_injected_(const std::string& key) {
    std::lock_guard<std::mutex> lock(proactive_dedup_mu_());
    auto& seen = proactive_dedup_seen_();
    if (seen.count(key)) return;
    seen.insert(key);
    auto& fifo = proactive_dedup_fifo_();
    fifo.push_back(key);
    while (fifo.size() > kProactiveDedupMax) {
        seen.erase(fifo.front());
        fifo.pop_front();
    }
}

} // namespace

// Runs the SAME AgenttyDocRetriever the search_docs tool uses, but out of
// band — before the model sees the turn — and only surfaces its result when
// confidence clears a HIGH bar (higher than the tool's LOW floor: we're
// spending the user's context-window tokens unprompted, so the hit must be
// Read the confidence bar for UNPROMPTED injection. The tool's LOW floor is
// 0.25; we inject only well above it. Tunable via AGENTTY_RAG_PROACTIVE_MIN.
namespace {
double proactive_min_conf_() {
    // The confidence is now CRAG's CALIBRATED relevance grade (a real
    // model-free evaluator), not the old raw fusion score. CRAG grades a
    // genuinely-relevant retrieval in the ~0.3–0.6 band, so the unprompted
    // injection bar defaults to 0.35 — high enough to skip noise, low enough
    // that a solidly-relevant hit reaches the model.
    //
    // Source of truth is the retriever's LIVE config (which folds in the
    // persisted RAG picker settings). An explicit AGENTTY_RAG_PROACTIVE_MIN
    // still wins so a shell override is honoured for a one-off run.
    double min_conf = 0.35;
    try { min_conf = shared_retriever().snapshot_config().proactive_min_conf; }
    catch (...) { /* keep default */ }
    if (const char* mc = std::getenv("AGENTTY_RAG_PROACTIVE_MIN"); mc && mc[0]) {
        // Any non-negative value is honoured; a bar above 1.0 is a
        // legitimate "never inject" switch (confidence is clamped to [0,1]).
        try { double v = std::stod(mc); if (v >= 0) min_conf = v; }
        catch (...) { /* keep config/default */ }
    }
    return min_conf;
}

// The proactive retrieval funnel body: run the retriever, gate on confidence,
// build the fenced <retrieved-context> block, PEEK the cross-turn dedup and
// collect surviving keys on the hit. Does NOT commit the dedup keys — the
// caller commits them only when it actually returns/injects the block, so an
// abandoned or discarded run never suppresses a passage that was never shown.
// Never throws (best-effort).
std::optional<ProactiveHit>
run_proactive_funnel_(const std::string& query, int k, double min_conf) {
  try {
    AgenttyDocRetriever r;
    mt::DocQuery q;
    q.query = query;
    q.k     = k > 0 ? k : 3;
    std::string mode, err;
    // Proactive retrieval is explicit opt-in and already runs on an isolated
    // worker, so use the complete index once. The old cold-index shortcut could
    // silently search only skills/memory and miss the document that triggered
    // the knowledge-shaped query.
    auto passages = r.retrieve(q, mode, err);
    if (!err.empty() || passages.empty()) return std::nullopt;

    // Recover the confidence the pipeline computed (mode carries
    // ", confidence 0.NN"). If we can't parse it, be conservative and
    // don't inject.
    double conf = -1.0;
    if (auto p = mode.find("confidence "); p != std::string::npos) {
        try { conf = std::stod(mode.substr(p + 11)); } catch (...) {}
    }
    if (conf < min_conf) return std::nullopt;

    // Build the wire block. Fenced + provenance-labelled so the model
    // treats it as retrieved reference, not the user's words. Bounded.
    std::string block =
        "<retrieved-context>\n"
        "The following passages were auto-retrieved from the user's "
        "knowledge base (docs/skills/memory) because they look relevant "
        "to the request. Ground your answer in them where they apply; "
        "ignore any that don't. Cite the source path when you use one.\n\n";
    int n = 0;
    std::vector<std::string> keys;
    for (const auto& p : passages) {
        // CONTEXT-ECONOMY: memory facts already rendered in the system
        // prompt's <learned-memory> block would be pure double-spend —
        // the model can see them. select_for_prompt() decides that
        // rendering; mirror it here and drop any memory passage whose
        // record made the prompt cut. (Docs/skills/MCP passages are
        // never in the system prompt — always kept.)
        if (p.source == "memory" && memory_fact_in_prompt_(p.path))
            continue;

        // CROSS-TURN DEDUP: don't re-inject a passage the model was
        // already shown earlier this session — that's context spend for
        // zero new signal. Key on source:path:line so distinct chunks of
        // the same file are treated separately. PEEK only here — the caller
        // commits the surviving keys once it actually injects the block.
        std::string key = (p.source.empty() ? std::string{"docs"} : p.source)
                        + ":" + p.path + ":" + std::to_string(p.line_start);
        if (proactive_seen_(key))
            continue;
        keys.push_back(key);

        block += "[" + (p.source.empty() ? std::string{"docs"} : p.source)
               + ":" + p.path;
        if (p.line_start > 0)
            block += ":" + std::to_string(p.line_start);
        block += "]\n";
        block += p.text;
        if (!p.text.empty() && p.text.back() != '\n') block += '\n';
        block += '\n';
        ++n;
    }
    block += "</retrieved-context>";

    if (n == 0) return std::nullopt;   // everything deduped away

    // Independent ceiling on UNPROMPTED spend. The user didn't ask for this
    // block, so it must stay cheap even when the retriever returns rich
    // passages. Cap the whole assembled block; default ~6KiB (~1.5k tok),
    // roughly half the on-demand tool budget. Tunable via
    // AGENTTY_RAG_PROACTIVE_BYTES.
    {
        std::size_t cap = 6 * 1024;
        try {
            int cfg_bytes = shared_retriever().snapshot_config().proactive_bytes;
            if (cfg_bytes > 0)
                cap = std::clamp<std::size_t>(static_cast<std::size_t>(cfg_bytes),
                                              1024, 32 * 1024);
        } catch (...) {}
        if (const char* v = std::getenv("AGENTTY_RAG_PROACTIVE_BYTES"); v && v[0]) {
            try { cap = std::clamp<std::size_t>(std::stoull(v), 1024, 32 * 1024); }
            catch (...) {}
        }
        if (block.size() > cap) {
            std::size_t cut = cap;
            // Trim to a UTF-8 boundary, then re-close the fence cleanly.
            while (cut > 0 && (static_cast<unsigned char>(block[cut]) & 0xc0) == 0x80)
                --cut;
            block.resize(cut);
            block += "\n…\n</retrieved-context>";
        }
    }
    return ProactiveHit{std::move(block), conf, n, std::move(keys)};
  } catch (...) {
    return std::nullopt;   // proactive retrieval is best-effort, never fatal
  }
}
} // namespace

// Compatibility entry point. Proactive retrieval is now opt-in and the submit
// path always owns an isolated worker, so there is no reason to start a second
// detached hedge. Execute the funnel exactly once.
std::optional<ProactiveHit> proactive_retrieve(const std::string& query, int k) {
    return proactive_retrieve_blocking(query, k);
}

// Full funnel for the isolated worker owned by the app. Commits dedup keys only
// when a block is actually returned for injection.
std::optional<ProactiveHit>
proactive_retrieve_blocking(const std::string& query, int k) {
    auto hit = run_proactive_funnel_(query, k, proactive_min_conf_());
    if (hit)
        for (const auto& key : hit->dedup_keys)
            proactive_mark_injected_(key);
    return hit;
}

namespace subagent {
// Provenance query for the task card (declared in subagent.hpp). Resolves the
// type through the SAME lookup the runner uses, then maps its origin to a
// stable label. Unknown → "builtin" (no tag shown). Defined at end-of-TU so
// it doesn't shadow the subagent::current() calls used earlier in the file.
std::string_view agent_origin(std::string_view name) noexcept {
    const AgentType& t = resolve_agent_type(name);
    switch (t.origin) {
        case AgentOrigin::User:    return "user";
        case AgentOrigin::Project: return "project";
        case AgentOrigin::Builtin: break;
    }
    return "builtin";
}
}  // namespace subagent

} // namespace agentty::tools
