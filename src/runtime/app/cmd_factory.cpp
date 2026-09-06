#include "agentty/runtime/app/cmd_factory.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/update.hpp"
#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/provider/prompt_policy.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/util/modelsdev.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/mcp/client.hpp"   // plugin_model(), reload_mcp_plugins via registry
#include "agentty/tool/hooks.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace agentty::app::cmd {

using maya::Cmd;

namespace {

// ── Salvaged-call re-leak dedup ──────────────────────────────────────────────
// Weak local models on the OpenAI-compat path (qwen2.5-coder:7b via Ollama,
// llama.cpp templates) leak tool calls as bare JSON in the `content` channel
// instead of the structured tool_calls[] channel. The transport salvages those
// into real tool calls with a synthetic `call_salvaged_N` id (see
// provider/openai/transport.cpp). But these models routinely RE-LEAK the same
// call on the post-tool sub-turn: they see the tool_result, then emit the
// identical {"name":...,"arguments":...} again. Without a guard that re-leaked
// call runs a SECOND time (the duplicate stuck card the user reported, e.g. a
// `remember` that ran DONE then a clone stuck RUNNING).
//
// A structured tool call is the model's deliberate intent (calling `read`
// twice with the same path is legitimate), so we ONLY dedup SALVAGED calls —
// identified by the `call_salvaged_` id prefix, which no real server ever
// mints. The scope is the current agent turn: the run of consecutive Assistant
// messages since the last User message. If a pending salvaged call is
// byte-identical (same name + same args) to a tool call already TERMINAL in
// that run, it's a re-leak — we resolve it as Failed-without-side-effects
// (never re-execute) so the wire-layer tool_use ↔ tool_result pairing stays
// valid while the side effect happens exactly once.
[[nodiscard]] bool is_salvaged_call(const ToolCallId& id) noexcept {
    return std::string_view{id.value}.starts_with("call_salvaged_");
}

} // namespace

// Resolve every PENDING salvaged call in the back assistant message that
// duplicates a call already terminal earlier in the same agent turn. Returns
// the number deduped (for tests). Must run BEFORE kick_pending_tools promotes
// anything to Running.
std::size_t dedup_releaked_salvage_calls(Model& m) {
    if (m.d.current.messages.empty()) return 0;
    auto& msgs = m.d.current.messages;
    if (msgs.back().role != Role::Assistant) return 0;

    // Walk back to the first Assistant message of this turn (stop at the
    // User message that opened it). The turn is [turn_start, size).
    std::size_t turn_start = msgs.size();
    while (turn_start > 0 && msgs[turn_start - 1].role == Role::Assistant)
        --turn_start;

    // Collect (name, args.dump()) of every terminal call across the turn —
    // the set of side effects that have ALREADY happened (or failed) this
    // turn. args.dump() is canonical (key order is nlohmann's stable insert
    // order from the same parse path), so byte-equality == semantic equality
    // for the leaked-then-reparsed JSON. Uses the cached args_dump() so a
    // long agentic run doesn't re-serialize every prior call's args each
    // sub-turn (the doom-loop breaker walks the whole run per tool kick).
    auto sig = [](const ToolUse& tc) {
        return tc.name.value + '\0' + tc.args_dump();
    };
    std::unordered_set<std::string> terminal_sigs;
    std::size_t terminal_salvaged = 0;   // how many salvaged calls already ran
    for (std::size_t i = turn_start; i < msgs.size(); ++i)
        for (const auto& tc : msgs[i].tool_calls)
            if (tc.is_terminal()) {
                terminal_sigs.insert(sig(tc));
                if (is_salvaged_call(tc.id)) ++terminal_salvaged;
            }

    // Runaway-loop guard. Exact-match dedup (below) only catches a re-leak
    // whose args are BYTE-identical to a prior terminal call. Weak local
    // models (qwen2.5-coder:7b via Ollama) routinely re-leak the SAME tool
    // with slightly drifted args every post-tool sub-turn — different scope,
    // reworded text — so dedup misses it and the agent loops forever (the
    // "stuck RUNNING" card the user sees). Once a turn has already executed
    // kMaxSalvagedPerTurn salvaged calls, treat any further pending salvaged
    // call as a leak loop and fail it WITHOUT running, regardless of args.
    // Structured tool calls (deliberate model intent) are never counted or
    // capped here — only synthetic salvaged leaks.
    //
    // 3 matches the industry consensus for the "doom-loop" / repeat threshold
    // on weak local models (Hermes-agent doom-loop=3; deer-flow warns at 3,
    // hard-stops at 5; LangChain's 15 is for strong hosted models). A leaked
    // tool call only ever comes from a small Ollama model that ignored the
    // local-model guidance, so a tight bound turns a multi-second "stuck"
    // hang into bounded degradation (~3 sub-turns) before we force the model
    // to answer in plain text.
    constexpr std::size_t kMaxSalvagedPerTurn = 3;

    // Memory-management tools (remember / forget / wipe_memory) are META: they
    // mutate the agent's own learned-memory store and must only fire on an
    // EXPLICIT user request ("remember X", "forget Y"). A weak local model
    // leaks them into `content` on greetings/small-talk ("remember: Hi there!",
    // "forget: Hi there!") — always junk, never the user's intent. A SALVAGED
    // call to one of these is therefore always illegitimate; fail it WITHOUT
    // running, regardless of budget or exact-match. Structured calls are left
    // alone (deliberate intent / the /slash activation path). This is the
    // single most-leaked tool class on small Ollama models, so it gets its own
    // hard block ahead of the generic loop guard.
    static constexpr std::string_view kMemoryTools[] = {
        "remember", "forget", "wipe_memory"};
    auto is_memory_tool = [](std::string_view n) {
        for (auto t : kMemoryTools) if (t == n) return true;
        return false;
    };
    std::size_t mem_blocked = 0;
    const auto now0 = std::chrono::steady_clock::now();
    for (auto& tc : msgs.back().tool_calls) {
        if (!tc.is_pending() && !tc.is_approved()) continue;
        if (!is_salvaged_call(tc.id)) continue;
        if (!is_memory_tool(tc.name.value)) continue;
        tc.status = ToolUse::Failed{
            tc.started_at(), now0,
            std::string{"not run ("} + tc.name.value
            + ": memory tools run only on an explicit user request)"};
        ++mem_blocked;
    }

    if (terminal_sigs.empty() && terminal_salvaged < kMaxSalvagedPerTurn)
        return mem_blocked;

    std::size_t deduped = 0;
    const auto now = std::chrono::steady_clock::now();
    for (auto& tc : msgs.back().tool_calls) {
        if (!tc.is_pending() && !tc.is_approved()) continue;
        if (!is_salvaged_call(tc.id)) continue;   // only dedup salvaged leaks
        const bool is_exact_releak = terminal_sigs.contains(sig(tc));
        const bool over_budget     = terminal_salvaged >= kMaxSalvagedPerTurn;
        if (!is_exact_releak && !over_budget) continue;
        // Re-leak of a call already settled this turn (exact match), OR the
        // turn has blown its salvage budget (drifting-args leak loop).
        // Resolve as Failed WITHOUT running it — any real side effect already
        // happened. The message tells the model to stop re-emitting and finish.
        tc.status = ToolUse::Failed{
            tc.started_at(), now,
            over_budget && !is_exact_releak
                ? std::string{"not run (too many repeated tool calls this "
                  "turn)"}
                : std::string{"not run (duplicate — this exact call already "
                  "ran this turn; its result is above)"}};
        ++deduped;
    }
    return deduped + mem_blocked;
}

