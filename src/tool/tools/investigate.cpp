// `investigate` — fire a self-contained sub-agent loop.
//
// Why this exists: the parent thread pays for every byte that lands in
// its context, *forever*. A tour to discover "where is X implemented and
// how does it work?" can spend 30k tokens worth of grep + read output
// that the parent then re-tokenises on every subsequent turn. This tool
// runs the same investigation in an isolated context, returns only the
// final synthesis, and the parent's context grows by ~500 tokens.
//
// Design — speed + power knobs:
//   * Parallel tool execution within a turn. When the sub-agent emits
//     N tool_use blocks in one round, all N run on independent worker
//     threads. A bash-free codebase scan that would take 5 × 1.2 s
//     sequential drops to ~1.2 s wall-clock.
//   * Model selectable. `model: "haiku" | "sonnet" | "opus"` (or any
//     literal `claude-…` id). Defaults to Haiku 4.5 for speed —
//     synthesis-of-evidence is comprehension-heavy but doesn't need a
//     frontier model, and Haiku's 2-3 s/turn keeps the round-trip
//     under 10 s for typical investigations.
//   * Observation tools, not just ReadFs. The sub-agent gets web_fetch
//     and web_search alongside the local read tools — "what does
//     library X document about Y?" is exactly the kind of open-ended
//     question that benefits from sub-agent isolation. WriteFs and
//     Exec stay blocked: anything that mutates state must surface in
//     the parent's UI for the user to see and approve.
//   * Live progress to the parent UI. Every turn boundary, every tool
//     call dispatched, and the final synthesis text deltas all stream
//     through `tools::progress::emit` so the parent's tool card shows
//     what the sub-agent is doing in real time.
//   * Higher turn cap (20) so deep investigations don't truncate.
//   * Self-recursion blocked (excluded from the tool subset).

#include "moha/tool/spec.hpp"
#include "moha/tool/tool.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/tool/util/utf8.hpp"

#include "moha/domain/conversation.hpp"
#include "moha/provider/anthropic/transport.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/msg.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

// ── Tunables ──────────────────────────────────────────────────────────
constexpr int      kMaxTurns           = 20;     // hard cap, parent context bound
constexpr int      kSubAgentMaxTok     = 8192;   // per-turn output cap
constexpr unsigned kMaxParallelTools   = 8;      // concurrent worker fan-out
constexpr auto     kProgressMinGap     = std::chrono::milliseconds{120};

constexpr const char* kInvestigatorPrompt =
    "You are an isolated investigation sub-agent inside a larger coding "
    "session. The parent agent dispatched a focused query — your job is "
    "to gather evidence efficiently and return a *single dense "
    "synthesis*.\n\n"
    "Workflow:\n"
    "  1. **Plan** silently — identify the 2-4 evidence items that would "
    "answer the question.\n"
    "  2. **Parallelise** — emit MULTIPLE tool calls in the same "
    "response when they're independent. The runtime executes them "
    "concurrently, so 4 reads at once finish in the time of 1 read.\n"
    "  3. **Refine** — follow up with targeted reads/greps once the "
    "first pass surfaces leads.\n"
    "  4. **Synthesize** — return the answer in 250-450 words. Names, "
    "line numbers, key relationships, gotchas, the bottom line. NO "
    "tool-call recap, NO long file excerpts pasted in — only the "
    "extracted facts the parent needs to act.\n\n"
    "Available tools: `repo_map`, `outline(path)`, `signatures(name)`, "
    "`find_definition(symbol)`, `read(path, offset, limit)`, `grep`, "
    "`glob`, `list_dir`, `git_status`, `git_diff`, `git_log`, "
    "`web_fetch`, `web_search`. Prefer `outline`/`signatures`/"
    "`repo_map` over blind `read`/`grep` — they're 10-50× cheaper.\n\n"
    "Critical: the parent's context only grows by your final answer. "
    "Be ruthlessly compact. End with a one-line bottom line in **bold**.";

// ── Argument parsing ─────────────────────────────────────────────────
struct InvestigateArgs {
    std::string query;
    std::string model;          // "" / "haiku" / "sonnet" / "opus" / literal id
    std::string display_description;
};

[[nodiscard]] std::expected<InvestigateArgs, ToolError>
parse_investigate_args(const json& j) {
    util::ArgReader ar(j);
    auto q = ar.require_str("query");
    if (!q)
        return std::unexpected(ToolError::invalid_args("query required"));
    return InvestigateArgs{
        *std::move(q),
        ar.str("model", ""),
        ar.str("display_description", ""),
    };
}

