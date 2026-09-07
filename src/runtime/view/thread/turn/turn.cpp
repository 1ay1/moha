#include "agentty/runtime/view/thread/turn/turn.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/thinking.hpp>  // reasoning/thinking block
#include <maya/widget/reasoning.hpp>  // ReasoningStream (streaming reasoning block)
#include <maya/core/render_context.hpp> // available_height (resize-shrink detect)
#include <maya/core/anim_clock.hpp>     // maya::anim_now_ms (deterministic under a test clock)
#include <maya/render/cache_id.hpp>
#include <maya/render/renderer.hpp> // build_layout_tree / layout::compute
#include <maya/layout/yoga.hpp>     // maya::layout::compute / LayoutNode
#include <maya/platform/io.hpp>
#include <maya/style/theme.hpp>
#include <maya/core/motion.hpp>   // anim::keep_animating — frame requests

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/model_name.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agent_timeline/tool_body_common.hpp"   // stream_card_rows_bound
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/permission.hpp"
#include "agentty/runtime/view/thread/seam.hpp"

namespace agentty::ui {

namespace {

// ── Cached markdown render. The ONE Element-returning helper kept in
//    agentty — strictly because cross-frame cache state lives in the
//    StreamingMarkdown widget instance, which we keep alive across
//    frames so its block cache survives.
//
//    Single rendering path: the StreamingMarkdown widget is used for
//    live AND settled messages. The widget's pre-finish output (prefix
//    ComponentElement + tail Element wrapped in vstack.gap(1)) has a
//    slightly different total height than the one-shot maya::markdown()
//    parser's output (flat blocks under the same vstack wrapper but no
//    ComponentElement seam). Swapping between them at StreamFinished
//    shifted the canvas by ~3 rows, which propagated through the per-row
//    diff and left the composer at a different terminal row — visible
//    as "composer pulled down + duplicate composer above it on the
//    first keypress."
//
//    Staying on the streaming widget keeps the height stable across the
//    streaming → idle transition. set_content with byte-identical bytes
//    is an internal no-op; build() returns cached_build_ when nothing
//    has dirtied, so the per-frame cost is the same as the finalized
//    path. The tail re-parses on each frame for finalized messages too,
//    but that's a single inline parse on the last few bytes — cheap.
// Which body of a message a StreamingMarkdown slot renders. The ANSWER
// path (default) streams msg.text + streaming_text + pending_stream through
// the message's own cache slot. The REASONING path streams
// msg.reasoning_display_text() through a SEPARATE, sibling cache slot
// (MessageId suffixed with "#r") so reasoning gets the identical smooth
// reveal + settle machinery as normal text — it streams like normal text,
// caches like normal text, and stays fully rendered after settle (never
// folds). The two slots are independent widgets so the answer body and the
// reasoning block don't fight over one reveal cursor.
enum class MdView : std::uint8_t { Answer, Reasoning };

// MessageId of the cache slot for `view` on message `mid`. Answer uses the
// message id verbatim; Reasoning appends "#r" so it lives in its own slot
// (dropped alongside the answer slot at freeze — see frozen.cpp).
inline MessageId md_slot_id(const MessageId& mid, MdView view) {
    return view == MdView::Reasoning ? MessageId{mid.value + "#r"} : mid;
}

maya::Element cached_markdown_for(const Message& msg, const Model& m,
                                  MdView view = MdView::Answer) {
    const MessageId slot_id = md_slot_id(msg.id, view);
    const bool reasoning_view = view == MdView::Reasoning;
    // Lifecycle-aware cache access (see cache.hpp's partition rationale).
    // A message's md slot holds LOAD-BEARING animation state — the
    // StreamingMarkdown reveal widget + defer bookkeeping — exactly while
    // it is still moving: live wire bytes arriving, OR the widget itself
    // still animating (live / finalize ramp / cursor gliding / async
    // parse). Evicting the slot in that window destroys the reveal
    // mid-glide and stalls the typewriter, so it must be PINNED (kept out
    // of any evictable set entirely). Once fully drained the slot is a
    // pure render memo that stages settled until freeze drops it.
    //
    // We can't read the widget's post-update animation state before we
    // access the slot, so pin on either signal available up front: live
    // wire bytes on the message, or an existing widget that reports
    // itself still animating from last frame. A message that has neither
    // is settled and staged (dropped at freeze). The predicate is
    // deliberately the same shape as turn.cpp's `subturn_stably_keyable`
    // negation — liveness has ONE definition in this view.
    // Reasoning is "live" while the stream is running AND this message hasn't
    // produced output yet (no answer prose, no tool calls, text block open) —
    // once it acts (tool_calls) or answers, no more reasoning bytes arrive, so
    // the reveal must finish and the header flip to "Reasoned". Mirrors
    // reasoning_slot's `active`.
    const bool reasoning_produced_output =
        !msg.text.empty() || !msg.streaming_text.empty()
        || !msg.tool_calls.empty() || msg.text_block_closed;
    const bool reasoning_active =
        m.s.is_streaming() && !reasoning_produced_output;
    const bool has_live_bytes = reasoning_view
        ? (!msg.streaming_text.empty() || !msg.pending_stream.empty()
           || reasoning_active)
        : (!msg.streaming_text.empty() || !msg.pending_stream.empty());
    const auto& probe = m.ui.view_cache; // const peek, no touch/reorder
    const bool widget_animating = [&] {
        // is_pinned is a cheap const lookup; if the slot is already
        // pinned from last frame we keep pinning until it drains (checked
        // post-update below via the same accessor). If it's settled or
        // absent we only pin when the message carries live bytes.
        return probe.is_pinned(m.d.current.id, slot_id);
    }();
    const bool want_pin = has_live_bytes || widget_animating;

    auto& cache = want_pin
        ? m.ui.view_cache.message_md_live(m.d.current.id, slot_id)
        : m.ui.view_cache.message_md    (m.d.current.id, slot_id);

    if (!cache.streaming) {
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        // Animated live tail: gradient trail + scramble→resolve + pulsing
        // caret on the streaming edge (maya reveal_fx). Only animates while
        // the widget is live_; the settled build is untouched.
        cache.streaming->set_reveal_fx(true);
        // Reveal pacing for the rate-smoothed bounded-lag cursor (maya
        // RateCursor). The cursor reveals at backlog / drain_secs, so it
        // TRACKS the model's own speed with a fixed time lag, and low-passes
        // that rate so a chunky wire slides in instead of teleporting. Args:
        //   • floor_cps = the MINIMUM reveal speed. A trickle still types out
        //     at >= this so a slow/local model doesn't inch in one char at a
        //     time. It is NOT a ceiling — a fast model reveals FASTER, at its
        //     own delivery rate (the cursor tracks the wire), so the reveal
        //     never falls permanently behind and dumps at settle. 90 cp/s is
        //     a brisk readable minimum.
        //   • drain_secs = the target LAG: how far behind the live edge the
        //     cursor rides, in seconds. rate = backlog / drain_secs holds the
        //     reveal ~drain_secs behind the wire at the wire's own speed.
        //
        // Retuned to 45 cps / 0.40 s (was 90 / 0.15). Real models stream in
        // BURSTS with idle gaps (profiled: ~57% of frames had the cursor
        // frozen AT the edge under 90/0.15 — it drained each fat delta then
        // sat idle until the next, a visible stop-and-go). A lower floor +
        // larger lag keeps a continuous buffer so the cursor GLIDES through
        // the idle gaps instead of freezing (same profile: <1% idle frames at
        // 45/0.40). The old rationale for a TINY lag was that the tool-
        // boundary hard-snapped the whole backlog to the edge (a paste), so
        // less lag meant a smaller paste. That is obsolete: the tool boundary
        // now uses snap_reveal_to_edge(glide_ms=150) \u2014 a bounded VISIBLE
        // glide, not a paste \u2014 so a larger steady-state backlog is drained
        // smoothly there too, and scrollback safety is preserved by the
        // glide landing within ~150 ms (scrollback_oracle_test green).
        cache.streaming->set_reveal_pacing(/*floor_cps=*/45.0,
                                           /*lead_secs=*/0.40);
        // Adaptive floor: auto-tune the reveal speed to the wire's observed
        // rate (clamped 25..180 cps) so the glide is smooth across models of
        // very different throughput without a hand-picked constant — a slow
        // local model won't freeze (cursor outrunning the wire) and a fast
        // hosted one won't lag. 45/0.40 above is the cold-start seed until
        // the estimate warms up; drain_secs (the lag buffer) still applies.
        cache.streaming->set_reveal_adaptive(true, /*min*/25.0, /*max*/180.0);

        // REASONING is paced for SPEED while keeping the animated slide.
        // Providers deliver reasoning as a summary that often lands in one/two
        // big deltas at the end of the thinking window, so the cursor sees a
        // large backlog at once. The reveal engine has NO cruise-speed cap:
        // it moves at backlog/drain_secs clamped to [min,max], and the
        // animation quality (fast scramble/ghost SLIDE, never a paste) comes
        // from the rate_tau low-pass — which is INDEPENDENT of the speed band.
        // So we lift the CEILING well above the answer path (reasoning is
        // bulk, skimmable content the user reads faster than prose) to drain a
        // big block quickly, keep the floor brisk so short reasoning types
        // promptly, and hold a tight-ish lag so the cursor tracks the wire and
        // never hoards a backlog to dump at the reasoning→answer settle. The
        // earlier 95 cps ceiling was the bottleneck that made it feel slow.
        if (reasoning_view) {
            cache.streaming->set_reveal_pacing(/*floor_cps=*/60.0,
                                               /*lead_secs=*/0.28);
            cache.streaming->set_reveal_adaptive(true, /*min*/45.0,
                                                 /*max*/280.0);
        }
    }

    // Pick the source bytes for THIS frame. The reveal cursor must see
    // EVERY byte that has arrived from the wire so it can pace them
    // smoothly on its own wall clock; if it only saw `streaming_text`,
    // the visible text could advance only when meta.cpp's Tick handler
    // drips pending_stream → streaming_text (33 ms sync / 100 ms non-
    // sync / more over SSH). Between ticks each text delta still forces
    // a render (fps=0), but streaming_text wouldn't have grown, so the
    // reveal had nothing new to show — the text sat STUCK until the next
    // tick dripped a chunk, then JUMPED. That is the "md gets stuck then
    // bursts" stutter, and it is structural: deltas feed pending_stream,
    // the drip feeds streaming_text only on Tick, the view read only
    // streaming_text. Reading text + streaming_text + pending_stream
    // makes the wall-clock reveal cursor the SINGLE display pacer over
    // all arrived bytes, so visible output advances continuously at
    // kRevealCharsPerSec independent of the tick cadence. (meta.cpp's
    // drip still runs — it moves bytes into streaming_text where the
    // settle path commits them — but it no longer controls what the
    // user SEES.)
    //
    //   • settled: msg.text holds the final body, streaming_text +
    //     pending_stream empty.
    //   • mid-sub-turn-2: msg.text holds the PRIOR sub-turn's settled
    //     body; streaming_text + pending_stream hold the in-flight
    //     follow-up bytes. Feed all so the live tail keeps growing.
    //   • sub-turn-1 streaming: msg.text empty, streaming_text +
    //     pending_stream grow.
    // The joined buffer is cached on MessageMdCache so the string_view
    // we hand to set_content_async stays valid across the call.
    //
    // Size-based no-op fast-path. Reveal_fx animates at 60 fps; bytes
    // arrive at 10-30 / s, so >90% of frames produce no source-size
    // change. Without this guard we re-concat msg.text + streaming_text
    // + pending_stream (O(N) alloc+memcpy) AND set_content_async memcmps
    // the result (O(N) memcmp) every frame for the in-flight turn. For
    // a 50 KB sub-turn-2 body that's ~100 KB of memory bandwidth per
    // frame just to discover nothing changed. Compare the three sizes
    // instead: if none grew, the source bytes are byte-identical and we
    // can skip straight to build().
    // In REASONING mode the body is a single growing string
    // (reasoning_display_text): no text/streaming/pending triple-buffer.
    // Stash it in the persistent combined_source buffer (NOT a local) so
    // set_content_async's string_view stays valid across frames, and diff
    // on its size like the answer path.
    if (reasoning_view) {
        // Reasoning body is the raw reasoning text (the ReasoningStream chrome
        // supplies the ┃ rail + dim). Stash it in the persistent buffer so
        // set_content_async's string_view stays valid across frames.
        std::string_view rsrc = msg.reasoning_display_text();
        if (rsrc != cache.combined_source)
            cache.combined_source.assign(rsrc);
    }

    const bool sizes_unchanged = reasoning_view
      ? (cache.last_text_size == cache.combined_source.size())
      : (cache.last_text_size      == msg.text.size()
     && cache.last_streaming_size == msg.streaming_text.size()
     && cache.last_pending_size   == msg.pending_stream.size());

    const std::string* source_ptr =
        reasoning_view ? &cache.combined_source : &msg.text;
    const bool has_live = reasoning_view
      ? false // reasoning has one buffer; source_ptr already points at it
      : (!msg.streaming_text.empty() || !msg.pending_stream.empty());
    if (has_live) {
        if (msg.text.empty() && msg.pending_stream.empty()) {
            // Streaming-text only, nothing buffered — reference it
            // directly, no copy.
            source_ptr = &msg.streaming_text;
        } else if (sizes_unchanged && !cache.combined_source.empty()) {
            // Sizes match what we already concatenated last frame; the
            // backing buffer is still valid. Reuse it; no copy.
            source_ptr = &cache.combined_source;
        } else {
            cache.combined_source.clear();
            cache.combined_source.reserve(
                msg.text.size() + msg.streaming_text.size()
                + msg.pending_stream.size());
            cache.combined_source.append(msg.text);
            cache.combined_source.append(msg.streaming_text);
            cache.combined_source.append(msg.pending_stream);
            source_ptr = &cache.combined_source;
        }
    }
    const std::string& source = *source_ptr;

    // Remember the component sizes for next frame's no-op check.
    cache.last_text_size      = reasoning_view ? source.size() : msg.text.size();
    cache.last_streaming_size = msg.streaming_text.size();
    cache.last_pending_size   = msg.pending_stream.size();

    // A reasoning slot settles when the stream stops OR its answer body has
    // begun (bytes will no longer arrive on the reasoning channel). The
    // answer slot settles the classic way.
    const bool settled = reasoning_view
      ? (!reasoning_active && !source.empty())
      : (!msg.text.empty()
         && msg.streaming_text.empty()
         && msg.pending_stream.empty());
    if (!reasoning_view && settled && !cache.combined_source.empty()) {
        // Reclaim the scratch buffer the moment streaming_text
        // drains — next freeze takes the snapshot off msg.text.
        // (Reasoning keeps its snapshot HERE, so it is exempt.)
        std::string{}.swap(cache.combined_source);
    }

    // Fast-path: fully settled message whose reveal has completed.
    // Skip all per-frame work (typewriter advance, set_content,
    // finish, live-mode checks) and return the cached element tree
    // directly. This is the dominant case on long sessions — N
    // visible turns × every frame would otherwise pay constant
    // overhead for each one.
    if (settled
        && cache.last_settled_size == source.size()
        && cache.revealed_size    == source.size()) {
        auto built = cache.streaming->build();
        // Lifecycle down-migration: this message is fully drained (no
        // live bytes, reveal complete). If its slot is still PINNED from
        // its streaming days, hand it back to the LRU now — it is a pure
        // render memo from here on, and leaving it pinned would leak a
        // permanent (uncapped) entry per settled turn. `build` is a value
        // (not a reference into the slot), so the migrate that follows
        // can safely move the Entry. Idempotent: no-op once settled.
        if (want_pin)
            (void)m.ui.view_cache.message_md(m.d.current.id, slot_id);
        return built;
    }

    // ── Reveal: feed ALL arrived bytes, every frame ──
    //
    // There is NO host-side typewriter cursor. The display pacer is
    // maya's reveal_fx (scramble→resolve + gradient trail + caret on the
    // live edge), which animates the trailing run of the text we feed.
    // The host's only job is to keep handing the widget the full set of
    // bytes that have arrived from the wire and to keep the animation
    // frame armed while the stream is live.
    //
    // Why no second cursor: a host pacing clock that withholds bytes and
    // releases them at a fixed char/sec is a SECOND clock beating against
    // (a) the wire-arrival clock and (b) the render-wake cadence. Any
    // beat between them is visible as stutter — and if the host clock
    // ever stops advancing (a render skipped, the wake cadence dropped to
    // the 100 ms Tick) the text sits STUCK mid-reveal until an unrelated
    // event flips the frame. Feeding the raw arrived bytes removes that
    // clock entirely: visible text == arrived text, always. Chunky
    // deltas (a multi-KB code block in one delta) appear at once, but
    // reveal_fx animates their trailing edge so the seam still reads as a
    // live stream, the same approach Zed / Claude Code use.
    //
    // revealed_size is kept == source.size() so the settled fast-path and
    // the freeze snapshot (which read revealed_size) see a fully-revealed
    // message and never freeze a partial body.
    cache.revealed_size = source.size();
    const std::string_view feed_source = source;

    // Settled-message fast path. Once a message has settled
    // (msg.text is final, streaming_text empty) the source bytes are
    // immutable for the rest of the session. set_content's equal-
    // content check still costs O(source.size()) memcmp every frame;
    // on a long thread with many visible turns that adds up. Skip
    // the call entirely once we've fed the final bytes through once.
    //
    // Gate is (size + settled-once flag) only. No reducer rewrites
    // an Assistant's msg.text in place after StreamFinished moves
    // streaming_text → text; later edits replace the Message
    // wholesale (new MessageId, new cache slot). Hashing the bytes
    // every frame to guard against a same-length in-place rewrite
    // that doesn't exist cost O(text) per visible settled turn per
    // frame — the dominant per-frame cost on long sessions.
    const bool already_settled_into_cache =
        settled
        && cache.last_settled_size == source.size()
        && cache.revealed_size == source.size();

    // Optional per-frame timer for the streaming-markdown widget. Set
    // AGENTTY_STREAM_PROF=1 to log set_content+finish+build() cost for
    // each non-fast-path call to /tmp/agentty-stream-prof.log. Isolates
    // the in-flight widget cost from the (separately-profiled) timeline
    // render. One line per call; skips the settled fast-path entirely.
    static const bool stream_prof = []{
        const char* e = std::getenv("AGENTTY_STREAM_PROF");
        return e && *e && *e != '0';
    }();
    const auto prof_t0 = stream_prof
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    if (!already_settled_into_cache) {
        // Skip set_content_async entirely on a sizes-unchanged frame.
        // The widget already saw these exact bytes last frame; its
        // internal source_ matches feed_source by length and prefix.
        // set_content_async's own no-op fast-path would memcmp
        // O(N) bytes to discover this; the size check here is O(1).
        // Still emit the call when sizes match but the widget is
        // currently parsing async (it needs the poll to adopt the
        // result) OR when this is the first feed (combined_source
        // empty above means we took the direct ref path).
        const bool skip_set_content =
            sizes_unchanged
            && cache.streaming->source().size() == feed_source.size()
            && !cache.streaming->is_parsing();
        if (!skip_set_content) {
            // Path split by liveness:
            //
            //   LIVE WIRE (!settled): always feed the sync
            //   set_content. The live combined feed is append-in-
            //   spirit — text + streaming_text + pending_stream grows
            //   monotonically within a sub-turn. But at each sub-turn
            //   boundary the prior sub-turn's streaming_text folds into
            //   msg.text and streaming_text resets, so the NEXT feed is
            //   a divergent-prefix change; once the accumulated body
            //   crosses set_content_async's 16 KB threshold that swap
            //   would spawn a detached parse worker. While the worker
            //   runs is_parsing() is true and the widget returns its
            //   PREVIOUS element tree — the live reveal FREEZES — and
            //   the result is only adopted (maybe_apply_async_) the next
            //   time build() is polled. If the wire goes quiet for a
            //   beat while the frozen-scrollback visual-hash gate
            //   suppresses redraws, build() stops being polled and the
            //   finished parse sits unapplied: the tail stalls until the
            //   next delta / 100 ms Tick. That's the "md streaming stops
            //   after a while in a long turn, fine again next turn"
            //   report (next turn = fresh <16 KB placeholder → sync
            //   path). The sync path has no is_parsing() window, so the
            //   live reveal can never freeze on a parse.
            //
            //   SETTLED / RELOAD (settled): keep the async variant. A
            //   thread reload or scrollback recovery swaps in a whole
            //   >=16 KB body at once as a divergent prefix; offloading
            //   that one-shot parse to a worker keeps the render thread
            //   responsive, and finish() below force-adopts it in the
            //   same pass, so there's no unbounded freeze window.
            if (!settled)
                cache.streaming->set_content(feed_source);
            else
                cache.streaming->set_content_async(feed_source);
        }

        // Settled message → commit any trailing tail to the prefix's
        // block list. finish() flushes whatever is still in the
        // streaming tail into the canonical committed block path
        // (md_block_to_element), so the settled render is byte-identical
        // to the live one.
        //
        // Historically this was load-bearing for a trailing closed code
        // fence: find_block_boundary only committed a fenced block once
        // its closing ``` was followed by a newline, so a message ending
        // at the closing backticks (the common case for a reply ending
        // in a code example) left the last block stuck in the tail,
        // rendered via render_tail's inline path. render_tail and
        // md_block_to_element feed the same border/padding builder
        // slightly different code strings, so their cells weren't
        // byte-identical — at settle the whole last block re-emitted to
        // the terminal (the "repaint", worst over SSH). That divergence
        // is now fixed upstream in maya (boundary.cpp eager-commits a
        // closing fence at end-of-buffer), so the live and settled cells
        // already match before this call. finish() is kept because it's
        // idempotent (no-op once committed_ == source_.size()) and still
        // the correct place to flush any OTHER trailing-block kind.
        //
        // At settle we kick off the finalize ramp FIRST — the widget
        // glides its reveal cursor to the live edge over ~200 ms and
        // flips live_ off itself when the cursor catches up. We defer
        // finish() (which forces live_=false) until that ramp completes,
        // so the reveal animation isn't cut short and dump its backlog
        // in one frame.
        //
        // Skip on a sizes-unchanged frame. set_live(true) goes through
        // the Tracked<> wrapper which auto-bumps build_dirty_ on EVERY
        // assignment (even same-value): a wasted rebuild per frame at 60
        // fps. request_finalize is idempotent (early-out on existing
        // deadline) but cheap to skip. With this gate, no-grow frames
        // leave build_dirty_ alone and build() returns cached_build_
        // directly.
        if (!sizes_unchanged) {
            if (settled) {
                // Finish so the live height equals the settled/frozen height
                // — agent_session's MessageStop discipline — UNLESS an
                // end-of-turn finalize ramp is still gliding (#5, interactive
                // terminals only: finalize_turn arms request_finalize instead
                // of settling, precisely so the steady-state backlog types
                // out over ~200 ms instead of pasting in one frame). While
                // the ramp runs, the widget is animating dense frames and
                // flips live_ off ON ITS OWN once the tail visually
                // settles; forcing finish() here would cut the glide and
                // dump the backlog — the exact paste the ramp exists to
                // prevent. Once the widget settles itself (is_finalizing
                // false again), this branch runs finish() as before — a
                // no-op shape-wise, but it flushes any trailing block and
                // keeps the fps=0/SSH path (where the reducer already
                // settled via settle_message_md) in lockstep.
                const bool end_glide_running =
                    cache.streaming->is_finalizing()
                    || (cache.streaming->is_live()
                        && cache.streaming->reveal_in_progress());
                if (!end_glide_running) cache.streaming->finish();
            } else {
                cache.streaming->set_live(true);
            }
        }

        // Auto-fold of long code blocks is DISABLED: code blocks always
        // render in full, never collapsed to a "▸ N lines of code hidden —
        // unfold" stub. Long fences stay expanded even in a long
        // conversation. (If the fold is ever reinstated, prose_rows in
        // frozen.cpp must mirror the same threshold/kinds or the mid-run
        // keep-loop strands a scrollback ghost.)

        if (settled
            && cache.revealed_size == source.size()
            && !cache.streaming->is_finalizing())
        {
            cache.streaming->finish();
            cache.last_settled_size = source.size();
        }
    }

    // (Live/finalize transitions are handled above, before the finish()
    // gate that depends on them.)

    // Track when `source` last grew so we can tell "actively streaming"
    // (bytes flowing) from "streaming but stalled" (e.g. a 60–120 s
    // extended-thinking pause with no deltas). Used by the RAF gate just
    // below.
    {
        const std::int64_t now2 = ::maya::anim_now_ms();
        if (source.size() > cache.last_grow_size) {
            cache.last_grow_size = source.size();
            cache.last_grow_tick_ms = now2;
        } else if (source.size() < cache.last_grow_size) {
            // Source rolled / shrank (set_content rollback, message reset).
            cache.last_grow_size = source.size();
            cache.last_grow_tick_ms = now2;
        }
    }

    // Re-arm the 16 ms animation frame for the WHOLE active streaming
    // window. reveal_fx (the live-edge scramble/gradient/caret) is the
    // only animation now; it needs a wake every frame while bytes are
    // flowing so its trailing run resolves smoothly.
    //
    // PRIMARY signal: the model is authoritatively streaming. m.s
    // .is_streaming() is variant-backed (phase::Streaming) and exact —
    // it's the same signal the status bar / phase chip / sparkline gate
    // on. Combined with !settled (this message still has live bytes in
    // streaming_text/pending_stream), it means "the wire is open and
    // THIS turn is the one receiving it." While that holds, keep the
    // caret armed UNCONDITIONALLY — no matter how long the gap between
    // deltas. The pulsing caret means "waiting for the model," which is
    // exactly true during an inter-delta pause, even a 10 s one. This is
    // what makes the fix robust across models/networks: it keys off the
    // real in-flight state, not a guess about delta cadence.
    //
    // The earlier byte-recency window (since_grow_ms) raced the model's
    // gap: 250 ms lapsed inside every slow-model gap (median ~470 ms),
    // freezing the caret mid-sentence ("stream looks dead"); bumping it
    // to 3 s only moved the cliff. A fixed timeout can ALWAYS be out-run
    // by a slower model or a laggy link. The phase gate can't — it ends
    // the instant the wire closes (phase → Idle/ExecutingTool) and not a
    // frame before. Cost while streaming is bounded and already paid:
    // build() runs every frame the visual hash advances; an armed caret
    // adds a ~0.1-0.4 ms no-content repaint, only while genuinely live.
    //
    // The since_grow_ms window is kept as a SECONDARY fallback for the
    // edge where is_streaming() has already flipped (e.g. StreamFinished
    // landed, phase → Idle) but the reveal cursor still has a backlog to
    // glide out — reveal_in_progress() below already covers that, but the
    // window catches a stale-phase beat without re-introducing the race
    // (it only EXTENDS arming, never cuts it short while is_streaming()).
    const bool wire_streaming_here = !settled && m.s.is_streaming();
    constexpr std::int64_t kRevealActiveMs = 3000;
    const std::int64_t now3 = ::maya::anim_now_ms();
    const std::int64_t since_grow_ms =
        cache.last_grow_tick_ms == 0
            ? kRevealActiveMs + 1
            : now3 - cache.last_grow_tick_ms;
    const bool stream_in_motion =
        wire_streaming_here
        || (!settled && since_grow_ms <= kRevealActiveMs);

    // ── Scrollback safety: never let a mid-reveal row cross the viewport
    //    top. Rows above the top are committed to IMMUTABLE native
    //    scrollback; if one gets there while the typewriter is still
    //    behind the wire (ghost-blanked cells, scramble tip, hot
    //    gradient), later frames complete the reveal, the canvas row no
    //    longer matches the committed copy, maya's scrollback-invariant
    //    gate fires, and the grow-path recovery is a HardReset
    //    (\x1b[2J\x1b[3J wipe) — seen by the user as duplicated/garbled
    //    prose stranded above the tool cards.
    //
    //    (a) TOOL CARDS on this message: the Anthropic wire closes the
    //        text block BEFORE tool_use streams, so this message's prose
    //        is wire-complete the instant a ToolUse exists — there is
    //        nothing left to type out. Meanwhile the card panel renders
    //        BELOW this markdown and grows (Pending → Running progress
    //        → Done), pushing the prose toward/past the viewport top.
    //        Three-part hardening, all idempotent:
    //        • snap_reveal_to_edge(): un-ghosts the tail (bumps
    //          build_dirty_ so the rebuild is immediate);
    //        • set_reveal_fx(false): stops the per-frame tip mutation
    //          (scramble glyphs, gradient restyle, pulsing caret) so
    //          every row that scrolls off is in its FINAL glyphs+style;
    //        • finish(): force-commits the TRAILING PARAGRAPH into the
    //          block path NOW. A message's last paragraph never receives
    //          its terminating \n\n, so without this it rides in
    //          render_tail's inline path for the whole tool phase and
    //          only converts to a committed block at turn settle — and
    //          the two paths wrap at (off-by-one) different widths, so
    //          the conversion REWRITES the paragraph's rows. If those
    //          rows crossed the viewport top during the tool phase
    //          (they routinely do — the growing card pushes them up),
    //          the settle-time rewrite hits immutable scrollback and
    //          maya's gate can only HardReset (\x1b[2J\x1b[3J), wiping
    //          and re-stranding the transcript — the oracle's
    //          t1-settle corruption and the user's "prose duplicated
    //          above the cards" screenshots. Committing here makes the
    //          rewrap happen while the paragraph is still at the
    //          viewport bottom (diff-repaintable), so its bytes are
    //          FINAL before any later frame can scroll them off.
    //        Post-tool prose arrives on a NEW placeholder Message (own
    //        widget, fx on), so nothing visible is lost — the animation
    //        simply doesn't outlive the prose it animates.
    {
        const bool has_cards = !msg.tool_calls.empty();

        // ── Pre-emptive end-of-text drain (kills the burst at its ROOT) ──
        //
        // The reveal cursor rides ~drain_secs behind the wire to smooth
        // jitter, so when the model closes its text block there is a
        // backlog (~wire_cps × drain_secs) still to type out. The instant
        // a tool_use arrives the guard below MUST hard-snap that backlog
        // to the edge (a growing card would otherwise strand any lagged
        // inline line — scrollback corruption, oracle-proven on ANY
        // glide). That snap is the visible BURST ("first char sticks, then
        // it all appears with the next tool").
        //
        // But on the wire the text block goes QUIET a beat before the
        // tool_use streams. During that gap there is NO card yet, so the
        // reveal can safely GLIDE to the edge — nothing below it can
        // scroll a lagged row off. Detect the gap (live, no cards, bytes
        // have stopped growing for a short window) and request_finalize:
        // the cursor sprints to the edge over a bounded ramp WHILE fx
        // stays on, so it reads as the typewriter catching up, not a
        // paste. By the time the tool_use lands the cursor is already at
        // the edge and the mandatory snap below is a NO-OP → zero burst.
        //
        // Safe against a mid-text pause (model stalls mid-sentence, not
        // actually done): if more bytes arrive after a premature drain,
        // the widget simply resumes revealing from the new edge — the
        // early catch-up looks like "caught up, waiting," never a burst,
        // and never touches scrollback (still no card). Gated on the same
        // since_grow window the RAF logic uses so it never fights active
        // streaming.
        // Only drain when the WIRE HAS STOPPED streaming (is_streaming()
        // false), not on a mid-stream inter-delta gap. A slowly-streaming
        // model routinely pauses > kTextQuietMs BETWEEN deltas while still
        // mid-message; draining then request_finalize()s, the ramp completes
        // and flips the widget live_ off, and the NEXT delta re-lives it and
        // PASTES the whole delta in one frame (profiled: live=0 idle frames
        // immediately precede every +100-cell burst). The drain's real job is
        // the text→tool seam, where the wire genuinely goes quiet before the
        // tool_use streams; gating on !wire_streaming_here restricts it to
        // exactly that case. text_block_closed (an explicit end-of-text wire
        // event) still drains unconditionally.
        constexpr std::int64_t kTextQuietMs = 120;
        const bool text_gone_quiet =
            !settled && !has_cards && !wire_streaming_here
            && cache.streaming->is_live()
            && cache.streaming->reveal_in_progress()
            && cache.last_grow_tick_ms != 0
            && since_grow_ms >= kTextQuietMs;
        if (text_gone_quiet || msg.text_block_closed)
            cache.streaming->request_finalize(/*ramp_ms=*/160);

        // Gate on is_live(): finish() assigns live_=false through the
        // Tracked<> wrapper, which bumps build_dirty_ on EVERY assignment
        // (even same-value) — running this block per frame would force a
        // full widget rebuild every frame of the whole tool phase (the
        // long-turn "md streaming becomes slow" lag). After the first
        // pass live_ is off, is_live() is false, and the block is skipped;
        // snap/fx-off are no-ops on a finished widget anyway.
        //
        // Why HARD SNAP before the card can paint: the tool card renders
        // BELOW this prose and starts GROWING the same frame it appears
        // (Pending → Running → Done). A glide with the card visible
        // leaves the reveal cursor ~drain_secs behind the live edge, so
        // the lines between cursor and edge are still inline
        // (uncommitted) — and the growing card pushes exactly those lines
        // into native scrollback before the cursor reaches them
        // (oracle-proven: 124 gate recoveries when this glided). The snap
        // eliminates the lag instantly so there is no un-swept inline
        // window for the card to strand.
        //
        // ── Tool-panel DEFERRAL (kills the boundary burst) ──
        //
        // The pre-emptive drain above rarely gets to run: on the wire,
        // content_block_stop(text) and content_block_start(tool_use) are
        // CONSECUTIVE SSE events — usually the same TCP segment — so
        // text_block_closed and has_cards become true in the SAME reduce
        // batch and the drain gets ZERO frames before a card exists. The
        // mandatory snap then pastes the whole wire_cps×drain_secs
        // backlog in one frame: the user-reported "md sticks then bursts
        // when a tool use happens" (tool_boundary_burst_probe reproduces
        // it at 4-10× the steady reveal rate for close→tool gaps
        // ≤ 80 ms, and unconditionally on transports that never emit
        // StreamTextBlockClosed).
        //
        // Resolution: while the cursor is still mid-glide, HOLD THE TOOL
        // PANEL OFF-SCREEN (cache.defer_tool_panel — consumed by
        // append_assistant_body_slots / turn_config_for_assistant_run
        // this same frame) and arm the finalize ramp. With nothing
        // rendering below the prose there is no growing element to push
        // a mid-reveal row past the viewport top — the only rows that
        // can cross it are the OLDEST, fully-revealed committed ones,
        // exactly the normal mid-stream case the oracle already proves
        // safe. The typewriter finishes its glide (typically 300-700 ms
        // of visible catch-up, the RateCursor's natural backlog decay),
        // reveal_in_progress() flips false, and THEN the trio runs —
        // snap is a no-op, fx drops, finish() commits the trailing
        // paragraph — and the panel appears below FINAL prose. Zero
        // paste, and the scrollback invariant (card never paints under
        // un-swept inline rows) holds by construction.
        //
        // Bounds and bail-outs — the deferral must never hide a card
        // indefinitely or delay an interaction:
        //   • LAST-MESSAGE gate: the glide is only scrollback-safe while
        //     NOTHING renders below this prose. The instant a following
        //     message exists (the post-tool sub-turn placeholder — its
        //     markdown/panel renders underneath and grows), a lagged
        //     inline row above it can be pushed past the viewport top
        //     mid-reveal — the exact corruption the hard snap prevents
        //     (oracle-proven: deferring under a growing successor added
        //     9 gate recoveries). So the defer holds only while this
        //     message is the live tail's bottom-most element.
        //   • ALL-PENDING + height-budget gate: the unhide frame grows
        //     the tail by the hidden panel's full height in ONE frame.
        //     Rows that cross the viewport top on a grow are committed
        //     AS PAINTED — if the grow ≥ viewport height, the old
        //     bottom-rule row (mutated into panel content by the unhide)
        //     crosses un-repainted → shadow mismatch → HardReset
        //     (oracle-proven at 60x18: a Running card accumulating 2×H
        //     of hidden progress recovered on unhide). A Pending card's
        //     preview is tail-windowed (height-bounded ~a dozen rows);
        //     Running progress / Done output are unbounded. So the defer
        //     holds only while EVERY card is still Pending AND the
        //     estimated hidden height fits comfortably inside the
        //     viewport. Production timing makes this the common case:
        //     tools flip Running only after finalize_turn, so the whole
        //     args-streaming window (the burst window) stays deferrable.
        //   • kMaxCardDeferMs hard cap: a pathological backlog (the
        //     adaptive ramp in request_finalize stretches to 2.5 s on
        //     tens-of-KB dumps) falls back to the hard snap after 1.5 s
        //     — a small residual paste is the lesser evil vs. a card
        //     that looks stuck. Timed from cache.card_defer_since.
        //   • pending_permission: a tool awaiting approval floats its
        //     permission card as its own live-tail row the same frame —
        //     the user must see WHAT they're approving, so the panel
        //     cannot lag the prompt. Snap immediately.
        // Tool EXECUTION is untouched — kick_pending_tools runs in the
        // reducer regardless of panel visibility; a fast tool may
        // already be Running/Done when its card first paints, which
        // reads as "typewriter finished, then the result landed".
        if (has_cards && !settled && cache.streaming->is_live()) {
            constexpr std::int64_t kMaxCardDeferMs = 1500;
            const std::int64_t defer_now = ::maya::anim_now_ms();
            const std::int64_t deferred_ms =
                cache.card_defer_since_ms == 0
                    ? 0
                    : defer_now - cache.card_defer_since_ms;
            const bool is_tail_bottom =
                !m.d.current.messages.empty()
                && &msg == &m.d.current.messages.back();
            // Hidden-height budget: every card must be Pending, and the
            // worst-case hidden rows must fit well inside the viewport so
            // the unhide grow can never push the mutated seam row past the
            // commit boundary in one frame.
            //
            // This used to be a flat `12 rows per card`, which is not a
            // bound — it is a guess, and it was wrong in the direction that
            // corrupts. A STREAMING edit card renders
            //
            //     1 stat chip + 2×edit_tail_per_side diff rows
            //
            // where per_side = clamp((stream_body_budget()-1)/2, 1, 6)
            // widens with terminal height (edit_body.cpp). At an 80x30
            // terminal that is 1 + 2×6 = 13 body rows, and the card measured
            // 20 rows with chrome — against an estimate of 12. The
            // 8-row under-estimate authorized the instant card with more
            // rows in flight than the viewport could hold, so a
            // half-typed reveal line was committed to immutable scrollback
            // (scrollback_oracle_test, 80x30: marker uniq-2-1 stranded).
            //
            // Derive it from the SAME budget the renderer uses, so the two
            // cannot drift: see stream_card_rows_bound(). Over-estimating is
            // safe here — it only defers the card or resolves a few extra
            // rows; under-estimating strands glyphs in scrollback that no
            // repaint can ever fix.
            bool all_pending = true;
            for (const auto& tc : msg.tool_calls)
                if (!tc.is_pending()) { all_pending = false; break; }
            const int est_hidden_rows =
                static_cast<int>(msg.tool_calls.size())
                * ui::detail::stream_card_rows_bound();
            const bool hidden_fits =
                est_hidden_rows < ::maya::available_height() - 4;

            // ── The ONE fact this whole seam keys off: the reveal backlog ──
            //
            // reveal_backlog = source bytes the typewriter cursor still has
            // to catch up on before it reaches the live edge
            // (debug_reveal_byte_clip() is the cursor's byte position; -1 =
            // already AT the edge). This is the single source of truth for
            // "is there a burst to smooth here", and every branch below reads
            // it — nothing consults reveal_in_progress() (a raw cursor<edge
            // boolean that fires for a 3-byte tail as readily as a 3 KB one)
            // or whether the provider sent a StreamTextBlockClosed event.
            //
            // Why this matters: the deferral hides a fresh tool card until
            // the reveal cursor lands, purely to stop a big unrevealed tail
            // from pasting in one frame at the text→tool seam. Below the
            // threshold there's nothing to smooth, so deferring is pure
            // latency — the card sits hidden for the finalize ramp before it
            // appears. Keying the decision on reveal_in_progress() instead of
            // the backlog SIZE was the Anthropic-vs-OpenAI "liveness"
            // asymmetry: Anthropic ships prose in 50-100 char deltas with
            // 90-200 ms gaps, so at the seam a small last-delta tail is
            // almost always still gliding (reveal_in_progress() true → defer,
            // card held); OpenAI-compat usually has the cursor already at the
            // edge (false → card shows at once, "more live"). Gating on the
            // backlog SIZE makes every provider behave identically: pop the
            // card immediately for the common short-prose-then-tool case,
            // still smooth a genuine large-backlog burst. ~2 wrapped rows.
            constexpr std::size_t kMinDeferBacklogBytes = 160;
            const std::size_t reveal_clip =
                cache.streaming->debug_reveal_byte_clip();
            const std::size_t reveal_backlog =
                (reveal_clip == static_cast<std::size_t>(-1)
                 || source.size() <= reveal_clip)
                    ? 0                                   // cursor at the edge
                    : source.size() - reveal_clip;
            const bool backlog_worth_smoothing =
                reveal_backlog >= kMinDeferBacklogBytes;

            // ── Headroom proof: why the card can pop while prose reveals ──
            //
            // The scrollback oracle's invariant is that committed rows are
            // append-only. A mid-reveal row (ghosted tail / scramble glyphs)
            // is only DANGEROUS if it can reach immutable scrollback before
            // it resolves — and a row reaches scrollback ONLY by being pushed
            // past the viewport top, which requires total content height to
            // exceed the viewport.
            //
            // Geometry we control: the reveal tail is always the BOTTOM-MOST
            // prose of this message, and the only thing rendered below it is
            // this message's tool card(s). So the unresolved tail crosses the
            // top iff
            //
            //     rows_below_tail + tail_rows  >  viewport_rows
            //
            // The KEY refinement over a binary "does it all fit" gate: the
            // invariant is not "the tail is fully revealed" but "nothing
            // UNRESOLVED crosses the top". Those differ. Rows leaving the
            // viewport are only dangerous while they are still ghosted, and
            // their animation is about to become invisible anyway. So when
            // the tail does NOT fit we do not hide the card and we do not
            // paste the whole tail — we resolve EXACTLY the overflowing
            // prefix (advance_reveal_floor) and let every still-visible row
            // keep its typewriter. The card therefore appears immediately in
            // ALL cases; only the part you could not have watched is skipped.
            const int term_rows_now = ::maya::available_height();
            const int cols_now      = std::max(1, ::maya::available_width());

            // Row bound for the unrevealed tail. ceil(bytes/cols) ALONE is
            // NOT an upper bound, for TWO independent reasons — both measured
            // in tests/reveal_headroom_test.cpp:
            //
            //  1. Newlines. It assumes every byte consumes a column, which a
            //     '\n' violates: 100 bytes holding 40 '\n' occupies 40+ rows,
            //     not 2 (measured 41 vs 2, a 20x under-estimate). Newline-
            //     dense markdown (lists, tables, short fenced lines) is
            //     exactly the shape that tends to precede a tool call.
            //  2. Inset. Markdown blocks render inside padding/indent, so the
            //     usable text width is NARROWER than the terminal. Dividing
            //     by the full `cols` therefore over-states how much text fits
            //     per row (measured: a 500-char line at 46 cols renders 12
            //     rows, but cols-wide division predicts 11).
            //
            // So: bound each unrevealed LINE separately against a CONSERVATIVE
            // wrap width. A line of length L costs ceil(L/wrap_w) rows,
            // minimum 1 (a blank line still occupies a row). Summing that over
            // the tail dominates both the wrap term and the line-count term.
            // Word-wrap only breaks EARLIER than a hard character wrap (it
            // backs up to a space), so a character-wrap count at a width no
            // greater than the real one is a genuine upper bound.
            constexpr int kBlockInset = 2;   // padding/indent safety margin
            const int wrap_w = std::max(1, cols_now - kBlockInset);
            const std::string_view tail_sv =
                std::string_view{source}.substr(source.size() - reveal_backlog);
            auto rows_of_line = [&](std::size_t len) {
                return std::max<int>(
                    1, static_cast<int>((len + static_cast<std::size_t>(wrap_w) - 1)
                                        / static_cast<std::size_t>(wrap_w)));
            };
            int est_tail_rows = 0;
            {
                std::size_t line_start = 0;
                while (line_start <= tail_sv.size()) {
                    const std::size_t nl = tail_sv.find('\n', line_start);
                    const std::size_t len =
                        (nl == std::string_view::npos ? tail_sv.size() : nl) - line_start;
                    est_tail_rows += rows_of_line(len);
                    if (nl == std::string_view::npos) break;
                    line_start = nl + 1;
                }
            }

            // Chrome the tail shares the viewport with: composer + status +
            // the turn's own header/rail. Matches the margin hidden_fits uses.
            constexpr int kChromeRows = 6;
            // Slack for block-level markup in the unrevealed tail that adds
            // rows beyond its own text lines (code-fence borders, table
            // separators, blockquote padding). MEASURED, not guessed:
            // reveal_headroom_test renders a fence/quote/table corpus at 5
            // widths and asserts est_tail_rows + this slack covers every
            // real height — worst observed deficit is 2 rows (table borders),
            // so 4 holds with 2 rows of margin. Change maya's markdown
            // chrome and that corpus fails before this proof can authorize
            // a card that strands ghosted rows. Keep the two copies equal.
            constexpr int kBlockChromeSlack = 4;
            // How many rows must leave the viewport once the card is shown.
            // >0 means that many LEADING tail rows would scroll into
            // immutable scrollback while still ghosted.
            const int overflow_rows =
                (term_rows_now <= 0)
                    ? 0
                    : (est_hidden_rows + est_tail_rows + kChromeRows
                       + kBlockChromeSlack) - term_rows_now + 1;

            const bool instant_card_ok =
                backlog_worth_smoothing
                && is_tail_bottom
                && all_pending
                && !m.d.pending_permission
                && term_rows_now > 0;

            // Defer ONLY when the geometry is unusable (no viewport dims) or
            // a precondition unrelated to height fails. Height alone never
            // forces a hold any more — overflow is handled by resolving the
            // overflowing prefix instead.
            const bool can_defer =
                backlog_worth_smoothing
                && !instant_card_ok
                && is_tail_bottom
                && all_pending
                && hidden_fits
                && !m.d.pending_permission
                && deferred_ms < kMaxCardDeferMs;
            if (can_defer) {
                if (cache.card_defer_since_ms == 0)
                    cache.card_defer_since_ms = defer_now;
                cache.defer_tool_panel = true;
                // Glide to the edge now — covers transports that never
                // emit StreamTextBlockClosed (the drain above may not
                // have fired) and re-arms idempotently when it did.
                cache.streaming->request_finalize(/*ramp_ms=*/160);
            } else if (instant_card_ok) {
                // ── INSTANT CARD + PARTIAL RESOLVE ──
                //
                // The card is shown THIS frame unconditionally. If the tail
                // fits, nothing else is needed and the whole reveal keeps
                // animating. If it does NOT fit, resolve just the leading
                // `overflow_rows` rows so no ghosted cell can be stranded in
                // scrollback, and let the rest keep typing on screen.
                if (overflow_rows > 0) {
                    // Rows → codepoint prefix. Walk whole LINES and stop once
                    // the accumulated row cost covers the overflow. Rounding
                    // is deliberately UP (we consume the entire line that
                    // straddles the boundary): resolving MORE than strictly
                    // required is always safe, resolving less is not.
                    std::size_t resolve_bytes = 0;
                    int rows_acc = 0;
                    std::size_t line_start = 0;
                    while (line_start <= tail_sv.size() && rows_acc < overflow_rows) {
                        const std::size_t nl = tail_sv.find('\n', line_start);
                        const std::size_t len =
                            (nl == std::string_view::npos ? tail_sv.size() : nl) - line_start;
                        rows_acc += rows_of_line(len);
                        if (nl == std::string_view::npos) {
                            resolve_bytes = tail_sv.size();
                            break;
                        }
                        resolve_bytes = nl + 1;
                        line_start    = nl + 1;
                    }
                    // advance_reveal_floor takes an ABSOLUTE codepoint
                    // position in the whole source, so count the revealed
                    // prefix plus the slice we just decided to resolve.
                    const std::size_t abs_bytes =
                        (source.size() - reveal_backlog) + resolve_bytes;
                    std::size_t cp = 0;
                    for (std::size_t i = 0; i < abs_bytes && i < source.size(); ++i)
                        if ((static_cast<unsigned char>(source[i]) & 0xC0) != 0x80) ++cp;
                    cache.streaming->advance_reveal_floor(cp);
                }

                // Do NOT snap and do NOT finish(): the reveal stays live and
                // keeps gliding under the visible card. request_finalize arms
                // the adaptive ramp so the tail still lands promptly (and, on
                // transports without StreamTextBlockClosed, at all) instead of
                // crawling at the readable floor now that there is no wire
                // jitter left to smooth.
                cache.defer_tool_panel    = false;
                cache.defer_exit_finished = false;
                cache.card_defer_since_ms = 0;
                cache.streaming->request_finalize(/*ramp_ms=*/160);
                ::maya::anim::keep_animating();
            } else {
                // Exit (glide done / cap hit / bail-out). Instead of an
                // INSTANT snap+finish (which pasted the whole typed-but-
                // unrevealed backlog in one frame at the tool boundary — the
                // "first char sticks then it all appears with the next tool"
                // burst), arm a BOUNDED GLIDE: the reveal sprints to the edge
                // over ~150 ms (fast but visible), and finish() runs on a
                // LATER frame once the cursor lands, via the same phase-1/
                // phase-2 deferred-exit path below. 150 ms is short enough to
                // stay scrollback-safe (a growing card can't strand a lagged
                // row before the ramp lands) yet reads as "catching up to
                // land" rather than a paste.
                //
                // Two cases still finish IMMEDIATELY (no glide): the reveal
                // backlog is below the smoothing threshold (the common
                // already-caught-up / tiny-tail case — nothing to glide), or
                // we never entered a live reveal. Those keep the original
                // single-frame behaviour, which is imperceptible. Same
                // backlog_worth_smoothing SSOT the defer gate uses — no second
                // reveal_in_progress() reading with different semantics.
                const bool was_deferring = cache.defer_tool_panel;
                const bool has_backlog = backlog_worth_smoothing;
                if (has_backlog) {
                    // Bounded glide, then defer the finish to the exit path.
                    cache.streaming->snap_reveal_to_edge(/*glide_ms=*/150);
                    cache.defer_tool_panel    = true;
                    cache.defer_exit_finished = false;
                    ::maya::anim::keep_animating();
                } else {
                    cache.streaming->snap_reveal_to_edge();   // no-op / instant
                    cache.streaming->set_reveal_fx(false);
                    cache.streaming->finish();
                    cache.card_defer_since_ms = 0;
                    if (was_deferring) {
                        cache.defer_exit_finished = true;   // unhide next frame
                        ::maya::anim::keep_animating();
                    } else {
                        cache.defer_tool_panel = false;
                    }
                }
            }
        } else if (has_cards && !settled && cache.defer_tool_panel) {
            if (!cache.defer_exit_finished) {
                // The deferral's finalize ramp completed and the widget
                // flipped live_ off ON ITS OWN (advance_reveal_cursor_'s
                // scramble-settle gate) before the exit above got a frame.
                // finish() must STILL run exactly once: it force-commits
                // the trailing paragraph out of render_tail's inline path,
                // whose off-by-one wrap vs the committed-block path is the
                // settle-time row rewrite ("t1-settle corruption"). Panel
                // stays hidden this frame (phase 1); unhides next (phase 2).
                cache.streaming->set_reveal_fx(false);
                cache.streaming->finish();
                cache.defer_exit_finished = true;
                ::maya::anim::keep_animating();
            } else {
                // Phase 2: the finish-mutation frame has painted; the
                // panel now appears as a pure bottom-append grow.
                cache.defer_tool_panel   = false;
                cache.defer_exit_finished = false;
                cache.card_defer_since_ms = 0;
            }
        } else {
            // No cards / settled / widget already finished — make sure a
            // stale defer can never hide a panel on a later frame.
            cache.defer_tool_panel    = false;
            cache.defer_exit_finished = false;
            cache.card_defer_since_ms = 0;
        }
    }

    //    (b) Viewport HEIGHT SHRINK: the terminal autonomously pushes the
    //        top viewport rows into native scrollback. If the live reveal
    //        edge is among them it freezes stale (the height-resize
    //        corruption). A resize is a discrete user event, so rendering
    //        the tail fully-revealed for that one frame is imperceptible
    //        and leaves no ghosted row to strand. snap_reveal_to_edge is
    //        a no-op when settled / not reveal_fx / already at the edge.
    {
        const int cur_h = ::maya::available_height();
        if (cache.last_render_height > 0 && cur_h < cache.last_render_height
            && !settled)
            cache.streaming->snap_reveal_to_edge();
        cache.last_render_height = cur_h;
    }

    auto built = cache.streaming->build();

    // Keep the 16 ms frame armed while EITHER new bytes are actively
    // flowing (stream_in_motion) OR the widget's reveal cursor is still
    // gliding toward the live edge after a burst. The second condition is
    // what makes the typewriter continuous across a wire pause: the model
    // ships a burst then goes quiet for 100-200 ms, but the cursor still
    // has a backlog to type out — without re-arming here the loop would
    // fall to the Tick cadence and the cursor would jump on the next wake
    // (the "bursts, not continuous" symptom). build() advanced the cursor
    // just above, so reveal_in_progress() reflects this frame's state.
    //
    // Third condition — a background parse in flight (is_parsing()):
    // set_content_async hands a large divergent prefix to a worker and
    // keeps returning the PREVIOUS element tree until the result lands.
    // maybe_apply_async_ (which adopts the landed result) runs ONLY from
    // build(), i.e. only when view() runs, i.e. only when the visual hash
    // advances. If bytes pause mid-parse (a wire burst-then-quiet), the
    // hash stops advancing, build() stops being called, and the finished
    // parse sits unapplied — the tail freezes until the next delta or the
    // 100 ms Tick happens to wake the loop (the "md gets stuck then
    // bursts" stutter on big pastes / large reflows). Keeping the frame
    // armed while parsing makes build() keep polling so the result is
    // adopted the instant the worker finishes.
    //
    // Fourth condition — the widget is LIVE with reveal_fx (is_live()).
    // render_live_panel_ animates the trailing-edge scramble / gradient
    // / pulsing caret EVERY frame the widget is live_, even when the
    // reveal cursor has caught up to the edge (backlog 0) and is just
    // waiting for the next token. The first three terms can ALL be false
    // in that pinned-but-live state during a slow mid-stream gap: bytes
    // aren't flowing (stream_in_motion can lapse if phase briefly leaves
    // Streaming during a tool round-trip AND the 3 s window expires),
    // the cursor is at the edge (reveal_in_progress false), no ramp, no
    // parse. Without this term the caret would stop pulsing and the turn
    // would look frozen even though the model is still working. Gating on
    // is_live() (the exact condition render_live_panel_ animates under)
    // guarantees the caret keeps breathing for the whole live window,
    // independent of phase or any timeout — the same robustness principle
    // as the is_streaming() caret gate, applied to the widget's own live
    // state. Cost is the bounded ~0.1-0.4 ms no-content repaint, paid
    // only while a turn is genuinely live.
    const bool live_caret =
        !settled && cache.streaming->is_live();
    if (stream_in_motion
        || live_caret
        || cache.streaming->is_animating()) {
        ::maya::anim::keep_animating();
    }

    if (stream_prof) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - prof_t0).count();
        static std::FILE* out = []() -> std::FILE* {
            // Use the platform temp dir so the AGENTTY_STREAM_PROF knob
            // works on Windows (no /tmp) too; computed once.
            std::error_code ec;
            auto p = std::filesystem::temp_directory_path(ec);
            if (ec) return nullptr;
            p /= "agentty-stream-prof.log";
            return std::fopen(p.string().c_str(), "a");
        }();
        if (out) {
            // Real reveal cursor (the byte the typewriter has reached) and
            // the per-call jump in it — the number that shows a BURST. A
            // smooth glide moves the clip a few bytes per frame; a paste
            // (tool-boundary snap, in-progress-line reveal) jumps it by
            // hundreds in one call. dclip is the honest burst signal.
            const std::size_t clip = cache.streaming->debug_reveal_byte_clip();
            static std::size_t prev_clip = 0;
            const long long dclip =
                static_cast<long long>(clip == static_cast<std::size_t>(-1)
                                           ? source.size() : clip)
              - static_cast<long long>(prev_clip);
            prev_clip = (clip == static_cast<std::size_t>(-1)) ? source.size() : clip;
            std::fprintf(out,
                "[stream] src=%zu clip=%zu dclip=%+lld live=%d finalizing=%d "
                "settled=%d fastpath=%d build_us=%lld\n",
                source.size(),
                clip == static_cast<std::size_t>(-1) ? source.size() : clip,
                dclip,
                cache.streaming->is_live() ? 1 : 0,
                cache.streaming->is_finalizing() ? 1 : 0,
                settled ? 1 : 0, already_settled_into_cache ? 1 : 0,
                static_cast<long long>(us));
            std::fflush(out);
        }
    }

