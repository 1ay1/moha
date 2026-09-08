// thread_list.cpp — the thread switcher's reducer (open/pick/delete +
// the async ThreadsLoaded). Split from panels.cpp; shares only
// reset_to_fresh_thread (defined here, its sole user).

#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/core/anim_clock.hpp>   // anim_now_ms (catalog freshness clock)
#include <maya/platform/io.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/catalog_sources.hpp"
#include "agentty/auth/vault.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/provider_rows.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;
using maya::Cmd;
// ── Fresh-thread reset ────────────────────────────────────────────────────
// Swap the model over to a brand-new empty thread and return the terminal
// reset that wipes the departing thread's rendered turns off-screen.
//
// This is the SHARED core behind two entry points: `NewThread` (^N / picker
// `N`) and `ThreadListDelete` when the row removed is the active thread.
// It is deliberately the part *after* the caller's save/delete decision —
// NewThread persists the outgoing thread first, delete has just destroyed
// it — so the caller owns that policy and this owns the reset. Keeping the
// two callers on one code path is what stops them drifting (they had, and
// the delete copy was missing the phase reset + kernel release + inline
// wipe, which is exactly the machinery that makes a mid-stream swap safe).
//
// Returns the reset_inline Cmd so the caller can batch it with its own
// commands (delete also kicks a thread-list refresh + a toast).
[[nodiscard]] Cmd<Msg> reset_to_fresh_thread(Model& m) {
    // Skill activations belong to the departing thread's context; the new
    // thread must be able to re-load any skill from scratch.
    tools::skills::reset_activations();
    // Drop the whole render cache: every (tid,msg) entry belongs to the
    // thread we're leaving, whose messages will never freeze again (freeze
    // is the only per-entry drop, and it only runs on the CURRENT thread).
    // Keys embed thread_id so there's no collision — this purely reclaims
    // the old thread's staged/pinned entries so they don't linger.
    m.ui.view_cache.clear();
    m.d.current = Thread{};
    m.d.current.id = deps().new_thread_id();
    m.d.current.created_at = m.d.current.updated_at =
        std::chrono::system_clock::now();
    clear_frozen(m);
    // Close every modal that framed the OLD thread: the picker we acted
    // from, plus the palette / code-block picker whose contents belonged to
    // the departing thread's last reply.
    m.ui.panel.close<pn::ThreadList>();
    m.ui.panel.close<pn::Palette>();
    m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
    // Wipe the whole composer draft — a pasted-but-unsent image (or any
    // chip / queued message) belongs to the thread we're leaving. Leaking
    // it once carried an empty-bytes image attachment into the new thread's
    // first submit and 400'd the request.
    reset_composer_draft(m.ui.composer);
    // A fresh empty thread has no live turn — drop any streaming phase and
    // hand the kernel back so a mid-stream swap can't leave the wire running
    // against a thread that no longer exists.
    m.s.phase = phase::Idle{};
    // Smart-Mode per-THREAD routing state must not leak into the new thread:
    // complexity momentum (classify_score_with_context inherits a tier from
    // the PREVIOUS turn), the session cascade bias, and the last turn's
    // signature (outcome feedback would otherwise attribute the new thread's
    // first reply to the OLD thread's route). The learned per-workspace
    // priors (RoutingMemory) survive by design — they are cross-thread.
    m.s.smart_turn_complexity  = smart::Complexity::Standard;
    m.s.smart_effort_bias      = 0;
    release_to_kernel();
    // Re-warm the active provider's TLS socket. The launch-time prewarm in
    // main() has usually aged out of the pool by now — a user reads a reply,
    // composes, then hits ^N, and the 90 s idle TTL has evicted the warm
    // connection. Without this the FIRST turn of every new thread re-pays
    // the full DNS+TCP+TLS handshake (~150-300 ms) before its first SSE byte,
    // which reads as a per-new-thread lag. Opening the socket now overlaps
    // that cost with the user typing their first prompt. Non-blocking: spawns
    // a tracked background dial and returns immediately; a no-op when the pool
    // is already warm enough to serve the next request.
    provider::prewarm_active_provider();
    // Per maya's contract this is the ONE allowed wiring of reset_inline: an
    // explicit, user-initiated content swap. `\x1b[3J` wipes saved-lines
    // (including pre-agentty shell history), acceptable precisely because
    // the user asked to switch threads. Do NOT extend it to per-turn paths.
    return Cmd<Msg>::reset_inline();
}