namespace {

// Body of the synthetic summarisation prompt. Identical text to what
// CompactContext used to push directly into `messages` — we hoist it
// here so the compaction wire payload can build it without polluting
// the transcript. Mirrors Claude Code's `mm8` summary prompt (binary
// near offset 134600). The schema (Task / State / Discoveries / Next
// Steps / Context-to-Preserve) is deliberately verbose: it nudges the
// model to write a recoverable summary instead of a one-paragraph
// précis that loses operationally-load-bearing details.
constexpr std::string_view kCompactionSummaryPrompt =
    "You have been working on the task described above but have "
    "not yet completed it. Write a continuation summary that "
    "will allow you (or another instance of yourself) to resume "
    "work efficiently in a future context window where the "
    "conversation history will be replaced with this summary. "
    "Your summary should be structured, concise, and actionable. "
    "Include:\n"
    "1. Task Overview\n"
    "  The user's core request and success criteria\n"
    "  Any clarifications or constraints they specified\n"
    "2. Current State\n"
    "  What has been completed so far\n"
    "  Files created, modified, or analyzed (with paths if relevant)\n"
    "  Key outputs or artifacts produced\n"
    "3. Important Discoveries\n"
    "  Technical constraints or requirements uncovered\n"
    "  Decisions made and their rationale\n"
    "  Errors encountered and how they were resolved\n"
    "  What approaches were tried that didn't work (and why)\n"
    "4. Next Steps\n"
    "  Specific actions needed to complete the task\n"
    "  Any blockers or open questions to resolve\n"
    "  Priority order if multiple steps remain\n"
    "5. Context to Preserve\n"
    "  User preferences or style requirements\n"
    "  Domain-specific details that aren't obvious\n"
    "  Any promises made to the user\n"
    "Be concise but complete \xe2\x80\x94 err on the side of "
    "including information that would prevent duplicate work or "
    "repeated mistakes. Write in a way that enables immediate "
    "resumption of the task. Do not call any tools; just write "
    "the summary text. Wrap the summary in <summary></summary> "
    "tags.";

// Alternate summary styles for the fork picker. Every one still ends with
// the <summary></summary> contract and the no-tools directive so the same
// finalize path parses them identically — only the shape of the requested
// prose differs.
constexpr std::string_view kSummaryTail =
    " Do not call any tools; just write the summary text. Wrap the "
    "summary in <summary></summary> tags.";

constexpr std::string_view kBriefPrompt =
    "Summarize everything above as a single concise paragraph — a TL;DR "
    "a teammate could read in ten seconds to know what this session is "
    "about, what was done, and what's left.";

constexpr std::string_view kBulletsPrompt =
    "Summarize everything above as a tight bullet outline. Group under "
    "short headers (Goal, Done, Findings, Next) and keep each bullet to "
    "one line. Preserve file paths, decisions, and open questions.";

constexpr std::string_view kDecisionsPrompt =
    "Summarize everything above focusing on the DECISIONS made and the "
    "reasoning behind them — the why, not the mechanics. List each decision, "
    "the alternatives considered, and why the chosen path won. Note any "
    "decision still open.";

constexpr std::string_view kHandoffPrompt =
    "Write a handoff for a fresh agent picking up this task cold. Skip the "
    "history; give ONLY what's needed to continue: the concrete next steps "
    "in priority order, the files and commands involved, any blockers, and "
    "the success criteria. Actionable and imperative.";

constexpr std::string_view kNarrativePrompt =
    "Write a flowing prose recap of this whole session as a short story of "
    "the work: what the user wanted, how the approach evolved, what was "
    "discovered, and where things stand now. Readable, not a checklist.";

// Map a CompactionStyle to its summarisation prompt. Recoverable is the full
// structured prompt above (already self-contained); the rest append the
// shared <summary>-contract tail so finalize parses them uniformly.
std::string compaction_prompt_for(CompactionStyle style) {
    switch (style) {
        case CompactionStyle::Recoverable:
            return std::string{kCompactionSummaryPrompt};
        case CompactionStyle::Brief:
            return std::string{kBriefPrompt}     + std::string{kSummaryTail};
        case CompactionStyle::Bullets:
            return std::string{kBulletsPrompt}   + std::string{kSummaryTail};
        case CompactionStyle::Decisions:
            return std::string{kDecisionsPrompt} + std::string{kSummaryTail};
        case CompactionStyle::Handoff:
            return std::string{kHandoffPrompt}   + std::string{kSummaryTail};
        case CompactionStyle::Narrative:
            return std::string{kNarrativePrompt} + std::string{kSummaryTail};
    }
    return std::string{kCompactionSummaryPrompt};
}

// Body prefix wrapping the model's summary text into a synthetic User
// message that goes on the wire in place of [0, up_to_index). The
// trailing "Continue…" directive (CC trick, binary near offset
// 77409806) suppresses the model's tendency to start with a "let me
// recap what we were doing" preamble.
std::string wrap_summary_as_user_text(const std::string& summary) {
    return "This session is being continued from a previous "
           "conversation that ran out of context. The summary "
           "below covers the earlier portion of the "
           "conversation; recent messages are preserved "
           "verbatim after this summary.\n\nSummary:\n"
         + summary
         + "\n\nContinue the work from where it left off "
           "without re-acknowledging this summary or recapping "
           "what was happening. Pick up the last task as if "
           "the break never happened.";
}

// Bytes-based prefix token estimate over an arbitrary wire view
// (NOT a Thread). Used internally by the soft-trim path; matches the
// approximation that public estimate_wire_tokens / estimate_prefix_tokens
// use so all three agree on "is this payload too big."
// Per-message byte + image tally — the shared kernel behind every token
// estimate in this file. Split out so the front-trim helpers can price
// each message ONCE and then find their drop index in a single pass
// instead of re-summing the whole vector on every erase (the old
// O(N²)-with-front-shift loop).
struct MsgWeight { std::size_t bytes = 0; int images = 0; };

MsgWeight message_weight(const Message& m) {
    MsgWeight w;
    w.bytes += m.text.size();
    w.bytes += m.streaming_text.size();
    w.bytes += m.pending_stream.size();
    w.images += static_cast<int>(m.images.size());
    for (const auto& tc : m.tool_calls) {
        w.bytes += tc.name.value.size();
        w.bytes += tc.args_streaming.size();
        w.bytes += tc.output().size();
        w.bytes += tc.progress_text().size();
    }
    return w;
}

int tokens_from(std::size_t bytes, int images) {
    constexpr double kBytesPerToken  = 3.5;
    constexpr int    kTokensPerImage = 1500;
    return static_cast<int>(static_cast<double>(bytes) / kBytesPerToken)
         + images * kTokensPerImage;
}

[[maybe_unused]] int estimate_messages_tokens(const std::vector<Message>& v) {
    std::size_t bytes = 0;
    int images = 0;
    for (const auto& m : v) {
        const auto w = message_weight(m);
        bytes  += w.bytes;
        images += w.images;
    }
    return tokens_from(bytes, images);
}


// Single-pass front-trim: given a wire vector, return the number of
// entries to drop starting at index `keep_head` (1 = preserve the
// leading entry) so the estimated token cost of the survivors fits
// `ceiling`. Prices every message once, then peels weight off the
// front until the running remainder fits — O(N) total, replacing the
// O(N²) "re-sum + erase(begin())" loops. Returns a drop count relative
// to `keep_head` (i.e. erase [keep_head, keep_head + drop)).
std::size_t front_drop_count(const std::vector<Message>& v, int ceiling,
                             std::size_t keep_head) {
    if (v.size() <= keep_head) return 0;
    std::size_t total_bytes = 0;
    int total_images = 0;
    for (const auto& m : v) {
        const auto w = message_weight(m);
        total_bytes  += w.bytes;
        total_images += w.images;
    }
    std::size_t drop = 0;
    // Peel entries off the front (after the preserved head) while the
    // remaining estimate exceeds the ceiling and there's something left
    // to drop. Mirror the old loops' termination: stop once <= 1 total
    // entry would remain. A ceiling <= 0 means "drop as much as possible"
    // (the old compact loop's behaviour when context_max*0.65 rounded to
    // 0): any non-empty survivor estimates > 0, so it peels down to 1.
    while (tokens_from(total_bytes, total_images) > ceiling
           && (v.size() - drop) > 1
           && (keep_head + drop) < v.size()) {
        const auto w = message_weight(v[keep_head + drop]);
        total_bytes  -= w.bytes;
        total_images -= w.images;
        ++drop;
    }
    return drop;
}

// Adaptive soft-trim: drop oldest entries from a wire view until its
// estimated token cost fits a ceiling. Preserves the leading entry
// (typically the compaction-summary synth user OR the first real
// user turn) so the model retains task-rooting context, and ensures
// the result still starts with a User per Anthropic's wire rule.
//
// This is the workflow-protection knob. When the model is mid-tool-
// burst and the transcript has grown past context_max, we DON'T yank
// the agent into a compaction round (which would surface as "your
// agent got stopped"). We just trim the oldest raw turns off the
// wire payload and let the loop continue. Compaction is then a
// strictly-better optimisation the user can run at their leisure;
// the agent never observes a wedge.
//
// The trim is REVERSIBLE: nothing about the transcript or any
// CompactionRecord is mutated. Two requests apart with different
// soft-trim ceilings produce different wire payloads from the same
// source state. The next user turn that submits with more context
// available will send a fatter prefix automatically.
void soft_trim_to_ceiling(std::vector<Message>& v, int ceiling) {
    if (ceiling <= 0 || v.size() <= 1) return;
    // Sentinel: the leading entry stays put. Price every message once,
    // find how many to drop from index 1 forward in a single pass, then
    // erase them in ONE shift. If we run out of trimmable entries we send
    // what we have — the upstream will hard-reject with a clear error,
    // which is strictly better than the in-tool-burst yank.
    const std::size_t drop = front_drop_count(v, ceiling, /*keep_head=*/1);
    if (drop > 0)
        v.erase(v.begin() + 1, v.begin() + 1 + static_cast<std::ptrdiff_t>(drop));
    // Drop any leading Assistants exposed by the trim — the wire
    // must start with a User. (If [0] was a compaction-summary user,
    // it remains; if it was a real first-user-turn and we never
    // touched [0], we're already fine. The pathological case is
    // when [0] gets dropped by a future change — defensive guard.)
    std::size_t lead = 0;
    while (lead < v.size() && v[lead].role == Role::Assistant) ++lead;
    if (lead > 0) v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(lead));
}


// Normal-turn wire payload: if the thread has compaction records,
// replace the prefix [0, latest.up_to_index) with one synthetic User
// message carrying the summary. Tail [latest.up_to_index..end) goes
// verbatim, preserving the user's recent work as anchor for the model.
// No compactions → the transcript ships as-is.
//
// Anthropic wire requirement: the messages array must start with a
// User. The synthetic summary IS a User, and if it's absent the first
// real message at index 0 has historically been a User too (the user's
// initial prompt), so the invariant holds either way.
std::vector<Message> wire_messages_for_impl(const Thread& t) {
    if (t.compactions.empty()) return t.messages;
    const auto& rec = t.compactions.back();
    if (rec.up_to_index == 0 || rec.up_to_index > t.messages.size()) {
        return t.messages;  // defensive: malformed record → send raw transcript
    }
    std::vector<Message> out;
    out.reserve(1 + (t.messages.size() - rec.up_to_index));
    Message summary_msg;
    summary_msg.role = Role::User;
    summary_msg.is_compact_summary = true;   // tag for any downstream code that still keys on it
    summary_msg.text = wrap_summary_as_user_text(rec.summary);
    out.push_back(std::move(summary_msg));
    for (std::size_t i = rec.up_to_index; i < t.messages.size(); ++i) {
        out.push_back(t.messages[i]);
    }
    return out;
}

// Rvalue overload: when the caller OWNS the Thread (the stream worker owns
// its per-turn snapshot), the common no-compaction path can MOVE the
// messages out instead of deep-copying them a second time. The worker used
// to pay two full-transcript copies per turn — one into the task closure on
// the UI thread, then this builder's `return t.messages` copy — on a long
// thread that's two multi-MB clones. Moving here retires the second.
std::vector<Message> wire_messages_for_impl(Thread&& t) {
    if (t.compactions.empty()) return std::move(t.messages);
    const auto& rec = t.compactions.back();
    if (rec.up_to_index == 0 || rec.up_to_index > t.messages.size()) {
        return std::move(t.messages);   // defensive: malformed record
    }
    std::vector<Message> out;
    out.reserve(1 + (t.messages.size() - rec.up_to_index));
    Message summary_msg;
    summary_msg.role = Role::User;
    summary_msg.is_compact_summary = true;
    summary_msg.text = wrap_summary_as_user_text(rec.summary);
    out.push_back(std::move(summary_msg));
    for (std::size_t i = rec.up_to_index; i < t.messages.size(); ++i) {
        out.push_back(std::move(t.messages[i]));
    }
    return out;
}

// Compaction-kickoff wire payload: send the prefix the user wants to
// summarise (with any prior compactions already applied so we don't
// re-send already-summarised turns raw), followed by the summarisation
// prompt as a synthetic User turn. The model's reply IS the summary;
// it streams into `m.s.compaction_buffer` rather than into `messages`.
//
// `context_max` is consulted to keep the request itself from blowing
// the context window — we trim the OLDEST raw turns of the prefix
// (preserving the leading User-wire-shape, never trimming a synthetic
// summary already at the head) until the estimate fits ~65% of the
// window, leaving headroom for the prompt + the summary response.
std::vector<Message> wire_messages_for_compaction(const Thread& t, int context_max,
                                                  CompactionStyle style,
                                                  int ceiling_override) {
    // Start from the already-compaction-substituted view so stacked
    // compactions don't double-count the summarised prefix.
    std::vector<Message> base = wire_messages_for_impl(t);

    // Trim from the front until the slice we ask the model to summarise fits
    // a BOUNDED size. Two caps, whichever is smaller:
    //   • ~65% of context_max — leaves room for the summary prompt + reply on
    //     small windows;
    //   • kCompactionSliceCap (absolute) — the load-bearing cap. Summarising
    //     is a compression task, and a cheap/Haiku-class model summarises
    //     ~150k of recent transcript far better than 650k (65% of a 1M
    //     window) in one shot: less to lose, tighter output, lower cost. So
    //     even on a huge window we only ever hand the summariser the most
    //     recent bounded slab of history.
    // The trim keeps the first real User turn (keep_head=1) so the original
    // task framing survives into the summary instead of being dropped.
    if (context_max > 0 || ceiling_override > 0) {
        constexpr int kCompactionSliceCap = 150000;
        int ceiling = static_cast<int>(static_cast<double>(context_max) * 0.65);
        if (ceiling > kCompactionSliceCap) ceiling = kCompactionSliceCap;
        // An explicit override (the adaptive shrink-retry) wins outright —
        // it's the ceiling we already know fits under the provider's hard
        // limit. It can drive the slice far below the default, down to a
        // last-few-turns recap on a tiny window.
        if (ceiling_override > 0) ceiling = ceiling_override;
        // When shrinking hard (a small override), don't insist on keeping
        // the first User turn — if the head alone busts the ceiling, the
        // summary of whatever recent slab fits is better than a 400.
        const std::size_t keep_head = (ceiling_override > 0 && ceiling < 20000)
                                          ? 0u : 1u;
        const std::size_t drop = front_drop_count(base, ceiling, keep_head);
        if (drop > 0)
            base.erase(base.begin() + static_cast<std::ptrdiff_t>(keep_head),
                       base.begin() + static_cast<std::ptrdiff_t>(keep_head + drop));
        // Drop any leading Assistants exposed by the trim — Anthropic
        // requires the wire to start with a User.
        std::size_t lead = 0;
        while (lead < base.size() && base[lead].role == Role::Assistant) ++lead;
        if (lead > 0)
            base.erase(base.begin(),
                       base.begin() + static_cast<std::ptrdiff_t>(lead));
    }

    // Append the synthetic summarisation prompt as the trailing User. The
    // prompt text is chosen by the requested style (fork picker / compaction).
    Message synth;
    synth.role = Role::User;
    synth.text = compaction_prompt_for(style);
    base.push_back(std::move(synth));
    return base;
}

} // namespace

std::vector<Message> wire_messages_for(const Thread& t) {
    return wire_messages_for_impl(t);
}

