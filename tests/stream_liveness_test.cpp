// stream_liveness_test — UX-critical regression: the live-edge caret
// (maya reveal_fx) must NEVER look frozen while a model response is in
// flight. The user-visible failure this locks out: a slow model pauses
// between deltas (median ~470 ms, worst seconds) and the typewriter
// caret stops pulsing mid-sentence — reading as "the stream died".
//
// Root cause history (commits 498ec9e → a9760e7): the RAF re-arm gate in
// turn.cpp's cached_markdown_for was a fixed byte-recency timeout
// (250 ms, then 3 s). ANY fixed timeout can be out-run by a slower
// model or laggy link. The robust gate keys off m.s.is_streaming() —
// the variant-backed phase::Streaming signal — so the caret stays armed
// unconditionally while the wire is open, and drops only when the phase
// leaves Streaming.
//
// Contract asserted here, via maya::detail::animation_requested_ (the
// thread-local that request_animation_frame() sets and the run loop
// reads to schedule the next ~16 ms repaint):
//
//   1. While phase == Streaming and the message has live bytes, EVERY
//      view build re-arms the animation frame — even after a simulated
//      10-second gap with zero new bytes (out-runs any timeout anyone
//      might reintroduce).
//   2. After the stream settles (phase → Idle, streaming_text drained
//      into text), the widget finishes its ~200 ms finalize ramp and
//      then STOPS re-arming — the idle loop must return to zero wakes
//      (the other half of the contract: no frozen caret, but also no
//      60 fps burn at idle).
//
// If this test fails on (1), someone re-introduced a timeout race —
// the "stream looks dead" bug is back. If it fails on (2), the caret
// never disarms and idle CPU burns.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "agtest.hpp"

#include <maya/app/app.hpp>            // maya::detail::animation_requested_
#include <maya/core/anim_clock.hpp>     // maya::testing::advance_anim_clock_ms
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/thread.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;

// Render the CONVERSATION region only (frozen + live tail) and report
// whether any widget in it requested an animation frame during build.
// Mirrors the run loop: clear the flag, build + paint, read the flag.
//
// Deliberately NOT the full AppLayout: the composer's idle cursor blink
// (maya composer.hpp) re-arms the animation frame on every idle build,
// which would mask the markdown caret's disarm in test (2). The caret
// under test lives in the thread region; probe exactly that.
static bool frame_requests_animation(const Model& m,
                                     int width = 100, int height = 40) {
    maya::detail::animation_requested_ = false;

    auto root = maya::Thread{agentty::ui::thread_config(m)}.build();

    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    return maya::detail::animation_requested_;
}

// Deterministic reveal drain. The maya reveal cursor advances by
// anim_now_ms() (maya/core/anim_clock.hpp), so a test drives it by
// ADDING to the test-only anim skew instead of sleeping against a
// wall-clock deadline — the old sleep_for(16ms) + steady_clock::now()
// deadline pattern flakes under CPU starvation (wall time burns while
// the process is descheduled, the deadline expires before the ramp
// finishes). Here each step advances exactly 16 ms of anim time and
// runs `step` (which must call build() to consume it). Bounded to
// `max_steps` frames (default 600 = 9.6 s of anim time, far past any
// ramp) so a genuinely stuck reveal still terminates the loop with the
// predicate true, tripping the caller's CHECK.
template <class Pred, class Step>
static void drain_reveal(Pred still_animating, Step step,
                         int max_steps = 600) {
    for (int i = 0; i < max_steps && still_animating(); ++i) {
        maya::testing::advance_anim_clock_ms(16);
        step();
    }
}