    // Live markdown body returned UNPADDED. Composer anti-bounce — the
    // stream-start indicator→first-content seam, the typewriter crossing
    // a block boundary, a tool card collapsing as it settles — is handled
    // autonomously in maya (Runtime::render): it tracks the live
    // transcript's running-max height and pads up to it across a
    // transient dip, decaying once the shrink proves real so idle carries
    // no dead space. maya owns that mechanism end-to-end; this body
    // element just renders the revealed extent honestly.
    return built;
}

// ── Per-speaker visual identity: rail color + glyph + display name.
//    Centralized so the rail color, the header glyph, and the bottom
//    streaming indicator stay in lockstep.
struct SpeakerStyle {
    maya::Color color;
    std::string glyph;
    std::string label;
    // Smart Mode role accent, empty when Smart Mode didn't route this turn.
    // Rendered as a dim " · strategist" suffix next to the model name so
    // delegation is legible without a second UI surface.
    std::string role_label;
    maya::Color role_color;
};

// Per-role accent for Smart Mode turns. Distinct hues so a scrolled
// transcript reads as a delegation trace at a glance: who did the thinking,
// who did the work, who did the grunt task.
struct RoleAccent { std::string label; maya::Color color; };
[[nodiscard]] std::optional<RoleAccent> role_accent_for(std::string_view role) {
    if (role == "strategic")      return RoleAccent{"strategist",  role_brand_alt};
    if (role == "implementation") return RoleAccent{"implementer", role_info};
    if (role == "utility")        return RoleAccent{"utility",     code_path};
    return std::nullopt;
}