int estimate_wire_tokens(const Thread& t) {
    // Same bytes/3.5 + ~1500-per-image approximation as
    // estimate_prefix_tokens(Thread), but against the WIRE view so any
    // compaction summary substitution is accounted for. Auto-compaction
    // triggers MUST use this; using the raw-transcript estimate would
    // re-fire compaction immediately after every round because the
    // user-visible transcript never shrinks.
    //
    // Byte-count the wire view WITHOUT materialising it. The old body did
    // `wire_messages_for_impl(t)` — a full deep-copy/transform of every
    // message — purely to sum bytes and throw the vector away. This runs on
    // the REDUCER thread twice per turn (the proactive-compaction trigger at
    // finalize_turn and the est-calibration on StreamUsage), so on a large
    // context it was allocating and freeing a multi-MB transcript clone on
    // the UI thread every turn — exactly the per-turn cost that makes big
    // contexts feel sluggish. Walk the messages in place instead: sum the
    // same MsgWeight the materialised path would, applying the identical
    // compaction-substitution rule, with zero allocation beyond the one-off
    // wrapped-summary weight.
    const bool has_compaction =
        !t.compactions.empty()
        && t.compactions.back().up_to_index != 0
        && t.compactions.back().up_to_index <= t.messages.size();

    std::size_t bytes = 0;
    int images = 0;
    std::size_t start = 0;
    if (has_compaction) {
        // The covered prefix [0, up_to_index) is replaced on the wire by a
        // single synthetic User summary message. Price only that summary's
        // text (matching wire_messages_for_impl's summary_msg), then the raw
        // tail from up_to_index on.
        const auto& rec = t.compactions.back();
        bytes += wrap_summary_as_user_text(rec.summary).size();
        start = rec.up_to_index;
    }
    for (std::size_t i = start; i < t.messages.size(); ++i) {
        const auto w = message_weight(t.messages[i]);
        bytes  += w.bytes;
        images += w.images;
    }
    return tokens_from(bytes, images);
}

std::optional<Message> build_smart_routing_card(const Model& m) {
    // Only emit the card when orchestration is actually driving the turn.
    // (Internal/subagent routing without orchestration doesn't change the
    // MAIN turn's model, so there's no per-turn decision worth surfacing.)
    if (!m.d.smart.orchestration()) return std::nullopt;

    const std::string& model_id = m.d.model_id.value;
    smart::RoleProfile prof =
        smart::resolve_role(smart::ModelRole::Strategic, model_id,
                            m.d.effort, m.d.available_models, m.d.smart,
                            detail::active_provider_id());
    // Classify the newest real user turn (same logic launch_stream uses) so
    // the card's effort matches what the wire will actually carry.
    std::string_view newest_user;
    std::size_t nu_attach_bytes = 0;
    int         nu_images = 0;
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it)
        // !smart_routing: never classify a prior turn's zero-text 🧠 card —
        // keeps this scan IDENTICAL to launch_stream's so the card's shown
        // route can never disagree with the wire. !fork_note: the fork
        // provenance card is not the user's prompt either.
        if (it->role == Role::User && !it->is_proactive_context()
            && !it->smart_routing && !it->fork_note) {
            newest_user = it->text;
            for (const auto& a : it->attachments) nu_attach_bytes += a.body.size();
            nu_images = static_cast<int>(it->images.size());
            break;
        }
    const smart::ComplexityScore cx_text =
        smart::classify_score_with_context(newest_user, m.s.smart_turn_complexity,
                                           m.d.smart.complex_threshold);
    // THE composed classifier — same call as launch_stream so card == wire.
    // That equality now includes the TUNING: a card classified at the default
    // threshold while the wire used the user's would show a route the request
    // never took.
    const smart::ComplexityScore cx = smart::classify_turn(
        newest_user, m.s.smart_turn_complexity, nu_attach_bytes, nu_images,
        m.d.smart.complex_threshold);
    const auto caps = resolved_caps(prof.model);
    // Session cascade bias only. The per-workspace LEARNED prior (a Beta-
    // smoothed regret rate keyed by turn signature) was deleted with the rest
    // of the self-supervised layers: it mutated routing from persisted state
    // that was never measured against the fixed policy.
    const int bias = m.s.smart_effort_bias;
    const Effort scaled = smart::effort_for_score(prof.effort, cx, caps, bias,
                                                  m.d.smart.deep_margin);

    // Effort PROVENANCE — make the adaptive decision legible: base effort, the
    // complexity step, and the blended correction that moved it (shown as the
    // winning source so the card matches what the wire actually carries).
    std::string note{effort_label(prof.effort)};
    note += " \xe2\x86\x92 ";                       // →
    note += smart::to_string(cx.tier);
    // Lift provenance: the tier moved past the raw text score — an attached
    // payload, a continuation cue ("continue" resuming hard work), or a
    // correction floor ("still broken" never routes below Standard). Name it
    // so a heavy route on a 3-word prompt doesn't read as classifier noise.
    if (cx.tier != cx_text.tier) {
        if (smart::is_routing_correction(newest_user)) note += " (correction)";
        else if (smart::is_continuation_cue(newest_user)) note += " (continuation)";
        else note += " (payload)";
    }
    if (bias != 0) {
        note += " \xc2\xb7 session ";
        note += (bias > 0 ? "+" : "-") + std::to_string(std::abs(bias));
    }

    Message card;
    // Message.id auto-inits via new_message_id().
    // User role → renders as a STANDALONE single-message turn (like the
    // proactive card), not grouped into the assistant run. It is wire-inert
    // (stripped in drop_stale_proactive) and skipped by every loop/wire walk
    // via the smart_routing flag, so the role is a pure rendering choice.
    card.role          = Role::User;
    card.smart_routing = true;
    card.smart_route_model      = prof.model;
    card.smart_route_effort     = std::string{effort_label(scaled)};
    card.smart_route_complexity = std::string{smart::to_string(cx.tier)};
    card.smart_route_note       = std::move(note);
    card.smart_route_orchestrate = m.d.smart.orchestration();
    card.smart_route_subagents   = m.d.smart.subagent_routing();
    return card;
}

