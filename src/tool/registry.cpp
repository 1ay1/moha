#include "agentty/tool/registry.hpp"

#include "agentty/mcp/client.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentty::tools {

std::string_view to_string(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::InvalidArgs:    return "invalid args";
        case ErrorKind::NotFound:       return "not found";
        case ErrorKind::NotAFile:       return "not a file";
        case ErrorKind::NotADirectory:  return "not a directory";
        case ErrorKind::TooLarge:       return "too large";
        case ErrorKind::Binary:         return "binary";
        case ErrorKind::Ambiguous:      return "ambiguous";
        case ErrorKind::NoMatch:        return "no match";
        case ErrorKind::InvalidRegex:   return "invalid regex";
        case ErrorKind::Network:        return "network";
        case ErrorKind::Spawn:          return "spawn failed";
        case ErrorKind::Subprocess:     return "subprocess failed";
        case ErrorKind::Io:             return "io";
        case ErrorKind::OutOfWorkspace: return "out of workspace";
        case ErrorKind::Unknown:        return "unknown";
    }
    return "unknown";
}

std::string ToolError::render() const {
    return std::format("[{}] {}", to_string(kind), detail);
}

std::string_view to_string(Effect e) noexcept {
    switch (e) {
        case Effect::ReadFs:  return "ReadFs";
        case Effect::WriteFs: return "WriteFs";
        case Effect::Net:     return "Net";
        case Effect::Exec:    return "Exec";
    }
    return "?";
}

std::string to_string(EffectSet e) {
    if (e.empty()) return "Pure";
    std::string out;
    auto add = [&](Effect bit) {
        if (!e.has(bit)) return;
        if (!out.empty()) out += ", ";
        out += to_string(bit);
    };
    add(Effect::Exec);
    add(Effect::WriteFs);
    add(Effect::Net);
    add(Effect::ReadFs);
    return out;
}

// ── Live progress sink (thread-local implementation) ────────────────────
//
// thread_local so the cmd runner's dispatch lambda can be captured without
// cross-thread synchronisation — each tool runs on its own worker, and
// cmd_factory installs/clears the sink on that worker via a RAII Scope.
// Subprocess runners (see util/subprocess.cpp) call progress::emit from the
// same thread, so it's a plain load from TLS — no atomics, no locking.
// The tools::progress sink itself now lives in its own TU (tool/progress.cpp)
// so subprocess-only consumers can link it without pulling in build_registry()
// and the MCP bridge behind it.

namespace {

// Assemble every tool. Order matters: the protocol treats the set as
// unordered but the model has a strong recall bias toward earlier-listed
// tools. Putting `edit` ahead of `write` is the cheapest single nudge to
// stop the model from rewriting whole files when a targeted substitution
// would do — and edit's tiny input_json_delta bodies sidestep the long
// mid-stream pause Anthropic's edge applies to multi-KB tool_use content.
// Assemble the local tool set. The implementations live in mcp-cpp's
// batteries-included toolset (mcp::tools::make_provider): build_mcp_tool_defs()
// re-wraps each advertised tool as a ToolDef whose execute() dispatches into
// the provider and decodes the `_mcp_tools` meta (effects + FileChange) back
// into ToolOutput. The host-coupled SHELLS (remember/forget/wipe/todo/skill/
// search_docs/task) are backed by agentty adapters injected via HostServices.
// mcp-cpp is the SOLE source of truth for tools — there is no native path.
std::vector<ToolDef> build_native_registry() {
    return build_mcp_tool_defs();
}

// Connect external MCP exactly once, on first catalog access. The returned
// ToolDefs are not made part of the immutable native baseline: every later
// generation is rebuilt from the MCP pool's current authoritative snapshot.
std::vector<ToolDef> connect_initial_mcp() {
    if (!mcp::mcp_config_present()) return {};
    static mcp::PoolHandle s_pool;
    return mcp::mcp_tools(s_pool);
}

} // namespace