// `msg` is the message being titled: an assistant turn is labelled by the
// model that ACTUALLY served it (Message::served_model), not by the live
// picker selection. Under Smart Mode those differ on most turns, and the
// selection also changes retroactively when the user switches models — so
// reading it here mislabelled the whole transcript.
SpeakerStyle speaker_style_for(Role role, const Model& m, const Message* msg) {
    if (role == Role::User) {
        // User rail is `role_brand` (magenta) — distinct from code-reference
        // cyan and matching the composer's accent color when has-text, so
        // the user's typed message visually flows into their turn header.
        return {role_brand, "\xe2\x9d\xaf", "You"};                  // ❯
    }
    // Prefer the turn's own provenance; fall back to the live selection for
    // turns that predate the field (or ran with Smart Mode off).
    const std::string& id = (msg && !msg->served_model.empty())
                                ? msg->served_model
                                : m.d.model_id.value;

    // ONE decode, from the domain SSOT (domain/model_name.hpp). This used to
    // be an if-chain over is_opus/is_sonnet/is_haiku plus a hand-rolled
    // goto-based version scanner — roughly 60 lines that duplicated both the
    // family classifier and the version extractor, and disagreed with the
    // other copies. Two concrete bugs it carried:
    //
    //   • the chain had no is_fable/is_mythos/is_gpt arm, so the 2026
    //     flagship lane fell through to the "non-Anthropic local model"
    //     branch and rendered WITH a vendor prefix nothing else used;
    //   • Haiku's hue here (bright_cyan) disagreed with the badge widget's
    //     (green), so one model had two identities.
    //
    // Both are now structurally impossible: the family table is exhaustive
    // over the enum and the colour table is proven not to collide with the
    // status hues. `medium()` — "Opus 4.8" — is the turn-header projection;
    // the `· 1M` annotation is the picker's business.
    const auto name = model_name::decode(id);
    SpeakerStyle out{name.color, "\xe2\x9c\xa6", name.medium()};     // ✦
    if (msg)
        if (auto ra = role_accent_for(msg->served_role)) {
            out.role_label = std::move(ra->label);
            out.role_color = ra->color;
        }
    return out;
}