Cmd<Msg> launch_stream(Model& m) {
    // Defer the wire-payload build to the worker. The UI thread used
    // to spend ~5-50 ms here on long threads (full t.messages deep
    // copy via wire_messages_for_impl, plus soft_trim_to_ceiling's
    // estimate loop) between the user pressing Enter and the next
    // render — visible as input lag on the "my message appears" frame.
    // Now the UI returns to render after a few microseconds of cheap
    // capture work; the heavy lift runs concurrently with the first
    // post-submit paint.
    //
    // What stays on the UI thread (must — they read the live Model):
    //   • cancel token mint + stash onto active_ctx
    //   • shallow Thread copy into the task closure (vector of
    //     Messages; each Message itself contains owned strings/vectors
    //     so the copy is deep — but it's the same copy wire_messages
    //     _for_impl was doing anyway, just relocated)
    //
    // What moves to the worker:
    //   • compaction-vs-normal payload branch
    //   • soft_trim_to_ceiling
    //   • tools::registry() walk
    //   • provider::system_prompt_for(active selection)
    //   • deps().stream(...) (already async)
    //
    // Mint a fresh cancel token per turn and stash it on the active
    // ctx so the Esc handler (Msg::CancelStream) can flip it. The
    // worker holds its own shared_ptr via the captured request; storing
    // it inside the phase variant on the UI thread is safe — both
    // sides only ever load/store the atomic flag. Caller has already
    // transitioned us into an active phase by the time launch_stream
    // runs, so active_ctx is non-null here.
    auto cancel = std::make_shared<http::CancelToken>();
    if (auto* a = active_ctx(m.s.phase)) a->cancel = cancel;

    // Snapshot the per-turn retry counter so the worker can stamp it on
    // the wire as x-stainless-retry-count. Reflecting the real attempt
    // number (instead of a hard-coded 0) is what the Anthropic SDK / Zed
    // do; the edge uses it for routing and to avoid penalising retried
    // traffic.
    const int retry_count = [&] {
        if (const auto* a = active_ctx(m.s.phase)) return a->transient_retries;
        return 0;
    }();

    // Capture the snapshot the worker needs. The Thread copy is the
    // one unavoidable cost; everything else is small.
    Thread thread_snapshot = m.d.current;
    std::string session_key = thread_snapshot.id.value;
    const bool compacting  = m.s.compacting;
    const CompactionStyle compaction_style = m.s.compaction_style;
    const int  compaction_ceiling = m.s.compaction_ceiling;
    const int  context_max = m.s.context_max;
    std::string model_id   = m.d.model_id.value;
    // auth_snapshot(), NOT deps().auth: the cached header is overwritten by
    // background OAuth refreshes (token_refreshed → update_auth installs the
    // ANTHROPIC bearer unconditionally), so reading the cache while e.g.
    // Mistral is active ships Anthropic's OAuth token to api.mistral.ai →
    // 401 "Invalid API Key". auth_snapshot resolves from the CURRENTLY-ACTIVE
    // provider through the central credential layer (falling back to the
    // cache only for oauth_native/local, whose transports own their tokens),
    // so the wire credential can never drift from provider::active().
    auth::AuthHeader auth  = auth_snapshot();
    // Reasoning effort, resolved + clamped to this model's capability here on
    // the UI thread (where the live Model is readable). Empty = off; an
    // unsupported tier degrades (Xhigh/Max → high) instead of 400ing.
    // gpt-5.x (ChatGPT/Codex) now decodes through ModelCapabilities (Family::
    // Gpt), so the SAME clamp path applies: gpt-5.6 takes the full low..max
    // ladder, gpt-5.4/5.5 top out at xhigh, and a stale `max` pick on a model
    // that only reaches xhigh degrades instead of being sent verbatim.
    std::string effort = std::string{
        effort_wire_for(m.d.effort, resolved_caps(model_id))};

    // Whether the user wants reasoning shown (global toggle, ^R in the model
    // picker). Captured for the worker; the Anthropic transport uses it to
    // request VISIBLE thinking so the reasoning block has text to render.
    const bool show_reasoning = m.d.show_reasoning;

    // Look up the selected model's supports_tools from available_models.
    // Ollama models have this set via /api/show probe at list time. If
    // the model reports supports_tools=false, we skip advertising tools
    // entirely (Zed-style: the model can only be used for plain chat).
    // std::nullopt = unknown/not probed = fall through to heuristic.
    std::optional<bool> model_supports_tools;
    int model_context_window = 0;
    for (const auto& mi : m.d.available_models) {
        if (mi.id.value == model_id) {
            model_supports_tools = mi.supports_tools;
            model_context_window = mi.context_window;
            break;
        }
    }

    // Compaction runs on the CHEAPEST capable model this provider offers, not
    // the flagship the user is chatting with. Summarising a transcript is a
    // Haiku-class job; sending the whole prefix + the verbose summarisation
    // prompt to Opus/Sonnet on every compaction was the single biggest hidden
    // cost (each compaction billed a full flagship-priced input at ~context-max
    // size, with tools omitted so no cache hit). This mirrors Zed, which routes
    // compaction to a dedicated `compaction_model` (its default_fast_model).
    // `cheapest_capable_model` never routes UP and keeps the parent when
    // nothing cheaper exists, so an Opus-only / single-model user is unaffected.
    // Only used when `compacting` (summarisation is text-only, so the cheaper
    // model's weaker tool-use is irrelevant here). Smart Mode: if the user has
    // pinned a Utility slot, use it; otherwise this is exactly the existing
    // cheapest-capable default (turning Smart Mode off never regresses
    // compaction back up to the flagship).
    std::string compaction_model =
        smart::utility_model(model_id, m.d.available_models, m.d.smart,
                             detail::active_provider_id());

    // Layer 3a (orchestration): when active, the MAIN turn runs on the
    // Strategic role's (model, effort) so the flagship orchestrates and
    // delegates grunt work to subagents. Resolved here on the UI thread
    // (needs the live catalog); a no-op profile == (model_id, effort) when
    // orchestration is off, so the captured values below are always valid.
    const bool orchestrate = m.d.smart.orchestration();
    smart::RoleProfile strategic_profile =
        smart::resolve_role(smart::ModelRole::Strategic, model_id,
                            m.d.effort, m.d.available_models, m.d.smart,
                            detail::active_provider_id());
    // SOTA effort scaling (Anthropic multi-agent: "scale effort to query
    // complexity"). Classify the newest user turn and bump/drop the Strategic
    // model's reasoning effort accordingly, clamped to what the model supports.
    // Conservative: Standard leaves it unchanged, ambiguity biases upward.
    smart::Complexity turn_complexity = smart::Complexity::Standard;
    if (orchestrate) {
        std::string_view newest_user;
        std::size_t nu_attach_bytes = 0;
        int         nu_images = 0;
        for (auto it = m.d.current.messages.rbegin();
             it != m.d.current.messages.rend(); ++it)
            // Skip the zero-text 🧠 routing card (Role::User, smart_routing):
            // submit_message inserts it AFTER the real user message, so
            // without this guard the scan classifies an empty string — wrong
            // effort on the wire and a card that disagrees with it.
            if (it->role == Role::User && !it->is_proactive_context()
                && !it->smart_routing && !it->fork_note) {
                newest_user = it->text;
                for (const auto& a : it->attachments)
                    nu_attach_bytes += a.body.size();
                nu_images = static_cast<int>(it->images.size());
                break;
            }
        // Context-aware: a short follow-up to a Complex turn keeps some of
        // that weight instead of collapsing to Simple. m.s.smart_turn_complexity
        // still holds the PREVIOUS turn's tier here (overwritten below at 776).
        // Payload-aware + continuation-aware + correction-floored — the ONE
        // composed classifier, shared with build_smart_routing_card so the
        // 🧠 card can never disagree with the wire.
        const smart::ComplexityScore turn_cx = smart::classify_turn(
            newest_user, m.s.smart_turn_complexity, nu_attach_bytes, nu_images,
            m.d.smart.complex_threshold);
        turn_complexity = turn_cx.tier;
        const auto caps = resolved_caps(strategic_profile.model);
        // Session cascade bias only. The per-workspace learned prior, the
        // regret denominator it needed, and the plan-recall few-shot all went
        // with the self-supervised layers — see RoleConfig's comment.
        strategic_profile.effort =
            smart::effort_for_score(strategic_profile.effort, turn_cx,
                                    caps, m.s.smart_effort_bias,
                                    m.d.smart.deep_margin);
        // Stash for the cascade feedback at finalize_turn.
        m.s.smart_turn_complexity = turn_complexity;
    }

    // Record what this turn will ACTUALLY be dispatched on, so the assistant
    // header can name its true author instead of the picker selection (which
    // Smart Mode routinely overrides). Mirrors the req.model choice below;
    // orchestrate=false means the plain selection serves the turn, and an
    // empty string tells the view to fall back to it.
    if (!compacting && orchestrate && !strategic_profile.model.empty()) {
        m.s.smart_turn_model = strategic_profile.model;
        m.s.smart_turn_role  = "strategic";
    } else {
        m.s.smart_turn_model.clear();
        m.s.smart_turn_role.clear();
    }

    // Smart-channel telemetry. The `smart` log channel existed but NOTHING
    // wrote to it, so the only routing evidence in a debug file was whatever
    // the wire happened to dump — i.e. the Strategic turn only, with no way
    // to tell it apart from an ordinary one. Every dispatch that Smart Mode
    // influences now names itself here (subagents log their own line in
    // run_one_completion), so `AGENTTY_LOG=smart=debug` yields the complete
    // delegation trace.
    AGT_LOG(Smart, Debug, "route.turn",
            "role={} model={} effort={} complexity={} orchestrate={} "
            "subagents={} compacting={}",
            m.s.smart_turn_role.empty() ? "none" : m.s.smart_turn_role,
            m.s.smart_turn_model.empty() ? model_id : m.s.smart_turn_model,
            effort_label(strategic_profile.effort),
            smart::to_string(turn_complexity),
            orchestrate ? 1 : 0,
            m.d.smart.subagent_routing() ? 1 : 0,
            compacting ? 1 : 0);

    return Cmd<Msg>::task(
        [thread = std::move(thread_snapshot),
         compacting, compaction_style, compaction_ceiling, context_max, retry_count,
         orchestrate, strategic_profile, turn_complexity,
         session_key = std::move(session_key),
         model_id = std::move(model_id),
         compaction_model = std::move(compaction_model),
         effort = std::move(effort),
         show_reasoning,
         model_supports_tools,
         model_context_window,
         auth = std::move(auth),
         cancel]
        (std::function<void(Msg)> dispatch) mutable {
        // Build wire payload off the UI thread.
        provider::Request req;
        req.model         = compacting ? std::move(compaction_model)
                                       : (orchestrate ? strategic_profile.model
                                                      : std::move(model_id));
        // Compaction NEVER needs the 1M/2M extended-context window: it
        // summarises a transcript trimmed to ~65% of the BASE window into a
        // short text reply (no tools). If the parent is on a 1M variant and no
        // cheaper model exists, cheapest_capable_model returns that parent id
        // verbatim — keeping the picker-only `[1m]` marker, which makes the
        // transport send the entitlement-gated context-1m-2025-08-07 beta and
        // 400s ("long context beta is not yet available for this
        // subscription"), aborting the compaction. Strip the marker on the
        // compaction path so it always runs on the plain 200K window (same fix
        // the subagent path uses). The NORMAL turn keeps its marker — the user
        // explicitly chose the 1M window there.
        if (compacting) req.model = wire_model_id(req.model);
        // Per-model output-token ceiling. The default (kSafeMaxTokens=16384)
        // is shared across reasoning + tool JSON for the whole turn; a large
        // `edit` (verbatim old_text + new_text) can overrun it and arrive
        // truncated as "arguments look incomplete". Raise it to the model's
        // real output capacity. See max_output_tokens_for in catalog.hpp.
        req.max_tokens    = max_output_tokens_for(req.model);
        // Model's real context window (probed from Ollama /api/show; 0 for
        // hosted models). The Ollama transport turns this into options.num_ctx
        // so long agent conversations aren't truncated to Ollama's tiny default.
        req.context_window = model_context_window;
        // Prompt policy is shared by every entry point. Hosted Claude, Codex,
        // and OpenAI-compatible models receive the same complete agent/tool/RAG
        // instructions; only constrained local endpoints use a compact profile.
        const auto sel_now = provider::active();
        req.system_prompt = provider::system_prompt_for(sel_now);
        // Layer 3a (orchestration): teach the Strategic model to keep the
        // thinking and DELEGATE mechanical work to subagents — the
        // orchestrator-workers pattern. Only on the main turn (never on the
        // text-only compaction pass).
        if (orchestrate && !compacting) {
            // SOTA orchestrator-workers directive (Anthropic, "How we built our
            // multi-agent research system" + the wider orchestrator-workers
            // literature). Encodes the decision RULES for the documented failure
            // modes, not just aspirations: (1) over-delegation — the #1 failure,
            // spawning a worker for work cheaper to do directly; (2) blind
            // synthesis — relaying an unverified worker claim; (3) treating
            // dependent work as parallel; (4) losing the economic rationale
            // (delegation exists to spend a CHEAP model's context on mechanical
            // breadth, protecting the lead's expensive context for decisions).
            const char* budget =
                turn_complexity == smart::Complexity::Trivial  ? "This turn is trivial — answer directly. Do NOT spawn any subagent; the delegation overhead exceeds the work."
              : turn_complexity == smart::Complexity::Simple   ? "This turn is simple — do it yourself, or use at most ONE explorer if you must read widely first. Spawning workers for a one-step task loses time and money."
              : turn_complexity == smart::Complexity::Complex   ? "This turn is complex — plan the decomposition first, then run SEVERAL workers in parallel for the INDEPENDENT sub-tasks (aim 3+), and synthesise their reports into one verified answer."
              : "Delegate the clearly-separable mechanical sub-tasks; keep every decision and the synthesis yourself.";
            req.system_prompt +=
                "\n\n<smart-mode>\n"
                "You are the STRATEGIC lead of a role-routed team. Your context "
                "is the expensive resource: spend it on the high-value thinking "
                "— architecture, decisions, decomposition, review, synthesis — and "
                "push bounded, well-specified MECHANICAL work (broad reading, "
                "repro runs, mechanical edits) to cheaper workers via the `task` "
                "tool so their context, not yours, absorbs the bulk.\n"
                "DELEGATE ONLY WHEN IT PAYS. A `task` costs a full worker "
                "round-trip; if you could finish the step yourself in one or two "
                "tool calls, just do it. Delegate breadth (many files, parallel "
                "threads) and depth you'd rather not spend your own context on — "
                "never a single quick lookup.\n"
                "BRIEF EACH WORKER COMPLETELY: the objective, the exact output "
                "format you need back, which tools/paths are in scope, and clear "
                "boundaries (what NOT to touch). Pass a crisp brief — objective + "
                "constraints + relevant file:line refs — NOT your reasoning "
                "transcript. A vague brief makes workers duplicate effort or miss "
                "the point; that wasted work is on you.\n"
                "SEQUENCE vs PARALLEL: run genuinely INDEPENDENT work in parallel "
                "(launch the tasks together). When one step needs another's "
                "output (explore → then edit what you found), sequence them — "
                "don't guess at inputs you haven't gathered.\n"
                "OWN THE ANSWER: a worker's report is evidence, not truth. Verify "
                "claims that matter (spot-check a cited file:line, re-run a test) "
                "before you build on them. The final answer is YOURS — never "
                "relay a worker report verbatim as your conclusion.\n"
                "Routing: reading / searching / mapping → "
                "task(agent_type:\"explorer\"); edits + build → "
                "task(agent_type:\"coder\"); repro / diagnose → "
                "task(agent_type:\"tester\"); critical review → "
                "task(agent_type:\"reviewer\"). Start wide, then narrow.\n"
                + std::string{budget} +
                "\n</smart-mode>";
        }
        const bool openai_provider = sel_now.kind == provider::Kind::OpenAI;
        // Weak models (small local / coder ids) still hide a few footgun
        // tools below; the prompt no longer branches on it.
        const bool weak_model = is_weak_model(req.model);
        // First-class weak-model support (agent-zero style): on the Ollama
        // native endpoint, weak models get the JSON-protocol path — no native
        // `tools` array, the tool catalog is inlined in the prompt and the
        // model answers with one {tool_name,tool_args} object. Tiny models
        // follow "emit one JSON object" far more reliably than the native
        // function-call channel. Capable/large local models keep the native
        // structured channel.
        req.json_protocol =
            weak_model && openai_provider && sel_now.openai_endpoint.native_api;
        // The single biggest per-turn behaviour fork: a weak model on a
        // native endpoint abandons the structured tool channel entirely and
        // gets the inline-JSON protocol. When someone asks "why does model X
        // call tools fine but model Y doesn't", this line is the answer.
        if (weak_model)
            AGT_LOG(Model, Debug, "dispatch.weak_model",
                    "model={} json_protocol={}", req.model,
                    req.json_protocol ? 1 : 0);
        req.cancel        = cancel;
        req.auth          = std::move(auth);
        req.retry_count   = retry_count;
        // Reasoning effort (already clamped above). Empty = no thinking; the
        // Anthropic transport turns a non-empty value into adaptive thinking.
        // Compaction never needs reasoning — summarising is a mechanical text
        // task — and the cheap compaction model may not even support the
        // parent's effort tier, so force it OFF (no wasted thinking tokens).
        req.effort        = compacting ? std::string{}
                          : (orchestrate ? std::string{effort_wire(strategic_profile.effort)}
                                         : std::move(effort));
        // Reasoning display: never for compaction (a mechanical summarise);
        // otherwise carry the user's global "show reasoning" preference so the
        // Anthropic transport can request VISIBLE thinking.
        req.show_reasoning = !compacting && show_reasoning;
        req.session_key   = std::move(session_key);

        // Ollama capability gate: if /api/show reported the model does NOT
        // support tools (supports_tools == false), skip advertising ANY
        // tools — the model can only be used for plain chat. This matches
        // Zed's behavior: models without the "tools" capability don't get
        // tools on the wire, so they can't leak phantom calls or loop.
        // std::nullopt means unknown / not probed (non-Ollama endpoints or
        // probe failure): fall through to the normal heuristic.
        const bool tools_disabled_by_capability =
            model_supports_tools.has_value() && !model_supports_tools.value();

        // Wire payload diverges on compaction kickoff:
        //   normal turn  → wire_messages_for(thread) substitutes any
        //                  prior compaction summary in place of its
        //                  covered prefix.
        //   compaction   → wire_messages_for_compaction(thread, ctx_max)
        //                  appends the synthetic summarisation prompt and
        //                  trims the raw prefix to fit context_max. Tools
        //                  are omitted from the wire so the model can only
        //                  reply with text — a tool_use during compaction
        //                  would have nowhere to land (we don't surface
        //                  the synthetic turn in the UI).
        // Proactive context is useful for the turn it grounds, not a permanent
        // user message to replay on every later request. Keep only context
        // attached after the newest real user message.
        auto drop_stale_proactive = [](std::vector<Message>& messages) {
            // Also strip Smart Mode routing cards — they are view-only
            // telemetry (no wire content), never sent to the model.
            messages.erase(
                std::remove_if(messages.begin(), messages.end(),
                    [](const Message& mm){ return mm.smart_routing; }),
                messages.end());
            std::size_t newest_user = 0;
            bool have_user = false;
            for (std::size_t i = 0; i < messages.size(); ++i)
                if (messages[i].role == Role::User && !messages[i].is_proactive_context()
                    && !messages[i].fork_note) {
                    newest_user = i;
                    have_user = true;
                }
            if (!have_user) return;
            for (std::size_t i = 0; i < newest_user;) {
                if (messages[i].is_proactive_context()) {
                    messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(i));
                    --newest_user;
                } else {
                    ++i;
                }
            }
        };

        if (compacting) {
            // We stripped the `[1m]` marker from req.model above, so the wire
            // window is the model's BASE (200K), not the 1M the parent picked.
            // Trim the compaction payload to that base window — trimming to 65%
            // of 1M (650K) would then overflow the 200K request and fail.
            int compaction_ctx = context_max;
            if (int base = ui::context_max_for_model(req.model);
                base > 0 && (compaction_ctx <= 0 || base < compaction_ctx))
                compaction_ctx = base;
            req.messages = wire_messages_for_compaction(thread, compaction_ctx, compaction_style, compaction_ceiling);
            drop_stale_proactive(req.messages);
            // req.tools left empty — summarisation is text-only.
        } else {
            // Worker owns `thread` — move it into the builder so the common
            // no-compaction path relocates the messages vector instead of
            // deep-copying it a second time (see wire_messages_for_impl&&).
            req.messages = wire_messages_for_impl(std::move(thread));
            drop_stale_proactive(req.messages);
            // Adaptive soft-trim: if the wire view is still over the
            // window (no compactions yet, or the latest one is stale
            // and the user has run many turns since), drop oldest raw
            // turns from the wire until it fits ~95% of context_max.
            if (context_max > 0) {
                const int soft_ceiling = static_cast<int>(
                    static_cast<double>(context_max) * 0.95);
                soft_trim_to_ceiling(req.messages, soft_ceiling);
            }
            // All tools, every profile. Gating is the policy layer's job
            // (`tool::DynamicDispatch::needs_permission`, called from
            // `kick_pending_tools`). EXCEPTION 1: if Ollama's /api/show
            // reported supports_tools=false, skip ALL tools — the model
            // can only be used for plain chat. EXCEPTION 2: weak local
            // models hallucinate `skill` and the memory tools on greetings
            // /small talk — don't put those on the wire for weak models.
            // (Memory tools still run via /slash command; skills /skill-name.)
            if (!tools_disabled_by_capability) {
                auto weak_hidden = [](std::string_view n) {
                    return n == "skill" || n == "remember"
                        || n == "forget" || n == "wipe_memory";
                };
                std::string_view newest_user;
                for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
                    if (it->role == Role::User && !it->is_proactive_context() && !it->fork_note) {
                        newest_user = it->text;
                        break;
                    }
                }
                for (const auto* tool : tools::select_wire_tools(newest_user)) {
                    const auto& t = *tool;
                    if (weak_model && weak_hidden(t.name.value)) continue;
                    req.tools.push_back({t.name.value, t.description, t.input_schema,
                                         t.eager_input_streaming});
                }
            }
        }

        // Suppress every event after the token is tripped — including the
        // worker's own terminal StreamError("cancelled"). The UI thread
        // already did the cancel-side cleanup synchronously in the
        // CancelStream handler (phase=Idle, status="cancelled", tool
        // calls marked Cancelled, empty placeholder dropped), so a
        // trailing event here would either be a no-op against an Idle
        // phase or — worse — a write into a brand-new turn's state.
        // The check matches the worker's own cancellation gate, so once
        // the token is tripped no further events flow into the reducer
        // from this worker.
        auto guarded = [dispatch, cancel](Msg m) {
            if (cancel && cancel->is_cancelled()) return;
            dispatch(std::move(m));
        };
        try {
            // Pass `guarded` straight through as the EventSink instead of
            // wrapping it in a second lambda that just forwards to it — the
            // wrapper added one extra std::function indirection on every
            // delta (the wire's hottest path) for no behavioural gain.
            deps().stream(std::move(req), guarded);
        } catch (const std::exception& e) {
            // The stream backend threw before producing a terminal event —
            // surface it as StreamError so the UI doesn't hang on the spinner.
            guarded(StreamError{std::string{"stream backend: "} + e.what()});
        } catch (...) {
            guarded(StreamError{"stream backend: unknown exception"});
        }
    });
}

