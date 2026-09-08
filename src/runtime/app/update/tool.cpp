// Tool-execution-result helpers: apply_tool_output translates a
// ToolExecOutput into a Done/Failed status on the matching ToolUse;
// mark_tool_rejected is the symmetric one-liner for permission denial.
// Both walk m.d.current.messages because a ToolCallId is only locally
// unique within a turn — we don't index them.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/io/clipboard.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/core/cmd.hpp>      // maya::Cmd
#include <maya/core/motion.hpp>   // anim::keep_animating — frame requests
#include <nlohmann/json.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/host_escape.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/util/utf8.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using json = nlohmann::json;

namespace {

// Private OSC number for host (editor) integration escapes — see
// ui::host::file_event_osc. 5379 is unclaimed by mainstream terminals; the
// "agentty;" payload tag lets a host match ours and ignore other OSCs.
constexpr int kHostOsc = 5379;

// Build the host follow-along OSC payload for a finished FILE tool, or nullopt
// for non-file tools / inactive integration. Only tools that name a concrete
// path qualify (read/edit/write/move/list_dir); the path comes from the same
// streaming-aware reader the timeline header uses, and read's 1-based offset
// becomes the follow-along line.
[[nodiscard]] std::optional<std::string> file_event_osc_for(const ToolUse& tc) {
    const std::string& n = tc.name.value;
    const bool is_file_tool =
        n == "read" || n == "edit" || n == "write" || n == "move"
        || n == "list_dir";
    if (!is_file_tool) return std::nullopt;
    std::string path = ui::tool_path_arg(tc);
    if (n == "move" && path.empty())
        path = ui::safe_arg(tc.args, "destination");
    if (path.empty()) return std::nullopt;
    std::optional<int> line;
    if (n == "read") {
        if (int off = ui::safe_int_arg(tc.args, "offset", 0); off > 0) line = off;
    }
    return ui::host::file_event_osc(n, path, line);
}

// Per-tool-output ceiling carried in the conversation. Tool runners
// already cap their own captures (read = 1 MiB, grep = 8 MiB, bash =
// 30 KB, etc.), but those are tuned for fidelity inside one call.
// For long-lived process memory, a uniform conversation-side cap is
// the right knob: a session with 50 grep results at near-cap was
// 400+ MB of *terminal* tool output we never trim. Head + tail
// keeps the model-relevant context (top of file, last error, etc.)
// and lets a multi-MiB capture compact to ~256 KiB.
constexpr std::size_t kStoredOutputCap = 256u * 1024u; // 256 KiB

std::string clamp_output(std::string s) {
    if (s.size() <= kStoredOutputCap) return s;
    constexpr std::size_t half = kStoredOutputCap / 2;
    // Both cut points MUST land on UTF-8 code-point boundaries: the
    // clamped string is what goes onto the wire as the tool_result, and
    // nlohmann::json::dump() throws type_error.316 on a split multi-byte
    // sequence — which would kill serialization of the whole request.
    // Head: largest boundary ≤ half. Tail: walk the start FORWARD past
    // any continuation bytes so the kept suffix begins on a lead byte.
    const std::size_t head_end = tools::util::safe_utf8_cut(s, half);
    std::size_t tail_start = s.size() - half;
    while (tail_start < s.size()
           && (static_cast<unsigned char>(s[tail_start]) & 0xC0) == 0x80)
        ++tail_start;
    std::string out;
    out.reserve(kStoredOutputCap + 64);
    out.append(s, 0, head_end);
    out.append("\n\n… (");
    out.append(std::to_string((s.size() - head_end - (s.size() - tail_start)) / 1024));
    out.append(" KiB elided to keep memory bounded) …\n\n");
    out.append(s, tail_start, s.size() - tail_start);
    return out;
}

// Once a tool reaches a terminal state, the per-call streaming
// scratch buffers (raw delta accumulator, lazy args.dump cache) are
// dead weight — the wire is closed, the args are parsed, and any
// re-render comes from `args` directly. Free them so a long session
// doesn't pin one copy per finished tool call.
void release_streaming_buffers(ToolUse& tc) {
    std::string{}.swap(tc.args_streaming);
    tc.args_dump_cache.clear();
    tc.args_dump_cache.shrink_to_fit();
    tc.args_dump_valid = false;
}

// Keep the render clock ticking for a bounded window so maya gets the
// follow-up frames its live-tail shrink/overflow reconciliation needs
// after a card's rendered height changes mid-turn. This is the same
// lever the deferred settle-freeze uses (subscribe.cpp gates the tick
// on settle_cooldown_ticks > 0); arming it here at every card-height
// mutation reproduces agent_session's always-on clock at the exact
// seams where the height changes. Grow-only so a longer in-flight
// window is never truncated by a shorter one.
void arm_reconcile_cooldown(Model& m) {
    constexpr int kReconcileTicks = 6;
    if (m.ui.settle_cooldown_ticks < kReconcileTicks)
        m.ui.settle_cooldown_ticks = kReconcileTicks;
    ::maya::anim::keep_animating();
}

// Build the tool-output-viewer entry list: every settled tool call in the
// current thread with a non-empty stored output, NEWEST FIRST (the one the
// user just watched scroll past is entry 0). Bounded by kMaxEntries /
// kSnapshotBudget so a marathon session can't balloon the overlay open.
// Snapshotting (copying the output bytes) makes the overlay immune to the
// transcript mutating underneath it — each stored output is already
// clamped to 256 KiB upstream, so the copies are cheap and bounded.
[[nodiscard]] std::vector<tool_output::Entry> collect_viewer_entries(const Model& m) {
    std::vector<tool_output::Entry> out;
    std::size_t budget = tool_output::kSnapshotBudget;

    // Row 0 is the LIVE tool, if one is active right now: this includes
    // argument streaming / permission (Pending or Approved) as well as local
    // execution (Running). Restricting this to Running made Ctrl+O say
    // "nothing to inspect yet" during the entire model→tool-input phase —
    // exactly when edit/write previews are most useful.
    for (auto mit = m.d.current.messages.rbegin();
         mit != m.d.current.messages.rend(); ++mit) {
        bool found = false;
        for (auto tit = mit->tool_calls.rbegin();
             tit != mit->tool_calls.rend(); ++tit) {
            const auto* run = std::get_if<ToolUse::Running>(&tit->status);
            if (!run && !tit->is_pending() && !tit->is_approved()) continue;
            tool_output::Entry e;
            e.is_live  = true;
            e.name     = tit->name.value;
            e.title    = ui::tool_display_name(tit->name.value);
            e.detail   = ui::tool_timeline_detail(*tit);
            e.output   = run ? run->progress_text : std::string{};
            e.call     = *tit;
            e.failed   = false;
            char buf[32];
            const char* phase = run ? "running"
                : (tit->is_approved() ? "approved" : "streaming input");
            std::snprintf(buf, sizeof buf, "%s \xc2\xb7 %.1fs",
                          phase, ui::tool_elapsed(*tit));
            e.trailing = buf;
            if (!e.output.empty())
                e.trailing += (e.output.size() >= 1024)
                    ? " \xc2\xb7 " + std::to_string(e.output.size() / 1024) + " KB"
                    : " \xc2\xb7 " + std::to_string(e.output.size()) + " B";
            budget -= std::min(budget, e.output.size());
            out.push_back(std::move(e));
            found = true;
            break;
        }
        if (found) break;
    }
    for (auto mit = m.d.current.messages.rbegin();
         mit != m.d.current.messages.rend(); ++mit) {
        // Retrieved-context cards surface here too, so the FULL passages the
        // model was grounded on are readable in the Ctrl+O overlay — including
        // after the card has scrolled into the (immutable) frozen prefix,
        // where the in-place Ctrl+U expand can't reach. Rendered through the
        // viewer's plain-text (line-numbered) body path: the default-
        // constructed ToolUse `call` has no structured kind, so the body
        // stage falls through to the raw-text branch automatically.
        if (mit->is_proactive_context() && !mit->text.empty()) {
            // Strip the <retrieved-context> … </retrieved-context> fence and
            // the lead-in preamble so the body is just the passages.
            std::string body = mit->text;
            if (auto open = body.find('>'); open != std::string::npos
                && body.compare(0, 18, "<retrieved-context") == 0) {
                std::size_t start = body.find('\n', open);
                start = (start == std::string::npos) ? open + 1 : start + 1;
                std::size_t end = body.rfind("</retrieved-context>");
                if (end == std::string::npos || end < start) end = body.size();
                body = body.substr(start, end - start);
            }
            while (!body.empty() && (body.back() == '\n' || body.back() == ' '
                                  || body.back() == '\t' || body.back() == '\r'))
                body.pop_back();
            if (!body.empty() && out.size() < tool_output::kMaxEntries
                && body.size() <= budget) {
                budget -= body.size();
                tool_output::Entry e;
                e.name   = "retrieved_context";
                e.title  = "Retrieved context";
                e.output = std::move(body);
                e.failed = false;
                // Detail: passage count (each source block starts with "[").
                std::size_t passages = 0;
                for (std::size_t p = 0; (p = e.output.find("[", p)) != std::string::npos; ++p)
                    if (p == 0 || e.output[p - 1] == '\n') ++passages;
                e.detail = passages == 0 ? std::string{}
                    : std::to_string(passages)
                        + (passages == 1 ? " passage" : " passages");
                e.trailing = (e.output.size() >= 1024)
                    ? std::to_string(e.output.size() / 1024) + " KB"
                    : std::to_string(e.output.size()) + " B";
                out.push_back(std::move(e));
            }
            continue;
        }
        for (auto tit = mit->tool_calls.rbegin();
             tit != mit->tool_calls.rend(); ++tit) {
            const auto& tc = *tit;
            if (!tc.is_terminal()) continue;
            const auto& body = tc.output();
            if (body.empty()) continue;
            if (out.size() >= tool_output::kMaxEntries) return out;
            if (body.size() > budget) continue;   // skip, keep older smaller ones
            budget -= body.size();

            tool_output::Entry e;
            e.failed = tc.is_failed();
            e.output = body;
            // Raw name drives the category colour badge; display name +
            // detail reuse the timeline's helpers so the list reads
            // exactly like the transcript cards being re-inspected.
            e.name   = tc.name.value;
            e.title  = ui::tool_display_name(tc.name.value);
            e.detail = ui::tool_timeline_detail(tc);
            e.call   = tc;   // snapshot for the rich body render (diff/gutter)
            // Trailing: ok/failed · duration · size.
            e.trailing = e.failed ? "failed" : "ok";
            if (float secs = ui::tool_elapsed(tc); secs >= 0.05f) {
                char buf[32];
                std::snprintf(buf, sizeof buf, " \xc2\xb7 %.1fs", secs);
                e.trailing += buf;
            }
            e.trailing += (body.size() >= 1024)
                ? " \xc2\xb7 " + std::to_string(body.size() / 1024) + " KB"
                : " \xc2\xb7 " + std::to_string(body.size()) + " B";
            out.push_back(std::move(e));
        }
    }
    return out;
}

} // namespace