// ── Trailing meta strip for the turn header — `12:34 · 4.2s · turn N`.
// `checkpoint` appends a subtle `· ↺ checkpoint` tag so a restore-point
// user turn reads as an ordinary turn carrying a marker, NOT a separate
// full-width divider widget hanging above the rail (which looked like
// chrome / a broken empty message).
std::string format_turn_meta(const Message& msg, int turn_num,
                             std::optional<float> elapsed_secs,
                             bool checkpoint = false) {
    std::string meta = timestamp_hh_mm(msg.timestamp);
    // Only surface elapsed when it's meaningful. A near-instant local-model
    // reply yields ~0s wall-clock, which `format_duration_compact` renders
    // as a useless "0ms"/"12ms" — drop anything under 100ms.
    if (elapsed_secs && *elapsed_secs >= 0.1f)
        meta += "  \xc2\xb7  " + format_duration_compact(*elapsed_secs);
    if (turn_num > 0)
        meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    if (checkpoint)
        meta += "  \xc2\xb7  \xe2\x86\xba checkpoint";   // ↺ checkpoint
    return meta;
}

// ── Compute the assistant turn's wall-clock elapsed: from previous
//    user message timestamp to this one.
std::optional<float> assistant_elapsed(const Message& msg, const Model& m) {
    if (msg.role != Role::Assistant) return std::nullopt;
    // Memoized: the result depends only on msg.timestamp and the prior
    // User message's timestamp — both immutable once this message
    // exists — so a settled value never changes. Without the memo the
    // reverse scan below is O(sub-turns since the last user turn) run
    // every frame for the run head, which grows with turn depth (the
    // whole in-flight turn sits in the live tail until settle). Cache
    // on the head message's per-message slot; return the cached value
    // on every subsequent frame.
    //
    // Access is PARTITION-SAFE. The head of a live run is frequently the
    // PINNED streaming edge; routing this memo through the settled
    // message_md() would migrate it out of the pinned set every frame,
    // defeating the eviction-immunity. Read via a non-migrating peek()
    // first; only when the slot exists and already holds the memo do we
    // return it. On the (rare) miss we must write, so we touch the slot
    // in whichever home it already lives — message_md_live() if pinned
    // (preserves the pin), message_md() otherwise — so the write never
    // moves the entry across the partition.
    if (const auto* mc = m.ui.view_cache.peek(m.d.current.id, msg.id);
        mc && mc->elapsed_valid)
        return mc->elapsed_cached;
    std::optional<float> result;
    for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
        if (&m.d.current.messages[i] == &msg) continue;
        if (m.d.current.messages[i].role == Role::User) {
            auto dt = std::chrono::duration<float>(
                msg.timestamp - m.d.current.messages[i].timestamp).count();
            if (dt > 0.0f && dt < 3600.0f) result = dt;
            break;
        }
    }
    auto& cache = m.ui.view_cache.is_pinned(m.d.current.id, msg.id)
        ? m.ui.view_cache.message_md_live(m.d.current.id, msg.id)
        : m.ui.view_cache.message_md     (m.d.current.id, msg.id);
    cache.elapsed_cached = result;
    cache.elapsed_valid  = true;
    return result;
}