Cmd<Msg> run_tool(ToolCallId id, ToolName tool_name, nlohmann::json args,
                  http::CancelTokenPtr cancel) {
    // task_isolated, NOT task: a tool that wedges (e.g. read on a hung NFS
    // mount, bash on a process that won't unblock) must not consume a slot
    // in the shared BG worker pool. With Cmd::task the pool's max workers
    // — std::max(4, hw_concurrency) — get permanently filled by zombie
    // threads stuck in syscalls, and subsequent tools queue forever even
    // though the UI watchdog has long since flipped them to Failed. This
    // surfaced as "tools randomly get stuck" once enough wedged calls
    // accumulated. Per-call detached thread costs ~100-300 µs of
    // construction; tools run seconds apart so it's noise.
    return Cmd<Msg>::task_isolated(
        [id = std::move(id),
         name = std::move(tool_name),
         args = std::move(args),
         cancel = std::move(cancel)]
        (std::function<void(Msg)> dispatch) {
            // Install a thread-local progress sink *before* dispatch so the
            // subprocess runner inside the tool can stream stdout+stderr to
            // the UI as bytes arrive. RAII scope guarantees the sink is
            // cleared even if the tool throws, so the next tool run can't
            // inherit a stale dispatch lambda.
            agentty::tools::progress::Scope progress_scope{
                [dispatch, id](std::string_view snapshot) {
                    dispatch(ToolExecProgress{id, std::string{snapshot}});
                }};
            agentty::tools::cancellation::Scope cancellation_scope{
                [cancel] { return cancel && cancel->is_cancelled(); }};
            try {
                // ── pre_tool hooks (consent-gated, see hooks.hpp) ─────
                // A blocking decision becomes the tool's error result: the
                // model sees the hook's stdout and can adapt. Runs on this
                // tool's own isolated thread, so a slow hook never wedges
                // the UI — only this one call.
                const std::string args_dump = args.dump();
                if (auto pre = tools::hooks::run_pre_tool(name.value,
                                                          args_dump);
                    pre.blocked) {
                    dispatch(ToolExecOutput{id, std::unexpected(
                        tools::ToolError::unknown(
                            "blocked by pre_tool hook: " + pre.reason))});
                    return;
                }
                const auto t_start = std::chrono::steady_clock::now();
                auto result = tool::DynamicDispatch::execute(name.value, args);
                const auto t_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_start).count();
                // Every tool call, with its outcome and duration. A FAILING
                // tool logs at Warn with the error kind + message, so the
                // "[invalid args] pattern required" class of bug is visible
                // in the log with the arguments that produced it — the exact
                // evidence that was missing when Copilot's tool calls were
                // arriving empty.
                AGT_LOGL(Tool, result ? ::agentty::logx::Level::Debug
                                      : ::agentty::logx::Level::Warn,
                         "tool.exec", "name={} ms={} ok={} err={} args={}",
                         name.value, t_ms, result ? 1 : 0,
                         result ? std::string{"-"}
                                : std::string{tools::to_string(result.error().kind)}
                                      + ": " + result.error().detail,
                         args_dump);
                if (result) {
                    // post_tool hooks: fire-and-forget (never block, never
                    // rewrite the result).
                    tools::hooks::run_post_tool(name.value, args_dump,
                                                result->text);
                    // Carry the structured FileChange(s) — single-file (edit/
                    // write/apply_patch) via change, multi-file (replace) via
                    // changes — into the reducer for diff-review.
                    dispatch(ToolExecOutput{id, std::move(result->text),
                                            std::move(result->change),
                                            std::move(result->changes),
                                            std::move(result->images)});
                } else {
                    dispatch(ToolExecOutput{id,
                        std::unexpected(std::move(result).error())});
                }
            } catch (const std::exception& e) {
                // DynamicDispatch already catches tool exceptions, but guard
                // against anything in the dispatch infrastructure itself so
                // the tool never gets stuck in Running with no terminal Msg.
                AGT_LOG(Tool, Error, "tool.dispatch_throw", "name={} err={}",
                        name.value, e.what());
                dispatch(ToolExecOutput{id, std::unexpected(
                    tools::ToolError::unknown(
                        std::string{"dispatch error: "} + e.what()))});
            } catch (...) {
                dispatch(ToolExecOutput{id, std::unexpected(
                    tools::ToolError::unknown("dispatch error: unknown exception"))});
            }
        });
}

namespace {

// ── Path-aware parallel scheduling ───────────────────────────────────────────
//
// The effect-only gate (is_parallel_safe) is coarse: ANY writer forces
// exclusive access, so two edits to DIFFERENT files serialise, and a read of
// a.c blocks behind an unrelated write to b.c. That's correct but leaves
// latency on the table — the model routinely emits a fan of edits across
// disjoint files in one turn.
//
// We refine it the same way mcp-cpp's cap::scheduler does: track the SET OF
// PATHS each running/promoted tool touches, and let a writer (or reader)
// proceed concurrently when its paths don't OVERLAP any active path. A tool
// whose target path can't be extracted, or any Exec (bash — unbounded blast
// radius), keeps the conservative exclusive behaviour.

// Pull the fs target(s) out of a tool call's args, mirroring the built-in
// tool vocabulary (read/write/edit/find_definition use path|file_path;
// grep/glob/list_dir scope under an optional dir). Paths are resolved against
// the workspace and weakly canonicalised so lexical aliases (./, .., and
// absolute-vs-workspace-relative spellings) compare identically. nullopt means
// a path was present but could not be normalised; callers must fail closed.
[[nodiscard]] std::optional<std::vector<std::string>> tc_paths(const ToolUse& tc) {
    std::vector<std::string> out;
    if (!tc.args.is_object()) return out;
    bool uncertain = false;
    auto take = [&](const char* k) {
        if (auto it = tc.args.find(k); it != tc.args.end()) {
            if (!it->is_string()) { uncertain = true; return; }
            auto raw = it->get<std::string>();
            if (raw.empty()) { uncertain = true; return; }
            try {
                namespace fs = std::filesystem;
                fs::path p{raw};
                if (p.is_relative()) {
                    const auto& root = tools::util::workspace_root();
                    if (root.empty()) { uncertain = true; return; }
                    p = root / p;
                }
                std::error_code ec;
                p = fs::weakly_canonical(p, ec);
                if (ec || p.empty() || !p.is_absolute()) {
                    uncertain = true;
                    return;
                }
                auto normal = p.generic_string();
#ifdef _WIN32
                // NTFS and the Win32 API are case-insensitive by default;
                // compare scheduler resource identities the same way so
                // File.cpp and FILE.CPP cannot race a writer.
                std::ranges::transform(normal, normal.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
#endif
                out.push_back(std::move(normal));
            } catch (...) {
                uncertain = true;
            }
        }
    };
    take("path"); take("file_path"); take("filepath"); take("filename");
    if (tc.name.value == "move") { take("source"); take("destination"); }
    const auto& n = tc.name.value;
    if (n == "grep" || n == "glob" || n == "list_dir") {
        take("dir"); take("directory"); take("root");
    }
    if (uncertain) return std::nullopt;
    return out;
}

// Prefix-aware path overlap: identical, or one is a directory ancestor of the
// other (with a separator at the boundary so "src" doesn't match "srcfoo").
[[nodiscard]] bool paths_overlap(std::string_view a, std::string_view b) {
    if (a == b) return true;
    auto under = [](std::string_view shorter, std::string_view longer) {
        return longer.size() > shorter.size()
            && longer.substr(0, shorter.size()) == shorter
            && (shorter.back() == '/' || longer[shorter.size()] == '/');
    };
    return a.size() < b.size() ? under(a, b) : under(b, a);
}

} // namespace

SchedDecision schedule_parallel_batch(const std::vector<ToolUse>& batch) {
    // Scheduling metadata is carried by every ToolDef, including dynamic MCP
    // tools. Resolve once against the generation-stable catalog; unknown tools
    // still fail closed as Exec.
    auto effects_of = [](const ToolName& n) -> tools::EffectSet {
        if (const auto* def = tools::find(n.value))
            return def->scheduling_effects;
        return tools::EffectSet{tools::Effect::Exec};
    };
    tools::EffectSet active_effects;
    std::vector<std::string> active_paths;
    bool active_unbounded = false;
    auto note = [&](const ToolUse& tc, tools::EffectSet fx) {
        active_effects |= fx;
        auto ps = tc_paths(tc);
        if (fx.has(tools::Effect::Exec)
            || (fx.has(tools::Effect::WriteFs) && (!ps || ps->empty())))
            active_unbounded = true;
        if (ps) for (auto& p : *ps) active_paths.push_back(std::move(p));
    };
    auto can_run = [&](const ToolUse& tc, tools::EffectSet want) -> bool {
        if (tools::is_parallel_safe(active_effects, want)) return true;
        if (want.has(tools::Effect::Exec) || active_unbounded) return false;
        auto ps = tc_paths(tc);
        // Once a writer is involved, an absent or unnormalisable resource set
        // has unknown blast radius and must not be parallelised.
        if (!ps || ps->empty()) return false;
        for (const auto& cp : *ps)
            for (const auto& ap : active_paths)
                if (paths_overlap(cp, ap)) return false;
        return true;
    };
    // Seed with whatever's already running.
    for (const auto& tc : batch)
        if (tc.is_running()) note(tc, effects_of(tc.name));
    // Greedily promote pending/approved calls in submission order.
    SchedDecision out;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        const auto& tc = batch[i];
        if (!tc.is_pending() && !tc.is_approved()) continue;
        const auto want = effects_of(tc.name);
        if (!can_run(tc, want)) continue;   // stays pending; advances next kick
        note(tc, want);
        out.promote.push_back(i);
    }
    return out;
}

