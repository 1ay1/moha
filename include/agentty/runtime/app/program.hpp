#pragma once
// agentty::app::Program — the maya runtime binding.
//
// Forwards to the per-domain reducer / view / subscribe.  The init function
// reads settings + recent threads through the Store seam.

#include <chrono>
#include <cstdint>

#include <maya/maya.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"  // detail::live_tail_reveal_settled
#include "agentty/runtime/view/composer.hpp"        // composer_uses_hardware_caret
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/visual_parts.hpp"   // the structural hash walk
#include "agentty/runtime/visual.hpp"
#include "agentty/runtime/view/view.hpp"

namespace agentty::app {

namespace pn = agentty::ui::panel;   // the exclusive overlay slot's alternatives

[[nodiscard]] std::pair<Model, maya::Cmd<Msg>> init();

struct AgenttyApp {
    using Model = ::agentty::Model;
    using Msg   = ::agentty::Msg;

    static std::pair<Model, maya::Cmd<Msg>> init() { return ::agentty::app::init(); }

    static auto update(Model m, Msg msg) -> std::pair<Model, maya::Cmd<Msg>> {
        return ::agentty::app::update(std::move(m), std::move(msg));
    }

    static maya::Element view(const Model& m) {
        return ::agentty::ui::view(m);
    }

    static auto subscribe(const Model& m) -> maya::Sub<Msg> {
        return ::agentty::app::subscribe(m);
    }

    // Optional Program hook (see maya/app/app.hpp — detail::HasVisualHash).
    // The runtime calls this just before view(); when the hash is
    // unchanged from the previous render, view() + render() are skipped
    // entirely. Captures the axes that affect what the user can see;
    // intentionally omits things like Session::last_tick that change
    // every Tick without changing pixels.
    //
    // Time-driven animations (cursor blink, streaming caret pulse,
    // spinner) are bucketed at coarse intervals so each visible step
    // advances the hash. The bucket size is the upper bound on how
    // often we'll render purely for animation.
    //
    // ENFORCED CONTRACT — this is NOT a place to rely on care alone.
    // tests/visual_hash_coverage_test.cpp holds two declarative
    // tables: every view-affecting model axis must advance this hash,
    // and every non-visual axis (last_tick, token counters) must NOT.
    // If you add a `mix()` for a new field, add a matching row to that
    // test's kVisualAxes; if you add ephemeral state the view ignores,
    // add it to kInvariantAxes. Forgetting to mix a view-axis is a
    // SILENT dead region in production — the test converts it to a
    // loud CI failure.
    static std::uint64_t visual_hash(const Model& m) {
        std::uint64_t k = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) {
            k = (k ^ v) * 1099511628211ULL;
        };
        auto mix_str = [&](std::string_view s) {
            mix(s.size());
            // Don't hash every byte for long strings — a 50 KB tool
            // output would dominate the per-frame budget. Sample a
            // few stable offsets; combined with size + render_keys
            // computed elsewhere it's enough to detect change.
            if (!s.empty()) {
                mix(static_cast<std::uint8_t>(s.front()));
                mix(static_cast<std::uint8_t>(s.back()));
                if (s.size() >= 16)
                    mix(static_cast<std::uint8_t>(s[s.size() / 2]));
            }
        };

        // ── Domain.
        //
        // The frozen prefix — messages[0 .. ui.frozen_through) — is
        // an immutable archaeology layer (see m.ui.frozen). Its
        // contribution to the visual is fully captured by the
        // structural pair (frozen.size(), frozen_turn) which advance
        // only at freeze instants. Per-message render_keys for the
        // frozen range cannot change — the with_live_tool gate
        // refuses to mutate them — so iterating them every frame
        // would burn CPU for zero signal. Hash only the live tail.
        mix(m.d.current.messages.size());
        mix(static_cast<std::uint64_t>(m.ui.frozen.size()));
        mix(static_cast<std::uint64_t>(m.ui.frozen_turn));
        for (std::size_t i = m.ui.frozen_through;
             i < m.d.current.messages.size(); ++i) {
            mix(m.d.current.messages[i].compute_render_key());
        }
        mix(static_cast<std::uint64_t>(m.d.profile));
        // Reasoning effort is rendered in the model badge (status bar), so a
        // tier change (←/→ in the picker) must move the hash or the gate would
        // skip the repaint. Also affects the picker's reasoning line.
        mix(static_cast<std::uint64_t>(m.d.effort));
        mix_str(m.d.model_id.value);
        mix(m.d.pending_permission ? 1ULL : 0ULL);