// Keep the Ctrl+O snapshot synchronized from both stream.cpp (tool-input
// deltas/snapshots) and this TU (execution progress/results).
void resync_live_tool_viewer(Model& m) {
    auto* o = m.ui.panel.get<pn::ToolOutput>();
    if (!o) return;
    // Anchor: remember which entry the user is on by identity, not index —
    // prepending/removing the live row shifts indices under them.
    const bool was_live_selected =
        o->index >= 0 && o->index < static_cast<int>(o->entries.size())
        && o->entries[static_cast<std::size_t>(o->index)].is_live;
    auto fresh = collect_viewer_entries(m);
    const bool has_live = !fresh.empty() && fresh.front().is_live;
    if (fresh.empty()) { return; }   // keep the last snapshot rather than blank
    // If the user was reading the live row, keep them pinned to row 0 (it
    // either still lives there or has just settled into the newest entry).
    if (was_live_selected) {
        o->index = 0;
    } else {
        // Non-live selection: the live row's presence shifts everything by
        // one. Adjust the index by the delta in live-row count so the same
        // finished entry stays under the cursor.
        const bool had_live = !o->entries.empty() && o->entries.front().is_live;
        o->index += (has_live ? 1 : 0) - (had_live ? 1 : 0);
    }
    o->entries = std::move(fresh);
    o->index = std::clamp(o->index, 0,
                          std::max(0, static_cast<int>(o->entries.size()) - 1));
}

