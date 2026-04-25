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

#include "moha/memory/file_card_store.hpp"
#include "moha/memory/memo_store.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tool.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/tool/util/utf8.hpp"

#include "moha/domain/conversation.hpp"
#include "moha/provider/anthropic/transport.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/msg.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_set>
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
//
// The transcript uses a structured line vocabulary that the view-side
// renderer (tool_card.cpp:investigate branch) parses into a beautiful
// per-turn card. Line prefixes are stable contracts; if you change one
// here, update the parser there in lockstep.
//
// Vocabulary:
//   Q: <query text>                          — once at start
//   T<n> thinking                            — turn N is calling the model
//   T<n> dispatch <count>: a, b, c           — fan-out of N tools begins
//   T<n> ok <toolname> <ms>ms                — per-tool success
//   T<n> err <toolname> <ms>ms <reason>      — per-tool failure
//   T<n> done <ok>/<n>                       — turn summary
//   T<n> synthesis                           — synthesis section starts
//   <free text>                              — synthesis body (multi-line)
struct Progress {
    std::string                              transcript;
    std::chrono::steady_clock::time_point    last_emit{};
    bool                                     in_synthesis = false;

    void line(std::string s) {
        if (!transcript.empty() && transcript.back() != '\n')
            transcript += '\n';
        transcript += std::move(s);
        transcript += '\n';
        flush(/*force=*/true);
    }
    // Append synthesis text deltas verbatim (no per-line marker — the
    // model's output is already paragraphed). The first delta opens the
    // synthesis section; subsequent ones extend it.
    void synthesis_open(int turn) {
        if (in_synthesis) return;
        in_synthesis = true;
        line("T" + std::to_string(turn) + " synthesis");
    }
    void synthesis_delta(std::string_view delta) {
        if (!in_synthesis) return;
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
             int turn_number = 0) {
    TurnResult tr;

    std::string current_tool_id;
    std::string current_tool_name;
    std::string current_tool_args;
    bool got_finished = false;
    // Track whether we've opened the synthesis section yet for this
    // turn — first non-empty text delta in a tool-less turn opens it.
    bool synthesis_opened = false;

    provider::anthropic::run_stream_sync(std::move(req), [&](Msg m) {
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::same_as<T, StreamTextDelta>) {
                assistant_out.text += e.text;
                if (progress) {
                    if (!synthesis_opened) {
                        progress->synthesis_open(turn_number);
                        synthesis_opened = true;
                    }
                    progress->synthesis_delta(e.text);
                }
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

// Build a "T<n> dispatch <count>: tool, tool, tool" line.
[[nodiscard]] std::string
dispatch_line(int turn, const std::vector<ToolUse>& tcs) {
    std::ostringstream o;
    o << "T" << turn << " dispatch " << tcs.size() << ":";
    bool first = true;
    for (const auto& tc : tcs) {
        o << (first ? " " : ", ");
        o << tc.name.value;
        first = false;
    }
    return o.str();
}

// Relativize an absolute path against the workspace root so the
// display is "src/auth.cpp" not "C:/Users/.../moha/src/auth.cpp".
// Falls back to the original string when the path isn't under the
// workspace (or canonicalisation fails — be lenient).
[[nodiscard]] std::string
relpath(std::string s) {
    if (s.empty()) return s;
    std::error_code ec;
    auto root = tools::util::workspace_root();
    if (root.empty()) return s;
    auto abs = std::filesystem::weakly_canonical(s, ec);
    if (ec) return s;
    auto rel = std::filesystem::relative(abs, root, ec);
    if (ec) return s;
    auto out = rel.generic_string();
    if (out.empty() || out == ".") return s;
    if (out.starts_with("..")) return s;     // outside workspace
    return out;
}

// One-line summary of a tool call's primary argument(s) — what the
// renderer wants to put next to the tool name so the user can tell
// `read src/auth.cpp` apart from `read src/oauth.hpp`. Falls back to
// "" when the tool has no surface-level noun (e.g. todo). Capped TIGHT
// because the row also has to fit a name column, a result summary,
// and a right-aligned timing — collectively they need to live within
// a single terminal line so yoga doesn't wrap them mid-word.
[[nodiscard]] std::string
tool_arg_summary(const std::string& name, const nlohmann::json& args) {
    if (!args.is_object()) return "";
    auto sval = [&](std::initializer_list<const char*> keys) -> std::string {
        for (auto k : keys) {
            if (auto it = args.find(k); it != args.end() && it->is_string())
                return it->template get<std::string>();
        }
        return "";
    };
    // Tighter cap (was 70). 50 chars is enough for "src/runtime/app/
    // update/stream.cpp" comfortably, and forces longer paths to
    // truncate cleanly with an ellipsis instead of overflowing the
    // row width when paired with a result summary + timing.
    auto truncate = [](std::string s, std::size_t cap = 50) {
        if (s.size() > cap) { s.resize(cap - 3); s += "..."; }
        return s;
    };
    if (name == "read") {
        // Just the (relative) path. The "L<a>-<b>" range info now
        // lives in the RESULT summary so we don't double-show it
        // (and don't blow the row width when the model paged a
        // big file).
        return truncate(relpath(sval({"path","file_path","filepath","filename"})));
    }
    if (name == "edit" || name == "write")
        return truncate(relpath(sval({"path","file_path","filepath","filename"})));
    if (name == "grep") {
        auto pat = sval({"pattern"});
        auto root = sval({"path"});
        auto glob = sval({"glob"});
        std::string out = "\"" + pat + "\"";
        if (!glob.empty())                          out += " " + glob;
        else if (!root.empty() && root != ".")      out += " " + relpath(root);
        return truncate(out);
    }
    if (name == "glob")            return truncate(sval({"pattern"}));
    if (name == "list_dir") {
        auto p = sval({"path"});
        return truncate(p.empty() ? std::string{"."} : relpath(p));
    }
    if (name == "find_definition") return truncate(sval({"symbol"}));
    if (name == "outline")         return truncate(relpath(sval({"path"})));
    if (name == "repo_map") {
        auto p = sval({"path"});
        return p.empty() ? std::string{"workspace"} : truncate(relpath(p));
    }
    if (name == "signatures")      return truncate("\"" + sval({"pattern"}) + "\"");
    if (name == "web_fetch")       return truncate(sval({"url"}));
    if (name == "web_search")      return truncate("\"" + sval({"query"}) + "\"");
    if (name == "git_log") {
        auto r = sval({"ref"});
        return r.empty() ? std::string{} : truncate(r);
    }
    if (name == "git_diff") {
        auto r = sval({"ref"});
        auto p = sval({"path"});
        std::string out;
        if (!r.empty()) out = r;
        if (!p.empty()) out += (out.empty() ? "" : " ") + relpath(p);
        return truncate(out);
    }
    if (name == "git_commit")      return truncate(sval({"message"}));
    if (name == "bash" || name == "diagnostics")
        return truncate(sval({"command"}));
    return "";
}

// One-line *result* summary derived from the tool's output. Tells the
// user what the call FOUND, not just what it asked for. Cheap parses;
// returns "" when the tool's output doesn't lend itself to a tagline.
[[nodiscard]] std::string
tool_result_summary(const ToolUse& tc) {
    if (!tc.is_done()) return "";
    const auto& out = tc.output();
    if (out.empty()) return "";
    const std::string& n = tc.name.value;

    // Helper: count newlines in a string.
    auto count_lines = [](std::string_view s) {
        int n_ = 0;
        for (char c : s) if (c == '\n') ++n_;
        if (!s.empty() && s.back() != '\n') ++n_;
        return n_;
    };

    if (n == "read") {
        // Read's `[showing lines A-B of T; K more — pass offset=…]`
        // hint is verbose. We want JUST the "A-B of T" part — the
        // "pass offset=…" suffix is for the model's offset choice and
        // would otherwise blow the row width when shown in the card.
        if (auto p = out.find("[showing lines "); p != std::string::npos) {
            auto end = out.find(']', p);
            if (end != std::string::npos) {
                auto inner = out.substr(p + 15, end - (p + 15));
                // Take through the first ";" only.
                if (auto sc = inner.find(';'); sc != std::string::npos)
                    inner.resize(sc);
                // Trim trailing whitespace.
                while (!inner.empty() && (inner.back() == ' '
                                       || inner.back() == '\n'
                                       || inner.back() == '\r'))
                    inner.pop_back();
                return "L" + inner;
            }
        }
        return std::to_string(count_lines(out)) + " lines";
    }
    if (n == "grep" || n == "find_definition") {
        if (auto p = out.find("Found "); p != std::string::npos) {
            auto end = out.find('.', p);
            if (end != std::string::npos)
                return out.substr(p + 6, end - (p + 6));
        }
        if (out.starts_with("No matches")) return "no matches";
        return std::to_string(count_lines(out)) + " lines";
    }
    if (n == "glob")     return std::to_string(count_lines(out)) + " files";
    if (n == "list_dir") return std::to_string(count_lines(out)) + " entries";
    if (n == "outline" || n == "signatures") {
        if (auto p = out.find('('); p != std::string::npos) {
            auto end = out.find(')', p);
            if (end != std::string::npos)
                return out.substr(p + 1, end - p - 1);
        }
        return "";
    }
    if (n == "repo_map") return std::to_string(count_lines(out)) + " lines";
    if (n == "web_fetch" || n == "web_search") {
        // First line is "HTTP <status> ..." for fetch; for search just
        // count result lines.
        if (auto nl = out.find('\n'); nl != std::string::npos)
            return out.substr(0, std::min<std::size_t>(nl, 50));
        return "";
    }
    if (n == "git_status") {
        // Look for `# branch.head <name>` and count entry lines.
        std::string branch;
        int dirty = 0;
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.starts_with("# branch.head "))
                branch = line.substr(14);
            else if (!line.empty() && (line[0] == '1' || line[0] == '2'
                                    || line[0] == '?'))
                ++dirty;
        }
        if (branch.empty() && dirty == 0) return "";
        std::ostringstream o;
        if (!branch.empty()) o << branch;
        if (dirty > 0) o << (branch.empty() ? "" : " · ") << dirty << " dirty";
        return o.str();
    }
    return "";
}

// Format a per-tool result line in the structured vocabulary.
//   T3 ok outline 120ms
//   T3 err grep 0ms no match
[[nodiscard]] std::string
result_line(int turn, const ToolUse& tc) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tc.finished_at() - tc.started_at()).count();
    std::ostringstream o;
    o << "T" << turn << " "
      << (tc.is_done() ? "ok " : "err ")
      << tc.name.value << " " << ms << "ms";
    if (tc.is_failed()) {
        std::string_view first_line = tc.output();
        if (auto nl = first_line.find('\n'); nl != std::string_view::npos)
            first_line = first_line.substr(0, nl);
        if (first_line.size() > 80) first_line = first_line.substr(0, 80);
        if (!first_line.empty()) o << " " << first_line;
    }
    return o.str();
}