        // ── Session / phase.
        mix(static_cast<std::uint64_t>(m.s.phase.index()));
        mix_str(m.s.status);
        // Compaction summary streams into an off-transcript buffer; the
        // activity tape narrates its bytes live (conversation.cpp →
        // Config::stream). Size is the right term: every delta grows it,
        // and the tape's visible window is a pure function of the bytes.
        // Without this the tape freezes between unrelated repaints — the
        // same "changed but not hashed" class the walk work closed.
        mix(m.s.compaction_buffer.size());
        // Status expiry: bucket at 100 ms so the gauge’s rolling
        // counter doesn't force a render every microsecond.
        if (m.s.status_until.time_since_epoch().count() != 0) {
            const auto until_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    m.s.status_until.time_since_epoch()).count();
            mix(static_cast<std::uint64_t>(until_ms / 100));
        }
        // Spinner frame bucketed at 10 (its cycle length). Same bucket
        // size the turn-level AgentTimeline cache uses, so a hash
        // advance here corresponds to a new visual.
        //
        // The condition MUST match the advance gate in update/meta.cpp's
        // Tick arm: a frame that advances but isn't hashed animates
        // invisibly (no repaint), and one hashed but not advanced burns
        // renders for an unchanged glyph. Both cases are live: streaming
        // turns AND the fused picker waiting on provider catalogs.
        if (m.s.active() || m.s.models_loading
            || m.loading_spinner_visible()) {
            mix(static_cast<std::uint64_t>(m.s.spinner.frame_index() % 10));
        }

        // ── Composer / UI.
        mix_str(m.ui.composer.text);
        mix(static_cast<std::uint64_t>(m.ui.composer.cursor));
        mix(m.ui.composer.attachments.size());
        mix(m.ui.composer.queued.size());
        mix(m.ui.composer.expanded ? 1ULL : 0ULL);

        // ── Modal / picker state.
        //
        // CRITICAL: every modal's open/closed state AND its in-modal
        // cursor/query must feed the hash. These pickers are
        // selection-driven — pressing ↑/↓ only mutates an index (or a
        // query string) inside the variant; nothing else in the model
        // moves. If that index isn't hashed, ModelsMove produces a
        // model the gate considers visually identical, skip_render fires,
        // and the cursor doesn't repaint until some OTHER hashed axis
        // (caret-blink parity, status text) happens to flip ~265 ms later.
        // Symptom: "press 4-5 times, registers once."
        //
        // variant::index() captures Closed-vs-Open (and login's
        // sub-states); the OpenAt index / palette query+index capture the
        // cursor movement within an open picker.
        // The exclusive overlay slot: its variant index captures WHICH
        // overlay is open (None-vs-any and picker-vs-picker); the per-
        // alternative blocks below add cursor/query movement INSIDE the
        // open overlay.
        mix(static_cast<std::uint64_t>(m.ui.panel.raw().index()));
        visual::mix_any(mix, m.ui.panel.raw());