const std::vector<ToolDef>& native_registry() {
    static const std::vector<ToolDef> r = build_native_registry();
    return r;
}

namespace {
// Published snapshots are immutable and retained for process lifetime because
// dispatch resolves a ToolDef pointer once, drops the cache lock, then may run
// for minutes. Retention prevents a concurrent tools/list_changed refresh from
// invalidating that pointer.
struct Snapshot {
    std::vector<ToolDef> tools;
    std::unordered_map<std::string, const ToolDef*> idx;
};

struct WireCache {
    std::mutex mu;
    unsigned long generation = static_cast<unsigned long>(-1);
    bool connected = false;
    // True while some thread is INSIDE connect_initial_mcp() with mu
    // RELEASED (see refresh_wire_cache). Latecomers must not wait — they
    // get the native-only snapshot below and the full set on their next
    // access after the connect publishes.
    bool connecting = false;
    std::vector<ToolDef> initial_mcp;
    std::shared_ptr<const Snapshot> current;
    // Native-tools-only snapshot served while `connecting`. Cached apart
    // from `current` so the generation-equality fast path can never keep
    // serving it after the real connect lands.
    std::shared_ptr<const Snapshot> native_only;
    std::vector<std::shared_ptr<const Snapshot>> retired;
};

WireCache& wire_cache() { static WireCache c; return c; }

std::shared_ptr<const Snapshot> refresh_wire_cache(std::unique_lock<std::mutex>& lk,
                                                   WireCache& c) {
    if (!c.connected) {
        if (c.connecting) {
            // Another thread is mid-handshake. DO NOT WAIT: the connect is
            // bounded by a 15 s deadline, and the caller may be the UI
            // thread (needs_permission on a streaming tool call, a panel
            // action) — blocking it here froze the whole app for the
            // handshake's duration whenever input raced the startup warm.
            // Serve agentty's native tools now; the full set appears on
            // the first access after the connect publishes.
            if (!c.native_only) {
                auto ns = std::make_shared<Snapshot>();
                ns->tools = native_registry();
                ns->idx.reserve(ns->tools.size());
                for (const auto& tool : ns->tools)
                    ns->idx.emplace(tool.name.value, &tool);
                c.native_only = std::move(ns);
            }
            return c.native_only;
        }
        // First caller pays for the connect — but WITHOUT the lock, so
        // every other thread stays free. connect_initial_mcp() has its own
        // serialization (bridge.cpp g_connect_mu); `connecting` keeps a
        // second cold caller from queuing on that inner mutex too.
        c.connecting = true;
        lk.unlock();
        std::vector<ToolDef> ext;
        try {
            ext = connect_initial_mcp();
        } catch (...) {
            lk.lock();
            c.connecting = false;
            throw;
        }
        lk.lock();
        c.initial_mcp = std::move(ext);
        c.connecting  = false;
        c.connected   = true;
        // Force a rebuild below: the pool generation may still equal the
        // pre-connect value (0), so the equality fast path must not serve
        // a pre-connect snapshot.
        if (c.current) { c.retired.push_back(c.current); c.current.reset(); }
    }

    const unsigned long generation = mcp::mcp_generation();
    if (c.current && c.generation == generation) return c.current;

    std::vector<ToolDef> external = generation == 0
        ? c.initial_mcp
        : mcp::mcp_tools_live();

    auto next = std::make_shared<Snapshot>();
    next->tools.reserve(native_registry().size() + external.size());
    next->tools.insert(next->tools.end(), native_registry().begin(), native_registry().end());

    // External names are stable and namespaced, but still reject duplicates
    // defensively rather than sending ambiguous schemas to a model provider.
    std::unordered_map<std::string, bool> names;
    names.reserve(next->tools.capacity());
    for (const auto& tool : next->tools) names.emplace(tool.name.value, true);
    for (auto& tool : external) {
        if (!names.emplace(tool.name.value, true).second) continue;
        next->tools.push_back(std::move(tool));
    }

    next->idx.reserve(next->tools.size());
    for (const auto& tool : next->tools)
        next->idx.emplace(tool.name.value, &tool);

    if (c.current) c.retired.push_back(c.current);
    c.current = std::move(next);
    c.generation = generation;
    return c.current;
}
} // namespace