// Append one Assistant Message's body slots (markdown + tools panel +
// inline permission) to `cfg.body`. Pulled out of turn_config so a run
// of consecutive Assistant Messages can be rendered as ONE Turn (see
// turn_config_for_assistant_run), matching agent_session's discipline
// where every internal seam contributes a body slot to one Turn.
//
// Tool panel emission is split from text emission so a run of
// consecutive sub-turn Messages (each with one tool, no text) renders
// as ONE merged panel rather than N stacked single-tool panels. The
// panel cache key is the `anchor_msg_id` — stable across rebuilds
// because messages are append-only, and unique per merged group
// because each group is anchored at the FIRST contributing Message.
// Build the actions panel fresh every frame (agent_session pattern).
// Settled assistant runs get snapshotted into m.ui.frozen by
// freeze_range — they never re-enter this function. Live panels are
// bounded by the in-flight turn's tool count, so per-frame cost is
// O(active_tools). One AgentTimeline carrier per panel, born here,
// dropped when cfg.body goes out of scope: no shared_ptr identity
// flip races, no spinner-bucket cache, no freeze fast path. The old
// freeze cache was dead weight — the Element it built is snapshotted
// straight into m.ui.frozen the same frame the slot is populated.
//
// Permission card is NO LONGER appended here. The host floats it as
// its own live_tail entry below the active assistant Turn (mirrors
// agent_session's `Permission` sibling under the root vstack).
void append_assistant_tool_panel(maya::Turn::Config& cfg,
                                 const Message& msg,
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    if (tool_calls.empty()) return;

    // Route settled sub-turn panels through the dedicated per-message
    // panel memo (g_panel_render_memo, agent_timeline.cpp). It is keyed
    // on this message's stable id + compute_render_key() so a settled
    // sub-turn is a single-uint64-compare hit — skipping even the
    // O(tools) content-key string build a bare g_panel_cache hit would
    // pay. That is what keeps per-frame view cost flat as an in-flight
    // run accumulates hundreds of settled sub-turns (they stay in the
    // live tail until settle and are re-emitted every frame). The memo
    // lives next to g_panel_cache, NOT in the RAM-bounded ViewCache, so
    // its depth is decoupled from markdown-tree retention. A running
    // tool advances the render_key each frame → natural miss → rebuild
    // (spinner animates), handled inside the memoized helper.
    const int frame = m.s.spinner.frame_index();
    cfg.body.emplace_back(agent_timeline_element_memoized(
        msg.id.value, msg.compute_render_key(),
        tool_calls, frame, style.color));
}

// Reasoning/thinking block for one assistant message, or nullopt when the
// message carries no reasoning text. This is the SINGLE place reasoning is
// turned into a renderable slot; both the single-message path
// (append_assistant_body_slots) and the run-merged live/frozen path
// (emit_subturn) call it, so live and frozen transcripts render identically
// and every provider funnels through one implementation (DRY). The unified
// Message::reasoning_display_text() means Anthropic thinking, Codex reasoning
// summaries, and OpenAI-compat reasoning_content all render the same block.
//
// CENTRAL STREAMING: reasoning text flows through the SAME StreamingMarkdown
// reveal + cache machinery as normal answer text (cached_markdown_for with
// MdView::Reasoning, a sibling "#r" cache slot). So it streams smoothly and
// fast — incremental parse, rate-smoothed reveal cursor, component cache —
// exactly like the answer body, instead of re-parsing a tail window every
// frame.
//
// UX (no interaction, correct with immutable scrollback):
//   • LIVE: a dim, left-gutter-bordered thought stream with a breathing
//     "Reasoning" header, streaming the FULL text via the reveal cursor.
//   • SETTLED: the SAME block stays fully rendered — it does NOT fold to a
//     one-line summary. The header loses its spinner and reads "✦ Reasoned
//     (~N tokens)"; the complete reasoning remains below it, dim, above the
//     answer. Baked at freeze; never changes, never needs a keystroke.
std::optional<maya::Element> reasoning_slot(const Message& msg, const Model& m) {
    // Global display switch (^R in the model picker). Off => no reasoning block
    // at all, for every provider. This is also what makes the Anthropic
    // transport request visible thinking, so "off" is a clean, cheap default.
    if (!m.d.show_reasoning) return std::nullopt;
    if (msg.reasoning_display_text().empty()) return std::nullopt;

    // "Actively reasoning" = this message is the LIVE tail AND it hasn't
    // produced any output yet (no answer prose, no tool calls, text block not
    // closed) AND the stream is running. Intermediate sub-turns that already
    // reasoned and then ACTED (tool calls) are done thinking — they must read
    // "Reasoned", not stay stuck on "Thinking". A message that isn't the last
    // one has, by definition, been followed by more, so it too is settled.
    const auto& msgs = m.d.current.messages;
    const bool is_live_tail =
        !msgs.empty() && msgs.back().id == msg.id;
    const bool produced_output =
        !msg.text.empty() || !msg.streaming_text.empty()
        || !msg.tool_calls.empty() || msg.text_block_closed;
    // Interleaved thinking (Anthropic beta) and multi-item Responses streams
    // legitimately deliver MORE reasoning after the first answer/tool output.
    // If the header flipped to "Reasoned" purely on produced_output, those
    // late bytes would snap into a settled block. Stay live while the #r
    // reveal is still gliding — const peek, no touch/reorder.
    const auto* rslot = m.ui.view_cache.peek(
        m.d.current.id, MessageId{msg.id.value + "#r"});
    const bool reveal_animating =
        rslot && rslot->streaming && rslot->streaming->is_animating();
    const bool active =
        is_live_tail && m.s.is_streaming()
        && (!produced_output || reveal_animating);

    // The reasoning body streams through the CENTRAL streaming-markdown path
    // (own "#r" cache slot, cross-frame-persistent) so it reveals smoothly
    // like normal text and stays fully rendered after settle — no fold, no
    // height cap. The maya::ReasoningStream widget owns the polished chrome
    // (animated header with the live token meter + left rail) and we hand it
    // the cached body.
    maya::Element body = cached_markdown_for(msg, m, MdView::Reasoning);

    // Reasoning renders like a nested Turn: a real bold ┃ left rail (the same
    // atomic bordered box a Turn uses for its rail — turns don't corrupt
    // scrollback because the live edge streams through the shared reveal /
    // scrollback-safety machinery, and reasoning shares that machinery via
    // cached_markdown_for) plus a dimmed body so the answer below always wins
    // the eye. Whole body, smooth reveal, full rail, dim — distinct aside.
    maya::ReasoningStream::Config rcfg;
    rcfg.boxed    = true;    // real ┃ rail (like a Turn), not a per-line prefix
    rcfg.dim_body = true;    // recede the body by color so the answer wins
    // Colors MUST come from the named-ANSI palette, not the widget's hardcoded
    // truecolor defaults (0x8a gray body / indigo rail). agentty's rule is
    // "the terminal theme wins": named ANSI adapts to the user's palette, so
    // "recede" means the same thing on a light OR dark background. The
    // hardcoded gray was low-contrast-to-invisible on light terminals.
    //   body  → a fixed MID-gray (0x9a9a9a). A true mid-gray is the one color
    //           that stays visible on ANY background — it has real contrast
    //           against both a black and a white terminal, so "dimmed" never
    //           collapses to invisible. Named ANSI can't guarantee this (the
    //           theme may map bright_black to near-black, or white to near-bg);
    //           a fixed mid-gray does. Still clearly recedes below the bright-
    //           white answer prose.
    //   header→ a fixed gray (0x9a9a9a), same always-visible reasoning as the
    //           body: ui::muted (bright_black) collapsed to near-invisible on
    //           true-black themes, hiding the live "Thinking" word and the
    //           settled "Reasoned · ~N tokens" meter. NOTE: the widget's
    //           settled-rail dim(accent)=accent.darken() is a no-op on a named
    //           ANSI color, so the magenta rail stays constant (fine — a
    //           steady rail reads as one continuous aside).
    rcfg.accent      = ui::role_brand;                     // ┃ rail + sigil
    rcfg.header_word = maya::Color::rgb(0x9a, 0x9a, 0x9a);  // visible header/meter
    rcfg.body_fg     = maya::Color::rgb(0x9a, 0x9a, 0x9a);  // always-visible dim
    maya::ReasoningStream rs{rcfg};
    rs.set_live(active);
    rs.set_char_hint(msg.reasoning_display_text().size());
    // Reasoning duration for the "· 3.2s" header meter. Once sealed (answer/
    // tool arrived) show the final reasoning_ms; while still thinking, tick a
    // live elapsed off the steady-clock start stamp.
    std::int64_t elapsed = msg.reasoning_ms;
    if (elapsed == 0 && active && msg.reasoning_started_ms > 0) {
        const std::int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        elapsed = std::max<std::int64_t>(0, now_ms - msg.reasoning_started_ms);
    }
    rs.set_elapsed_ms(elapsed);
    return rs.build_with_body(std::move(body));
}