        // Panel-adjacent state OUTSIDE the slot value that the panel views
        // read — walked with the same machinery (their types carry
        // visual_parts where a member is derived cache or a clock):
        //   • fused catalogs + row cache — ONLY while the models panel is
        //     open (they are its async data; the catalogs persist after
        //     close as a warm cache, and walking 8×60 models per idle tick
        //     for a closed panel is pure waste — measured ~5µs vs ~540ns
        //     for the whole slot).
        if (m.ui.panel.is<pn::Models>()) {
            visual::mix_any(mix, m.d.provider_catalogs);
            visual::mix_any(mix, m.d.fused_rows);
        }
        //   • the todo modal: open/closed AND the items — the agent
        //     rewrites items mid-stream while the modal is open, which was
        //     hash-invisible (the exact forgotten-facet class, found by
        //     this migration).
        visual::mix_any(mix, m.ui.todo);
        //   • the plugins snapshot the settings list projects
        mix(m.ui.plugins_loading ? 1ULL : 0ULL);
        visual::mix_any(mix, m.ui.plugins);
        //   • body-scroll ScrollStates: walked via their parts list (x/y
        //     visible, render plumbing exempt). Gated to the panel that
        //     reads each, like the catalogs above.
        if (m.ui.panel.is<pn::ToolOutput>()) {
            visual::mix_any(mix, m.ui.tool_viewer_scroll);
            mix(m.ui.tool_viewer_tail ? 1ULL : 0ULL);   // loose Model bool
        }
        if (m.ui.panel.is<pn::CodeBlockResult>())
            visual::mix_any(mix, m.ui.code_blocks_scroll);

        // Login: its own variant outside the slot; same walk, same
        // guarantees (secret buffers digest length-only via parts lists).
        visual::mix_any(mix, m.ui.login);

        // Time-driven animation buckets. Each bucket flip forces a
        // render via hash advance. The bucket size is the FLOOR on
        // how often we'll re-render purely for animation; the actual
        // wake-up cadence is driven by the widget frame-request
        // scheduler (maya request_animation_frame() — the single
        // animation clock), so a tight bucket here costs nothing when
        // no animation is live.
        //
        // CRITICAL: the hash bucket must be PHASE-LOCKED to whatever
        // animation is actually on screen, otherwise the two beat
        // against each other and frames get skipped — the symptom is a
        // caret that blinks smoothly sometimes and freezes-until-
        // keypress other times. The render gate (skip_render when the
        // hash is unchanged) means: if the hash doesn't advance on the
        // exact frame an animation toggles, the widget's build() never
        // runs, so its request_animation_frame() is never re-armed, the
        // frame request is never re-issued, and the loop sleeps the
        // full idle timeout until the next keypress. So the bucket has
        // to step once per visible animation transition, no faster, no
        // slower.
        //
        // Three regimes:
        //   (a) fine animation live (spinner / streaming caret / welcome
        //       bob / queued-chip pulse): step at the SAME cadence the Tick
        //       subscription wakes the loop. On DEC-2026 terminals that's
        //       33 ms (~30 fps) and the synchronized-output wrapper makes
        //       each frame swap atomically — smooth, no tearing. On
        //       terminals WITHOUT mode 2026 (Apple Terminal, plain xterm,
        //       tmux without sync passthrough) every multi-row repaint
        //       paints progressively, so the chrome at the bottom of the
        //       frame (spinner / sparkline / status bar) visibly tears on
        //       each render. There the Tick already drops to 100 ms
        //       (subscribe.cpp), but this bucket must MATCH it: a 33 ms
        //       bucket against a 100 ms tick advances ~3x per wake, so the
        //       skip-render gate fires a torn repaint on every tick anyway
        //       and the 100 ms throttle buys nothing. Locking the bucket
        //       to the tick period means exactly one render per tick —
        //       ~10 fps of torn frames instead of ~10 redundant ones, and
        //       phase-locked so renders don't double up. The spinner is
        //       already capped at 10 fps by the tick on those terminals,
        //       so perceived smoothness is unchanged; we only stop the
        //       extra tearing repaints. On sync terminals the period is
        //       still 33 ms — UX identical to before.
        //   (b) idle with the composer caret blinking: lock to the blink
        //       HALF-period (265 ms = 530 ms / 2) so every hash step is
        //       exactly one caret toggle. This keeps the frame-request
        //       loop self-sustaining (each render re-issues the
        //       composer's frame request) and the blink perfectly
        //       regular, at ~4 renders/sec.
        //   (c) nothing animating at all (no caret — e.g. a modal owns
        //       focus): no time bucket, so a settled screen does ZERO
        //       idle renders until an event arrives.
        // Read the ANIMATION clock, not steady_clock directly.
        //
        // Every time-driven phase in this app reads maya::anim_now_ms() — it
        // is documented as the only clock they may use — because it carries
        // the test seams: advance_anim_clock_ms() to move time on purpose,
        // and a FREEZE that pins it so a synchronous render sequence is a
        // pure function of the model.
        //
        // Reading steady_clock here bypassed both. The buckets below then
        // sampled real wall time, so two visual_hash() calls on an
        // unchanged model could straddle a bucket boundary and disagree —
        // an invariant test asserting "an idle picker does NOT animate"
        // fails whenever the scheduler puts a bucket edge between its two
        // calls. That is why those cases only flaked under `ctest -j12`:
        // load widens the gap between the calls, it does not change the
        // logic. Same class of bug as the reveal-timing one already fixed
        // in turn.cpp; this was the last direct clock read in the render
        // path.
        const auto now_ms = maya::anim_now_ms();