// ── Doom-loop circuit breaker ────────────────────────────────────────────────
// Caps a non-converging weak-model tool loop. See the header doc on
// agent_loop_should_break for the rationale. Walks BACK from the end of the
// transcript over the current agent run (everything after the last real User
// message that isn't a synthetic TOOL-RESULT carrier) and applies two caps.
std::optional<LoopBreak> agent_loop_should_break(
        const std::vector<Message>& messages,
        bool enforce_step_cap) {
    // Tunables. Generous enough that a legitimate multi-step task (search →
    // read → edit → verify …) never trips, tight enough that a stuck model
    // bails in seconds rather than spinning until the user hits Esc.
    constexpr int kRepeatLimit  = 3;   // same failing call N times → stop
    constexpr int kMaxToolTurns = 25;  // tool-call turns w/o a text answer

    // Find the start of the current run: the last User message. (Sub-turn
    // continuations push Assistant placeholders; tool results live inside the
    // Assistant messages' tool_calls, so a User message only appears at a real
    // human turn boundary.)
    std::size_t run_start = 0;
    for (std::size_t i = messages.size(); i-- > 0;) {
        // A proactively-retrieved context message is a synthetic User turn,
        // NOT a human turn boundary — skip it so the run-start lands on the
        // real user message it was injected after.
        if (messages[i].role == Role::User && !messages[i].is_proactive_context()
            && !messages[i].smart_routing && !messages[i].fork_note) {
            run_start = i + 1; break;
        }
    }

    int tool_turns = 0;
    // (name + canonical args) → consecutive terminal failure/rejection
    // streak. A success resets that call's streak; historical failures before
    // demonstrated recovery must not trip the breaker later.
    std::unordered_map<std::string, int> failure_streaks;
    for (std::size_t i = run_start; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (msg.role != Role::Assistant || msg.tool_calls.empty()) continue;
        ++tool_turns;
        for (const auto& tc : msg.tool_calls) {
            // Only settled calls inform the breaker; in-flight ones are the
            // batch we're about to (re)kick.
            if (!tc.is_terminal()) continue;
            // Cached args_dump() avoids re-serializing every prior settled
            // call's args on each kick_pending_tools (this walks the whole run
            // per tool sub-turn — raw args.dump() made it O(N²) over the run).
            std::string key = tc.name.value + '\0'
                + (tc.args.is_null() ? std::string{} : tc.args_dump());
            auto& streak = failure_streaks[key];
            if (tc.is_failed() || tc.is_rejected()) ++streak;
            else streak = 0;
        }
    }

    for (const auto& [key, streak] : failure_streaks) {
        if (streak >= kRepeatLimit) {
            auto nul = key.find('\0');
            std::string name = nul == std::string::npos ? key : key.substr(0, nul);
            return LoopBreak{
                "Stopped: the `" + name + "` tool failed or was rejected "
                + std::to_string(streak) + " consecutive times with the same "
                "arguments. Re-trying the identical call won't help — "
                "change the arguments (check the path/target exists, or pick a "
                "different tool), or answer the user directly with what you "
                "already know."};
        }
    }
    // RUNAWAY step cap. Only enforced for weak local models (caller passes
    // enforce_step_cap). Capable models (Claude, hosted GPT) are NEVER step-
    // capped — matches Claude Code (max_turns unlimited by default) and aider
    // (no step cap). A long, legitimate search→read→edit→verify run must not be
    // cut off at an arbitrary count.
    if (enforce_step_cap && tool_turns >= kMaxToolTurns) {
        return LoopBreak{
            "Stopped after " + std::to_string(tool_turns) + " tool steps "
            "without finishing. Summarise what you found and answer the user "
            "directly, or ask them how to proceed."};
    }
    return std::nullopt;
}