// Single-message body slot append: text (if any) then this message's
// tool panel (if any). Used by `turn_config` for non-run-merged
// renders (User turns delegate to a different branch; this is hit by
// the single-Message Assistant path that pre-dates the run merge).
void append_assistant_body_slots(maya::Turn::Config& cfg,
                                 const Message& msg,
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    // Reasoning block first — it renders ABOVE the answer (chain-of-thought
    // precedes the conclusion). Single shared helper; see reasoning_slot.
    if (auto rs = reasoning_slot(msg, m))
        cfg.body.emplace_back(std::move(*rs));

    const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
    if (has_body) {
        cfg.body.emplace_back(cached_markdown_for(msg, m));
        // Tool-panel deferral: cached_markdown_for just decided (this
        // same frame) whether the reveal cursor is still mid-glide with
        // a fresh tool card. Read via non-migrating peek() — the slot may
        // be PINNED (live), and message_md() here would migrate it out
        // of the pinned set. See the decision site for the mechanism.
        //
        // BUT never defer once a tool is actually EXECUTING. Deferral only
        // exists to smooth the brief prose-glide vs. fresh-*Pending*-card
        // window; a Running/Done tool's card must show immediately. Without
        // this, if the defer-exit machine misses a follow-up frame (the
        // stream goes quiet, no animation frame fires) defer_tool_panel
        // stays true and the running card stays invisible — the "tool shows
        // running but I don't see its card" report.
        const bool any_executing = std::any_of(
            tool_calls.begin(), tool_calls.end(),
            [](const ToolUse& tc){ return !tc.is_pending(); });
        if (!any_executing) {
            if (const auto* mc = m.ui.view_cache.peek(m.d.current.id, msg.id);
                mc && mc->defer_tool_panel)
                return;
        }
    }
    append_assistant_tool_panel(cfg, msg, tool_calls, m, style);
}

} // namespace

