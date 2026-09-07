// meta_update — reducer for `msg::MetaMsg`. Session-level events that
// don't belong to any single domain: the Tick clock + stream-stall
// watchdog + token-rate sampler, profile cycling, conversation
// compaction kickoff, scroll/toggle, status-toast cleanup, and Quit.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/domain/smart_mode.hpp"   // kSmartModeRows

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/core/cmd.hpp>      // maya::Cmd
#include <maya/core/motion.hpp>   // anim::keep_animating — frame requests

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/selection.hpp"   // provider::active (stall watchdog)
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/panel/settings/items.hpp"      // ascend(): revalidate a restored list
#include "agentty/runtime/settings_registry.hpp"   // tuning row write-back
#include "agentty/runtime/view/helpers.hpp"   // ui::profile_label
#include "agentty/runtime/view/palette.hpp"   // ui::palette_context
#include "agentty/store/store.hpp"
#include "agentty/tool/subagent.hpp"   // set_smart: the task router's own copy
#include "agentty/workspace/checkpoint.hpp"
#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/app/settings_cache.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;
using maya::Cmd;

// Resolve the three role slots for DISPLAY and build the pane.
//
// Resolution is provider-scoped like the wire path, so a pin made under
// another provider shows the auto-fill that will really serve the turn rather
// than a model this endpoint cannot stream. That lookup needs the catalogue,
// which is why it lives here and not in smart_form.cpp — that file stays pure.
//
// Shared with picker.cpp, which reopens the pane after a slot assignment: one
// builder means the reopened pane cannot differ from the original.
form::Form build_smart_form(const Model& m, bool advanced) {
    const auto& sm = m.d.smart;
    const std::string parent = m.d.model_id.value;

    auto slot = [&](smart::ModelRole role) {
        auto rp = smart::resolve_role(role, parent, m.d.effort,
                                      m.d.available_models, sm,
                                      active_provider_id());
        std::string label = ui::pretty_model_label(rp.model);
        smart_form::SlotView v;
        v.label  = label.empty() ? rp.model : label;
        v.pinned = sm.enabled && sm.slot(role).set;
        return v;
    };

    smart_form::Inputs in;
    in.enabled = sm.enabled;
    // An env pin is a LOCK, not a late refusal: the row renders read-only and
    // names the variable, instead of looking live and toasting on Enter.
    if (smart::tuning::enabled_override())
        in.enabled_lock = "env: AGENTTY_SMART_MODE";
    in.strategic      = slot(smart::ModelRole::Strategic);
    in.implementation = slot(smart::ModelRole::Implementation);
    in.utility        = slot(smart::ModelRole::Utility);

    // The advanced rows are projected straight from the persisted config by
    // the settings registry — value, range, label, help, env lock and
    // provenance all come from the row. Nothing to mirror here.
    in.advanced = advanced;
    in.smart = m.d.smart;
    return smart_form::build_form(in);
}