const std::vector<ToolDef>& registry() { return wire_tools(); }

// Legacy tool-name aliases → canonical registry name. The exec tool was renamed
// bash → shell, but `bash` is so dominant in model training data (it is Claude
// Code's actual tool name — commonly emitted as "Bash", capital B) that a model
// reliably calls it by the old name on the FIRST turn, before it internalises
// our schema. Without this the call 404s ("unknown tool: Bash"), the model
// burns a wasted failed call, then retries as `shell`. Canonicalising here —
// the ONE lookup every dispatch path (agent loop, subagent, ACP, mcp-serve,
// permission check) funnels through — absorbs that misfire so the legacy name
// just works.
//
// The match is CASE-INSENSITIVE and whitespace-trimmed: models emit "bash",
// "Bash", and "BASH" interchangeably, and the exact-case check was the bug that
// let "Bash" slip through to a hard failure.
[[nodiscard]] static std::string_view canonical_tool_name(std::string_view name) noexcept {
    // Trim surrounding whitespace without allocating.
    auto b = name.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return name;
    auto e = name.find_last_not_of(" \t\r\n");
    std::string_view trimmed = name.substr(b, e - b + 1);
    auto ieq = [](std::string_view s, std::string_view lit) {
        if (s.size() != lit.size()) return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != lit[i]) return false;
        }
        return true;
    };
    if (ieq(trimmed, "bash")) return "shell";
    return name;
}

std::string unknown_tool_error(std::string_view name) {
    std::string msg = "unknown tool: " + std::string{name}
        + ". This tool is not part of agentty's toolset — if a proxy or "
          "gateway advertised it, call it through that channel, not here. "
          "Available tools:";
    for (const auto& td : wire_tools()) {
        msg += ' ';
        msg += td.name.value;
        msg += ',';
    }
    if (msg.back() == ',') msg.pop_back();
    msg += '.';
    return msg;
}

const ToolDef* find(std::string_view name) {
    auto& cache = wire_cache();
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::unique_lock<std::mutex> lock(cache.mu);
        snapshot = refresh_wire_cache(lock, cache);
    }
    if (auto it = snapshot->idx.find(std::string{name}); it != snapshot->idx.end())
        return it->second;
    // Miss: retry once under the canonical name so a legacy alias resolves.
    if (auto canon = canonical_tool_name(name); canon != name)
        if (auto it = snapshot->idx.find(std::string{canon}); it != snapshot->idx.end())
            return it->second;
    return nullptr;
}

const std::vector<ToolDef>& wire_tools() {
    auto& cache = wire_cache();
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::unique_lock<std::mutex> lock(cache.mu);
        snapshot = refresh_wire_cache(lock, cache);
    }
    return snapshot->tools;
}

std::vector<ToolDef> wire_tools_snapshot() {
    // Hold the snapshot's shared_ptr across the copy so a concurrent
    // reload swapping c.current can't free it mid-copy, and RETURN a value
    // the caller owns — no reference into cache memory escapes. This is the
    // safe accessor for any thread that may overlap reload_mcp_plugins().
    auto& cache = wire_cache();
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::unique_lock<std::mutex> lock(cache.mu);
        snapshot = refresh_wire_cache(lock, cache);
    }
    return snapshot->tools;   // deep copy while snapshot keeps it alive
}