        // The WELCOME screen paces ITSELF, so we add no time term for it.
        //
        // welcome_screen.hpp is RAF-driven: it runs its cascade at 60 fps,
        // then calls request_animation_frame_after(110) to step the bob at
        // ~9 fps. maya's run loop already guarantees those frames render —
        // its RAF override bypasses this hash precisely so a widget can
        // never be stranded by a host's bucket coverage gap.
        //
        // So a bucket here is not just redundant, it is harmful: it is a
        // SECOND clock for one visual. This used to bucket at the streaming
        // tick (80 ms over ssh) against the widget's 110 ms — 42 hash
        // values per 30 requested frames, with the extra renders landing
        // part-way through an animation step. That is what made an idle
        // welcome screen flicker instead of bobbing.
        //
        // maya::animation_pending() reports whether a widget already asked
        // for the next frame. When it has, the loop is scheduled on ITS
        // cadence and will render regardless of what we return — so we stay
        // out of the way. Asking maya beats hard-coding its interval: the
        // widget can change its pacing and this stays correct.
        const bool widget_paced = maya::animation_pending();

        const bool fine_anim_live =
            m.s.active()                              // spinner / streaming caret
            || !m.ui.composer.queued.empty();         // queued-chip pulse
        // The composer caret: ask, don't re-derive.
        //
        // This used to be `caret_blinking = !m.s.active()`, with a comment
        // claiming "maya's own gate is simply !active, so match it
        // exactly". That was wrong, and it is the last instance of the bug
        // that ran through this whole file: a host restating a condition
        // that belongs to a widget.
        //
        // maya's real gate is `!active && !hardware_caret` — with the
        // HARDWARE caret the terminal owns the blink, so maya paints
        // nothing, schedules nothing, and there is no visible step for the
        // hash to track. Mixing a 265 ms parity anyway meant the hash
        // flipped ~4x/sec on a screen where nothing moved. The run loop
        // dutifully repainted each time, redrawing the composer under a
        // caret the terminal was blinking on its own schedule — the two
        // clocks beat, and the caret visibly flickered.
        //
        // The composer config already carries the resolved answer (it is
        // what we hand maya), so read it instead of guessing: blink only
        // when maya will actually PAINT a blinking caret.
        constexpr std::int64_t kBlinkHalfMs = 265;
        const bool caret_blinking =
            !m.s.active() && !ui::composer_uses_hardware_caret(m);
        // Fine-animation bucket period. PHASE-LOCKED to the Tick
        // subscription by construction: it is the SAME value
        // streaming_tick_period() hands subscribe.cpp for the
        // `Sub::every(period, Tick{})` interval, so the hash advances
        // exactly once per loop wake (33 ms sync / 100 ms non-sync /
        // ≥ 80 ms SSH). Sharing the one function eliminates the old
        // hand-maintained duplicate heuristic here — which silently
        // omitted the SSH floor and could beat against the real tick.
        const std::int64_t kFineAnimMs = streaming_tick_period().count();