Step meta_update(Model m, msg::MetaMsg mm) {
    return std::visit(overload{
        [&](CompactContext) -> Step {
            // Refuse if a turn is already in flight or compaction is
            // already running — the next CompactContext lands cleanly
            // on Idle. Refuse on an empty thread (nothing to compact).
            // Toast in both cases: a manual compaction that silently does
            // nothing reads as a broken keybind.
            if (!m.s.is_idle() || m.s.compacting)
                return {std::move(m),
                        set_status_toast(m,
                            "cannot compact while the agent is working")};
            if (m.d.current.messages.empty())
                return {std::move(m),
                        set_status_toast(m, "nothing to compact yet")};

            // Compaction is wire-only: we never mutate the transcript.
            // The summarisation prompt is built and trimmed on the fly
            // by `cmd_factory::wire_messages_for_compaction` (which
            // consults `m.s.context_max` and drops oldest turns until
            // the request fits ~65% of the window). The streamed reply
            // lands in `m.s.compaction_buffer`, not in `messages`.
            // The user keeps seeing every turn they ever had — the
            // status banner is the only UI signal that compaction is
            // happening.
            //
            // `compaction_target_index` is the boundary the resulting
            // CompactionRecord will cover: "replace [0, this) with the
            // summary on future wire payloads." Captured at kickoff so
            // turns submitted by the user during compaction (queued
            // via the queue-on-compact path) don't get retroactively
            // folded into a summary they weren't part of.
            m.s.compaction_target_index = m.d.current.messages.size();
            m.s.compaction_buffer.clear();

            auto now = std::chrono::steady_clock::now();
            phase::Active ctx;
            ctx.started       = now;
            ctx.last_event_at = now;
            m.s.phase     = phase::Streaming{std::move(ctx)};
            m.s.compacting = true;
            m.s.status      = "compacting context\xe2\x80\xa6";   // …
            m.s.status_until = {};   // sticky until compaction completes
            return {std::move(m), cmd::launch_stream(m)};
        },

        [&](CycleProfile) -> Step {
            m.d.profile = m.d.profile == Profile::Write   ? Profile::Ask
                      : m.d.profile == Profile::Ask     ? Profile::Minimal
                                                       : Profile::Write;
            // A profile switch re-establishes the trust baseline; drop
            // session "always allow" grants so tightening to Minimal
            // actually re-arms the prompts the user expects. Clear the
            // PERSISTED grants too — otherwise they resurrect at next
            // launch (init re-seeds session_grants from settings) and the
            // re-arm silently un-does itself. This is also the ONLY
            // user-facing way to revoke a stale always-allow.
            const bool had_grants = !m.d.session_grants.empty();
            m.d.session_grants.clear();
            {
                auto s = deps().load_settings();
                if (!s.always_allow_tools.empty()) {
                    s.always_allow_tools.clear();
                    deps().save_settings(s);
                }
            }
            persist_settings(m);
            // Confirm the switch — a profile change is invisible otherwise
            // (the composer chip updates, but a keyboard-driven cycle needs a
            // beat of feedback naming the new mode + what it does).
            const char* gist =
                  m.d.profile == Profile::Write   ? "auto-approves every tool"
                : m.d.profile == Profile::Ask     ? "asks before writes & commands"
                                                  : "asks before every tool";
            auto toast = set_status_toast(
                m, "profile: " + std::string{ui::profile_label(m.d.profile)}
                     + " \xc2\xb7 " + gist
                     + (had_grants ? " \xc2\xb7 always-allow grants reset" : ""),
                std::chrono::seconds{4});
            return {std::move(m), std::move(toast)};
        },
        [&](OpenSmartMode) -> Step {
            // descend(): whatever is open right now (palette, settings list,
            // nothing) becomes this pane's Esc target automatically — no
            // caller stamps an origin any more.
            m.ui.panel.descend(pn::SmartMode{{}, build_smart_form(m), false});
            return done(std::move(m));
        },
        [&](SmartModeAdvanced) -> Step {
            // Reveal/hide the routing-policy rows. The row SET changes, so the
            // form is rebuilt; the cursor is kept and clamped, since hiding
            // rows can leave it past the end.
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (!o) return done(std::move(m));
            o->advanced = !o->advanced;
            const int cursor = o->form.cursor;
            o->form = build_smart_form(m, o->advanced);
            const int n = static_cast<int>(o->form.fields.size());
            o->form.cursor = n > 0 ? std::min(cursor, n - 1) : 0;
            return done(std::move(m));
        },
        [&](CloseSmartMode) -> Step {
            // Esc unwinds ONE level, to whatever opened this pane — ^S from
            // the thread closes; a palette row or the settings list is
            // restored with its full state (query, cursor) intact.
            ascend(m);
            return done(std::move(m));
        },
        [&](SmartModeKey& e) -> Step {
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (!o) return done(std::move(m));

            // What the cursor is on BEFORE the shared reducer runs — activate
            // may hand off, and we need to know which row asked.
            const auto* row = o->form.focused();
            const std::string row_id = row ? row->id : std::string{};
            const auto role = smart_form::role_of_field(row_id);

            const auto applied = form::keys::apply(o->form, e.action);

            if (applied.close) {
                // Route through CloseSmartMode rather than closing here: that
                // is where the origin is read, so a direct close would send
                // Esc to the thread no matter how the pane was opened. Two
                // ways to close one pane is two behaviours to keep in step.
                return agentty::app::update(std::move(m), Msg{CloseSmartMode{}});
            }

            // The master toggle. A locked row is never `changed`, so the env
            // pin is enforced by the form rather than by a toast after the
            // fact — the row renders read-only and names the variable.
            if (applied.changed && !role) {
                // A tuning row — edit the config and hand it to apply_smart,
                // which persists it, installs it on the UI thread and pushes
                // it to the subagent router. The registry clamps on the way in.
                if (const auto* d = settings::registry::find(row_id)) {
                    smart::RoleConfig cfg = m.d.smart;
                    if (const auto* f = o->form.find(row_id))
                        if (const auto* num =
                                std::get_if<form::field::Number>(&f->value))
                            (void)settings::registry::set(
                                cfg, *d, std::to_string(num->value));
                    apply_smart(m, std::move(cfg));
                    const int cursor = o->form.cursor;
                    o->form = build_smart_form(m, o->advanced);
                    o->form.cursor = cursor;
                    return done(std::move(m));
                }

                // The master toggle. Routed through apply_smart like every
                // other config change, so the subagent router is pushed too —
                // turning Smart Mode on while a `task` was queued used to leave
                // that worker routing as if it were still off.
                smart::RoleConfig cfg = m.d.smart;
                cfg.enabled = smart_form::enabled_from_form(o->form,
                                                            m.d.smart.enabled);
                apply_smart(m, std::move(cfg));
                // The slots' locked state depends on the switch, so rebuild.
                const int cursor = o->form.cursor;
                o->form = build_smart_form(m, o->advanced);
                o->form.cursor = cursor;
                return {std::move(m), set_status_toast(m,
                    m.d.smart.enabled ? "Smart Mode on" : "Smart Mode off")};
            }

            // A slot row → hand off to the model picker. The candidate set is
            // the whole catalogue, which is exactly what a `Pick` row means:
            // too large and too dynamic for an inline dropdown.
            if (applied.hand_off && role) {
                m.ui.smart_assign_slot = *role;
                // The overlay is destroyed by the hand-off, so park the
                // advanced flag on the Model — the picker reopens this pane
                // and must bring it back in the state the user left it.
                m.ui.smart_assign_advanced = o->advanced;
                m.ui.smart_assign_from     = o->from;
                m.ui.panel.close<pn::SmartMode>();
                return agentty::app::update(std::move(m), Msg{OpenFusedPicker{}});
            }

            // 'x' resets the focused slot to auto.
            if (e.action.intent == form::keys::Intent::ResetField && role) {
                return agentty::app::update(std::move(m), Msg{SmartModeClearSlot{}});
            }
            return done(std::move(m));
        },
        [&](SmartModeClearSlot) -> Step {
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (!o) return done(std::move(m));
            // `x` only means something on a slot row. role_of_field returns
            // nullopt for the master switch, so "not a slot" cannot fall
            // through into a slot the way an int comparison could.
            const auto* row = o->form.focused();
            if (!row || row->locked) return done(std::move(m));
            const auto role = smart_form::role_of_field(row->id);
            if (!role) return done(std::move(m));
            // Reset to auto through the ONE entry point, so the subagent
            // router loses the pin too. Clearing a slot but leaving a worker
            // routing on it is the same class of bug as the save-without-apply.
            smart::RoleConfig cfg = m.d.smart;
            cfg.slot(*role) = smart::SlotOverride{};
            apply_smart(m, std::move(cfg));
            // The row shows the RESOLVED model, which just changed.
            const int cursor = o->form.cursor;
            o->form = build_smart_form(m, o->advanced);
            o->form.cursor = cursor;
            return {std::move(m), set_status_toast(m, "slot reset to auto")};
        },
        [&](RestoreCheckpoint& e) -> Step {
            // Rewind = destructive double restore: worktree files AND the
            // transcript both return to the instant before the checkpointed
            // user turn was submitted (Zed's "restore checkpoint"). Only
            // from Idle — a running turn holds tool workers that write
            // files; racing them against checkout-index would interleave
            // snapshot bytes with fresh tool output.
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading)
                return {std::move(m),
                        set_status_toast(m, "cannot rewind while the agent is working")};
            // The checkpointed message must still exist (compaction never
            // deletes messages, so this only fails on a stale id).
            const auto& msgs = m.d.current.messages;
            const bool found = std::any_of(msgs.begin(), msgs.end(),
                [&](const Message& msg) {
                    return msg.checkpoint_id && *msg.checkpoint_id == e.id;
                });
            if (!found)
                return {std::move(m),
                        set_status_toast(m, "checkpoint not found in this thread")};
            // thread_loading doubles as the "transcript is about to be
            // rewritten" guard: submit_message queues instead of firing,
            // so no message can land between the index computed at
            // CheckpointRestored time and the truncation.
            m.s.thread_loading = true;
            m.s.status         = "rewinding to checkpoint\xe2\x80\xa6";
            m.s.status_until   = {};
            // Isolated worker: git checkout-index on a large snapshot can
            // block for seconds; never on the shared BG pool.
            auto cmd = Cmd<Msg>::task_isolated(
                [id = e.id](std::function<void(Msg)> dispatch) {
                    CheckpointRestored done;
                    done.id = id;
                    std::string err;
                    done.ok = workspace::restore_checkpoint(id.value, &err);
                    if (!done.ok) done.error = err.empty() ? "restore failed" : err;
                    dispatch(Msg{std::move(done)});
                });
            return {std::move(m), std::move(cmd)};
        },
        [&](CheckpointRestored& e) -> Step {
            m.s.thread_loading = false;
            if (!e.ok) {
                return {std::move(m),
                        set_status_toast(m, "rewind failed: " + e.error,
                                         std::chrono::seconds{6})};
            }
            // Files are already byte-identical to the snapshot; now cut the
            // transcript back to just before the checkpointed user turn and
            // hand its text back to the composer for editing + resubmit.
            auto& msgs = m.d.current.messages;
            auto it = std::find_if(msgs.begin(), msgs.end(),
                [&](const Message& msg) {
                    return msg.checkpoint_id && *msg.checkpoint_id == e.id;
                });
            if (it == msgs.end()) {
                // Restored the files but the message vanished (shouldn't
                // happen — dispatch is gated on Idle + thread_loading).
                return {std::move(m),
                        set_status_toast(m, "files rewound (turn already gone)")};
            }
            // Composer refill: plain-text render of the old prompt — chip
            // placeholders become their visible labels. Raw placeholders
            // can't be resurrected safely (image bodies were drained onto
            // Message.images at submit; a dangling chip 400s the request).
            std::string refill;
            refill.reserve(it->text.size());
            for (std::size_t i = 0; i < it->text.size(); ) {
                if (static_cast<unsigned char>(it->text[i]) == attachment::kSentinel) {
                    auto len = attachment::placeholder_len_at(it->text, i);
                    if (len > 0) {
                        auto idx = attachment::placeholder_index(it->text, i);
                        if (idx < it->attachments.size()) {
                            refill.push_back('[');
                            refill.append(attachment::chip_label(it->attachments[idx]));
                            refill.push_back(']');
                        }
                        i += len;
                        continue;
                    }
                }
                refill.push_back(it->text[i++]);
            }
            const auto cut = static_cast<std::size_t>(it - msgs.begin());
            msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(cut), msgs.end());
            // Compaction records covering now-deleted messages are
            // meaningless (up_to_index points past the end) — drop them.
            std::erase_if(m.d.current.compactions,
                          [&](const Thread::CompactionRecord& r) {
                              return r.up_to_index > cut;
                          });
            m.d.current.updated_at = std::chrono::system_clock::now();
            deps().save_thread(m.d.current);

            reset_composer_draft(m.ui.composer);
            m.ui.composer.text   = std::move(refill);
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            if (m.ui.composer.text.find('\n') != std::string::npos)
                m.ui.composer.expanded = true;

            // Wholesale content swap into a SHORTER transcript — same
            // recovery as ThreadLoaded/NewThread: rebuild the frozen
            // prefix and reset the inline region (the truncated turns'
            // rows live in native scrollback and only reset_inline can
            // clear them; the user explicitly asked for the rewind).
            //
            // Drop the whole render cache too: the truncated tail's
            // messages are gone (their entries would never freeze again
            // — the only per-entry drop), and the surviving prefix is
            // about to be rebuilt into the frozen ledger. Both repopulate
            // lazily on the next render.
            m.ui.view_cache.clear();
            rehydrate_frozen(m);
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            release_to_kernel();
            auto toast = set_status_toast(
                m, "rewound \xc2\xb7 files restored, prompt back in composer",
                std::chrono::seconds{5});
            return {std::move(m),
                    Cmd<Msg>::batch(Cmd<Msg>::reset_inline(), std::move(toast))};
        },
        [&](ScrollThread& e) -> Step {
            m.ui.thread_scroll = std::max(0, m.ui.thread_scroll + e.delta);
            return done(std::move(m));
        },
        [&](TerminalFocus& e) -> Step {
            m.ui.terminal_focused = e.focused;
            return done(std::move(m));
        },
        [&](ToggleRetrievedExpanded& e) -> Step {
            // Flip the addressed retrieved-context card between its compact
            // snippet form and full-passage expansion. Frozen-prefix gate,
            // same frozen-prefix gate as every id-addressed mutation: a card
            // into m.ui.frozen has its expanded state baked into the
            // snapshotted Element (hash_id stamped at freeze, never
            // recomputed), so mutating proactive_expanded there would change
            // model state without reaching the canvas. Only toggle a LIVE
            // (not-yet-frozen) card; on scrollback it no-ops, matching
            // "scrollback is archaeology". The full-passage viewer for a
            // frozen card is the Ctrl+O overlay, which never touches the
            // canvas.
            auto& msgs = m.d.current.messages;
            for (std::size_t i = m.ui.frozen_through; i < msgs.size(); ++i) {
                if (msgs[i].id == e.id && msgs[i].proactive) {
                    msgs[i].proactive->expanded = !msgs[i].proactive->expanded;
                    break;
                }
            }
            return done(std::move(m));
        },
        [&](Tick) -> Step {
            auto now = std::chrono::steady_clock::now();
            // Wall-clock gap since the previous Tick — kept for the
            // suspend-recovery rebase below (last_event_at += tick_gap).
            // NOTE: no animation dt is derived here any more. Every
            // Tick-era animation (spinner, tok/s smoothing) is now
            // clock-driven inside maya's animation framework: the spinner
            // reads its frame off the shared animation clock, and the
            // disp_rate Motion self-ticks on the view-loop frame clock.
            // The old hand-fed dt — and the subtle bug class where the
            // two clocks disagreed ("spinner crawls when view() consumed
            // the frame gap first") — is gone with it.
            if (m.s.last_tick.time_since_epoch().count() == 0) m.s.last_tick = now;
            const auto tick_gap = now - m.s.last_tick;
            m.s.last_tick = now;

            // ── LOOP resume ───────────────────────────────────────
            // A failed iteration parked the loop behind a backoff deadline.
            // finalize_turn is the normal re-send site, but it only runs when
            // a turn ENDS — a sleeping loop has no turn to end, so the Tick
            // (kept armed by animation_demand while a wait is pending) is what
            // brings it back. Fire the moment the deadline passes and the
            // session is genuinely idle.
            if (m.ui.composer.loop_wait_until_ms > 0 && m.s.is_idle()) {
                const auto now_ms = maya::anim::default_clock().now_ms();
                if (m.ui.composer.loop_ready(now_ms)) {
                    m.ui.composer.loop_note_success();   // clears the deadline
                    ++m.ui.composer.loop_iterations;
                    m.ui.composer.text        = m.ui.composer.loop_text;
                    m.ui.composer.attachments = m.ui.composer.loop_attachments;
                    m.ui.composer.cursor =
                        static_cast<int>(m.ui.composer.text.size());
                    auto step = detail::submit_message(std::move(m));
                    // Keep the prompt on screen (the box is read-only while
                    // looping) — same restore finalize_turn does.
                    auto& sc = step.first.ui.composer;
                    sc.text        = sc.loop_text;
                    sc.attachments = sc.loop_attachments;
                    sc.cursor      = static_cast<int>(sc.text.size());
                    return step;
                }
            }

            // Glide the BIG tok/s readout. Retarget the smoothing Motion at
            // the same raw instantaneous rate the status bar would show. The
            // VIEW reads disp_rate.get(), which self-ticks on the shared
            // frame clock and keeps frames armed while gliding — no dt here.
            // When nothing is in flight, pull it to rest at 0.
            {
                double raw_target = 0.0;
                if (auto* ra = active_ctx(m.s.phase);
                    ra && m.s.is_streaming()
                    && ra->first_delta_at.time_since_epoch().count() != 0) {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - ra->first_delta_at).count();
                    if (ms >= 250) {
                        double sec = std::max(0.001, static_cast<double>(ms) / 1000.0);
                        raw_target = (static_cast<double>(ra->live_delta_bytes) / 4.0) / sec;
                    } else {
                        raw_target = m.s.disp_rate.target();  // hold during warm-up
                    }
                }
                if (raw_target != m.s.disp_rate.target())
                    m.s.disp_rate.spring_to(raw_target);
            }

            // Post-freeze settling window countdown. Keeps the tick
            // subscription armed (subscribe.cpp gates on it) for a few
            // frames after the settle-freeze so maya's renderer fully
            // reconciles the live-tail→frozen collapse before the clock
            // stops. Decrement here so each rendered frame burns one.
            if (m.ui.settle_cooldown_ticks > 0) --m.ui.settle_cooldown_ticks;

            // ── Deferred post-rehydrate trim ──────────────────────
            // Armed by ThreadLoaded. The rehydrate budget walk ran on
            // ESTIMATED heights; once the first paint has stamped every
            // sealed block's REAL laid-out height (record_paint), re-run
            // the trim so the frozen window is bounded by ground truth,
            // not by estimate drift (tool-heavy threads under-estimate
            // several-fold). Fires once; waits until every block has a
            // recorded height so the drop provability gate in drop_front
            // can actually release rows.
            maya::Cmd<Msg> rehydrate_trim = maya::Cmd<Msg>::none();
            if (m.ui.pending_rehydrate_trim) {
                bool all_recorded = !m.ui.frozen.empty();
                for (std::size_t k = 0; k < m.ui.frozen.size(); ++k)
                    if (!m.ui.frozen.recorded_at(k)) { all_recorded = false; break; }
                if (all_recorded) {
                    m.ui.pending_rehydrate_trim = false;
                    rehydrate_trim = trim_frozen_if_oversized(m);
                } else if (m.ui.frozen.empty()) {
                    m.ui.pending_rehydrate_trim = false;   // nothing to trim
                }
                // Else: not painted yet — the Tick subscription stays armed
                // on pending_rehydrate_trim (subscribe.cpp) until it is.
            }

            // ── Deferred settle-freeze (post-stream redraw fix) ──────
            // finalize_turn settled the just-finished assistant message
            // (finish() on its StreamingMarkdown) but deferred the freeze
            // to AVOID the post-settle redraw: finish() changes the md
            // widget's build shape + prefix generation, so freezing in
            // the same tick would diff a post-finish frozen tree against
            // the last pre-finish live frame still in maya's prev_cells
            // (cache miss = re-emit the whole turn from the top). By now
            // one view() has painted the settled (post-finish) message
            // via the live tail, so prev_cells holds the post-finish
            // hash. Freezing the byte-and-hash-identical tree HERE is a
            // cache hit (no re-emit). Trim in the same step, exactly as
            // the old inline path did.
            // ── Deferred settle-freeze (post-stream redraw fix) ──────
            // finalize_turn armed the reveal finalize ramp on the
            // just-finished assistant message(s) and set
            // pending_settle_freeze WITHOUT calling finish(). We hold the
            // freeze until the reveal has FULLY drained
            // (live_tail_reveal_settled): the typewriter cursor reached
            // the live edge, the finalize ramp completed, the widget
            // flipped live_ off ON ITS OWN, and the live tail has painted
            // the settled (post-finish-shape) tree into maya's prev_cells
            // at least once. Only THEN do we run settle_message_md (a
            // now-shape-neutral finish() that just stamps the cache
            // fast-path) and freeze. Because the snapshot equals the
            // on-screen frame byte-for-byte, the live-tail→frozen handoff
            // is a maya component-cache HIT — zero re-emit, zero
            // scrollback duplication. This is the agent_session guarantee
            // (live height == settled height at the freeze instant) with
            // the reveal animation fully intact.
            //
            // While the reveal is still animating we keep the flag set
            // and arm an animation frame so the typewriter keeps gliding
            // and the gate is re-checked next frame — it never looks
            // stuck and the freeze never fires early.
            maya::Cmd<Msg> settle_freeze_trim = maya::Cmd<Msg>::none();
            if (m.ui.pending_settle_freeze && m.s.is_idle()) {
                if (live_tail_reveal_settled(m)) {
                    m.ui.pending_settle_freeze = false;
                    for (std::size_t i = m.ui.frozen_through;
                         i < m.d.current.messages.size(); ++i) {
                        auto& mm = m.d.current.messages[i];
                        if (mm.role != Role::Assistant || mm.text.empty())
                            continue;
                        settle_message_md(m, mm);
                    }
                    freeze_through(m, m.d.current.messages.size());
                    // Drop oldest frozen entries past the budget. The
                    // live-tail collapse and the front-trim are TWO
                    // distinct shrinks with OPPOSITE wire requirements,
                    // so they must not be lumped under one none():
                    //
                    //  • The collapse (live tail folds into the frozen
                    //    prefix) is a BOTTOM-shrink with an unchanged
                    //    overflowed prefix. maya's scrollback_prefix_
                    //    matches branch falls through to the append-only
                    //    per-row diff — corruption-free, NO host commit.
                    //
                    //  • The trim drops entries from the TOP of the
                    //    frozen prefix — a top-DELETION maya cannot
                    //    reconcile at render time (the shifted-up canvas
                    //    fails the prefix memcmp and maya commits the NEW
                    //    prefix over PHYSICAL old rows, stranding a
                    //    duplicate). It MUST host-commit exactly the
                    //    dropped rows. trim_frozen_if_oversized returns
                    //    that commit_scrollback(N) when (and only when)
                    //    it actually trimmed; none() when it didn't.
                    //
                    // Forward the trim's Cmd verbatim: none() on the
                    // common collapse-only Tick, commit_scrollback(N) on
                    // the rarer Tick that also trims the front.
                    settle_freeze_trim = trim_frozen_if_oversized(m);

                    // Keep the clock alive for a few frames so maya's
                    // shrink reconciliation (its own diff-path collapse)
                    // completes at fps=0, mirroring agent_session's
                    // always-on clock.
                    m.ui.settle_cooldown_ticks = 6;
                    ::maya::anim::keep_animating();
                } else {
                    // Reveal still draining — keep the frame armed so the
                    // typewriter animates and we re-test the gate next
                    // tick. Never freeze a still-animating turn.
                    ::maya::anim::keep_animating();
                }
            } else if (m.ui.pending_settle_freeze) {
                // Armed to freeze, but the FSM is not yet Idle (a trailing
                // sub-turn teardown, a retry ctx still parked in the phase,
                // or the off-thread markdown parse finishing a hair before
                // the phase reaches Idle). The freeze body above is gated on
                // is_idle(), so it did NOT run this tick — and it did NOT
                // re-arm a frame either. On an fps=0 event-driven screen the
                // periodic Tick can be coalesced away, so if nothing else
                // schedules a frame the reveal settles silently off-thread
                // and pending_settle_freeze strands set: the just-finished
                // markdown sits in the live tail until the next keystroke,
                // which then diffs against stale prev_cells (cache miss →
                // duplicated / late-committed turn). Self-perpetuate a frame
                // as long as the flag is pending so the gate is always
                // re-tested on a real frame once is_idle() flips true.
                ::maya::anim::keep_animating();
            }

            // ── pending_stream → streaming_text ──────────────────────
            // Move buffered wire bytes into streaming_text every tick.
            // No host-side pacing: agentty does NOT animate the stream.
            // The view feeds text + streaming_text + pending_stream in
            // full to the StreamingMarkdown widget, and maya's reveal_fx
            // owns the live-edge animation. It is a plain append; the
            // old rate / backlog / burst-flush / wire-quiet smoother was
            // a second pacing clock that beat against the widget's and
            // produced the "stuck then burst" stutter — removed.
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant)
            {
                auto& msg = m.d.current.messages.back();
                if (!msg.pending_stream.empty()) {
                    msg.streaming_text.append(msg.pending_stream);
                    msg.pending_stream.clear();
                }
            }

            // No mid-stream freeze or trim on Tick. The single freeze
            // site is finalize_turn (via pending_settle_freeze, the
            // agent_session MessageStop analog). Carving during streaming
            // was the documented source of "redraws from top + scrollback
            // corruption": each mid-stream freeze stamped a frozen Turn
            // whose hash_id maya's component cache had not seen on the
            // previous live-tail frame, so the cache missed and re-emitted
            // those rows — sometimes over committed scrollback. The
            // claimed per-frame cost it was meant to bound ("5k-line
            // prose reply re-lays-out every frame") is in fact handled
            // by maya's component cache on a STABLE live-tail hash; the
            // mid-stream carves were the ones invalidating that cache.
            // agent_session never carves mid-stream and shows zero
            // corruption / zero slowdown on long runs.
            maya::Cmd<Msg> midrun_trim = maya::Cmd<Msg>::none();

            // ── Stream-stall watchdog ──────────────────────────────────
            // 120 s of total silence is overwhelmingly likely to be a
            // wedged transport rather than legitimate model behaviour.
            // Clock-skew guard: if a render pass took > 2 s we rebase
            // last_event_at forward so one slow frame can't synthesize
            // a spurious stream-stalled error.
            constexpr auto kTickRebaseThreshold = std::chrono::seconds(2);
            if (auto* a = active_ctx(m.s.phase);
                a && tick_gap >= kTickRebaseThreshold
                && a->last_event_at.time_since_epoch().count() != 0) {
                a->last_event_at += tick_gap;
            }
            constexpr auto kStallSecs = std::chrono::seconds(120);
            // LOCAL providers process the prompt in silence — no SSE bytes,
            // no keepalives — and a 20-30B model on consumer hardware can
            // grind for minutes on a long prompt before its first token.
            // 120 s would cut it mid-processing and the retry would start
            // the same grind over (another face of the local dead loop).
            // Match the transport's local idle allowance; Esc still cancels.
            const bool local_provider = [&] {
                const auto& sel = provider::active();
                return sel.kind == provider::Kind::OpenAI
                    && !sel.openai_endpoint.use_tls;
            }();
            const auto stall_after =
                local_provider ? std::chrono::seconds(600) : kStallSecs;
            if (auto* a = active_ctx(m.s.phase);
                a && m.s.is_streaming()
                && std::holds_alternative<retry::Fresh>(a->retry)
                && a->last_event_at.time_since_epoch().count() != 0
                && now - a->last_event_at >= stall_after) {
                a->retry = retry::StallFired{};
                if (a->cancel) a->cancel->cancel();
                auto since = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - a->last_event_at).count();
                std::string msg = "stream stalled — no events for "
                                + std::to_string(since) + "s";
                StreamError err{std::move(msg)};
                err.from_stall = true;   // carry stall intent on the message
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::milliseconds(0),
                    Msg{std::move(err)})};
            }

            // ── ExecutingTool wedge watchdog ───────────────────────────
            // The stream-stall watchdog above never covers ExecutingTool
            // (is_streaming() is false there) and no SSE events flow once
            // a tool runs, so a turn that strands in this phase has no
            // recovery — it just dies silently (spinner stops, no error,
            // only a new submit revives the app). Two distinct wedges:
            //
            //   (a) NO tool is actually is_running() on the back message,
            //       yet the phase is ExecutingTool. Nothing in flight will
            //       ever dispatch a terminal ToolExecOutput, so the FSM is
            //       permanently stuck. This is a true logic wedge — recover
            //       after a short grace measured from last_event_at (the
            //       phase clock, untouched during ExecutingTool).
            //
            //   (b) A tool IS running but its worker is hung in a blocking
            //       syscall (dead NFS/FUSE mount, frozen network). This is
            //       indistinguishable from a legitimately slow tool by time
            //       alone, so the net is intentionally LONG (kToolWedge) and
            //       measured from the tool's own started_at — never the
            //       stale last_event_at. It is a last-resort safety valve,
            //       NOT the per-tool timeout that was removed at user
            //       request; normal builds / `gh run watch` finish well
            //       under it.
            //
            // Recovery in both cases: fail the offending running tool(s)
            // (or, for (a), just re-fire the scheduler) and call
            // kick_pending_tools, which either continues the turn
            // (post-tool sub-turn) or drops cleanly to Idle.
            //
            // kToolWedge is kept just ABOVE bash's own max self-timeout
            // (300s, the `timeout` arg ceiling in mcp-cpp shell.cpp) so a
            // legitimately long user-set bash command finishes on its own
            // terms before this last-resort net ever trips. The real fix for
            // a SPINNING weak-model loop is the doom-loop breaker in
            // kick_pending_tools, not this hung-syscall net.
            constexpr auto kNoRunningGrace = std::chrono::seconds(30);
            constexpr auto kToolWedge      = std::chrono::seconds(330);
            if (m.s.is_executing_tool()
                && !m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& back = m.d.current.messages.back();
                bool any_running = false;
                bool wedged      = false;
                for (auto& tc : back.tool_calls) {
                    if (!tc.is_running()) continue;
                    any_running = true;
                    auto started = tc.started_at();
                    // Liveness reference: the LATER of launch and the most
                    // recent progress snapshot. A healthy long-runner (a
                    // subagent working through tool calls, a streaming build)
                    // keeps bumping last_progress_at, so the wedge cap resets
                    // with every sign of life and never guillotines it at a
                    // flat 330 s from launch. A tool truly hung on a blocking
                    // syscall emits no progress, so last_progress_at stays put
                    // (or zero) and the net trips exactly as before.
                    auto liveness = started;
                    if (const auto* r = std::get_if<ToolUse::Running>(&tc.status);
                        r && r->last_progress_at.time_since_epoch().count() != 0
                        && r->last_progress_at > liveness)
                        liveness = r->last_progress_at;
                    if (started.time_since_epoch().count() != 0
                        && now - liveness >= kToolWedge) {
                        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - started).count();
                        auto quiet = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - liveness).count();
                        tc.status = ToolUse::Failed{started, now,
                            "tool ran " + std::to_string(secs) + "s ("
                            + std::to_string(quiet) + "s with no progress) "
                            "\xe2\x80\x94 worker likely hung on a blocking "
                            "syscall; failing it so the turn can recover. "
                            "The worker thread may continue in the "
                            "background; its result is discarded if it "
                            "ever returns."};
                        wedged = true;
                    }
                }
                if (!any_running) {
                    auto* a = active_ctx(m.s.phase);
                    if (a && a->last_event_at.time_since_epoch().count() != 0
                        && now - a->last_event_at >= kNoRunningGrace) {
                        wedged = true;   // FSM stranded with nothing in flight
                    }
                }
                if (wedged) {
                    auto kick = cmd::kick_pending_tools(m);
                    return {std::move(m), std::move(kick)};
                }
            }

            // Sample tok/s into the sparkline ring every ~500 ms while
            // the stream is actively producing bytes.
            if (auto* a = active_ctx(m.s.phase);
                a && m.s.is_streaming()
                && a->first_delta_at.time_since_epoch().count() != 0) {
                constexpr auto kSampleInterval = std::chrono::milliseconds{500};
                if (a->rate_last_sample_at.time_since_epoch().count() == 0) {
                    a->rate_last_sample_at    = now;
                    a->rate_last_sample_bytes = a->live_delta_bytes;
                } else if (now - a->rate_last_sample_at >= kSampleInterval) {
                    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - a->rate_last_sample_at).count();
                    auto bytes_delta = (a->live_delta_bytes >= a->rate_last_sample_bytes)
                                       ? (a->live_delta_bytes - a->rate_last_sample_bytes)
                                       : 0;
                    float rate = window_ms > 0
                               ? (static_cast<float>(bytes_delta) / 4.0f)
                                 * (1000.0f / static_cast<float>(window_ms))
                               : 0.0f;
                    m.s.rate_history[m.s.rate_history_pos] = rate;
                    m.s.rate_history_pos =
                        (m.s.rate_history_pos + 1) % StreamState::kRateSamples;
                    if (m.s.rate_history_pos == 0) m.s.rate_history_full = true;
                    a->rate_last_sample_at    = now;
                    a->rate_last_sample_bytes = a->live_delta_bytes;
                }
            }
            {
                std::vector<Cmd<Msg>> trims;
                if (!rehydrate_trim.is_none())
                    trims.push_back(std::move(rehydrate_trim));
                if (!midrun_trim.is_none())
                    trims.push_back(std::move(midrun_trim));
                if (!settle_freeze_trim.is_none())
                    trims.push_back(std::move(settle_freeze_trim));
                if (trims.size() == 1)
                    return {std::move(m), std::move(trims.front())};
                if (!trims.empty())
                    return {std::move(m), Cmd<Msg>::batch(std::move(trims))};
            }

            // ── Idle cache-lapse pre-compaction ───────────────────────────
            // Deep-ride (95 %) is cheap only while the prompt cache holds the
            // prefix. If we've sat idle long enough that the cache is about
            // to lapse AND the prefix is large, the user's NEXT message would
            // re-price the whole thing at full input rate. Pre-empt it: fire
            // a compaction NOW while the prefix is still a warm cache hit, so
            // they come back to a small warm context. This is the ONLY
            // proactive early-compaction path; it is bounded to genuinely
            // large + near-TTL-idle states so ordinary pauses never trip it.
            if (m.s.is_idle()
                && !m.s.compacting
                && !m.s.autocompact_disabled
                && !m.d.current.messages.empty()) {
                const int est = static_cast<int>(
                    cmd::estimate_wire_tokens(m.d.current) * m.s.est_calibration);
                if (m.s.should_compact_on_idle(now, est)) {
                    // Clear last_wire_at so we don't re-fire every Tick while
                    // the compaction request is being dispatched; StreamStarted
                    // re-stamps it when the summary request actually goes out.
                    m.s.last_wire_at = {};
                    auto compact_cmd = Cmd<Msg>::task(
                        [](std::function<void(Msg)> dispatch) {
                            dispatch(CompactContext{});
                        });
                    return {std::move(m), std::move(compact_cmd)};
                }
            }
            return done(std::move(m));
        },
        [&](Quit) -> Step {
            // Trip the active turn's cancel token FIRST. If a stream (or a tool)
            // is in flight — exactly when the spinner/typewriter is animating —
            // its worker is blocked in the HTTP poll/SSL_read. Nothing else
            // signals it on Quit, so without this the loop exits but
            // ~BackgroundQueue's join waits for that worker to notice
            // cancellation the slow way (the next server byte or the idle
            // timeout) — the 4–10 s "^C doesn't quit during an animation" hang.
            // Esc/CancelStream already does this; Quit must too. The token is
            // atomic and the worker's poll loop checks it every ~200 ms, so the
            // join returns promptly.
            if (auto* a = active_ctx(m.s.phase); a && a->cancel) a->cancel->cancel();

            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            // Always file the model the user is CURRENTLY on under the active
            // provider before exit, so a relaunch (or a later switch back to
            // this backend) restores exactly this model instead of the
            // built-in default. Without this, a session where the user never
            // opened the model picker nor switched provider recorded no
            // per-provider recall at all — the "it forgets the model I was
            // using" complaint. persist_settings is load-modify-save, so it
            // preserves every other backend's recall + keys.
            persist_settings(m);
            // The settings seam is write-behind, so that save is queued rather
            // than on disk. Drain it here — the ONE place a synchronous wait is
            // correct, because no frame is drawn after this.
            settings_cache::flush();
            return {std::move(m), Cmd<Msg>::quit()};
        },
        [&](NoOp) -> Step { return done(std::move(m)); },
        [&](RedrawScreen) -> Step {
            // Ctrl-L → viewport-only soft redraw.
            //
            // Drops maya's renderer cell cache (force_redraw zeroes
            // prev_rows on the active InlineFrameState) and on the
            // next compose triggers serialize.cpp's case-(B) path:
            // cursor_up + re-serialize the last `term_h` rows +
            // \x1b[J. Net effect: every cell inside the live viewport
            // is rewritten from the canvas, wiping any ghost glyphs
            // a half-written frame or stray subprocess output left
            // behind. Cheap; preserves scrollback; preserves cursor
            // position relative to the viewport (no "composer jumps
            // to the bottom" jolt).
            //
            // ── SCOPE ─────────────────────────────────────────────────
            //
            // This hotkey fixes viewport corruption ONLY. Scrollback
            // rows that have already overflowed the terminal viewport
            // are owned by the terminal emulator and cannot be
            // repainted by an inline-mode application without also
            // overwriting any non-agentty content above the frame.
            // The hard contract lives at
            // maya/src/render/serialize.cpp’s case-(B) emit — see
            // the SCOPE CONTRACT comment there for the full rationale.
            //
            // If the user's scrollback is mangled (a stray subprocess
            // wrote past agentty, a tmux pane swap got confused,
            // bytes were dropped during a resize), Ctrl-L will not
            // help. The right recoveries in that situation are:
            //   • The terminal emulator's own redraw hotkey — most
            //     terminals bind their Ctrl-L to a full repaint of
            //     the emulator's local cell grid, which IS able to
            //     reach scrollback rows.
            //   • Resizing the terminal window — maya treats a WIDTH
            //     change as a coherence-collapse, routing the next
            //     render through HardReset which emits
            //     \x1b[2J\x1b[3J\x1b[H (every row's wrap points
            //     shift, prev_cells is byte-invalid, no other
            //     recovery is coherent). That sequence wipes host
            //     scrollback too — destructive but structurally
            //     unavoidable on a width change. A HEIGHT-only
            //     resize stays soft (case-(B), no scrollback wipe).
            //     Either way it's a passive consequence of the
            //     resize event, not bound to a keystroke.
            return {std::move(m), Cmd<Msg>::force_redraw()};
        },
        [&](ClearStatus& e) -> Step {
            // No-op if the user (or another handler) wrote a newer
            // status since this cleaner was scheduled — stamps won't
            // match, so the current banner stays.
            if (m.s.status_until == e.stamp) {
                m.s.status.clear();
                m.s.status_until = {};
            }
            return done(std::move(m));
        },
        [&](UpdateCheckDone& e) -> Step {
            // Background release check landed. Store the signal — the
            // status bar's version chip and the palette entry both read
            // update_latest. No toast, no banner: an update NOTICE must
            // never interrupt; the chip is the whole announcement.
            if (e.update_available && !e.latest.empty()) {
                m.s.update_latest = std::move(e.latest);
                m.s.update_url    = std::move(e.url);
            }
            return done(std::move(m));
        },
        [&](UpdateApplied& e) -> Step {
            m.s.update_in_flight = false;
            if (e.ok) {
                // Updated on disk; the running process is the old image.
                // Persistent (no-expiry) banner — restarting is the one
                // thing the user must know to do.
                m.s.update_latest.clear();   // chip disappears
                m.s.status = "✓ updated to v" + e.detail +
                             " — restart agentty to use it";
                m.s.status_until = {};       // sticky until overwritten
                return done(std::move(m));
            }
            auto toast = set_status_toast(
                m, "update failed: " + e.detail +
                   " — try `agentty update` from a shell",
                std::chrono::seconds{8});
            return {std::move(m), std::move(toast)};
        },
    }, mm);
}