// ── (1) The caret must stay armed across an arbitrarily long inter-
//        delta gap while the phase says Streaming. ──────────────────────
TEST_CASE("caret armed across delta gap") {
    std::printf("test_caret_armed_across_delta_gap\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"liveness"};
    Message u; u.role = Role::User; u.text = "explain something";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // In-flight assistant message: first delta arrived, wire still open.
    // Body kept SHORT so the reveal cursor catches up to the live edge
    // quickly — the gap probe below must isolate the RAF gate, and
    // reveal_in_progress() (cursor still gliding backlog) arms the frame
    // independently of it, which would mask a broken gate.
    Message a; a.role = Role::Assistant;
    a.streaming_text = "Hi";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Frame right after the delta: must be armed (bytes just grew).
    CHECK(frame_requests_animation(m),
          "caret armed on the frame after a delta lands");

    // Let the typewriter reach the live edge (2 cp at 30 cps ≈ 70 ms;
    // generous deadline). Once reveal_in_progress() is false, the ONLY
    // legitimate arming source left while the wire is open is the
    // phase gate under test.
    // NB: the entry migrates between the pinned (live) and settled maps
    // as its lifecycle flips, and in the two-map ViewCache a migration is
    // a node transfer that invalidates any long-lived reference. So the
    // drain re-fetches through the NON-migrating peek() each step instead
    // of caching an Entry& across frame_requests_animation calls.
    const auto rip = [&] {
        const auto* mc =
            m.ui.view_cache.peek(m.d.current.id, m.d.current.messages.back().id);
        return mc && mc->streaming && mc->streaming->reveal_in_progress();
    };
    drain_reveal(rip, [&] { (void)frame_requests_animation(m); });  // build() advances the cursor
    CHECK(!rip(), "reveal cursor reached the live edge (test precondition)");

    // Simulate the killer gap: NO new bytes, wire still open. TWO
    // clocks must lapse for the probe to be honest:
    //   • the widget's internal recency window (maya reveal_fx,
    //     age_at_tail_ms ≤ 250 ms keeps it self-arming every build) —
    //     defeated by advancing the anim clock (maya reads anim_now_ms);
    //   • agentty's cache recency window (kRevealActiveMs = 3 s) —
    //     agentty reads REAL steady_clock there, so backdate the stamp
    //     directly, exactly what a slow model's gap does to it.
    // Past both windows, maya's quiescent regime arms only once per
    // 33/100 ms phase bucket — NOT every frame — so consecutive
    // same-bucket builds return false unless agentty's phase gate
    // (wire_streaming_here) holds the caret armed. That's the gate
    // under test.
    maya::testing::advance_anim_clock_ms(400);
    m.ui.view_cache
        .message_md_live(m.d.current.id, m.d.current.messages.back().id)
        .last_grow_tick_ms =
        maya::anim_now_ms() - 10000;   // simulate a 10 s inter-delta gap

    // Six back-to-back frames inside the gap — EVERY one must re-arm.
    // A timeout-only gate leaves same-phase-bucket frames unarmed and
    // fails here (verified by sabotaging the gate to timeout-only).
    for (int f = 0; f < 6; ++f) {
        CHECK(frame_requests_animation(m),
              "caret STILL armed mid-gap (400 ms + simulated 10 s, "
              "zero new bytes, phase=Streaming) — a timeout race fails here");
        maya::testing::advance_anim_clock_ms(2);
    }

    // Sanity: the gate is the phase, not the backdated clock. Flip the
    // phase to Idle while bytes remain unsettled (cancel-like edge) —
    // the reveal cursor may still glide its backlog out, so arming may
    // persist transiently, but the UNCONDITIONAL guarantee is gone.
    // (No CHECK here: transient glide is legitimate. The hard assert
    // for disarm is test_caret_disarms_after_settle.)
}

// ── (2) After settle, the caret must disarm once the finalize ramp
//        completes — idle must not burn frames forever. ────────────────
TEST_CASE("caret disarms after settle") {
    std::printf("test_caret_disarms_after_settle\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"liveness2"};
    Message u; u.role = Role::User; u.text = "explain something";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    Message a; a.role = Role::Assistant;
    a.streaming_text = "Short reply body.";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    CHECK(frame_requests_animation(m), "armed while streaming");

    // Stream finishes: reducer moves streaming_text → text, phase → Idle.
    {
        Message& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
        back.pending_stream.clear();
    }
    m.s.phase = agentty::phase::Idle{};

    // The widget runs a ~200 ms finalize ramp (request_finalize(200))
    // gliding the reveal cursor to the live edge; frames during the
    // ramp legitimately re-arm. Advance the anim clock frame by frame
    // until it disarms — bounded far past the ramp. If it never disarms,
    // the caret (and 60 fps wakes) would run forever at idle and the
    // CHECK below fires.
    bool disarmed = false;
    for (int i = 0; i < 600 && !disarmed; ++i) {
        if (!frame_requests_animation(m)) { disarmed = true; break; }
        maya::testing::advance_anim_clock_ms(16);
    }
    CHECK(disarmed,
          "caret disarms after settle + finalize ramp (idle must not "
          "burn animation frames)");

    // And stays disarmed: settled fast-path must not re-arm.
    if (disarmed) {
        for (int f = 0; f < 3; ++f) {
            CHECK(!frame_requests_animation(m),
                  "caret stays disarmed on settled frames");
        }
    }
}

// ── (3) The deferred settle-freeze must NEVER fire while the reveal is
//        mid-glide — the structural guarantee against post-stream
//        scrollback duplication / ghosting. live_tail_reveal_settled()
//        is the gate meta.cpp checks before freezing; it must return
//        false while the typewriter is still animating (so the freeze
//        waits) and true only after the widget flips live_ off on its
//        own (so the snapshot equals the on-screen frame = maya cache
//        HIT = zero re-emit). Reproduces the screenshot bug class: a
//        freeze taken on a still-animating turn snapshots a shape that
//        diverges from the live frame in maya's prev_cells, re-emitting
//        the whole turn over committed scrollback. ────────────────────
TEST_CASE("freeze gated on reveal drain") {
    std::printf("test_freeze_gated_on_reveal_drain\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"freezegate"};
    Message u; u.role = Role::User; u.text = "write a longish reply";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Assistant turn whose reveal is mid-glide with a finalize ramp armed
    // — the exact widget state the freeze gate must refuse to snapshot.
    // ONE long paragraph (NO blank-line block boundary): the whole body
    // stays in the uncommitted reveal tail, so reveal_cp starts at 0 and
    // the typewriter has real distance to glide. (A \n\n-delimited body
    // commits every block on set_content, and the committed-snap jumps
    // reveal_cp straight to the edge — no animation to observe.)
    std::string body;
    for (int i = 0; i < 80; ++i)
        body += "word" + std::to_string(i) + " ";
    Message a; a.role = Role::Assistant; a.text = body;
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Idle{};

    // Build the widget by hand so we drive its reveal DIRECTLY below.
    auto& cache = m.ui.view_cache.message_md(
        m.d.current.id, m.d.current.messages.back().id);
    if (!cache.streaming)
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
    cache.streaming->set_reveal_fx(true);
    cache.streaming->set_reveal_pacing(30.0, 0.25);
    cache.streaming->set_content(body);
    cache.streaming->set_live(true);
    cache.streaming->request_finalize(200);
    // Drive the widget DIRECTLY (not through frame_requests_animation /
    // the view): cached_markdown_for finish()es a SETTLED message on its
    // first frame, which flips live_ off and orphans the finalize ramp
    // (advance_reveal_cursor_ early-outs once !live_, so finalize_deadline_
    // never clears and is_finalizing() sticks true forever). Building the
    // widget directly lets the ramp run to completion and flip live_ off on
    // its own — exactly the self-drain this test asserts on. (Production
    // no longer arms a post-settle glide at all; settle_message_md calls
    // finish() outright. request_finalize is kept here only to exercise the
    // gate's is_finalizing() term in isolation.)
    (void)cache.streaming->build();  // advance the reveal cursor once

    // While the reveal is mid-glide the gate MUST block the freeze.
    CHECK(cache.streaming->reveal_in_progress()
              || cache.streaming->is_finalizing(),
          "reveal is animating (test precondition)");
    CHECK(!agentty::app::detail::live_tail_reveal_settled(m),
          "freeze BLOCKED while the reveal is still animating — freezing "
          "a mid-glide turn is the post-stream duplication root cause");

    // Drain the reveal to completion (build() advances the cursor; the
    // finalize ramp flips live_ off once the cursor reaches the edge).
    drain_reveal(
        [&] {
            return cache.streaming->reveal_in_progress()
                   || cache.streaming->is_finalizing()
                   || cache.streaming->is_live()
                   || cache.streaming->is_parsing();
        },
        [&] { (void)cache.streaming->build(); });

    // Now the widget has flipped live_ off on its own and the live tail
    // has painted the settled shape — the gate must OPEN so the freeze
    // can take a byte-identical snapshot.
    CHECK(agentty::app::detail::live_tail_reveal_settled(m),
          "freeze ALLOWED once the reveal fully drained (settled shape "
          "is on screen — snapshot is a maya cache HIT)");

    // And a turn that still has UNCOMMITTED wire bytes is never
    // freezable regardless of reveal state — it isn't done arriving.
    m.d.current.messages.back().streaming_text = "trailing";
    CHECK(!agentty::app::detail::live_tail_reveal_settled(m),
          "freeze BLOCKED while streaming_text still has unsettled bytes");
    m.d.current.messages.back().streaming_text.clear();
}

// ── (4) The caret must stay armed when the widget is LIVE but the
//        phase has left Streaming (e.g. a mid-run tool round-trip:
//        Streaming → ExecutingTool → Streaming) AND the reveal cursor
//        has caught up to the live edge. In that pinned-but-live window
//        render_live_panel_ still animates the scramble/gradient/
//        pulsing caret every frame, so the RAF must keep firing or the
//        turn looks frozen mid-response. is_streaming() is false here, so
//        the phase gate (wire_streaming_here) does NOT cover it; the
//        is_live() term must. Reproduces "md feels stuck in the middle
//        when the stream is slow / pauses for a tool". ──────────────────
TEST_CASE("caret armed when live but phase not streaming") {
    std::printf("test_caret_armed_when_live_but_phase_not_streaming\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"liveness4"};
    Message u; u.role = Role::User; u.text = "do some work";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // In-flight assistant message, short body so the cursor reaches the
    // edge fast. Mark it live via a streaming render first.
    Message a; a.role = Role::Assistant;
    a.streaming_text = "Ok";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    CHECK(frame_requests_animation(m),
          "armed on the first streaming frame (sets the widget live_)");

    // Drain the reveal cursor to the live edge so reveal_in_progress()
    // can't be what arms the frame below. Re-fetch through the
    // non-migrating peek() each step: frame_requests_animation migrates
    // the entry between the pinned/settled maps, invalidating any held
    // Entry&.
    const auto live_state = [&] {
        const auto* mc =
            m.ui.view_cache.peek(m.d.current.id, m.d.current.messages.back().id);
        return mc ? mc->streaming.get() : nullptr;
    };
    drain_reveal(
        [&] { auto* s = live_state(); return s && s->reveal_in_progress(); },
        [&] { (void)frame_requests_animation(m); });
    CHECK(live_state() && live_state()->is_live(),
          "widget still live after reaching the edge (precondition)");
    CHECK(live_state() && !live_state()->reveal_in_progress(),
          "reveal cursor reached the live edge (precondition)");

    // The wire briefly leaves Streaming for a tool round-trip. The
    // assistant message is NOT settled (streaming_text still holds the
    // unsettled bytes) so the widget stays live_. Backdate the recency
    // clock past kRevealActiveMs (3 s) so the since_grow window can't be
    // what arms it either. Now ONLY the is_live() term remains.
    m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
    m.ui.view_cache
        .message_md_live(m.d.current.id, m.d.current.messages.back().id)
        .last_grow_tick_ms =
        maya::anim_now_ms() - 10000;   // simulate a 10 s inter-delta gap

    // Advance the anim clock past maya's own recency window so its
    // quiescent regime only self-arms once per phase bucket —
    // consecutive same-bucket builds would return false without
    // agentty's is_live() gate.
    maya::testing::advance_anim_clock_ms(400);

    for (int f = 0; f < 6; ++f) {
        CHECK(frame_requests_animation(m),
              "caret STILL armed while live_ but phase=ExecutingTool, "
              "cursor at edge, recency window expired — the is_live() RAF "
              "term must keep the typewriter caret breathing");
        maya::testing::advance_anim_clock_ms(2);
    }
}

// ── (5) The freeze gate must BLOCK while a tail md widget is still live_,
//        even when the reveal cursor is at the edge and nothing else is
//        animating (reveal_in_progress / is_finalizing / is_parsing all
//        false). live_tail_reveal_settled() must be the EXACT mirror of
//        build_live_tail's `reveal_settled` (the hash-stamp gate), which
//        ORs is_live(). If the freeze gate dropped the is_live() term it
//        would green-light a freeze in a state where the live tail refused
//        to stamp the cacheable assistant_run_hash_id — so freeze_range
//        would stamp a key the live frame never painted (maya cache MISS),
//        rebuild the run under FrozenBuildScope (show_all) at a possibly
//        different height, and strand a duplicate in scrollback. This locks
//        the two gates' predicate sets together so they can't drift. ─────
TEST_CASE("freeze gate blocks while widget live") {
    std::printf("test_freeze_gate_blocks_while_widget_live\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"freezegate_live"};
    Message u; u.role = Role::User; u.text = "q";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Settled assistant turn: text FINAL, no streaming bytes in flight
    // (so the `!streaming_text.empty()` guard can't be what blocks the
    // gate — we need to isolate the is_live() term).
    const std::string body = "Short settled reply body.";
    Message a; a.role = Role::Assistant; a.text = body;
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Idle{};

    auto& cache = m.ui.view_cache.message_md(
        m.d.current.id, m.d.current.messages.back().id);
    cache.streaming = std::make_shared<maya::StreamingMarkdown>();
    // reveal_fx OFF makes reveal_in_progress() false unconditionally; NOT
    // calling request_finalize keeps is_finalizing() false; no async keeps
    // is_parsing() false. The ONLY non-settled signal left is is_live(),
    // armed here exactly as the live-tail render arms it for a streaming
    // message that hasn't been finish()ed yet.
    cache.streaming->set_reveal_fx(false);
    cache.streaming->set_content(body);
    cache.streaming->set_live(true);

    // Precondition: the isolated is_live()-only state.
    CHECK(cache.streaming->is_live(), "widget live_ (precondition)");
    CHECK(!cache.streaming->reveal_in_progress(),
          "reveal_in_progress false with reveal_fx off (precondition)");
    CHECK(!cache.streaming->is_finalizing(),
          "no finalize ramp armed (precondition)");
    CHECK(!cache.streaming->is_parsing(), "no async parse (precondition)");

    // THE CONTRACT: the freeze gate blocks while the widget is live_.
    CHECK(!agentty::app::detail::live_tail_reveal_settled(m),
          "freeze BLOCKED while the md widget is still live_ — the gate "
          "must mirror build_live_tail's hash-stamp condition (is_live "
          "term) or freeze_range stamps an unkeyed run and strands a "
          "duplicate");

    // finish() drops live_ (the other three were already false), so now
    // build_live_tail WOULD stamp the cacheable key — the freeze gate must
    // open in lockstep so the handoff is a maya cache HIT.
    cache.streaming->finish();
    CHECK(!cache.streaming->is_live(),
          "finish() dropped live_ (precondition)");
    CHECK(agentty::app::detail::live_tail_reveal_settled(m),
          "freeze ALLOWED once finish() settles the widget (live tail has "
          "stamped the cacheable key — freeze is a byte-identical cache HIT)");
}

// ── (6) DEEP RUN, live edge below many settled sub-turns. The reported
//        low-CPU stall: in a long autopilot run the streaming edge is a
//        fresh placeholder message BELOW dozens of settled tool sub-turns.
//        turn_config_for_assistant_run wraps each SETTLED sub-turn in its
//        own stably-keyed bare Turn (maya blits it, flat cost) but MUST
//        keep the still-streaming edge built inline so its
//        cached_markdown_for re-arms the animation frame every build. If
//        the edge were stably keyed (e.g. because its text prefix looked
//        settled), maya would blit the cached component, cached_markdown_
//        for would never re-run, the RAF would never re-arm, and the
//        typewriter would freeze mid-turn at ~0% CPU until an unrelated
//        hash axis flips. This asserts the edge stays armed. ────────────
TEST_CASE("deep run live edge stays armed") {
    std::printf("test_deep_run_live_edge_stays_armed\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"liveness6"};
    Message u; u.role = Role::User; u.text = "do a long series of edits";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Many SETTLED tool-only sub-turns (each its own Assistant message) —
    // the deep run's quiescent prefix. Tool terminal, no prose.
    for (int e = 0; e < 40; ++e) {
        Message a; a.role = Role::Assistant;
        agentty::ToolUse tc;
        tc.id   = agentty::ToolCallId{"e" + std::to_string(e)};
        tc.name = agentty::ToolName{"edit"};
        tc.args = {{"path", "src/f" + std::to_string(e) + ".cpp"}};
        auto now = std::chrono::steady_clock::now();
        tc.status = agentty::ToolUse::Done{
            now - std::chrono::milliseconds{5}, now, "edited"};
        a.tool_calls.push_back(std::move(tc));
        m.d.current.messages.push_back(std::move(a));
    }

    // The LIVE EDGE: a fresh streaming placeholder with live prose bytes,
    // mid-tool-execution phase (the wire left Streaming for a tool
    // round-trip). This is the message that must stay inline + armed.
    Message edge; edge.role = Role::Assistant;
    edge.streaming_text = "Now I will summarize the edits I performed";
    m.d.current.messages.push_back(std::move(edge));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    CHECK(frame_requests_animation(m, 100, 4000),
          "armed on the first streaming frame of the deep-run edge");

    // The edge keeps receiving bytes (a real stream), so it stays live.
    // Across several frames — including after the recency clock is
    // backdated past kRevealActiveMs and the phase leaves Streaming for a
    // tool round-trip — the edge's build must re-arm the animation frame
    // EVERY time. If the deep-run keying swallowed the edge into a cached
    // bare Turn, cached_markdown_for would stop running and the arm would
    // drop (the low-CPU freeze).
    m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
    for (int f = 0; f < 6; ++f) {
        // Feed a byte so the edge is unambiguously still streaming, and
        // backdate the per-message recency clock so the since_grow window
        // can't be what arms it — only the inline reveal path can.
        m.d.current.messages.back().streaming_text += ".";
        auto& edge_cache = m.ui.view_cache.message_md(
            m.d.current.id, m.d.current.messages.back().id);
        edge_cache.last_grow_tick_ms =
            maya::anim_now_ms() - 10000;   // simulate a 10 s inter-delta gap
        CHECK(frame_requests_animation(m, 100, 4000),
              "deep-run live edge STILL armed (40 settled sub-turns above, "
              "phase=ExecutingTool, recency expired) — the streaming edge "
              "must be built inline, never stably keyed");
        maya::testing::advance_anim_clock_ms(2);
    }
}