        // Streaming-text render bucket. While an assistant message is
        // actively streaming, the live edge runs maya's reveal_fx
        // (scramble→resolve + gradient + caret), a ~60 fps animation that
        // wants a render on every RAF wake (16 ms). The host also re-feeds
        // the full arrived source each frame, so newly arrived bytes show
        // up promptly. If we bucketed this at the tick period (100 ms on
        // non-sync terminals) the render gate would SKIP the intervening
        // RAF wakes — view() wouldn't run, the live-edge FX would freeze,
        // and a chunk of newly arrived text would pop in at once on the
        // next 100 ms flip (the "stuck then burst" symptom). Stepping the
        // bucket at the RAF interval renders each 16 ms wake. The
        // spinner-only case (tool running, no streaming_text) keeps the
        // calmer tick-period bucket so non-sync terminals don't tear the
        // chrome at 60 fps.
        // Reveal-frame demand: ask the shared predicate, don't re-derive.
        // This block used to restate the whole condition inline (active +
        // tail role + streaming_text/pending_stream + the reasoning-channel
        // term), and the reasoning term was BORN from that restatement
        // drifting: it was added here after "Thinking gets stuck" shipped,
        // while subscribe.cpp's copy still lacked it. One definition —
        // reveal_needs_frames in subscribe.hpp — now serves both.
        const bool revealing_text = reveal_needs_frames(m);
        // Reveal render cadence.
        //
        // On a SYNC-output terminal (DEC mode 2026: kitty / ghostty /
        // wezterm / foot / VTE 0.62+ / Windows Terminal / Konsole 22.04+)
        // each multi-row frame swaps atomically, so 16 ms / 60 fps gives a
        // smooth tear-free typewriter — keep it.
        //
        // On a NON-SYNC terminal (Apple Terminal, plain xterm, tmux w/o
        // sync passthrough, and — the reported repro — the default Termux /
        // Android terminal) every multi-row repaint paints progressively,
        // so the live edge ALREADY tears on each render regardless of rate:
        // 60 fps buys ZERO extra smoothness there, it only floods a slow
        // emulator with ~6x the ANSI diff bytes it can composite. On a
        // constrained device (slow ARM + a heavyweight terminal app) that
        // flood is exactly the "streaming gets stuck" symptom — the display
        // falls seconds behind the model because the paint/wire pipe can't
        // drain 60 fps of frames. The Tick subscription already wakes at
        // only 100 ms (streaming_tick_period()) on these terminals, and the
        // RAF override forces a render on the intervening 16 ms wakes for no
        // visible gain. Match the reveal bucket to the tick period so the
        // hash advances ONCE per tick: exactly one render per wake, the
        // freshly arrived bytes still show on that very next tick (no "burst"
        // — the tick period is the byte-delivery cadence anyway), and the
        // redundant torn repaints stop. The reveal SPEED is unchanged (the
        // pacer is bytes/second, not bytes/frame), so prose fills at the same
        // wall-clock rate, just in fewer frames. Sharing streaming_tick_
        // period() keeps this phase-locked to the Tick by construction — the
        // same guarantee kFineAnimMs relies on.
        const std::int64_t kRevealBucketMs =
            maya::ansi::env_supports_synchronized_output()
                ? 16                              // sync: 60 fps, atomic swap
                : streaming_tick_period().count(); // non-sync: 1 render / tick