ExecResult run_investigate(const InvestigateArgs& a) {
    const auto& d = app::deps();
    if (d.auth_header.empty())
        return std::unexpected(ToolError::network(
            "investigate: parent session is not authenticated"));

    // ── Layer 2: investigation cache lookup ─────────────────────────
    // Before spawning the sub-agent, ask the memo store if we've
    // already answered a similar question and the answer is still
    // fresh (git HEAD unchanged or referenced files un-modified). On
    // a hit, return the cached synthesis instantly — zero sub-agent
    // turns, zero tokens. The framing tag tells the user (and the
    // model) that this is replayed memory, not a fresh run.
    if (auto cached = memory::shared().find_similar(a.query);
        cached && memory::shared().is_fresh(*cached)) {
        std::ostringstream o;
        o << "[investigate · CACHED · 0 turns · 0 tool calls "
          << "· instant · matched memo " << cached->id
          << " stored " << std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now() - cached->created_at).count()
          << "s ago]\n\n" << cached->synthesis;
        std::string body = o.str();
        if (!a.display_description.empty())
            body = a.display_description + "\n\n" + body;
        return ToolOutput{std::move(body), std::nullopt};
    }

    const std::string model_id = resolve_model(a.model);
    Progress progress;
    progress.line("Q: " + a.query);
    progress.line("M: " + model_id);

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

        // Each turn opens with a "thinking" line. Subsequent text deltas
        // (from run_one_turn) lazily open a "synthesis" section if any.
        // Reset the per-turn synthesis flag so the next turn can open
        // a fresh section if it streams text.
        progress.in_synthesis = false;
        progress.line("T" + std::to_string(turn + 1) + " thinking");
        auto tr = run_one_turn(std::move(req), assistant, /*cancel=*/{},
                               &progress, /*turn_number=*/turn + 1);

        if (!tr.error.empty()) {
            return std::unexpected(ToolError::network(
                "investigate sub-agent failed mid-stream: " + tr.error));
        }

        // No tool calls → terminal answer. The synthesis_delta calls
        // already streamed the body; just record the final and exit.
        if (assistant.tool_calls.empty()) {
            final_text = assistant.text;
            last_stop  = tr.stop;
            break;
        }

        total_tools += static_cast<int>(assistant.tool_calls.size());
        progress.line(dispatch_line(turn + 1, assistant.tool_calls));

        // Per-tool ARG lines — emitted before execution so the user sees
        // *what* each tool is about to do, not just its name. Format:
        //   T<n> arg <name> <one-line summary>
        // Parser matches lines to tool rows in dispatch order.
        for (const auto& tc : assistant.tool_calls) {
            auto sum = tool_arg_summary(tc.name.value, tc.args);
            if (sum.empty()) continue;
            progress.line("T" + std::to_string(turn + 1)
                          + " arg " + tc.name.value + " " + sum);
        }

        // Concurrent dispatch — read-only by construction, no permission
        // gate or cross-effect serialisation needed inside the sub-agent.
        // The parent's effect-aware scheduler still handles the parent-
        // level interaction (the investigate Cmd itself).
        execute_calls_parallel(assistant.tool_calls);

        // Per-tool result + result-summary lines — gives the view both
        // ✓/✗ + ms AND a short "what was found" tag (e.g. "12 matches
        // in 4 files", "src/foo.cpp 1.2 KB", "no matches"). The arg
        // tells you what was asked; the res tells you what came back.
        int ok_count = 0, err_count = 0;
        for (const auto& tc : assistant.tool_calls) {
            progress.line(result_line(turn + 1, tc));
            if (auto rsum = tool_result_summary(tc); !rsum.empty()) {
                progress.line("T" + std::to_string(turn + 1)
                              + " res " + tc.name.value + " " + rsum);
            }
            if (tc.is_done()) ++ok_count; else ++err_count;
        }
        progress.line("T" + std::to_string(turn + 1) + " done "
                      + std::to_string(ok_count) + "/"
                      + std::to_string(assistant.tool_calls.size()));

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

    // ── Layer 1: persist memo ────────────────────────────────────
    // Walk the sub-agent's message history and harvest every file
    // path it touched (read / outline / signatures / etc.). These
    // become the memo's `file_refs` — used both by the freshness
    // check (have any of them changed?) and by Layer 3 (repo_map
    // surfacing "this file was discussed in investigation X").
    if (!final_text.empty()) {
        memory::Memo memo;
        memo.query     = a.query;
        memo.synthesis = final_text;
        memo.created_at = std::chrono::system_clock::now();

        // Extract file_refs from BOTH the sub-agent's tool args AND
        // the tool OUTPUTS — this is the richer signal. A grep
        // doesn't have a single "path" arg but its output names every
        // file with matches; same for glob and find_definition. The
        // freshness check then knows about every file the agent
        // actually inspected, not just the entry points.
        std::unordered_set<std::string> seen;
        auto add_path = [&](std::string s) {
            if (s.empty()) return;
            // Normalise to forward-slash workspace-relative.
            std::error_code ec;
            std::filesystem::path abs = std::filesystem::weakly_canonical(s, ec);
            if (ec) abs = s;
            std::filesystem::path rel = std::filesystem::relative(
                abs, tools::util::workspace_root(), ec);
            std::string out = ec ? abs.generic_string() : rel.generic_string();
            // Reject stuff outside the workspace (`../something`) and
            // duplicates. Cap total file_refs to keep memos compact.
            if (out.empty() || seen.contains(out)) return;
            if (out.size() >= 2 && out[0] == '.' && out[1] == '.') return;
            if (memo.file_refs.size() >= 32) return;
            seen.insert(out);
            memo.file_refs.push_back(std::move(out));
        };
        // Pull anything that LOOKS like a workspace-relative path from
        // a string. Heuristic: extension-bearing tokens (`foo.cpp`,
        // `src/runtime/main.cpp`) plus `path:line:` line prefixes
        // (find_definition / legacy grep output).
        auto harvest_paths = [&](std::string_view body) {
            // 1. `## Matches in <path>` — grep markdown header.
            std::size_t pos = 0;
            while ((pos = body.find("## Matches in ", pos)) != std::string::npos) {
                pos += 14;
                auto end = body.find('\n', pos);
                std::string p{body.substr(pos,
                    end == std::string_view::npos ? body.size() - pos : end - pos)};
                while (!p.empty() && (p.back() == ' ' || p.back() == '\r')) p.pop_back();
                if (p.starts_with("./")) p = p.substr(2);
                add_path(std::move(p));
                pos = end == std::string_view::npos ? body.size() : end;
            }
            // 2. `path:line:content` — find_definition + legacy grep.
            pos = 0;
            while (pos < body.size()) {
                auto nl = body.find('\n', pos);
                std::string_view line = body.substr(pos,
                    nl == std::string_view::npos ? body.size() - pos : nl - pos);
                pos = nl == std::string_view::npos ? body.size() : nl + 1;
                auto c1 = line.find(':');
                if (c1 == std::string_view::npos || c1 == 0) continue;
                if (c1 + 1 >= line.size() || !std::isdigit(static_cast<unsigned char>(line[c1+1])))
                    continue;
                std::string_view path = line.substr(0, c1);
                if (path.find('.') == std::string_view::npos) continue;
                if (path.starts_with("./")) path.remove_prefix(2);
                add_path(std::string{path});
            }
        };
        for (const auto& msg : messages) {
            for (const auto& tc : msg.tool_calls) {
                if (!tc.args.is_object()) continue;
                // Args path.
                for (const char* k : {"path","file_path","filepath","filename"}) {
                    if (auto it = tc.args.find(k);
                        it != tc.args.end() && it->is_string()) {
                        std::string s = it->get<std::string>();
                        // Skip directory args ("." / "src/" etc.) — they
                        // create false positives in freshness checks.
                        if (s == "." || s == "" || s.back() == '/') continue;
                        add_path(std::move(s));
                    }
                }
                // Output path harvest — far richer signal than args alone.
                if (tc.is_done() || tc.is_failed()) {
                    harvest_paths(tc.output());
                }
            }
        }
        // Layer 3 trigger: now that we know which files this
        // investigation touched, queue them for async knowledge-card
        // distillation. The worker calls Haiku in the background,
        // saves the card to <workspace>/.moha/cards/, and the next
        // turn's repo_map will surface the cards inline. Cheap
        // (Haiku ~$0.0008/file) and one-time per file:mtime.
        for (const auto& rel : memo.file_refs) {
            std::filesystem::path abs = tools::util::workspace_root() / rel;
            std::error_code ec;
            if (std::filesystem::exists(abs, ec))
                memory::shared_cards().request(abs);
        }
        memory::shared().add(std::move(memo));
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