Step thread_list_update(Model m, msg::ThreadListMsg tm) {
    return std::visit(overload{
        [&](OpenThreadList) -> Step {
            // Refresh in the background if no load is in flight — the
            // walk + parse is too slow (seconds, with hundreds of
            // multi-MB thread files) to do synchronously here. The
            // picker opens immediately against the cached list; new
            // entries fade in when ThreadsLoaded lands.
            Cmd<Msg> cmd = Cmd<Msg>::none();
            if (!m.s.threads_loading) {
                m.s.threads_loading = true;
                cmd = cmd::load_threads_async();
            }
            // Open AT the current thread, not row 0 — the user's mental
            // anchor is "where am I", and cycling from there (↑ newer /
            // ↓ older) mirrors the Alt+←/→ quick-cycle order.
            int at = 0;
            for (int i = 0; i < static_cast<int>(m.d.threads.size()); ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    at = i;
                    break;
                }
            m.ui.panel = pn::ThreadList{{at}};
            return {std::move(m), std::move(cmd)};
        },
        [&](CloseThreadList) -> Step {
            ascend(m);   // Esc: back to whatever opened this, or close
            return done(std::move(m));
        },
        [&](ThreadListMove& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return done(std::move(m));
            p->confirm_remove.clear();   // moving disarms a pending `d`
            int sz = static_cast<int>(m.d.threads.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListJump& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return done(std::move(m));
            p->confirm_remove.clear();   // jumping disarms a pending `d`
            int sz = static_cast<int>(m.d.threads.size());
            using W = ThreadListJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = sz - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(sz - 1, p->index + kPage); break;
            }
            return done(std::move(m));
        },
        // ── Model swap: commit overflow before swapping ──────────────
        //
        // ThreadListSelect and NewThread replace m.d.current wholesale.
        // Before the swap we dispatch Cmd::commit_scrollback_overflow()
        // — NOT force_redraw (see history below).
        //
        // Why commit-overflow is required:
        //   maya's inline diff treats rows [0, prev_rows - term_h) as
        //   committed scrollback ("updatable_start" in serialize.cpp).
        //   When the old thread overflowed (prev_rows > term_h) those
        //   rows are skipped by the diff scan and per-row emit. After
        //   a wholesale model swap the new thread's canvas rows at
        //   those Y positions are entirely different content — but
        //   the diff still considers them "scrollback, untouchable"
        //   and never emits them. Result: visible seam mid-viewport
        //   where the wire still holds old-thread bytes against the
        //   new-thread canvas, manifesting as two unrelated text
        //   fragments on adjacent rows.
        //
        //   commit_scrollback_overflow() calls into maya's
        //   commit_inline_overflow which advances prev_cells by
        //   max(0, prev_rows - term_h) rows. After it runs,
        //   prev_rows ≤ term_h, updatable_start drops to 0, and the
        //   diff scans the full common range — every visible row
        //   gets correctly emitted against the new thread.
        //
        //   The rows that scroll out of prev_cells are bytes the
        //   terminal already committed to its native scrollback
        //   anyway (they were emitted via bottom-edge \r\n's during
        //   streaming). commit just acknowledges that fact — zero
        //   wire effect.
        //
        // Why NOT force_redraw:
        //   Cmd::force_redraw demotes Synced → Stale, routing the
        //   next render through compose case (B). Case (B)'s
        //   scroll-to-fit branch (scroll_n > 0) emits \n at the
        //   viewport bottom when the new frame is taller than the
        //   old cursor's offset from viewport top — each \n there
        //   scrolls a row of whatever was on screen (old thread
        //   tail + host shell history above it) up into
        //   terminal-owned scrollback, permanently. History: commit
        //   8becb88 did exactly that and reverted in 0b24148.
        [&](ThreadListSelect) -> Step {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            Cmd<Msg> cmd = Cmd<Msg>::none();
            if (p) p->confirm_remove.clear();   // selecting disarms a pending `d`
            if (p && !m.d.threads.empty() && !m.s.thread_loading) {
                // Re-clamp: p->index can be stale if an async refresh shrank
                // the list since the last navigation (see ThreadListDelete).
                p->index = std::clamp(p->index, 0,
                                      static_cast<int>(m.d.threads.size()) - 1);
                const Thread& meta = m.d.threads[static_cast<std::size_t>(p->index)];
                // Same-thread re-select — closing the picker is the
                // only useful action. No async load: would just
                // reparse the same bytes and flash.
                if (meta.id == m.d.current.id) {
                    m.ui.panel.close<pn::ThreadList>();
                    return done(std::move(m));
                }
                m.s.thread_loading = true;
                // Warm the socket now so the first turn in the thread the user
                // is switching INTO doesn't re-pay the handshake (the pool's
                // idle TTL has usually evicted it during composer breathing
                // room). Non-blocking; no-op if already warm.
                provider::prewarm_active_provider();
                cmd = cmd::load_thread_async(meta.id);
            }
            m.ui.panel.close<pn::ThreadList>();
            return {std::move(m), std::move(cmd)};
        },
        [&](ThreadListDelete) -> Step {
            // `d` / `D` in the thread picker — two-press delete with
            // confirm_remove, mirroring SettingsListRemove / AccountRemove.
            // First press on a row marks it pending (⚠ badge in the view);
            // second press on the SAME row commits via deps().delete_thread().
            // Any move/jump/select/new/close disarms the pending state.
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || m.d.threads.empty()) return done(std::move(m));
            // Bounds-guard the cursor before indexing. Navigation handlers
            // clamp p->index on every move, but the thread list can be
            // mutated out from under the picker by an async refresh (or a
            // prior delete) that shrinks it, leaving a stale index that
            // points past the new end. Reading m.d.threads[idx] then is an
            // out-of-bounds access; the erase(begin()+idx) below would
            // compound it. Re-clamp into range instead of trusting p->index.
            const int sz_now = static_cast<int>(m.d.threads.size());
            const int idx = std::clamp(p->index, 0, sz_now - 1);
            p->index = idx;
            const Thread& target = m.d.threads[static_cast<std::size_t>(idx)];
            // Use the thread id as the confirm key — stable across title edits.
            const std::string key = target.id.value;
            if (p->confirm_remove != key) {
                p->confirm_remove = key;
                return done(std::move(m));
            }
            // Second press — commit. Snapshot everything we need OUT of the
            // vector element BEFORE erase(): the erase invalidates `target`,
            // so reading target.title / target.id afterward is a
            // use-after-free. Copy them here while the reference is live.
            const ThreadId  target_id = target.id;
            const bool      was_current = (target_id == m.d.current.id);
            const std::string label =
                target.title.empty() ? "(untitled)" : target.title;

            p->confirm_remove.clear();
            deps().delete_thread(target_id);
            m.d.threads.erase(m.d.threads.begin() + idx);
            // Clamp the cursor so it stays valid after removal.
            const int sz = static_cast<int>(m.d.threads.size());
            if (sz == 0) {
                p->index = 0;
            } else if (p->index >= sz) {
                p->index = sz - 1;
            }
            std::string msg = "deleted \"" + label + "\"";
            if (was_current) msg += " \xe2\x80\x94 started a new thread";
            auto toast = set_status_toast(m, std::move(msg));
            // Deleting the ACTIVE thread leaves m.d.current pointing at a
            // thread whose file no longer exists — swap to a fresh empty
            // thread through the SAME core NewThread uses. That single code
            // path is what guarantees the phase reset + kernel release (so a
            // mid-stream delete can't leave the wire running against a dead
            // thread), the modal/skill/cache teardown, and the reset_inline
            // that wipes the deleted thread's rendered turns off-screen.
            if (was_current) {
                auto reset = reset_to_fresh_thread(m);
                return {std::move(m),
                        Cmd<Msg>::batch(cmd::load_threads_async(),
                                        std::move(reset), std::move(toast))};
            }
            return {std::move(m), std::move(toast)};
        },
        [&](ThreadCycle& e) -> Step {
            // Alt+←/→ — jump to the adjacent thread without the picker.
            // Recency order (same as ^J): index 0 = newest; +1 = older,
            // -1 = newer, wrapping at both ends. Gated on an idle
            // session — swapping m.d.current under an active stream
            // would strand the in-flight ctx's writes.
            if (m.s.active()) {
                auto cmd = set_status_toast(m,
                    "wait for the reply to finish before switching threads");
                return {std::move(m), std::move(cmd)};
            }
            if (m.s.thread_loading) return done(std::move(m));
            const int sz = static_cast<int>(m.d.threads.size());
            if (sz == 0) {
                // History not loaded yet (or genuinely empty) — kick a
                // refresh so the NEXT press works, and say so.
                Cmd<Msg> cmd = Cmd<Msg>::none();
                if (!m.s.threads_loading) {
                    m.s.threads_loading = true;
                    cmd = cmd::load_threads_async();
                }
                auto toast = set_status_toast(m, "no other threads yet");
                return {std::move(m),
                        Cmd<Msg>::batch(std::move(cmd), std::move(toast))};
            }
            // Locate the current thread in the recency list. A fresh
            // unsaved thread isn't in it — treat "newest" as the anchor
            // so the first press lands on the most recent saved thread.
            int cur = -1;
            for (int i = 0; i < sz; ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    cur = i;
                    break;
                }
            int target;
            if (cur < 0) {
                target = (e.delta >= 0) ? 0 : sz - 1;
            } else {
                if (sz == 1) {
                    auto toast = set_status_toast(m, "only one thread");
                    return {std::move(m), std::move(toast)};
                }
                target = ((cur + e.delta) % sz + sz) % sz;
            }
            const Thread& meta = m.d.threads[static_cast<std::size_t>(target)];
            if (meta.id == m.d.current.id) return done(std::move(m));
            // Preserve the thread being left — same courtesy NewThread
            // extends. finalize_turn saves per turn, but a title edit or
            // an un-persisted tail shouldn't be lost to a quick cycle.
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            m.s.thread_loading = true;
            // Warm the socket for the switched-into thread's first turn.
            provider::prewarm_active_provider();
            // "thread k/N · title" — the positional readout that makes
            // repeated Alt+←/→ presses feel like flipping through a
            // deck rather than teleporting blind. Survives the swap
            // because ThreadLoaded doesn't touch m.s.status.
            auto toast = set_status_toast(m,
                "thread " + std::to_string(target + 1) + "/"
                    + std::to_string(sz) + " \xc2\xb7 "
                    + (meta.title.empty() ? "(untitled)" : meta.title));
            return {std::move(m),
                    Cmd<Msg>::batch(cmd::load_thread_async(meta.id),
                                    std::move(toast))};
        },
        [&](NewThread) -> Step {
            // Persist the outgoing thread before we drop it (delete's
            // active-row path does the opposite — it just removed the
            // thread, so it must NOT save). The shared reset below owns
            // everything after this policy decision.
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            auto reset = reset_to_fresh_thread(m);
            return {std::move(m), std::move(reset)};
        },
        [&](ThreadsLoaded& e) -> Step {
            m.d.threads = std::move(e.threads);
            m.s.threads_loading = false;
            // If the thread picker is open, its cursor may now point past the
            // end of the freshly-loaded (possibly shorter) list. Re-clamp so
            // the view and every ThreadList* handler index safely.
            if (auto* p = m.ui.panel.get<pn::ThreadList>()) {
                const int sz = static_cast<int>(m.d.threads.size());
                p->index = sz > 0 ? std::clamp(p->index, 0, sz - 1) : 0;
            }
            return done(std::move(m));
        },
        [&](ThreadLoaded& e) -> Step {
            // Result of the async single-thread load kicked off by
            // ThreadListSelect. Empty Thread (default ThreadId) means
            // the disk read or parse failed; just clear the spinner
            // and leave the current thread in place.
            m.s.thread_loading = false;
            if (e.thread.id.value.empty()) return done(std::move(m));
            // Old thread's skill activations leave context with it.
            tools::skills::reset_activations();
            // Smart-Mode per-thread routing state belongs to the departing
            // thread too — same reset as reset_to_fresh_thread (momentum,
            // cascade bias, outcome-feedback signature). Without it the
            // loaded thread's first turn inherits the OLD thread's tier
            // momentum and its first follow-up trains the old signature.
            m.s.smart_turn_complexity  = smart::Complexity::Standard;
            m.s.smart_effort_bias      = 0;
            // Optional timing probe. AGENTTY_LOAD_PROF=1 keeps surfacing
            // the synchronous portion of the load (rehydrate +
            // release_to_kernel) that still lives on the UI thread.
            const bool prof = []{
                static const bool on = [] {
                    const char* e = std::getenv("AGENTTY_LOAD_PROF");
                    return e && *e && *e != '0';
                }();
                return on;
            }();
            std::FILE* prof_out = nullptr;
            if (prof) prof_out = std::fopen("/tmp/agentty-load-prof.log", "a");
            auto stamp = [&](const char* tag, auto t0) {
                if (!prof_out) return;
                auto dt = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::fprintf(prof_out, "[load-async] %s: %.2f ms\n", tag, dt);
                std::fflush(prof_out);
            };
            m.d.current = std::move(e.thread);
            // Drop the whole render cache — same rationale as NewThread:
            // the entries belong to the thread being left, which won't
            // freeze again. The loaded thread rebuilds its frozen prefix
            // via rehydrate_frozen below and repopulates the cache lazily.
            m.ui.view_cache.clear();
            // Wipe the composer draft — same rationale as NewThread: a
            // pasted-but-unsent image / chip / queued message belongs to
            // the thread being left, and the leftover image Attachment has
            // empty bytes (drained into a prior Message), which serializes
            // an empty image block and 400s the next submit.
            reset_composer_draft(m.ui.composer);
            auto t1 = std::chrono::steady_clock::now();
            rehydrate_frozen(m);
            stamp("rehydrate_frozen", t1);
            // Frozen scrollback was just built from cold; the very
            // first render() would otherwise pay full layout+paint
            // over every frozen Turn. Flip the warmup flag so maya's
            // run loop pre-warms the component cache before the
            // wire-bound render — see Program::needs_warmup hook.
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            // Arm the one-shot post-paint trim: the rehydrate budget used
            // ESTIMATED heights; the first paint records real ones into the
            // ledger, and the Tick arm re-trims against those. The Tick
            // subscription gates on this flag (subscribe.cpp) until it fires.
            m.ui.pending_rehydrate_trim = !m.ui.frozen.empty();
            auto t2 = std::chrono::steady_clock::now();
            release_to_kernel();
            stamp("release_to_kernel", t2);
            if (prof_out) {
                const auto _ts = maya::platform::query_terminal_size(
                    maya::platform::stdout_handle());
                std::fprintf(prof_out,
                    "[load-async] msgs=%zu frozen=%zu frozen_rows=%zu "
                    "frozen_through=%zu term_h=%d\n",
                    m.d.current.messages.size(),
                    m.ui.frozen.size(),
                    m.ui.frozen.row_total(),
                    m.ui.frozen_through,
                    _ts.height.value);
                std::fflush(prof_out);
                std::fclose(prof_out);
            }
            // Wholesale model swap into the loaded thread. Same
            // rationale as NewThread above: the previous thread's
            // overflow rows are committed to native scrollback and only
            // reset_inline (which emits `\x1b[2J\x1b[3J\x1b[H`) can
            // erase them. Without it the previous thread's tail turns
            // are visible above the rehydrated thread's first turn.
            //
            // Per maya/app/app.hpp reset_inline() docs: this is the
            // sanctioned recovery for thread switch / new thread. The
            // `\x1b[3J` cost (wipes the user's pre-agentty shell
            // scrollback) is acceptable because the user explicitly
            // asked for the content swap (picker select).
            return {std::move(m), Cmd<Msg>::reset_inline()};
        },
    }, tm);
}

} // namespace agentty::app::detail