// Resolve the `model` arg. Shorthands map to family pinned IDs;
// `claude-…` literals pass through unchanged for forward-compat with
// new model releases.
[[nodiscard]] std::string resolve_model(std::string_view m) {
    if (m.empty() || m == "default" || m == "haiku")
        return "claude-haiku-4-5-20251001";
    if (m == "sonnet") return "claude-sonnet-4-6";
    if (m == "opus")   return "claude-opus-4-7";
    if (m.starts_with("claude-")) return std::string{m};
    return "claude-haiku-4-5-20251001";  // unknown → safe default
}

// Sub-agent's tool subset: every tool whose effects sit within the
// "observation only" set (ReadFs and/or Net, no WriteFs, no Exec, no
// self-recursion). Built once on first call — registry is immutable.
const std::vector<provider::anthropic::ToolSpec>& observation_tools() {
    static const std::vector<provider::anthropic::ToolSpec> v = []{
        std::vector<provider::anthropic::ToolSpec> out;
        for (const auto& td : tools::registry()) {
            if (td.effects.has(Effect::WriteFs)) continue;  // no mutation
            if (td.effects.has(Effect::Exec))    continue;  // no shells
            if (td.name.value == "investigate")  continue;  // no recursion
            out.push_back({td.name.value, td.description, td.input_schema,
                           td.eager_input_streaming});
        }
        return out;
    }();
    return v;
}

// ── Live-progress sink for the parent tool card ─────────────────────
// Throttled snapshot-emitter. The reducer expects FULL accumulated
// snapshots (not deltas), so each call replaces what the parent shows.
// We keep an in-memory transcript and re-emit it whenever we cross the
// throttle window — chatty token-by-token streaming becomes one paint
// per ~120 ms instead of per delta.
struct Progress {
    std::string                              transcript;
    std::chrono::steady_clock::time_point    last_emit{};

    void push_line(std::string line) {
        if (!transcript.empty()) transcript += '\n';
        transcript += std::move(line);
        flush(/*force=*/true);
    }
    void update_synthesis(std::string_view delta) {
        // Append to the in-progress synthesis section. We rebuild the
        // last "## synthesis" block each time so the model's text shows
        // live in the parent's tool card.
        constexpr std::string_view kHdr = "\n## synthesis\n";
        if (transcript.find(kHdr) == std::string::npos) {
            transcript += kHdr;
        }
        transcript += delta;
        flush(/*force=*/false);
    }
    void flush(bool force) {
        auto now = std::chrono::steady_clock::now();
        if (!force && (now - last_emit) < kProgressMinGap) return;
        progress::emit(transcript);
        last_emit = now;
    }
};

// ── One synchronous round-trip with the provider ────────────────────
// Collects assistant text + tool_use blocks into the supplied Message;
// surfaces errors as a nonempty `error` field. Optionally streams text
// deltas through the progress sink so the parent UI sees the synthesis
// being written in real time.
struct TurnResult {
    StopReason  stop = StopReason::Unspecified;
    std::string error;       // empty on success
};

[[nodiscard]] TurnResult
run_one_turn(provider::anthropic::Request req,
             Message& assistant_out,
             http::CancelTokenPtr cancel,
             Progress* progress = nullptr,
             bool stream_text_to_progress = false) {
    TurnResult tr;

    std::string current_tool_id;
    std::string current_tool_name;
    std::string current_tool_args;
    bool got_finished = false;

    provider::anthropic::run_stream_sync(std::move(req), [&](Msg m) {
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::same_as<T, StreamTextDelta>) {
                assistant_out.text += e.text;
                if (stream_text_to_progress && progress)
                    progress->update_synthesis(e.text);
            } else if constexpr (std::same_as<T, StreamToolUseStart>) {
                current_tool_id   = e.id.value;
                current_tool_name = e.name.value;
                current_tool_args.clear();
            } else if constexpr (std::same_as<T, StreamToolUseDelta>) {
                current_tool_args += e.partial_json;
            } else if constexpr (std::same_as<T, StreamToolUseEnd>) {
                json args_j = json::object();
                if (!current_tool_args.empty()) {
                    auto p = json::parse(current_tool_args, /*cb*/nullptr,
                                         /*throw*/false);
                    if (!p.is_discarded() && p.is_object()) args_j = std::move(p);
                }
                ToolUse tu;
                tu.id   = ToolCallId{current_tool_id};
                tu.name = ToolName{current_tool_name};
                tu.args = std::move(args_j);
                tu.status = ToolUse::Pending{};
                assistant_out.tool_calls.push_back(std::move(tu));
                current_tool_id.clear();
                current_tool_name.clear();
                current_tool_args.clear();
            } else if constexpr (std::same_as<T, StreamFinished>) {
                tr.stop = e.stop_reason;
                got_finished = true;
            } else if constexpr (std::same_as<T, StreamError>) {
                tr.error = e.message;
                got_finished = true;
            }
            // Ignore StreamStarted, StreamUsage, StreamHeartbeat — the
            // sub-agent loop is a wire collector, not a reducer.
        }, m);
    }, std::move(cancel));

    if (!got_finished && tr.error.empty())
        tr.error = "stream ended without a terminal event";
    return tr;
}