Cmd<Msg> kick_pending_tools(Model& m) {
    if (m.d.current.messages.empty()) return Cmd<Msg>::none();
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return Cmd<Msg>::none();

    // Bail early if the session is already Idle. This is the
    // late-arrival window: a tool worker thread can return a
    // ToolExecOutput AFTER the user has cancelled (Esc → phase=Idle)
    // or after StreamError dropped to Idle. In those cases there's
    // no in-flight request to attach a sub-turn to, no ctx to take,
    // and the result has nowhere to go. The phase transitions below are
    // TOTAL (reschedule_streaming / optional-checked seats), so a slip
    // past this guard degrades to a no-op rather than aborting — but the
    // early return keeps the intent obvious and skips dead work.
    //
    // Tools that were Pending/Approved at cancel-time are already
    // marked Failed/Rejected by CancelStream's teardown loop, so
    // there's nothing left to advance anyway.
    if (m.s.is_idle()) return Cmd<Msg>::none();

    // Drop re-leaked salvaged calls (weak local models re-emitting a tool
    // call they already ran this turn) BEFORE any promotion to Running, so a
    // duplicate never executes a second side effect. See
    // dedup_releaked_salvage_calls.
    dedup_releaked_salvage_calls(m);

    std::vector<Cmd<Msg>> cmds;
    bool any_pending = false;

    // Effect- and path-aware scheduler. When the assistant emits multiple
    // tool calls in one turn they all start Pending; maya's BG worker pool
    // runs Task cmds on independent threads, so any set of tools we dispatch
    // in this tick runs concurrently. The scheduling decision — which
    // pending/approved calls may join the in-flight wave — is computed by
    // the PURE planner `schedule_parallel_batch` (defined just above, unit-
    // tested in scheduler_path_test). Keeping the live gate as a thin
    // consumer of that planner means the test exercises the real rule, not a
    // parallel reimplementation that could silently drift.
    //
    // Deferred (conflicted) tools stay Pending. When the blocking tool's
    // ToolExecOutput lands, update.cpp re-fires kick_pending_tools and the
    // now-unblocked siblings advance.
    const auto plan = schedule_parallel_batch(last.tool_calls);
    std::vector<bool> promote(last.tool_calls.size(), false);
    for (auto i : plan.promote) promote[i] = true;

    // Permission preflight is deliberately a separate, non-dispatching phase.
    // If a later sibling needs approval, return before marking any earlier
    // sibling Running: returning a permission prompt discards this function's
    // accumulated Cmd, so mutating first would strand that tool forever.
    if (!m.d.pending_permission) {
        for (std::size_t i = 0; i < last.tool_calls.size(); ++i) {
            const auto& tc = last.tool_calls[i];
            // Only a call selected for this wave may prompt. Conflicting calls
            // stay pending until the active wave completes; prompting them
            // early would reorder permission UX ahead of runnable work.
            if (!promote[i] || !tc.is_pending()) continue;
            if (m.d.session_grants.contains(tc.name.value)
                || !tool::DynamicDispatch::needs_permission(tc.name.value, m.d.profile))
                continue;
            m.d.pending_permission = PendingPermission{
                tc.id, tc.name,
                "Tool " + tc.name.value + " needs permission under "
                    + std::string{ui::profile_label(m.d.profile)} + " profile"};
            if (auto ctx = take_active_ctx(std::move(m.s.phase)); ctx)
                m.s.phase = phase::AwaitingPermission{std::move(*ctx)};
            else
                m.s.phase = phase::Idle{};   // late arrival: stay Idle
            return Cmd<Msg>::none();
        }
    }

    for (std::size_t i = 0; i < last.tool_calls.size(); ++i) {
        auto& tc = last.tool_calls[i];
        // Approved: user already granted permission; advance it exactly
        // like a Pending-but-no-permission-needed tool, minus the
        // permission check. Keeps the planner as the single source of
        // truth for scheduling.
        const bool ready = tc.is_pending() || tc.is_approved();
        if (ready) {
            const bool needs_perm = tc.is_approved()
                ? false
                : (!m.d.session_grants.contains(tc.name.value)
                   && tool::DynamicDispatch::needs_permission(tc.name.value, m.d.profile));
            if (needs_perm) {
                // A permission request is already displayed for this call (or
                // another sibling). Keep it Pending and do not dispatch it.
                any_pending = true;
                continue;
            }
            {
                // Effect/path-compatibility gate: the planner decided this
                // tool conflicts with the in-flight wave (or an earlier
                // sibling promoted this same tick). Leave it Pending;
                // kick_pending_tools re-fires on every terminal
                // ToolExecOutput, so deferred siblings advance
                // automatically without explicit requeueing.
                if (!promote[i]) {
                    any_pending = true;
                    continue;
                }

                // started_at was stamped at StreamToolUseStart so the
                // timer covers the full card lifetime (args streaming +
                // execution). Preserve it as we move into Running.
                tc.status = ToolUse::Running{tc.started_at(), {}};
                auto cancel = active_ctx(m.s.phase)
                    ? active_ctx(m.s.phase)->cancel
                    : http::CancelTokenPtr{};
                cmds.push_back(run_tool(tc.id, tc.name, tc.args, std::move(cancel)));

                // Tool wall-clock watchdog removed at user request.
                // Tools now run for as long as their worker takes;
                // user cancels via Esc.  bash / diagnostics still
                // self-timeout via subprocess.cpp.  Tools that wedge
                // in a blocking syscall (slow NFS, dead FUSE mount)
                // will leave the card in Running until cancellation —
                // accept this trade-off explicitly.

                // {Streaming, AwaitingPermission, ExecutingTool} →
                // ExecutingTool. Source is whatever phase produced
                // the now-running tool: Streaming for a tool the
                // model just emitted, AwaitingPermission for one the
                // user just granted, ExecutingTool for a sibling
                // promotion within the same batch. Active ctx flows
                // through unchanged. ExecutingTool is non-Idle so
                // active() flips on automatically: the Tick subscrip-
                // tion stays armed, the spinner advances, the view's
                // live elapsed timer keeps ticking — without that
                // the UI looks frozen on long-running bash commands.
                if (auto ctx = take_active_ctx(std::move(m.s.phase)); ctx)
                    m.s.phase = phase::ExecutingTool{std::move(*ctx)};
                else
                    m.s.phase = phase::Idle{};   // total: never aborts
                any_pending = true;
            }
        } else if (tc.is_running()) {
            any_pending = true;
            // A SPECULATIVE read-only tool (launched mid-stream at
            // StreamToolUseEnd) is still running now that the stream has
            // finished. Adopt it: without this transition the phase stays
            // Streaming after StreamFinished, and the ToolExecOutput
            // guard (which suppresses kicks while streaming) would block
            // the completion kick forever — a wedge. Same ctx handoff as
            // the promotion branch above.
            if (m.s.is_streaming()) {
                if (auto ctx = take_active_ctx(std::move(m.s.phase)); ctx)
                    m.s.phase = phase::ExecutingTool{std::move(*ctx)};
                else
                    m.s.phase = phase::Idle{};   // total: never aborts
            }
        }
    }

    if (!any_pending) {
        const bool has_results = std::ranges::any_of(last.tool_calls, [](const auto& tc){
            return tc.is_terminal();
        });
        if (has_results) {
            // ── Doom-loop circuit breaker ────────────────────────────────
            // Before spending another model completion, check whether this
            // agent run has stopped converging. Two independent caps, each
            // matching what production agent tools ship:
            //
            //   • REPEAT-FAILURE cap — the same call failed 3× in a row. This
            //     is the UNIVERSAL pattern (aider's max_reflections=3,
            //     MindStudio's "2–3 attempts then stop"). Applied to EVERY
            //     model, Claude included: a capable model genuinely stuck on a
            //     dead call (bad path, wrong tool) is helped by it too.
            //
            //   • RUNAWAY step cap (25 tool turns) — NOT something production
            //     tools impose on a capable model: Claude Code's max_turns is
            //     UNLIMITED by default, aider never step-caps. So it's enforced
            //     ONLY for weak local models (qwen2.5-coder/codellama on the
            //     Ollama native path) that doom-loop without a completion
            //     signal. Claude (Kind::Anthropic) and capable hosted models
            //     run as long as the task legitimately needs.
            //
            // If a cap trips, DON'T re-stream: surface the nudge as the run's
            // final assistant turn and drop to Idle so the loop ends in seconds
            // instead of spinning until the user hits Esc. The nudge also lands
            // in history, so a follow-up shows the model why it stopped.
            const bool weak_step_cap =
                provider::active().kind == provider::Kind::OpenAI
                && is_weak_model(m.d.model_id.value);
            if (auto brk = agent_loop_should_break(m.d.current.messages,
                                                   /*enforce_step_cap=*/weak_step_cap)) {
                m.s.phase = phase::Idle{};
                Message note;
                note.role = Role::Assistant;
                note.text = std::move(brk->reason);
                m.d.current.messages.push_back(std::move(note));
                deps().save_thread(m.d.current);
                return Cmd<Msg>::batch(std::move(cmds));
            }

            // when the prefix estimate crossed 90% of context_max,
            // surfacing as a user-visible "context limit reached —
            // compacting before continuing…" toast that yanked the
            // agent out of a multi-tool burst. That broke workflow
            // hard: the user's agent loop would just *stop* mid-task
            // while the model rewrote a summary.
            //
            // Now: launch_stream's soft-trim on the normal-turn path
            // drops oldest raw turns from the wire view until it fits
            // ~95% of context_max, transparently. The agent keeps
            // running. Compaction becomes a strictly-better optimisation
            // (summary > truncation) the user can invoke at their
            // leisure via /compact, or the auto-trigger picks up on
            // the NEXT user submit. Workflow uninterrupted.
            //
            // The trim is wire-only: the transcript stays whole, the
            // dropped turns reappear on subsequent requests if context
            // frees up (e.g. after a manual compact).

            // ExecutingTool → Streaming (post-tool sub-turn). Active
            // ctx flows through: cancel token's still alive (the
            // request is still open, sub-turn streams over the same
            // SSE), retry counters preserved.
            //
            // We push a fresh Assistant placeholder so the wire-layer
            // tool_use ↔ tool_result pairing stays correct (the
            // transport synthesises one User-with-tool_results turn
            // per Assistant Message; collapsing sub-turns into the
            // same Message would interleave new tool_use blocks
            // before their tool_results). The VIEW collapses these
            // sub-turn Messages back into one visual Turn via the
            // shared `ui::turn_run_end` helper in build_live_tail
            // (and freeze_range), matching agent_session's
            // "one agent turn = one Turn" shape.
            //
            // No freeze fires here: mid-stream freezing would split
            // an in-flight assistant run across the frozen / live
            // boundary, producing two visual Turns where the user
            // should see one. The single freeze site is in
            // `finalize_turn` once `phase::Idle` is reached.
            // Re-arm the stall watchdog across the tool boundary. No SSE
            // events flow during ExecutingTool, so last_event_at is as
            // old as the last delta before the tool ran — minutes, for a
            // long `bash`/`gh run watch`. Carrying it into Streaming lets
            // a Tick land before the new sub-turn's StreamStarted resets
            // it, firing a spurious "stream stalled — no events for Ns".
            // The sub-turn is a fresh wire phase; start its clock now.
            if (!reschedule_streaming(m.s.phase, [](phase::Active& c) {
                    c.last_event_at = std::chrono::steady_clock::now();
                    c.retry         = retry::Fresh{};
                }))
                return Cmd<Msg>::batch(std::move(cmds));   // late arrival
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            cmds.push_back(launch_stream(m));
        } else {
            // ExecutingTool → Idle (no continuation). Active ctx is
            // discarded — the request is finished, the cancel token
            // can drop, the retry counters reset on the next user
            // turn. Idle drops active() to false, stopping the Tick
            // subscription.
            m.s.phase = phase::Idle{};
        }
    }
    return Cmd<Msg>::batch(std::move(cmds));
}

// ── Self-update ────────────────────────────────────────────────

Cmd<Msg> check_for_update() {
    return Cmd<Msg>::task([](std::function<void(Msg)> dispatch) {
        // 24h-cached; the fast path is one small file read. Errors are
        // swallowed — an update NOTICE must never surface as a failure.
        try {
            auto c = update::check_latest(/*force=*/false);
            if (c.error.empty() && c.update_available) {
                dispatch(Msg{UpdateCheckDone{true, std::move(c.latest),
                                             std::move(c.url)}});
                return;
            }
        } catch (const std::exception& e) {
            util::dbglog("update.check", e.what());
        } catch (...) {
            util::dbglog("update.check", "non-std exception");
        }
        dispatch(Msg{UpdateCheckDone{}});
    });
}

Cmd<Msg> refresh_modelsdev() {
    return Cmd<Msg>::task_isolated([](std::function<void(Msg)>) {
        // Fire-and-forget: registries are consulted at render/wire time, so
        // there is no Msg to dispatch — the moment the fetch lands, the next
        // frame's ladder/badges and the next request's clamp see it. Errors
        // are swallowed; a metadata refresh must never surface as a failure.
        try { (void)agentty::modelsdev::refresh(); }
        catch (const std::exception& e) { util::dbglog("modelsdev.refresh", e.what()); }
        catch (...) { util::dbglog("modelsdev.refresh", "non-std exception"); }
    });
}

Cmd<Msg> perform_self_update(std::string version) {
    return Cmd<Msg>::task([version = std::move(version)](
                              std::function<void(Msg)> dispatch) {
        try {
            auto err = update::perform_update(version);
            if (err.empty())
                dispatch(Msg{UpdateApplied{true, version}});
            else
                dispatch(Msg{UpdateApplied{false, std::move(err)}});
        } catch (const std::exception& e) {
            dispatch(Msg{UpdateApplied{false, e.what()}});
        } catch (...) {
            dispatch(Msg{UpdateApplied{false, "unknown error"}});
        }
    });
}

Cmd<Msg> fetch_models() {
    return Cmd<Msg>::task([](std::function<void(Msg)> dispatch) {
        // Stamp the fetch with the provider it is FOR. The reducer compares
        // this against the provider active at DELIVERY time and drops a
        // stale payload — two quick provider switches otherwise interleave
        // (A's slow fetch lands after B's) and install the wrong catalog.
        const std::string for_provider = detail::active_provider_id();
        try {
            // ONE model-list router (provider/selection.cpp), dispatched on the
            // same axes as the stream path. No is_chatgpt/OpenAI/Anthropic
            // ladder here — a new provider inherits its catalog from its row.
            // auth_snapshot(), not deps().auth: this runs on a WORKER thread
            // and a bare read races the UI thread's auth swap mid-switch.
            auto models = provider::list_models_for(provider::active(),
                                                    auth_snapshot());
            // EMPTY catalog on a LOCAL host = the server didn't answer (down,
            // wrong port, wrong path) — list_models returns {} on any HTTP
            // failure rather than throwing. Attach the reason so the reducer
            // toasts it IMMEDIATELY on connect, instead of the picker sitting
            // in a silent "no models" that the user discovers later. This is
            // the probe-on-connect: the switch always fetches, so a broken
            // host is called out within seconds of Enter.
            std::string err;
            if (models.empty()) {
                const auto& sel = provider::active();
                if (sel.kind == provider::Kind::OpenAI
                    && !sel.openai_endpoint.use_tls)
                    err = "no response from " + sel.openai_endpoint.host + ":"
                        + std::to_string(sel.openai_endpoint.port)
                        + " \xe2\x80\x94 is the server running?";
            }
            // The model catalog is the other half of "which model ran".
            // An empty list is how BOTH the custom-host Copilot bug and a
            // dead local server present, so log the count and the reason
            // together — they are indistinguishable in the UI.
            AGT_LOGL(Wire, models.empty() ? ::agentty::logx::Level::Warn
                                          : ::agentty::logx::Level::Info,
                     "models.loaded", "provider={} count={} err={}",
                     for_provider, models.size(),
                     err.empty() ? std::string{"-"} : err);
            dispatch(ModelsLoaded{std::move(models), for_provider,
                                  std::move(err)});
        } catch (const std::exception& e) {
            // Dispatch an EMPTY ModelsLoaded carrying the reason (NOT a
            // StreamError) so the reducer always clears `models_loading`
            // and the model picker drops out of "Loading models…" into a
            // "no models" state the user can escape. A StreamError here
            // would be routed into the LIVE TURN's retry state machine:
            // opened mid-stream (Ctrl+/ is not gated on turn_active), a
            // failed catalog fetch would pop the assistant message that is
            // receiving deltas and race a RetryStream worker into the
            // session — or latch a healthy turn terminal. The reducer
            // surfaces `error` as a transient status toast instead.
            AGT_LOG(Wire, Warn, "models.failed", "provider={} err={}",
                    for_provider, e.what());
            dispatch(ModelsLoaded{std::vector<ModelInfo>{}, for_provider,
                                  std::string{"models fetch: "} + e.what()});
        } catch (...) {
            dispatch(ModelsLoaded{std::vector<ModelInfo>{}, for_provider,
                                  "models fetch: unknown exception"});
        }
    });
}

Cmd<Msg> fetch_models_for(std::string spec) {
    // Fetch ANY provider's catalog WITHOUT switching to it — the fused
    // cross-provider picker fans one of these out per authed provider on
    // open. Builds a Selection from `spec` (not active()), so the router
    // returns that backend's list; dispatches FusedCatalogLoaded, which the
    // reducer merges into Model::d.provider_catalogs (guarded by provider_id
    // against a provider signed out mid-fetch). list_models_for falls back to
    // the bundled seed on empty auth / unreachable host, so this is fast and
    // non-empty for hosted providers even before a live fetch succeeds.
    return Cmd<Msg>::task([spec = std::move(spec)](std::function<void(Msg)> dispatch) {
        try {
            auto sel = provider::parse_selection(spec);
            // Resolve credentials for THIS spec — not auth_snapshot(), which
            // resolves for the ACTIVE provider. The fused picker fans these
            // out for every OTHER authed provider, so the snapshot would ship
            // e.g. Anthropic's OAuth bearer to api.mistral.ai → 401.
            const std::string pid =
                sel.kind == provider::Kind::OpenAI
                    ? sel.openai_endpoint.label
                    : std::string{provider::default_provider_id()};
            auto models = provider::list_models_for(
                sel, provider::credentials::resolve(pid));
            dispatch(FusedCatalogLoaded{spec, std::move(models), true});
        } catch (...) {
            dispatch(FusedCatalogLoaded{spec, std::vector<ModelInfo>{}, false});
        }
    });
}