maya::Turn::Config turn_config(const Message& msg, std::size_t msg_idx,
                               int turn_num, const Model& m,
                               bool continuation,
                               std::string_view meta_override,
                               std::span<const ToolUse> tool_calls_override) {
    // agent_session pattern: build a fresh Config every call. Settled
    // turns get their Element snapshotted into m.ui.frozen at freeze
    // time and rendered from there; the live tail rebuilds each frame
    // but is bounded to the in-flight turn. No Config / Element
    // memoization here.
    (void)msg_idx;

    // Tool-batch merge plumbing: when the caller passes an override
    // span, treat it as the effective tool_calls for this turn. Saves
    // an O(N) deep-copy of `msg` every frame on the live tail's merged
    // path — the originals stay in `m.d.current.messages[*]` and we
    // borrow them through the span.
    std::span<const ToolUse> tool_calls = tool_calls_override.empty()
        ? std::span<const ToolUse>{msg.tool_calls}
        : tool_calls_override;

    auto style = speaker_style_for(msg.role, m, &msg);

    maya::Turn::Config cfg;
    cfg.glyph        = style.glyph;
    cfg.label        = style.label;
    cfg.rail_color   = style.color;
    cfg.continuation = continuation;
    cfg.meta         = format_turn_meta(msg, turn_num,
                          msg.role == Role::Assistant
                              ? assistant_elapsed(msg, m)
                              : std::nullopt,
                          /*checkpoint=*/msg.role == Role::User
                              && msg.checkpoint_id.has_value());
    if (!meta_override.empty()) cfg.meta = std::string{meta_override};
    // Smart Mode role tag, leading the meta strip so the delegation trace
    // reads down the right-hand gutter: "strategist · 12:34 · 4.2s · turn 3".
    if (!style.role_label.empty())
        cfg.meta = style.role_label + "  \xc2\xb7  " + cfg.meta;

    // Compact-boundary turn: rendered as a real, minimal SYSTEM turn
    // rather than a bare full-width divider (which read as chrome / a
    // stray rule floating in the transcript). It gets its own quiet
    // speaker identity — a `≡` glyph, a muted rail, a "Compacted"
    // label + timestamp meta — and a one-line body that says what
    // happened. This slots into the conversation as a legible event,
    // the same shape as a User/Assistant turn, so it no longer looks
    // broken.
    //
    // The model still receives the full summary text on the wire
    // (msg.text is untouched); the view deliberately elides the raw
    // summary prose (it's written for the model, can be many KB, and
    // would push the preserved tail off-screen the instant compaction
    // lands). CC does the same — its `compact_boundary` transcript line
    // renders as a marker, not the summary body.
    if (msg.is_compact_summary) {
        cfg.glyph      = "\xe2\x89\xa1";              // ≡
        cfg.label      = "Compacted";
        cfg.rail_color = muted;
        cfg.meta       = timestamp_hh_mm(msg.timestamp);
        cfg.body.emplace_back(maya::Turn::PlainText{
            .content = "Earlier conversation summarized to reclaim context.",
            .color   = muted});
        return cfg;
    }

    // Fork provenance card: a synthetic Role::User message seeded at the
    // head of a freshly-forked thread. WITHOUT this the fork would look
    // like a blank screen — messages.empty() is false (this note exists)
    // so the welcome screen is suppressed, yet a plain User bubble of the
    // raw wire instruction would read as if the USER typed the pointer.
    // Instead it gets its own quiet identity — a "\u2443" fork glyph, an info
    // rail, a "Forked" label — and a one-line body telling the user the
    // parent transcript is readable on demand. The model still receives
    // msg.text in full on the wire (it's a real User message, not elided).
    if (msg.fork_note) {
        using namespace maya::dsl;
        cfg.glyph      = "\xe2\x91\x83";              // ⑃
        cfg.label      = "Forked";
        cfg.rail_color = status_info;
        cfg.meta       = timestamp_hh_mm(msg.timestamp);
        cfg.body.emplace_back(maya::Turn::PlainText{
            .content = "Branched into a fresh thread with near-zero context.",
            .color   = fg});
        // Second line names the parent transcript so the user (and the
        // scrollback) can see WHERE the prior conversation lives — the
        // model reads it on demand. Path in code-reference cyan, clipped
        // so a long absolute path can never wrap the card.
        if (!msg.fork_transcript.empty()) {
            std::string content;
            std::vector<maya::StyledRun> runs;
            auto push = [&](std::string_view part, maya::Style st) {
                if (part.empty()) return;
                runs.push_back(maya::StyledRun{content.size(), part.size(), st});
                content.append(part);
            };
            push("prior transcript \xc2\xb7 ", maya::Style{}.with_fg(muted));
            push(msg.fork_transcript, maya::Style{}.with_fg(code_path));
            cfg.body.emplace_back(maya::Turn::BodySlot{maya::Element{maya::TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = maya::TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }}});
        }
        return cfg;
    }

    // Smart Mode ROUTING card: a synthetic, wire-inert event surfacing the
    // per-turn routing DECISION (which model + effort the turn ran on, the
    // classified complexity that scaled it, and which layers are active).
    // Its own identity — a brain glyph, purple rail, "Smart Mode" label —
    // marks it as an orchestration event, not a real turn. The actual
    // subagent delegations render as ordinary `task` tool cards.
    if (msg.smart_routing) {
        using namespace maya::dsl;
        cfg.glyph      = "\xf0\x9f\xa7\xa0";          // 🧠
        cfg.label      = "Smart Mode";
        cfg.rail_color = accent;                       // purple/accent axis
        cfg.meta       = timestamp_hh_mm(msg.timestamp);

        // Complexity → status hue: complex=warn, standard=info, simple/trivial=muted.
        auto cx_color = [&](const std::string& c) -> maya::Color {
            if (c == "complex")  return status_warn;
            if (c == "standard") return status_info;
            return muted;
        };

        // Line 1: "routed → <model>  · effort <e>  · <complexity>" as one
        // truncating node so it can never wrap.
        {
            std::string content;
            std::vector<maya::StyledRun> runs;
            auto push = [&](std::string_view part, maya::Style st) {
                if (part.empty()) return;
                runs.push_back(maya::StyledRun{content.size(), part.size(), st});
                content.append(part);
            };
            push("\xe2\x86\x92 ", maya::Style{}.with_fg(muted));   // →
            push(msg.smart_route_model, maya::Style{}.with_fg(code_path).with_bold());
            push("  \xc2\xb7 effort ", maya::Style{}.with_fg(muted));
            push(msg.smart_route_effort.empty() ? "off" : msg.smart_route_effort,
                 maya::Style{}.with_fg(fg));
            push("  \xc2\xb7 ", maya::Style{}.with_fg(muted));
            push(msg.smart_route_complexity,
                 maya::Style{}.with_fg(cx_color(msg.smart_route_complexity)));
            if (!msg.smart_route_note.empty()) {
                push("   ", maya::Style{});
                push(msg.smart_route_note, maya::Style{}.with_fg(muted).with_italic());
            }
            cfg.body.emplace_back(maya::Turn::BodySlot{maya::Element{maya::TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = maya::TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }}});
        }

        // Line 2: active layers as compact chips.
        {
            std::string content;
            std::vector<maya::StyledRun> runs;
            auto chip = [&](std::string_view label, bool on) {
                const std::size_t s = content.size();
                content.append(on ? "\xe2\x97\x8f " : "\xe2\x97\x8b ");  // ● / ○
                content.append(label);
                content.append("   ");
                runs.push_back(maya::StyledRun{s, content.size() - s,
                    maya::Style{}.with_fg(on ? status_ok : muted)});
            };
            chip("orchestrate", msg.smart_route_orchestrate);
            chip("subagents",   msg.smart_route_subagents);
            cfg.body.emplace_back(maya::Turn::BodySlot{maya::Element{maya::TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = maya::TextWrap::TruncateEnd,
                .runs    = std::move(runs),
            }}});
        }
        return cfg;
    }

    // Proactive-retrieval context turn: like the compact boundary, this is
    // a synthetic User message the MODEL sees in full (msg.text carries the
    // <retrieved-context> block) but the transcript renders as a quiet
    // one-liner so the raw passages don't dominate the user's view. Its
    // own muted identity (a book glyph, "Retrieved context" label) marks
    // it as auto-injected reference, not the user's words.
    if (const auto& pc = msg.proactive) {
        // Parse the [source:path:line] headers out of the block so the
        // card can show the user WHAT grounded the answer — not just how
        // many passages. Each header is a line of the form
        //   [docs:path/to/file.md:12]  /  [skill:git:0]  /  [memory:...]
        // We collect the DISTINCT "source path" pairs (dropping the line
        // number and any duplicate chunks of the same file) so a file
        // that contributed three passages shows once. Cheap: one linear
        // scan of a bounded block.
        int n = 0;
        // Per-distinct-source provenance, in first-seen order. We capture
        // everything the block can tell us so the card reads like a real
        // citation, not just a filename: the kind, the path, the FIRST line
        // number it cited, how many passages (chunks) that same source
        // contributed, and a preview of the first passage's text.
        struct Src {
            std::string kind;      // docs / skill / memory / mcp
            std::string path;      // file path or record id (no :line)
            std::string line;      // first cited line number ("" if none)
            std::string snippet;   // preview of the first passage body
            std::string full;      // full passage body (bounded) for expand
            int         chunks{0}; // # passages from this same source
        };
        std::vector<Src> sources;
        // Per-kind passage tallies (docs/skill/memory/mcp/…) for a compact
        // "2 memory · 1 doc" breakdown on the meta line — in first-seen order.
        std::vector<std::pair<std::string, int>> kinds;
        auto bump_kind = [&](const std::string& k) {
            for (auto& kv : kinds) if (kv.first == k) { ++kv.second; return; }
            kinds.emplace_back(k, 1);
        };
        for (std::size_t p = msg.text.find("\n["); p != std::string::npos;
             p = msg.text.find("\n[", p + 1)) {
            ++n;
            std::size_t open = p + 2;                       // past "\n["
            std::size_t close = msg.text.find(']', open);
            if (close == std::string::npos) continue;
            std::string tag = msg.text.substr(open, close - open);
            // Split "source:path:line" → "source" + "path" (peel a trailing
            // ":line" if the last colon-field is all digits).
            std::size_t colon = tag.find(':');
            std::string src  = colon == std::string::npos
                                 ? std::string{"docs"} : tag.substr(0, colon);
            std::string path = colon == std::string::npos
                                 ? tag : tag.substr(colon + 1);
            bump_kind(src);
            std::string lineno;
            if (std::size_t lc = path.rfind(':'); lc != std::string::npos) {
                std::string_view tail{path.data() + lc + 1,
                                      path.size() - lc - 1};
                if (!tail.empty()
                    && std::all_of(tail.begin(), tail.end(),
                                   [](unsigned char c){ return std::isdigit(c); })) {
                    lineno = std::string{tail};
                    path.resize(lc);
                }
            }

            // Pull the first non-empty line of THIS passage's body as a
            // preview, and the FULL body (to the next "\n[" header or the
            // end) for the expanded view. Body starts just after the
            // header's closing "]\n".
            std::string snippet, full;
            std::size_t body = msg.text.find('\n', close);
            if (body != std::string::npos) {
                ++body;                                     // past the newline
                // Full passage runs to the next header ("\n[") or block end.
                std::size_t next = msg.text.find("\n[", body);
                std::size_t stop = next == std::string::npos
                                     ? msg.text.size() : next;
                full = msg.text.substr(body, stop - body);
                // Trim trailing blank lines / whitespace.
                while (!full.empty()
                       && (full.back() == '\n' || full.back() == ' '
                        || full.back() == '\t' || full.back() == '\r'))
                    full.pop_back();
                // Bound the expansion so a huge passage can't blow the card.
                constexpr std::size_t kFullCap = 800;
                if (full.size() > kFullCap) {
                    full.resize(kFullCap);
                    full += "\xe2\x80\xa6";                 // …
                }

                std::size_t eol = msg.text.find('\n', body);
                std::string line = eol == std::string::npos
                    ? msg.text.substr(body)
                    : msg.text.substr(body, eol - body);
                std::size_t a = line.find_first_not_of(" \t");
                if (a != std::string::npos) {
                    line.erase(0, a);
                    // Collapse internal whitespace runs to a single space so
                    // a citation lifted from an ALIGNED markdown table row
                    // ("col a          | col b") renders as one compact line
                    // instead of a snippet split by a dead gutter of padding
                    // spaces. Tabs/CR normalized too.
                    std::string compact;
                    compact.reserve(line.size());
                    bool in_ws = false;
                    for (char ch : line) {
                        const bool is_ws = ch == ' ' || ch == '\t'
                                        || ch == '\r' || ch == '\n';
                        if (is_ws) {
                            if (!in_ws) compact.push_back(' ');
                            in_ws = true;
                        } else {
                            compact.push_back(ch);
                            in_ws = false;
                        }
                    }
                    // Drop any trailing single space the collapse left.
                    if (!compact.empty() && compact.back() == ' ')
                        compact.pop_back();
                    line = std::move(compact);
                    // Keep the WHOLE first line (bounded only so the render
                    // key / cache stays stable on pathological input) — the
                    // view clips it to the ACTUAL card width at paint time via
                    // `| clip`, so a wide terminal shows far more than a narrow
                    // one instead of a fixed 60-char stub with dead space.
                    constexpr std::size_t kSnipCap = 512;
                    if (line.size() > kSnipCap) {
                        line.resize(kSnipCap);
                        line += "\xe2\x80\xa6";             // …
                    }
                    snippet = std::move(line);
                }
            }

            // Merge into the distinct-source list: a file that contributed
            // three chunks shows once, with chunks==3 and the first cited
            // line / snippet / full body retained.
            auto it = std::find_if(sources.begin(), sources.end(),
                [&](const Src& s){ return s.kind == src && s.path == path; });
            if (it != sources.end()) {
                ++it->chunks;
            } else {
                sources.push_back(Src{std::move(src), std::move(path),
                                      std::move(lineno), std::move(snippet),
                                      std::move(full), 1});
            }
        }

        cfg.glyph      = "\xf0\x9f\x93\x9a";          // 📚
        cfg.label      = "Retrieved context";
        // Blue rail: this is a CONTEXT / reference turn (status_info axis),
        // which also lifts the whole card out of the flat muted gray.
        cfg.rail_color = status_info;

        // Meta line (right-aligned, like elapsed time on assistant turns)
        // carries the whole "how good / how much" summary so the body is
        // free for pure provenance. Rendered as a COLOR-SEGMENTED strip
        // (cfg.meta_element) — a single dim string can't tint the gauge by
        // confidence, and this reads at a glance:
        //   ▰▰▰▰▱▱▱▱ 46%·moderate · 2 doc · 1 mem
        //   └─ filled cells + %+word tinted green/yellow/red by strength;
        //      empty cells muted; kind counts in code-cyan, labels dim.
        // Kept compact (8-cell gauge, abbreviated kind labels, no " total"
        // padding) and clipped so it can NEVER wrap onto a second row —
        // fully responsive: narrow terminals ellipsize the tally tail,
        // wide ones show it whole, and the gauge+percent always survive.
        {
            using namespace maya::dsl;
            // The meta strip is built as a SINGLE TextElement with per-segment
            // color runs, rendered TextWrap::TruncateEnd. A single truncating
            // text node is GUARANTEED to occupy exactly one row at any width:
            // an hstack of separate colored nodes can (and on a ~30-col mobile
            // SSH terminal did) overflow the header row and WRAP onto a second
            // line ("67%·str" / "ong"). Folding every segment into one node +
            // TruncateEnd makes wrapping structurally impossible — narrow
            // terminals ellipsize the tally tail while the gauge+percent
            // (emitted leftmost) always survive.
            std::string meta;
            std::vector<maya::StyledRun> mruns;
            auto push = [&](std::string_view s, maya::Style st) {
                if (s.empty()) return;
                mruns.push_back(maya::StyledRun{meta.size(), s.size(), st});
                meta.append(s);
            };

            // Inside the `pc` guard, so the confidence is reachable only
            // on a message that actually has one — no sentinel compare.
            if (pc->confidence) {
                const double c = std::clamp(*pc->confidence, 0.0, 1.0);
                constexpr int kCells = 8;
                const int filled = static_cast<int>(c * kCells + 0.5);
                // One hue = one meaning: strength maps to the status axis
                // (green ok / yellow warn / red weak) so the gauge color
                // itself answers "trust this grounding?" pre-attentively.
                const char* word = c >= 0.60 ? "strong"
                                 : c >= 0.35 ? "moderate"
                                             : "weak";
                const maya::Color tint = c >= 0.60 ? status_ok
                                       : c >= 0.35 ? status_warn
                                                   : status_error;
                std::string filled_cells, empty_cells;
                for (int i = 0; i < filled; ++i)      filled_cells += "\xe2\x96\xb0";
                for (int i = filled; i < kCells; ++i) empty_cells  += "\xe2\x96\xb1";
                const int pct = static_cast<int>(c * 100.0 + 0.5);
                // Filled portion in the strength hue, empty in muted so the
                // gauge reads as a true fill bar, then bold percent + dim
                // word both tinted so the whole cluster coheres as one signal.
                push(filled_cells, maya::Style{}.with_fg(tint));
                push(empty_cells,  maya::Style{}.with_fg(muted));
                push(" " + std::to_string(pct) + "%", maya::Style{}.with_fg(tint).with_bold());
                push(std::string("\xc2\xb7") + word, maya::Style{}.with_fg(tint).with_dim());
            }

            // By-kind breakdown ("2 doc · 1 mem") answers what & how much.
            // Counts in code-reference cyan (the quantities you scan),
            // labels + dots dim. Abbreviate to keep the strip short; a
            // trailing dim total ties multi-kind rows off.
            auto short_kind = [](std::string k) -> std::string {
                // singularize/abbrev: "memory"->"mem", "docs"->"doc", etc.
                if (k == "memory")  return "mem";
                if (k == "docs" || k == "doc") return "doc";
                if (k == "skill" || k == "skills") return "skill";
                if (k.size() > 5) k.resize(5);
                return k;
            };
            if (!meta.empty())
                push(" \xc2\xb7 ", maya::Style{}.with_fg(muted));
            if (!kinds.empty()) {
                for (std::size_t i = 0; i < kinds.size(); ++i) {
                    if (i) push(" \xc2\xb7 ", maya::Style{}.with_fg(muted));
                    push(std::to_string(kinds[i].second) + " ", maya::Style{}.with_fg(code_path));
                    push(short_kind(kinds[i].first), maya::Style{}.with_fg(muted));
                }
                if (kinds.size() > 1 && n > 0)
                    push("  (" + std::to_string(n) + ")", maya::Style{}.with_fg(muted));
            } else {
                const int shown_n = n > 0 ? n : 1;
                push(std::to_string(shown_n) + " ", maya::Style{}.with_fg(code_path));
                push(shown_n == 1 ? "passage" : "passages", maya::Style{}.with_fg(muted));
            }

            cfg.meta_element = maya::Element{maya::TextElement{
                .content = std::move(meta),
                .style   = {},
                .wrap    = maya::TextWrap::TruncateEnd,
                .runs    = std::move(mruns),
            }};
        }

        // Body = pure provenance, one dense line per distinct source:
        //   └ memory · 0357a6d8            “agentty now has a native…”
        //   └ docs · RUST-CRITIQUE.md:42 ×3  “Where a Rust advocate…”
        // kind is muted, path in code-reference cyan (the actionable
        // anchor), ":line" appended when the citation carried one, and a
        // "×N" chunk badge when one source contributed several passages.
        // The snippet trails in quotes so you see the CONTENT that grounded
        // the answer, all on ONE row. Capped; overflow → "… N more".
        {
            using namespace maya::dsl;
            constexpr std::size_t kMaxSources = 5;
            const std::size_t total = sources.size();
            const bool expanded = pc->expanded;
            // Collect every provenance row into ONE gap-0 vstack pushed as a
            // single BodySlot. maya::Turn inserts a blank gap between adjacent
            // non-blank body slots, so emitting each source as its own slot
            // stranded an empty line between rows. Bundling them into one slot
            // makes maya see a single block → the rows sit flush against each
            // other, and the one inter-slot gap only separates this whole
            // provenance block from the meta/affordance around it.
            std::vector<maya::Element> rows;
            for (std::size_t i = 0; i < sources.size() && i < kMaxSources; ++i) {
                const Src& s = sources[i];
                std::string kindpart = s.kind + " \xc2\xb7 ";     // "docs · "
                std::string pathpart = s.path.empty() ? s.kind : s.path;
                if (!s.line.empty()) pathpart += ":" + s.line;
                // "×N" badge only when this source contributed >1 passage.
                std::string badge = s.chunks > 1
                    ? "  \xc3\x97" + std::to_string(s.chunks) : std::string{};

                // Each provenance entry must be ONE truncating text node.
                // A horizontal stack of individually styled fragments still
                // asks Yoga to lay out every fragment; once the fixed prefix
                // consumes the row, later fragments can wrap even if the
                // final snippet has `| clip`. Combining the runs makes the
                // width constraint structural rather than best-effort.
                auto source_row = [&](std::string_view preview) {
                    std::string content;
                    std::vector<maya::StyledRun> runs;
                    auto push = [&](std::string_view part, maya::Style style) {
                        if (part.empty()) return;
                        runs.push_back(maya::StyledRun{content.size(), part.size(), style});
                        content.append(part);
                    };
                    push("  \xe2\x94\x94 ", maya::Style{}.with_fg(muted));
                    push(kindpart, maya::Style{}.with_fg(muted));
                    push(pathpart, maya::Style{}.with_fg(code_path));
                    push(badge, maya::Style{}.with_fg(status_warn));
                    if (!preview.empty()) {
                        push("  ", maya::Style{}.with_fg(muted));
                        push(preview, maya::Style{}.with_fg(muted));
                    }
                    return maya::Element{maya::TextElement{
                        .content = std::move(content),
                        .style = {},
                        .wrap = maya::TextWrap::TruncateEnd,
                        .runs = std::move(runs),
                    }};
                };

                if (expanded) {
                    // Expanded content remains a single row too. The full
                    // passage is available to the renderer, but is clipped
                    // at the current terminal width rather than pushing the
                    // retrieval card (and following turns) onto extra rows.
                    std::string quoted = s.full.empty() ? std::string{}
                        : " \xe2\x80\x9c" + s.full + "\xe2\x80\x9d";
                    rows.push_back(source_row(quoted));
                } else if (!s.snippet.empty()) {
                    // Keep quotation marks in the same truncating node: no
                    // dangling close quote and, critically, no second line.
                    rows.push_back(source_row(
                        "\xe2\x80\x9c" + s.snippet + "\xe2\x80\x9d"));
                } else {
                    rows.push_back(source_row({}));
                }
            }
            if (total > kMaxSources) {
                rows.push_back(maya::Element{maya::TextElement{
                    .content = "  \xe2\x80\xa6 " + std::to_string(total - kMaxSources)
                             + " more source"
                             + (total - kMaxSources == 1 ? "" : "s"),
                    .style = maya::Style{}.with_fg(muted),
                    .wrap = maya::TextWrap::TruncateEnd,
                }});
            }
            if (!rows.empty()) {
                cfg.body.emplace_back(maya::Turn::BodySlot{
                    maya::detail::vstack().gap(0)(std::move(rows)).build()});
            }
            // Affordance footer — only when there's a full body worth
            // expanding to. Tells the user the toggle exists and what it
            // does; dim so it never competes with the citation content.
            // Kept as its own slot so the single inter-slot gap sets it
            // apart from the flush provenance block above.
            const bool any_full = std::any_of(sources.begin(), sources.end(),
                [](const Src& s){ return !s.full.empty(); });
            if (any_full) {
                const std::string affordance = expanded
                    ? "  ^U collapse"
                    : "  ^U expand full passages";
                cfg.body.emplace_back(maya::Turn::BodySlot{
                    maya::Element{maya::TextElement{
                        .content = affordance,
                        .style = maya::Style{}.with_fg(status_info),
                        .wrap = maya::TextWrap::TruncateEnd,
                    }}});
            }
        }
        return cfg;
    }

    if (msg.role == Role::User) {
        // Substitute chip placeholders (\x01ATT:N\x01) with their
        // human-readable captions so a 400-line paste renders as
        // "[Pasted text · 412 lines · 14 KB]" in the transcript
        // instead of inlining the whole body. The wire still sees
        // the full bytes — the transport calls attachment::expand()
        // at request-build time. Image placeholders consult
        // msg.attachments (which still holds an entry per image with
        // path/media_type/byte_count populated even after the bytes
        // were lifted onto msg.images), so the same chip label
        // formula used in the composer applies here verbatim.
        std::string display;
        if (msg.attachments.empty()) {
            display = msg.text;
        } else {
            display.reserve(msg.text.size());
            std::size_t i = 0;
            while (i < msg.text.size()) {
                if (static_cast<unsigned char>(msg.text[i]) == attachment::kSentinel) {
                    auto len = attachment::placeholder_len_at(msg.text, i);
                    if (len > 0) {
                        auto idx = attachment::placeholder_index(msg.text, i);
                        if (idx < msg.attachments.size()) {
                            display.push_back('[');
                            display.append(attachment::chip_label(msg.attachments[idx]));
                            display.push_back(']');
                        }
                        i += len;
                        continue;
                    }
                }
                display.push_back(msg.text[i++]);
            }
        }
        cfg.body.emplace_back(maya::Turn::PlainText{.content = std::move(display), .color = fg});
    } else if (msg.role == Role::Assistant) {
        append_assistant_body_slots(cfg, msg, tool_calls, m, style);
        if (msg.error) cfg.error = *msg.error;
    }

    return cfg;
}