// ── Parallel tool execution ─────────────────────────────────────────
// Fan out N tool calls to up to `kMaxParallelTools` worker threads.
// Single-tool turns skip the spawn overhead. ToolUse entries live in
// the `tcs` vector — concurrent writes target *different* indices so
// no synchronisation is needed beyond the implicit join barrier.
//
// The DynamicDispatch::execute path is thread-safe by design: each
// tool's `execute()` closure operates on its own arguments + thread-
// local state (subprocess pipes, file handles, etc.). The shared
// repo-symbol index uses an internal mutex, so concurrent `outline`
// or `signatures` calls serialise on it but still benefit from
// pipelined I/O for the actual file reads they do beforehand.
void execute_calls_parallel(std::vector<ToolUse>& tcs) {
    if (tcs.empty()) return;
    auto run_one = [](ToolUse& tc) {
        auto t_start = std::chrono::steady_clock::now();
        auto result = tool::DynamicDispatch::execute(tc.name.value, tc.args);
        auto t_end = std::chrono::steady_clock::now();
        if (result) {
            tc.status = ToolUse::Done{
                t_start, t_end,
                util::to_valid_utf8(std::move(result->text)),
            };
        } else {
            tc.status = ToolUse::Failed{
                t_start, t_end,
                util::to_valid_utf8(result.error().render()),
            };
        }
    };
    if (tcs.size() == 1) { run_one(tcs[0]); return; }

    // Producer-consumer dispatch: workers pull next index atomically
    // until the queue is drained. Bounds threads to min(N, cap) so a
    // 1-tool turn doesn't spawn 8 idle joins.
    std::atomic<std::size_t> next{0};
    auto worker = [&]() {
        for (;;) {
            std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= tcs.size()) return;
            run_one(tcs[i]);
        }
    };
    unsigned threads = std::min<unsigned>(
        kMaxParallelTools, static_cast<unsigned>(tcs.size()));
    std::vector<std::jthread> pool;
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
    // jthread destructors auto-join here — barrier is the scope exit.
}

// Format a one-line "what's happening now" snapshot for the parent UI.
[[nodiscard]] std::string
turn_summary(int turn, const std::vector<ToolUse>& tcs,
             const std::string& model_id) {
    std::ostringstream o;
    o << "[investigate · " << model_id << " · turn " << turn << "] ";
    if (tcs.empty()) {
        o << "synthesising answer…";
    } else {
        o << (tcs.size() == 1 ? "calling " : "fan-out: ");
        bool first = true;
        for (const auto& tc : tcs) {
            if (!first) o << ", ";
            o << tc.name.value;
            first = false;
        }
    }
    return o.str();
}