void apply_tool_output(Model& m, const ToolCallId& id,
                       std::expected<std::string, tools::ToolError>&& result,
                       std::optional<FileChange>&& change,
                       std::vector<FileChange>&& changes,
                       std::vector<ImageContent>&& images) {
    with_live_tool(m, id, [&](ToolUse& tc) {
        // Idempotent: a tool already in a terminal state
        // (Done / Failed / Rejected) keeps that state. Realistic
        // ways a late ToolExecOutput can land here:
        //   (a) Wall-clock watchdog force-failed the tool at
        //       60 s; the worker thread eventually unwound
        //       seconds/minutes later. The original failure
        //       reason ("hung") is more useful to the user
        //       than the late output would be — and overwriting
        //       could re-arm a turn that's already advanced
        //       past this tool.
        //   (b) A duplicate dispatch on the same id (shouldn't
        //       happen but cheap to defend against).
        // Either way, dropping the late result keeps history
        // stable.
        //
        // Frozen prefix: with_live_tool already skips messages with
        // index < frozen_through, so a ToolExecOutput that races a
        // freeze (turn settled, user submitted again, tool worker
        // finally returned) silently no-ops here. Without the gate
        // the mutation would land on a Message whose rendered
        // Element in m.ui.frozen is immutable — visible as a
        // permanently-Running spinner in scrollback.
        if (tc.is_terminal()) return;
        auto now = std::chrono::steady_clock::now();
        auto started = tc.started_at();
        if (result) {
            tc.status = ToolUse::Done{started, now,
                clamp_output(std::move(*result)), std::move(images)};
        } else {
            // Render typed error as "[kind] detail" so the category
            // is visible in tool-card / history without losing the
            // human-readable detail. The model needs only the
            // string back; the kind is preserved structurally for
            // the future, when the view branches on category.
            tc.status = ToolUse::Failed{started, now,
                clamp_output(result.error().render())};
        }
        release_streaming_buffers(tc);
    });
    // Queue the structured change(s) (edit / write / apply_patch / replace)
    // for diff-review. Multi-file tools (replace) fill `changes`; single-file
    // tools fill `change`. DEDUP per file: successive edits to the SAME path
    // collapse into one review entry showing net original→latest, so the pane
    // reads as "here's what changed in login.cpp", not a stack of overlapping
    // diffs. Only queue on success — a failed tool wrote nothing.
    if (result) {
        auto queue_one = [&](FileChange&& ch) {
            auto& q = m.d.pending_changes;
            auto it = std::ranges::find(q, ch.path, &FileChange::path);
            if (it != q.end()) {
                std::string original = it->original_contents;
                *it = diff::compute(ch.path, original, ch.new_contents);
            } else {
                q.push_back(std::move(ch));
            }
        };
        if (!changes.empty())
            for (auto& ch : changes) queue_one(std::move(ch));
        else if (change)
            queue_one(std::move(*change));
    }
    // running spinner into the full output/error body — the card's
    // rendered height changes at this instant (a Failed card in
    // particular grows by its error rows). If that height change shifts
    // the overflowed prefix while the clock is about to lapse (last tool
    // of the turn dropping toward Idle, or a coalesced fps=0 frame),
    // maya composes the shifted frame ONCE and never gets the follow-up
    // frames its shrink/overflow reconciliation needs — the old card's
    // top rows strand in scrollback (the "card cut off one screen up"
    // corruption). agent_session never sees this because its clock ticks
    // UNCONDITIONALLY. Mirror that: arm the reconciliation cooldown so
    // the clock keeps running for a few frames after any card-height
    // change, guaranteeing maya reconciles the seam exactly like the
    // reference's always-on tick.
    arm_reconcile_cooldown(m);
}