maya::Turn::Config turn_config_for_assistant_run(
    std::size_t run_first, std::size_t run_end,
    int turn_num, const Model& m)
{
    const auto& msgs = m.d.current.messages;
    // Pre-conditions defended at the only two call sites (build_live_tail
    // / freeze_range), but guard anyway so this function can be reused
    // without subtle row-shape corruption if the range ever turns out empty.
    if (run_first >= run_end || run_first >= msgs.size())
        return {};
    const std::size_t end = std::min(run_end, msgs.size());

    const Message& head = msgs[run_first];
    auto style = speaker_style_for(head.role, m, &head);

    maya::Turn::Config cfg;
    cfg.glyph        = style.glyph;
    cfg.label        = style.label;
    cfg.rail_color   = style.color;
    cfg.meta         = format_turn_meta(head, turn_num,
                          head.role == Role::Assistant
                              ? assistant_elapsed(head, m)
                              : std::nullopt);
    if (!style.role_label.empty())
        cfg.meta = style.role_label + "  \xc2\xb7  " + cfg.meta;

    if (head.role != Role::Assistant) {
        // Defensive: only Assistant runs use the multi-message path.
        // For a User head this collapses to the single-message build.
        return turn_config(head, run_first, turn_num, m,
                           /*continuation=*/false,
                           /*meta_override=*/{},
                           /*tool_calls_override=*/{});
    }

    // Walk the run, emitting one tool panel per Message that carries
    // tools. agent_session's discipline: one tool batch = one panel,
    // flushed when the next text block starts OR the run ends. In
    // agentty terms a "batch" is the tools attached to a single
    // Anthropic Message; the post-tool placeholder model gives us
    // one Message per sub-turn already, so each Message's tool_calls
    // are conceptually one batch (the model's reply text for that
    // sub-turn either precedes them in the same Message or arrives
    // on the following Message).
    //
    // Order in the rendered body mirrors wire order: text(i)
    // → panel(i) → text(i+1) → panel(i+1) …  Sub-turns whose only
    // contribution is a tool batch (no text) emit just a panel.
    // No cross-Message merging: every sub-turn gets its own panel,
    // matching agent_session where each ev::ToolEnd batch becomes
    // its own actions_panel(...).
    std::string error_accum;

    // ── Emit one sub-turn's body slots into `into`. Factored out of
    //    the run loop so the settled-prefix hoist below can reuse the
    //    EXACT same slot construction (byte-identical rows) inside a
    //    nested Turn.
    auto emit_subturn = [&](std::size_t idx, maya::Turn::Config& into) {
        const Message& m_i = msgs[idx];
        // Reasoning block first — above this sub-turn's answer text. Same DRY
        // helper as the single-message path so live and frozen match.
        if (auto rs = reasoning_slot(m_i, m))
            into.body.emplace_back(std::move(*rs));
        const bool has_text = !m_i.text.empty() || !m_i.streaming_text.empty();
        bool defer_panel = false;
        if (has_text) {
            into.body.emplace_back(cached_markdown_for(m_i, m));
            // Same-frame deferral handshake as append_assistant_body_slots:
            // cached_markdown_for just decided (this frame) whether the
            // reveal cursor is still mid-glide with a fresh tool card.
            // Read the flag via a non-migrating peek() — cached_markdown_for
            // may have PINNED this slot (it's live), and re-accessing it
            // through the settled message_md() here would migrate it back
            // out of the pinned set, un-pinning the very edge we just
            // protected. peek() reads from whichever home it lives in
            // without perturbing the partition.
            if (const auto* mc = m.ui.view_cache.peek(m.d.current.id, m_i.id))
                defer_panel = mc->defer_tool_panel;
            // Never defer once a tool is actually EXECUTING — same guard as
            // append_assistant_body_slots. Deferral only smooths the brief
            // prose-glide vs. fresh-*Pending*-card window; a Running/Done
            // card must show immediately, even if the defer-exit machine
            // misses a follow-up frame ("tool shows running but no card").
            if (defer_panel
                && std::any_of(m_i.tool_calls.begin(), m_i.tool_calls.end(),
                               [](const ToolUse& tc){ return !tc.is_pending(); }))
                defer_panel = false;
        }
        if (!m_i.tool_calls.empty() && !defer_panel) {
            append_assistant_tool_panel(
                into,
                m_i,
                std::span<const ToolUse>{m_i.tool_calls},
                m, style);
        }
        if (m_i.error && error_accum.empty()) error_accum = *m_i.error;
    };

    // ── Per-sub-turn stable-identity slots (flat per-frame cost + a
    //    corruption-proof live tail vs. turn depth). ──
    //
    //    An in-flight assistant run can accumulate hundreds of settled
    //    sub-turns that all sit in the live tail until the whole run
    //    settles and freezes (freeze happens only at turn-settle; there
    //    is no sound mid-run freeze — see frozen.cpp). So the live tail
    //    grows unbounded during a deep autopilot run, and its leading
    //    sub-turns overflow into the terminal's IMMUTABLE native
    //    scrollback while the run is still live. Two requirements fall
    //    out of that, and a single mechanism satisfies both:
    //
    //      (1) FLAT per-frame cost: a settled sub-turn must not be
    //          rebuilt/re-walked from scratch every frame, or per-frame
    //          CPU grows O(turn depth).
    //      (2) APPEND-ONLY scrollback: once a sub-turn's rows have
    //          committed to native scrollback they can NEVER be re-
    //          emitted at a shifted position or under a changed element
    //          identity — maya's inline diff treats any committed-row
    //          mutation as uncorrectable and HardResets (\x1b[2J\x1b[3J),
    //          wiping+restranding the transcript. THIS is the user's
    //          report: "a running tool card doesn't show, then the reply
    //          and the prior cards render broken."
    //
    //    The mechanism: wrap EACH settled sub-turn in its OWN bare Turn
    //    keyed on a STABLE per-sub-turn hash (its message id + its own
    //    compute_render_key). Consequences:
    //
    //      • Stable identity for life. A sub-turn's slot occupies a
    //        fixed body index (its ordinal in the run) and, once the
    //        sub-turn settles, a fixed hash_id — for every frame until
    //        it freezes. maya blits it from its component cache (flat
    //        cost) and never re-emits it (append-only safe). A settling
    //        sub-turn just goes from "hash changes each frame" (live) to
    //        "hash frozen" — the exact lifecycle maya already handles
    //        for the whole live tail.
    //      • NO regrouping, ever. The earlier growing-prefix and
    //        fixed/row chunk designs both MOVED a sub-turn from an eager
    //        top-level slot INTO a differently-keyed group Turn as the
    //        boundary advanced. On a viewport short enough that the sub-
    //        turn had already overflowed, that move rewrote a committed
    //        row → HardReset. Per-sub-turn slots never move between
    //        containers, so that transition cannot occur at ANY depth or
    //        terminal size (scrollback_oracle deep_run_turn: green on
    //        every shape).
    //
    //    Robustness carve-outs (a slot must stay per-frame REBUILT, not
    //    stably keyed, while any of these hold — else a card vanishes or
    //    an animation stalls):
    //      • A non-terminal tool (still running): its render_key already
    //        advances every frame (progress/elapsed), so keying it is a
    //        natural miss+rebuild anyway; we still stamp the key so the
    //        blit engages the instant it settles.
    //      • A live/finalizing/revealing/parsing reveal widget: the hash
    //        is invariant across the scramble→clean transition, so a
    //        stable key mid-reveal would freeze scramble glyphs. Keep
    //        such a sub-turn UNKEYED (rebuild every frame) until drained.
    //      • An active tool-panel DEFER machine (defer_tool_panel /
    //        defer_exit_finished / card_defer_since): the defer flag is
    //        per-frame mutable state NOT folded into the render key, and
    //        its two-phase exit only advances while cached_markdown_for
    //        runs every frame. A stable key would (a) bake a panel-hidden
    //        body under a key the defer-clear never bumps → card
    //        invisible forever, and (b) stall the exit machine. Keep it
    //        UNKEYED until the defer machine is idle.
    //
    //    Byte-identity with the flat build (freeze_range) and the whole-
    //    run-hash live-tail path holds by construction: each bare Turn
    //    reuses maya::Turn's own is_blank-gated body assembly and sits as
    //    a top-level body slot, so build_inner inserts exactly one gap
    //    between adjacent non-blank slots — the same rows as emitting the
    //    sub-turn's markdown/panel inline.
    auto subturn_stably_keyable = [&](std::size_t j) -> bool {
        const auto& mj = msgs[j];
        if (mj.role != Role::Assistant) return false;
        // Live wire bytes still arriving → this is the streaming edge (or
        // a sub-turn mid-stream). Its reveal_fx animates the scramble /
        // gradient / caret BETWEEN byte arrivals with NO size change, so
        // a content-keyed hash would freeze between deltas: maya blits the
        // cached bare Turn, cached_markdown_for never re-runs, its
        // request_animation_frame() is never re-armed, and the typewriter
        // FREEZES until an unrelated hash axis flips (the low-CPU "md gets
        // stuck mid-turn" report). Must be built inline so its per-frame
        // builder keeps running. Checked FIRST — independent of whether
        // msg.text has any settled prefix yet.
        if (!mj.streaming_text.empty() || !mj.pending_stream.empty())
            return false;
        for (const auto& tc : mj.tool_calls)
            if (!tc.is_terminal()) return false;
        // Non-migrating peek(): this is a READ-ONLY state probe. Routing
        // it through message_md() would migrate a pinned live entry down
        // into the settled map — exactly the un-pin we must avoid. A slot
        // that doesn't exist yet is trivially not animating (no widget),
        // so nullptr → keyable-so-far.
        const auto* mc = m.ui.view_cache.peek(m.d.current.id, mj.id);
        if (mc && (mc->defer_tool_panel || mc->defer_exit_finished
                || mc->card_defer_since_ms != 0))
            return false;
        // Reveal widget still animating (live / finalize ramp / cursor
        // gliding backlog / async parse) → same freeze hazard as live
        // bytes: the hash is invariant across the scramble→clean
        // transition, so keying it would strand the animation. Applies
        // whether or not text has committed — a message can have a
        // settled text prefix with the widget still live_ during the
        // finalize ramp, so DON'T gate this on text being non-empty.
        if (mc && mc->streaming && mc->streaming->is_animating())
            return false;
        // SAME hazard for the sibling REASONING slot (mj.id + "#r"): reasoning
        // streams through its own StreamingMarkdown whose reveal animates
        // BETWEEN thinking deltas with no answer-channel size change. If we
        // key the turn off content while the reasoning reveal is mid-glide,
        // the reasoning widget's builder stops re-running and the reasoning
        // typewriter FREEZES ("reasoning shows up fully, not streaming"). Must
        // stay inline while the reasoning reveal is live/animating too.
        const auto* rc =
            m.ui.view_cache.peek(m.d.current.id, MessageId{mj.id.value + "#r"});
        if (rc && rc->streaming && rc->streaming->is_animating())
            return false;
        return true;
    };

    // ── Live-edge protection is STRUCTURAL, not a manual touch. ──
    //
    //    The loop below calls cached_markdown_for() once per sub-turn
    //    that has text — O(depth) distinct (thread,msg) accesses per
    //    frame. Previously that walk shared ONE LRU with the live edge,
    //    so once depth exceeded the cap the walk evicted the oldest
    //    entries as it inserted new ones — and the streaming edge (last
    //    message, touched last) sat at the LRU back when the walk began.
    //    Evicting it destroyed its StreamingMarkdown widget + reveal
    //    bookkeeping, restarting the reveal from scratch and stalling the
    //    typewriter (the "md animation not smooth in a long turn"
    //    report). The old fix was a belt-and-braces manual touch of the
    //    edge BEFORE the walk plus a bumped cap so realistic depth
    //    wouldn't thrash — both mitigations of a shared-LRU hazard.
    //
    //    That hazard no longer exists, and there is no LRU at all now.
    //    cached_markdown_for routes a live message (live wire bytes OR an
    //    animating widget) through the cache's PINNED map, which is never
    //    evicted; settled sub-turns go in a separate staging map that is
    //    never evicted either — it is DROPPED per-message at freeze (see
    //    cache.hpp / freeze_range). No walk depth can touch a live edge,
    //    and no walk depth grows memory: both maps are bounded by the
    //    active turn. The stall class is unrepresentable, not merely
    //    unlikely. Nothing to pre-touch here.

    for (std::size_t i = run_first; i < end; ++i) {
        if (msgs[i].role != Role::Assistant) break;   // run boundary
        if (subturn_stably_keyable(i)) {
            // Settled sub-turn: emit as its own bare Turn with a stable
            // per-sub-turn hash_id. maya caches + blits it every frame
            // and never re-emits it once its rows commit to scrollback.
            maya::Turn::Config sub;
            sub.bare       = true;
            sub.rail_color = style.color;
            emit_subturn(i, sub);
            if (sub.body.empty()) continue;   // nothing to show
            sub.hash_id = maya::CacheIdBuilder{}
                .add(std::string_view{"agentty.turn.subturn"})
                .add(std::string_view{msgs[i].id.value})
                .add(msgs[i].compute_render_key())
                .build();
            cfg.body.emplace_back(maya::Turn{std::move(sub)}.build());
        } else {
            // Live / animating / deferring sub-turn: build inline into
            // the outer Turn so its side-effecting per-frame builders
            // (reveal cursor, defer exit machine, spinner) keep running.
            emit_subturn(i, cfg);
        }
    }

    if (!error_accum.empty()) cfg.error = std::move(error_accum);

    return cfg;
}

std::size_t turn_run_end(const std::vector<Message>& messages,
                         std::size_t from)
{
    if (from >= messages.size()) return from;
    if (messages[from].role != Role::Assistant) return from + 1;
    std::size_t end = from + 1;
    while (end < messages.size()
           && messages[end].role == Role::Assistant) {
        ++end;
    }
    return end;
}

maya::CacheId assistant_run_hash_id(
    const Model& m, std::size_t run_start, std::size_t run_end)
{
    const auto& msgs = m.d.current.messages;
    maya::CacheIdBuilder kb;
    kb.add(std::string_view{"agentty.turn.assistant_run"})
      .add(static_cast<std::uint64_t>(run_end - run_start));
    for (std::size_t j = run_start; j < run_end && j < msgs.size(); ++j) {
        kb.add(std::string_view{msgs[j].id.value});
        kb.add(msgs[j].compute_render_key());
    }
    return kb.build();
}


} // namespace agentty::ui