void ascend(Model& m) {
    if (!m.ui.panel.ascend()) {   // no parent → opened over the thread
        m.ui.panel.close_all();
        return;
    }
    // Revalidate: the restored snapshot is as old as the descent, and the
    // model may have moved. Only state that indexes a LIVE list needs work —
    // free text (queries) cannot go stale.
    if (auto* p = m.ui.panel.get<pn::CommandPalette>()) {
        // The filtered list is model-dependent (gated rows appear/vanish);
        // clamp the cursor into today's list rather than yesterday's.
        const int sz = static_cast<int>(
            filtered_commands(p->query, ui::palette_context(m)).size());
        p->index = sz <= 0 ? 0 : std::clamp(p->index, 0, sz - 1);
    } else if (auto* s = m.ui.panel.get<pn::SettingsList>()) {
        // Rows derive from live model state (a plugin may have connected or
        // been removed while the child pane was open); clamp the restored
        // cursor into today's list. Same policy as the old reopen path,
        // which also carried a bare index.
        const int rows = static_cast<int>(
            settings::items_for(m, s->concern).size());
        s->index = rows <= 0 ? 0 : std::clamp(s->index, 0, rows - 1);
    } else if (auto* sm = m.ui.panel.get<pn::SmartMode>()) {
        // The form snapshot may predate a config change made in the child
        // (a fused-picker slot assignment writes smart config). Rebuild from
        // the live model — the same builder every (re)open uses — keeping
        // only view state: the cursor and the advanced toggle.
        const int cur = sm->form.cursor;
        auto f = build_smart_form(m, sm->advanced);
        f.cursor = std::clamp(cur, 0,
                              static_cast<int>(f.fields.size()) - 1);
        sm->form = std::move(f);
    }
}

void apply_smart(Model& m, smart::RoleConfig cfg) {
    // Env overrides win over anything a pane or a config file can say, and
    // they win HERE so every holder below sees the same resolved value —
    // rather than each one re-reading the environment and possibly disagreeing.
    settings::registry::apply_env(cfg);

    // 1. Persist. Load-modify-save so a concurrent change to an unrelated
    //    setting is not clobbered by writing a stale whole-Settings.
    auto s = deps().load_settings();
    s.smart = cfg;
    deps().save_settings(s);

    // 2. The UI thread's copy — what the classifier and the effort scaler read
    //    on the next turn.
    m.d.smart = std::move(cfg);

    // 3. The subagent router's copy. `task` runs on a worker with no Model, so
    //    it genuinely needs its own snapshot; this is the push that keeps it
    //    from routing on a policy the user already changed.
    tools::subagent::set_smart(m.d.smart);
}

} // namespace agentty::app::detail