        // POST-STREAM SETTLE bucket. After StreamFinished the phase goes
        // Idle and streaming_text drains into `text`, so BOTH m.s.active()
        // and `revealing_text` go false. finalize_turn settles every tail
        // message IMMEDIATELY (settle_message_md → finish(), the
        // agent_session MessageStop discipline — NO ~200 ms glide) and sets
        // pending_settle_freeze so meta.cpp's next Tick performs the freeze
        // once live_tail_reveal_settled(). That deferred-freeze Tick — and
        // maya's subsequent live-tail→frozen shrink reconciliation — advance
        // ONLY inside frames that actually render, and the run loop's
        // visual_hash gate skips any frame whose hash didn't move
        // (maya/app/app.hpp). Without a fast time term here the hash falls
        // to the caret-blink PARITY bucket (one flip / 265 ms): the freeze
        // Tick and the post-freeze reconciliation frames get gated away,
        // the collapse never finishes, and a duplicate turn is stranded in
        // scrollback. pending_settle_freeze marks the one Tick until the
        // freeze fires (finalize_turn sets it; meta.cpp's Tick clears it),
        // so key the fast bucket on it to keep ticking the 16 ms clock
        // until the freeze handoff lands.
        // The reveal-drain time bucket must keep ticking through BOTH the
        // pre-freeze settle (pending_settle_freeze) AND the post-freeze
        // settle cooldown (settle_cooldown_ticks). The cooldown frames are
        // where maya reconciles the live-tail→frozen collapse (its
        // detect→commit→demote→repaint shrink chain). request_animation_
        // frame() alone CANNOT drive them: the run loop's visual_hash gate
        // skips any frame whose hash didn't move (maya/app/app.hpp), so
        // without advancing the hash here those frames are gated away and
        // the collapse never finishes reconciling — leaving a stranded
        // duplicate turn in scrollback. Advancing the hash while the
        // cooldown counts down makes each armed Tick actually render.
        //
        // THIRD term — an in-flight reveal that has ALREADY drained its
        // wire bytes (`!detail::live_tail_reveal_settled(m)`). This closes
        // the "freeze then everything-at-once" (symptom 2) hole. When the
        // wire completes a text block, its bytes land in streaming_text and
        // — at StreamTextBlockClosed / a tool round-trip — get committed
        // into `text` while the widget's reveal cursor is STILL gliding to
        // the edge (is_finalizing / reveal_in_progress). In that window
        // BOTH m.s.active() and `revealing_text` (which keys off
        // streaming_text/pending_stream being non-empty) can be false, and
        // pending_settle_freeze may not be set yet. The subscription keeps
        // the Tick armed off the SAME predicate (`!live_tail_reveal_settled`
        // in subscribe.cpp) and cached_markdown_for keeps re-arming RAF —
        // but if the hash falls through to the 265 ms caret-blink parity
        // bucket, the run loop's visual_hash gate SKIPS every one of those
        // armed frames, view() never runs, the typewriter freezes, and the
        // remaining tail snaps in on the next caret flip / keypress. Keying
        // the fast bucket on the exact predicate that keeps the frame
        // sources armed guarantees each of those frames actually renders,
        // so the reveal glides continuously to the edge over SSH / any
        // link where the wire goes quiet mid-reveal.
        // Keying the fast bucket on the exact predicate that keeps the
        // frame sources armed guarantees each of those frames actually
        // renders — reveal_draining (subscribe.hpp) is that predicate, the
        // SAME one the Tick subscription arms on via animation_demand, so
        // the two gates cannot drift: a frame source the subscription
        // keeps alive is a frame this hash lets through.
        const bool draining_reveal = reveal_draining(m);

        if (revealing_text || draining_reveal) {
            mix(static_cast<std::uint64_t>(now_ms / kRevealBucketMs));
        } else if (widget_paced) {
            // A widget owns the next frame. maya will render it on that
            // widget's schedule regardless of this hash, so contribute NO
            // time term — a second clock here is what produces the beat.
        } else if (fine_anim_live) {
            mix(static_cast<std::uint64_t>(now_ms / kFineAnimMs));
        } else if (caret_blinking) {
            // Phase-locked: feed the blink PARITY, not a time bucket, so
            // the hash advances on exactly the same boundary maya uses
            // to flip the caret cell. Beat-free by construction.
            mix(static_cast<std::uint64_t>((now_ms / kBlinkHalfMs) & 1));
        }
        // else: nothing animating — contribute no time term at all.

        return k;
    }

    // Optional Program hook (see maya/app/app.hpp — detail::HasNeedsWarmup).
    // Returns true when the next view() result contains a freshly
    // rehydrated frozen scrollback whose cells haven't been captured
    // into maya's component cache yet. The runtime fires a one-shot
    // off-wire warmup_render() before the wire-bound render, which
    // converts the user-visible first frame from O(content) to O(blit)
    // — typically 50–660 ms to <1 ms on tool-heavy thread resume.
    //
    // The flag is set in the reducer (e.g. ThreadLoaded handler in
    // picker.cpp). Maya's loop edge-detects it (rising-edge fires
    // warmup, falling-edge resets the latch), so it's safe to leave
    // the flag set across subsequent reducer steps; we just won't
    // re-warm until it goes false then true again.
    static bool needs_warmup(const Model& m) {
        return m.ui.needs_warmup_render;
    }
};

static_assert(maya::Program<AgenttyApp>);

} // namespace agentty::app