ExecResult run_investigate(const InvestigateArgs& a) {
    const auto& d = app::deps();
    if (d.auth_header.empty())
        return std::unexpected(ToolError::network(
            "investigate: parent session is not authenticated"));

    const std::string model_id = resolve_model(a.model);
    Progress progress;
    progress.push_line("[investigate] query: \""
                       + (a.query.size() > 80
                          ? a.query.substr(0, 77) + "..."
                          : a.query) + "\"");

    // Conversation accumulates over the loop. First entry: the parent's
    // question framed as a user message. Subsequent assistant messages
    // carry their own tool_calls (with terminal Done/Failed status); the
    // existing transport.cpp:build_messages emits both the tool_use and a
    // synthetic user-side tool_result block, so we don't need to manage
    // the protocol ourselves — just keep appending Messages.
    std::vector<Message> messages;
    messages.push_back(Message{Role::User, a.query, {}, {},
                               std::chrono::system_clock::now(),
                               std::nullopt, std::nullopt});

    auto t0 = std::chrono::steady_clock::now();
    std::string final_text;
    int turns_used   = 0;
    int total_tools  = 0;
    StopReason last_stop = StopReason::Unspecified;

    for (int turn = 0; turn < kMaxTurns; ++turn) {
        ++turns_used;

        provider::anthropic::Request req;
        req.model         = model_id;
        req.system_prompt = kInvestigatorPrompt;
        req.messages      = messages;
        req.tools         = observation_tools();
        req.max_tokens    = kSubAgentMaxTok;
        req.auth_header   = d.auth_header;
        req.auth_style    = d.auth_style;

        Message assistant{Role::Assistant, {}, {}, {},
                          std::chrono::system_clock::now(),
                          std::nullopt, std::nullopt};

        // Stream text deltas to the parent's tool card on the LAST
        // turn (final synthesis) — but we don't know it's the last turn
        // until the model decides not to emit any tool calls. Easiest
        // proxy: stream every turn's text. Most non-final turns emit
        // very little text (just "I'll grep for X then read Y" type
        // chatter) so the noise is minimal and the synthesis still
        // dominates the live preview.
        progress.push_line("[turn " + std::to_string(turn + 1)
                           + "] thinking…");
        auto tr = run_one_turn(std::move(req), assistant, /*cancel=*/{},
                               &progress, /*stream_text_to_progress=*/true);

        if (!tr.error.empty()) {
            return std::unexpected(ToolError::network(
                "investigate sub-agent failed mid-stream: " + tr.error));
        }

        // No tool calls → terminal answer. Capture and return.
        if (assistant.tool_calls.empty()) {
            final_text = assistant.text;
            last_stop  = tr.stop;
            break;
        }

        total_tools += static_cast<int>(assistant.tool_calls.size());
        progress.push_line(turn_summary(turn + 1, assistant.tool_calls, model_id));

        // Concurrent dispatch — read-only by construction, no permission
        // gate or cross-effect serialisation needed inside the sub-agent.
        // The parent's effect-aware scheduler still handles the parent-
        // level interaction (the investigate Cmd itself).
        execute_calls_parallel(assistant.tool_calls);

        messages.push_back(std::move(assistant));
        last_stop = tr.stop;
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    auto framing = [&](std::string_view tag, const std::string& body) {
        std::ostringstream o;
        o << "[" << tag << " · " << model_id
          << " · " << turns_used << " turn"
          << (turns_used == 1 ? "" : "s")
          << " · " << total_tools << " tool call"
          << (total_tools == 1 ? "" : "s")
          << " · " << elapsed_ms << " ms"
          << " · stop=" << to_string(last_stop) << "]\n\n"
          << body;
        std::string s = o.str();
        if (!a.display_description.empty())
            s = a.display_description + "\n\n" + s;
        return s;
    };

    if (final_text.empty() && turns_used >= kMaxTurns) {
        std::string partial;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == Role::Assistant && !it->text.empty()) {
                partial = it->text;
                break;
            }
        }
        return ToolOutput{
            framing("investigate · CAPPED",
                    "Hit " + std::to_string(kMaxTurns)
                    + "-turn cap without a final answer. Last partial:\n"
                    + partial),
            std::nullopt};
    }

    return ToolOutput{framing("investigate", final_text), std::nullopt};
}

} // namespace

ToolDef tool_investigate() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"investigate">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Spawn an isolated, parallel sub-agent that researches `query` "
        "and returns one compact synthesis. The sub-agent has the full "
        "observation toolkit (read / grep / glob / list_dir / outline / "
        "repo_map / signatures / find_definition / git_* / web_fetch / "
        "web_search) and runs its tool calls in parallel within each "
        "turn — fan-out is the speed knob. It CANNOT write, edit, or "
        "shell out (those must surface in the parent thread for the "
        "user to see).\n\n"
        "Use for open-ended discovery where the *answer* is small but "
        "the *journey* would otherwise dump tens of thousands of "
        "tokens of grep/read output into THIS thread:\n"
        "  • 'where and how is HTTP/2 cancellation wired?'\n"
        "  • 'list every place that mutates session state'\n"
        "  • 'explain the streaming retry state machine'\n"
        "  • 'how does library X document Y?' (web_fetch backed)\n\n"
        "Optional `model`: 'haiku' (default, fast), 'sonnet' "
        "(balanced), 'opus' (max quality), or any literal claude-* id. "
        "20-turn cap, 8K max_tokens per turn, 8-way parallel tool fan-out.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"query", {{"type","string"},
                {"description","Natural-language research question for the sub-agent"}}},
            {"model", {{"type","string"},
                {"description","'haiku' (default) | 'sonnet' | 'opus' | claude-* id"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<InvestigateArgs>(parse_investigate_args, run_investigate);
    return t;
}

} // namespace moha::tools