std::vector<const ToolDef*> select_wire_tools(
    std::string_view query, std::size_t max_external) {
    const auto& catalog = wire_tools();
    std::vector<const ToolDef*> selected;
    std::vector<std::pair<int, const ToolDef*>> candidates;
    selected.reserve(catalog.size());

    std::string q;
    q.reserve(query.size());
    for (unsigned char c : query)
        q.push_back(std::isalnum(c) ? static_cast<char>(std::tolower(c)) : ' ');
    std::vector<std::string> terms;
    for (std::size_t pos = 0; pos < q.size();) {
        while (pos < q.size() && q[pos] == ' ') ++pos;
        const auto begin = pos;
        while (pos < q.size() && q[pos] != ' ') ++pos;
        if (pos - begin >= 2) terms.emplace_back(q.substr(begin, pos - begin));
    }

    for (const auto& tool : catalog) {
        // Dispatch-only tools (passthrough fulfilling a proxy-advertised
        // schema) never enter the wire list — the proxy already injected
        // their schema into the request; ours would collide with it.
        if (!tool.advertise) continue;
        if (tool.origin == ToolOrigin::Native || tool.always_expose) {
            selected.push_back(&tool);
            continue;
        }
        std::string haystack = tool.name.value + " " + tool.origin_id + " " + tool.description;
        std::ranges::transform(haystack, haystack.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        int score = 0;
        for (const auto& term : terms) {
            if (tool.name.value.find(term) != std::string::npos) score += 8;
            if (tool.origin_id.find(term) != std::string::npos) score += 5;
            if (haystack.find(term) != std::string::npos) score += 2;
        }
        candidates.emplace_back(score, &tool);
    }

    if (candidates.size() <= max_external) {
        for (const auto& [_, tool] : candidates) selected.push_back(tool);
        return selected;
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < max_external; ++i)
        selected.push_back(candidates[i].second);
    return selected;
}

unsigned long mcp_generation() noexcept {
    return mcp::mcp_generation();
}

std::size_t reload_mcp_plugins() {
    // Serialize + coalesce reloads. Each plugin add/remove/toggle fires this
    // on a detached thread; the connect handshake can take seconds (bounded
    // by the bridge's 15s deadline). Without a guard, rapid toggles (disable
    // then re-enable) stack multiple reloads that each re-spawn the server
    // and race on the process-wide pool handle — the observed hang.
    //
    // Design: one reload runs at a time (reload_mu). A request that arrives
    // while a reload is in flight sets `pending` and returns immediately —
    // the running reload will loop once more to pick up the newer config,
    // so no edit is ever lost. This is the classic coalescing-worker
    // pattern: at most one extra pass, never a lost update, never a stack
    // of concurrent spawns.
    static std::mutex reload_mu;
    static std::atomic<bool> pending{false};

    std::unique_lock<std::mutex> lk(reload_mu, std::try_to_lock);
    if (!lk.owns_lock()) {
        pending.store(true, std::memory_order_release);
        return 0;   // the in-flight reload will re-run for our edit
    }

    std::size_t n = 0;
    do {
        pending.store(false, std::memory_order_release);
        n = mcp::mcp_reload();
        {
            auto& c = wire_cache();
            std::lock_guard<std::mutex> clk(c.mu);
            c.generation = static_cast<unsigned long>(-1);
        }
        // If a toggle landed during mcp_reload(), loop once more so the
        // config it wrote is reflected. Bounded: each pass clears the flag
        // first, so it only re-runs for edits that arrived AFTER this pass
        // started reading.
    } while (pending.load(std::memory_order_acquire));
    return n;
}

void invalidate_mcp_catalog() {
    // No re-spawn: bump the pool generation (so refresh_wire_cache
    // rebuilds) and drop the published snapshot. project_tools re-reads the
    // live tools.exclude on the rebuild, so an enable/disable toggle takes
    // effect on the next catalog access with zero server churn.
    mcp::mcp_bump_generation();
    auto& c = wire_cache();
    std::lock_guard<std::mutex> clk(c.mu);
    c.generation = static_cast<unsigned long>(-1);
}

} // namespace agentty::tools