Cmd<Msg> open_browser_async(std::string url) {
    // task_isolated rather than task: posix_spawn / ShellExecute can
    // wedge on a hung WindowServer or a bizarre default-opener.
    // Isolated thread keeps a wedge from starving the shared BG pool.
    return Cmd<Msg>::task_isolated([url = std::move(url)]
                                   (std::function<void(Msg)>) {
        // No dispatch — the reducer doesn't care whether the browser
        // launched. The user can always paste auth_url manually from
        // the modal if their default opener is broken.
        auth::open_browser(url);
    });
}

Cmd<Msg> oauth_exchange(auth::OAuthCode    code,
                        auth::PkceVerifier verifier,
                        auth::OAuthState   state) {
    return Cmd<Msg>::task(
        [code = std::move(code),
         verifier = std::move(verifier),
         state = std::move(state)]
        (std::function<void(Msg)> dispatch) {
            try {
                auto r = auth::exchange_code(code, verifier, state);
                dispatch(LoginExchanged{std::move(r)});
            } catch (const std::exception& e) {
                dispatch(LoginExchanged{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    std::string{"exchange threw: "} + e.what()})});
            } catch (...) {
                dispatch(LoginExchanged{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    "exchange threw: unknown exception"})});
            }
        });
}

Cmd<Msg> load_threads_async() {
    // task_isolated rather than task: the JSON parse can take seconds
    // on a deep history (the directory walk is `directory_iterator` +
    // `nlohmann::json::parse` per file). Isolating it keeps the shared
    // worker pool free for stream / tool tasks the user fires in the
    // meantime.
    return Cmd<Msg>::task_isolated([](std::function<void(Msg)> dispatch) {
        try {
            auto threads = deps().load_threads();
            dispatch(ThreadsLoaded{std::move(threads)});
        } catch (...) {
            // Best-effort: a corrupt file is already logged + skipped
            // by load_all_threads, so any throw here is something the
            // user can't act on. Dispatch an empty list so the UI
            // doesn't sit on "loading…" forever.
            dispatch(ThreadsLoaded{std::vector<Thread>{}});
        }
    });
}

Cmd<Msg> load_plugins_async(bool reconnect) {
    // Bring the MCP connection snapshot INTO the Model. reconnect=true runs
    // the full reload (respawn + handshake, bounded by the bridge's 15s
    // deadline) for add/remove/toggle and first open; reconnect=false just
    // snapshots the already-live pool. Either way it dispatches
    // PluginsUpdated{plugin_model()} so the reducer stores the result in
    // m.ui.plugins — the view reads THAT, never the global pool. This is the
    // Cmd→Msg discipline that makes the panel a pure function of the Model.
    return Cmd<Msg>::task_isolated(
        [reconnect](std::function<void(Msg)> dispatch) {
            if (reconnect) {
                // Force the connect. On a COLD start nothing has accessed the
                // registry yet, so the pool is unbuilt — touching registry()
                // runs connect_initial_mcp() (spawn + handshake) once. On a
                // WARM path (add/remove/toggle) the pool already exists, so we
                // also run reload_mcp_plugins() to re-sync it to the edited
                // config. Order matters: build-if-cold, then reload.
                (void)tools::registry();
                (void)tools::reload_mcp_plugins();
            }
            dispatch(PluginsUpdated{mcp::plugin_model()});
        });
}

Cmd<Msg> load_thread_async(ThreadId id) {
    // task_isolated rather than task: a single big thread (multi-MB,
    // hundreds of messages) still takes 20-50ms of synchronous parse,
    // small enough to keep on the worker pool — but isolating matches
    // the load_threads_async policy and keeps the per-thread parse
    // off the same pool that tools/stream contend for.
    return Cmd<Msg>::task_isolated(
        [id = std::move(id)](std::function<void(Msg)> dispatch) {
            try {
                auto loaded = deps().load_thread(id);
                if (loaded) {
                    dispatch(ThreadLoaded{std::move(*loaded)});
                } else {
                    // Disk read / parse failure: surface an empty
                    // Thread so the reducer clears `thread_loading`
                    // and the UI doesn't sit stuck on the spinner.
                    // Reducer detects empty ThreadId == no swap.
                    dispatch(ThreadLoaded{Thread{}});
                }
            } catch (...) {
                dispatch(ThreadLoaded{Thread{}});
            }
        });
}

std::uint64_t next_codex_login_attempt_id() noexcept {
    static std::atomic_uint64_t next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

Cmd<Msg> probe_host_async(std::string spec, std::uint64_t attempt_id,
                          auth::AuthHeader auth) {
    // Connect-probe a custom host on a worker: dial its model list
    // (configured path → /v1/models → Ollama /api/tags) and report the
    // DETECTED dialect. Bounded by probe_host's own 3s/6s timeouts, so the
    // modal's "probing…" state resolves quickly either way.
    return Cmd<Msg>::task([spec = std::move(spec), attempt_id,
                           auth = std::move(auth)]
                          (std::function<void(Msg)> dispatch) {
        HostProbed r;
        r.attempt_id = attempt_id;
        r.spec       = spec;
        try {
            const auto sel = provider::parse_selection(spec);
            const auto probe =
                provider::openai::probe_host(auth, sel.openai_endpoint);
            using D = provider::openai::HostProbe::Dialect;
            if (probe.dialect != D::None) {
                r.ok          = true;
                r.models_path = probe.models_path;
                r.native_api  = probe.dialect == D::OllamaNative;
                r.model_count = probe.model_count;
                r.latency_ms  = probe.latency_ms;
            } else if (probe.http_status == 401 || probe.http_status == 403) {
                r.error = "HTTP " + std::to_string(probe.http_status)
                        + " \xe2\x80\x94 this host needs an API key";
            } else if (probe.http_status != 0) {
                r.error = "HTTP " + std::to_string(probe.http_status)
                        + " \xe2\x80\x94 no model list at any known path";
            } else {
                r.error = "nothing listening \xe2\x80\x94 is the server "
                          "running on that host:port?";
            }
        } catch (const std::exception& e) {
            r.error = e.what();
        } catch (...) {
            r.error = "probe failed";
        }
        dispatch(std::move(r));
    });
}

Cmd<Msg> device_login_async(std::string provider, std::string provider_label,
                            std::uint64_t attempt_id,
                            std::shared_ptr<std::atomic_bool> cancel) {
    // Native OAuth device flow, provider-generic. Requests a one-time code
    // (dispatched to the modal via DeviceCodeReady), then block-polls until the
    // user approves. Every message carries provider + attempt_id so a stale
    // worker can't complete a newer login; Esc trips `cancel` for cooperative
    // shutdown. Runs isolated because login() blocks while the user signs in.
    return Cmd<Msg>::task_isolated(
        [provider = std::move(provider), provider_label = std::move(provider_label),
         attempt_id, cancel = std::move(cancel)](std::function<void(Msg)> dispatch) {
        const auto cancelled = [cancel] {
            return cancel && cancel->load(std::memory_order_acquire);
        };
        auto emit_code = [&](std::string bare_url, std::string browser_url,
                             std::string user_code) {
            dispatch(DeviceCodeReady{
                .provider = provider,
                .attempt_id = attempt_id,
                .verification_url = std::move(bare_url),
                .browser_url = std::move(browser_url),
                .user_code = std::move(user_code),
            });
        };
        auto done = [&](std::optional<std::string> error) {
            dispatch(DeviceLoginDone{
                .provider = provider,
                .provider_label = provider_label,
                .attempt_id = attempt_id,
                .error = std::move(error),
            });
        };
        try {
            // Each provider's login() shares the same shape (timeout,
            // device-code sink, cancel-probe) but its own DeviceCode type;
            // map it to the generic dispatch. login() persists the token on
            // success, so we only forward success-or-error to the reducer.
            //
            // This is one of the few provider branches that legitimately
            // survives the registry migration: the two arms call DIFFERENT
            // functions returning DIFFERENT types, and each renders its code
            // URL differently (GitHub shows a bare URL + code; Kimi wants the
            // pre-filled ?user_code= link so an SSH user can paste one thing).
            // A registry flag can say "this provider does device login" — it
            // cannot erase a type difference. Erasing it would need a
            // DeviceLoginFn seam on the row, which is worth doing at three
            // providers, not two.
            std::optional<std::string> err;
            if (provider == "copilot") {
                auto r = provider::copilot::login(900,
                    [&](const provider::copilot::DeviceCode& c) {
                        // GitHub returns one URL (github.com/login/device); the
                        // user types the code there.
                        emit_code(c.verification_uri, c.verification_uri, c.user_code);
                    }, cancelled);
                if (!r) err = r.error().render();
            } else if (provider == "kimi") {
                auto r = provider::kimi::login(900,
                    [&](const provider::kimi::DeviceCode& c) {
                        // Show + open the PRE-FILLED url (…?user_code=XXXX) so
                        // that on SSH/headless, the url the user manually opens
                        // already carries the code. kimi.ai's approval page
                        // reads user_code from the query string.
                        const std::string complete =
                            c.verification_uri_complete.empty() ? c.verification_uri
                                                                : c.verification_uri_complete;
                        emit_code(complete, complete, c.user_code);
                    }, cancelled);
                if (!r) err = r.error().render();
            } else {
                err = "unknown device-login provider: " + provider;
            }
            done(std::move(err));
        } catch (const std::exception& e) {
            done(provider + " login threw: " + e.what());
        } catch (...) {
            done(provider + " login threw");
        }
    });
}

Cmd<Msg> codex_login_async(std::uint64_t attempt_id,
                           std::shared_ptr<std::atomic_bool> cancel) {
    // Isolated because either OAuth mode blocks while the user signs in. Every
    // message carries attempt_id so a late worker cannot complete a newer
    // login, and Esc trips cancel for cooperative polling shutdown.
    return Cmd<Msg>::task_isolated(
        [attempt_id, cancel = std::move(cancel)](std::function<void(Msg)> dispatch) {
        const auto cancelled = [cancel] {
            return cancel && cancel->load(std::memory_order_acquire);
        };
        try {
            auto r = provider::chatgpt::codex_login(
                900, [attempt_id, &dispatch](
                         const provider::chatgpt::CodexDeviceCode& code) {
                    dispatch(CodexDeviceCodeReady{
                        .attempt_id = attempt_id,
                        .verification_url = code.verification_url,
                        .user_code = code.user_code,
                    });
                }, cancelled);
            dispatch(CodexLoginDone{
                .attempt_id = attempt_id,
                .result = std::move(r),
            });
        } catch (const std::exception& e) {
            dispatch(CodexLoginDone{
                .attempt_id = attempt_id,
                .result = std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    std::string{"ChatGPT login threw: "} + e.what()}),
            });
        } catch (...) {
            dispatch(CodexLoginDone{
                .attempt_id = attempt_id,
                .result = std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    "ChatGPT login threw: unknown exception"}),
            });
        }
    });
}

Cmd<Msg> refresh_oauth(std::string refresh_token) {
    return Cmd<Msg>::task(
        [refresh_token = std::move(refresh_token)]
        (std::function<void(Msg)> dispatch) {
            try {
                auto r = auth::refresh_access_token_locked(
                    auth::RefreshToken{refresh_token});
                dispatch(TokenRefreshed{std::move(r)});
            } catch (const std::exception& e) {
                dispatch(TokenRefreshed{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    std::string{"refresh threw: "} + e.what()})});
            } catch (...) {
                dispatch(TokenRefreshed{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    "refresh threw: unknown exception"})});
            }
        });
}

} // namespace agentty::app::cmd