void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason) {
    with_live_tool(m, id, [&](ToolUse& tc) {
        auto now = std::chrono::steady_clock::now();
        if (reason.empty()) {
            tc.status = ToolUse::Rejected{now};
        } else {
            tc.status = ToolUse::Failed{tc.started_at(), now,
                clamp_output(std::string{reason})};
        }
        release_streaming_buffers(tc);
    });
    // Same rationale as apply_tool_output: a rejected tool's card body
    // changes height (spinner → rejection reason), so keep the clock
    // ticking a few frames to let maya reconcile the seam.
    arm_reconcile_cooldown(m);
}

// ============================================================================
// tool_update — reducer for `msg::ToolMsg`
// ============================================================================
// Tool-execution results from the local runner + permission-prompt
// resolutions from the user. Permission lives here because a permission
// prompt is always *about* a specific pending tool call and the resolution
// feeds back into the tool state machine — no clean split.

Step tool_update(Model m, msg::ToolMsg tm) {
    using maya::overload;
    using maya::Cmd;

    return std::visit(overload{
        // ── Live tool progress (streaming subprocess output) ────────────
        // Arrives from the subprocess runner every ~80 ms with the full
        // accumulated output so far. We just set it — no Cmd to return —
        // and rely on the existing Tick subscription (active during
        // ExecutingTool) to re-render. Ignore if the tool has already
        // finalised (a late snapshot racing the terminal ToolExecOutput).
        [&](ToolExecProgress& e) -> Step {
            // Frozen prefix is immutable — a late progress snapshot
            // for a turn that's already settled into m.ui.frozen
            // silently no-ops here.
            with_live_tool(m, e.id, [&](ToolUse& tc) {
                if (auto* r = std::get_if<ToolUse::Running>(&tc.status)) {
                    // Belt-and-braces terminal line-discipline. The in-tree
                    // subprocess runners already clean at the capture
                    // boundary, but snapshots can also arrive from paths
                    // that don't ride them (external MCP servers streaming
                    // progress, subagent feeds echoing tool output). A raw
                    // ESC/CR/BS byte reaching the card body paints control
                    // bytes as cells and commits them to scrollback — the
                    // stray-glyph corruption class. Cheap: O(n) scan, and
                    // the common (already-clean) case only copies.
                    e.snapshot = tools::util::strip_terminal_controls(e.snapshot);
                    // Cap the stored snapshot: the body preview shows
                    // the trailing window only, but `tc.progress_text`
                    // gets COPIED into a ToolBodyPreview Config every
                    // frame the live timeline is rebuilt. Unbounded
                    // bash output (e.g. `find /`) would otherwise push
                    // 100s of KB through that copy each frame and
                    // visibly stall the UI on long commands. Mirrors
                    // the write fast path's content cap.
                    constexpr std::size_t kProgressKeep = 16 * 1024;
                    if (e.snapshot.size() > kProgressKeep) {
                        // Keep the tail — newest bytes are the most
                        // useful confirmation of progress. Advance the
                        // cut past any UTF-8 continuation bytes so the
                        // kept suffix starts on a code-point boundary
                        // (a split sequence renders as mojibake in the
                        // card body).
                        std::size_t cut = e.snapshot.size() - kProgressKeep;
                        while (cut < e.snapshot.size()
                               && (static_cast<unsigned char>(
                                       e.snapshot[cut]) & 0xC0) == 0x80)
                            ++cut;
                        e.snapshot.erase(0, cut);
                    }
                    r->progress_text = std::move(e.snapshot);
                    // Reset the liveness clock: this tool is demonstrably
                    // making progress, so the wedge net's cap restarts from
                    // here rather than from the (possibly many-minutes-ago)
                    // launch time.
                    r->last_progress_at = std::chrono::steady_clock::now();
                }
            });
            // If the user is watching the Ctrl+O viewer, keep its Live row
            // (row 0) tailing this fresh output in place.
            resync_live_tool_viewer(m);
            return done(std::move(m));
        },

        // ── Per-tool wall-clock watchdog ──────────────────────────────────
        [&](ToolTimeoutCheck& e) -> Step {
            bool flipped = false;
            with_live_tool(m, e.id, [&](ToolUse& tc) {
                if (tc.is_terminal()) return;
                auto now = std::chrono::steady_clock::now();
                const auto* sp = tools::spec::lookup(tc.name.value);
                auto secs = sp ? sp->max_seconds : std::chrono::seconds{0};
                std::string reason;
                if (tc.is_pending() || tc.is_approved()) {
                    reason = "tool stayed " + std::string{tc.status_name()}
                        + " for " + std::to_string(secs.count())
                        + " s \xe2\x80\x94 args probably never finished streaming "
                        "(transient API error mid-tool_use, or the "
                        "stream silently exited without a terminal event).";
                } else {
                    reason = "tool execution exceeded "
                        + std::to_string(secs.count())
                        + " s wall-clock \xe2\x80\x94 likely hung on a blocking "
                        "syscall (slow/dead filesystem mount, network "
                        "freeze, or worker deadlock). The tool's worker "
                        "thread may continue in the background; its "
                        "result will be discarded if it ever returns.";
                }
                tc.status = ToolUse::Failed{
                    tc.started_at(), now, std::move(reason)};
                flipped = true;
            });
            if (!flipped) return done(std::move(m));
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Step {
            // todo's side effect on the UI's plan state — runs only
            // when the call actually succeeded; failures don't synthesise
            // a plan. The final exact state lands here even if the live
            // streaming sync (stream.cpp) raced a partial array.
            if (e.result) {
                for (const auto& msg_ : m.d.current.messages)
                    for (const auto& tc : msg_.tool_calls)
                        if (tc.id == e.id && tc.name == "todo")
                            sync_todo_state_from_args(m, tc.args);
            }
            apply_tool_output(m, e.id, std::move(e.result), std::move(e.change),
                              std::move(e.changes), std::move(e.images));
            // If the Ctrl+O viewer is open, refresh it so the Live row settles
            // into a finished entry the instant this tool completes.
            resync_live_tool_viewer(m);

            // Cooperating-host follow-along: when running on an editor PTY
            // (Emacs/vterm) that watches for our OSC, tell it which file the
            // agent just touched so it can open / reveal / diff it natively.
            // Frame-safe (maya Cmd::emit_osc, out-of-band), and a complete
            // no-op on a normal terminal (integration_active() is false).
            maya::Cmd<Msg> host_cmd = maya::Cmd<Msg>::none();
            if (ui::host::integration_active()) {
                for (const auto& msg_ : m.d.current.messages) {
                    bool done_scan = false;
                    for (const auto& tc : msg_.tool_calls) {
                        if (tc.id != e.id || !tc.is_done()) continue;
                        if (auto osc = file_event_osc_for(tc))
                            host_cmd = maya::Cmd<Msg>::emit_osc(kHostOsc, *osc);
                        done_scan = true; break;
                    }
                    if (done_scan) break;
                }
            }

            // No mid-run freeze or trim here. The single freeze site is
            // finalize_turn (the agent_session MessageStop analog) — the
            // whole agent turn is wrapped into one Turn Element and
            // pushed to m.ui.frozen atomically there. Carving mid-run
            // (the prior freeze_settled_subturns + trim_frozen_above_
            // viewport calls that lived here) was the documented source
            // of "redraws from top + scrollback corruption" at every
            // tool→continuation seam: the freeze pushed an entry whose
            // hash_id maya's component cache had not seen on the live
            // tail's previous frame, so the cache missed and re-emitted
            // those rows — sometimes over already-committed scrollback.
            // agent_session never carves mid-stream and shows zero
            // corruption / zero slowdown on long runs (proven by the
            // long_session bench); we now do the same.
            // A SPECULATIVE read-only tool (launched mid-stream at
            // StreamToolUseEnd) can finish while the wire is still
            // streaming this turn. Do NOT run kick_pending_tools then:
            // it would pull the Active ctx out of phase::Streaming (or
            // worse, see zero pending work and launch the continuation
            // stream against a wire that is still delivering). Just land
            // the output; StreamFinished → finalize_turn runs the normal
            // kick and finds this tool already terminal.
            if (m.s.is_streaming()) {
                if (host_cmd.is_none()) return done(std::move(m));
                return {std::move(m), std::move(host_cmd)};
            }
            auto kick = cmd::kick_pending_tools(m);
            if (host_cmd.is_none())
                return {std::move(m), std::move(kick)};
            return {std::move(m), maya::Cmd<Msg>::batch(
                std::vector<maya::Cmd<Msg>>{std::move(kick), std::move(host_cmd)})};
        },

        // ── Permission ──────────────────────────────────────────────
        [&](PermissionApprove) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            // Permission only ever fires against a tool in the live
            // tail — a frozen turn is by definition past every pending
            // permission. with_live_tool's frozen-prefix gate is the
            // structural guarantee of that invariant.
            with_live_tool(m, id, [&](ToolUse& tc) {
                // Mark approval as type state: Pending → Approved.
                // kick_pending_tools then treats Approved as
                // "permission already granted" and routes through
                // the same effect-parallel gate as a non-permissioned
                // tool — so if a sibling Read is still running, a
                // freshly approved Write/Bash waits for it instead
                // of racing.
                tc.status = ToolUse::Approved{tc.started_at()};
            });
            m.d.pending_permission.reset();
            return {std::move(m), cmd::kick_pending_tools(m)};
        },
        [&](PermissionReject) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            mark_tool_rejected(m, id, "User rejected this tool call.");
            m.d.pending_permission.reset();
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },
        [&](PermissionApproveAlways) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id   = m.d.pending_permission->id;
            auto name = m.d.pending_permission->tool_name;
            // Record a session-scoped grant for this tool NAME so every
            // future call to it this session auto-approves (consulted in
            // kick_pending_tools). This also propagates to sibling
            // pending tools of the same name in the current batch: the
            // re-kick below re-evaluates each pending tool against the
            // now-populated grant set, so a queued sibling `bash` won't
            // re-prompt. Mirrors Zed's per-session allow-list with live
            // sibling propagation.
            m.d.session_grants.insert(name.value);
            // Persist the grant (Zed's always_allow rules): reload-proof.
            // Load-modify-save so we never clobber provider keys etc.
            {
                auto s = deps().load_settings();
                if (std::find(s.always_allow_tools.begin(),
                              s.always_allow_tools.end(), name.value)
                        == s.always_allow_tools.end()) {
                    s.always_allow_tools.push_back(name.value);
                    deps().save_settings(s);
                }
            }
            m.s.status = name.value + ": always allowed (persists \xc2\xb7 "
                         "Shift+Tab profile cycle resets)";
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds{4};
            with_live_tool(m, id, [&](ToolUse& tc) {
                tc.status = ToolUse::Approved{tc.started_at()};
            });
            m.d.pending_permission.reset();
            return {std::move(m), cmd::kick_pending_tools(m)};
        },

        // ── Tool-output viewer ─────────────────────────
        [&](OpenToolOutput) -> Step {
            auto entries = collect_viewer_entries(m);
            if (entries.empty()) {
                auto cmd = set_status_toast(m, "nothing to inspect yet");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.panel = pn::ToolOutput{{std::move(entries), 0, false}};
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return done(std::move(m));
        },
        [&](CloseToolOutput) -> Step {
            // Esc semantics: body stage → back to the list; list → unwind
            // one level (the palette that opened this, or the thread).
            if (auto* o = m.ui.panel.get<pn::ToolOutput>(); o && o->viewing) {
                o->viewing = false;
                m.ui.tool_viewer_scroll.y = 0;
                return done(std::move(m));
            }
            ascend(m);
            return done(std::move(m));
        },
        [&](ToolOutputMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o) return done(std::move(m));
            if (o->viewing) {
                // Body stage: deltas scroll the output viewport directly.
                // max_y is paint-written-back by the Picker widget.
                auto& sc = m.ui.tool_viewer_scroll;
                sc.y = std::clamp(sc.y + e.delta, 0, std::max(0, sc.max_y));
                // Live tail-follow: scrolling UP off the bottom disengages
                // auto-tail so the user can read earlier output; scrolling
                // (or Ending) back to the bottom re-engages it.
                m.ui.tool_viewer_tail = (sc.y >= sc.max_y);
            } else {
                int sz = static_cast<int>(o->entries.size());
                if (sz > 0)
                    o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            }
            return done(std::move(m));
        },
        [&](ToolOutputSelect) -> Step {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || o->viewing) return done(std::move(m));
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            o->viewing = true;
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return done(std::move(m));
        },
        [&](ToolOutputStep& e) -> Step {
            // ←/→ while reading an output: hop to the neighbouring
            // entry's body directly. Clamped at the ends (no wrap — the
            // list is short and wrap-around disorients more than it
            // helps). List stage: no-op.
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || !o->viewing) return done(std::move(m));
            int sz = static_cast<int>(o->entries.size());
            if (sz <= 0) return done(std::move(m));
            int next = std::clamp(o->index + e.delta, 0, sz - 1);
            if (next != o->index) {
                o->index = next;
                m.ui.tool_viewer_scroll.y = 0;
            }
            return done(std::move(m));
        },
        [&](ToolOutputCopy) -> Step {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o) return done(std::move(m));
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            std::string body =
                o->entries[static_cast<std::size_t>(o->index)].output;
            (void)write_clipboard_text(body);   // native pbcopy/wl-copy/xclip
            auto toast = set_status_toast(m, "tool output copied to clipboard");
            return {std::move(m), maya::Cmd<Msg>::batch(
                maya::Cmd<Msg>::write_clipboard(std::move(body)),
                std::move(toast))};
        },

    }, tm);
}

} // namespace agentty::app::detail
