// rag_settings_update — reducer for the RAG mode picker.
//
// One decision: how proactive (pre-turn) retrieval behaves — On / First turn
// only / Off. The cursor row IS the choice; selecting it sets store::RagMode,
// derives the proactive gate, persists to settings.json, and live-applies to
// the process-wide retriever. The advanced knobs stay at their defaults.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/rag.hpp"
#include "agentty/rag/embed_secret.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"
#include "agentty/tool/subagent.hpp"   // set_smart: the task router's own copy

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace rs = agentty::rag_settings;
namespace eb = agentty::rag::embed;
using maya::Cmd;
using maya::overload;

namespace {

// Persist + live-apply the chosen mode. `proactive` is derived from the mode
// (Off ⇒ no pre-turn injection; the First-turn gate is enforced in modal.cpp).
//
// Both halves are non-blocking BY CONSTRUCTION, not by care taken here:
// `save_settings` is write-behind at the Deps seam, and `rag_apply_settings`
// does its probe + rebuild on a worker. A reducer cannot stall a frame on
// either even if it wanted to.
void commit_mode(store::RagMode mode) {
    auto s = deps().load_settings();
    s.rag.configured = true;
    s.rag.mode = mode;
    s.rag.proactive = (mode != store::RagMode::Off);
    deps().save_settings(s);
    tools::rag_apply_settings(s.rag);
}

// Project an EmbedConfig onto the persisted settings shape. The API key is
// deliberately NOT part of this — it goes to the keystore.
void write_embed_into(store::RagConfig& r, const eb::EmbedConfig& c) {
    r.embed_backend        = std::string{eb::id_of(c.backend)};
    r.embed_model          = c.model;
    r.embed_host           = c.host;
    r.embed_port           = c.port;
    r.embed_tls            = c.tls;
    r.embed_path           = c.path;
    r.embed_model_path     = c.model_path;
    r.embed_tokenizer_path = c.tokenizer_path;
    r.embed_dim            = c.dim;
}

// Load the live embed config for the form's initial state.
[[nodiscard]] eb::EmbedConfig current_embed_config() {
    eb::EmbedConfig c;
    eb::apply_env(c);
    const auto s = deps().load_settings();
    if (!s.rag.embed_backend.empty()) {
        c.backend = eb::backend_from_id(s.rag.embed_backend);
        if (!s.rag.embed_model.empty()) c.model = s.rag.embed_model;
        if (!s.rag.embed_host.empty())  c.host  = s.rag.embed_host;
        if (s.rag.embed_port != 0)      c.port  = s.rag.embed_port;
        c.tls            = s.rag.embed_tls;
        c.path           = s.rag.embed_path;
        c.model_path     = s.rag.embed_model_path;
        c.tokenizer_path = s.rag.embed_tokenizer_path;
        c.dim            = s.rag.embed_dim;
    }
    if (eb::needs_api_key(c.backend))
        c.api_key = eb::load_key(eb::endpoint_key(c));
    return c;
}

// Rebuild the rows after a change that alters WHICH rows exist, keeping the
// cursor on the same logical field. `advanced` must be carried through every
// rebuild: dropping it would silently collapse the pane back to the basic rows
// the next time any field changed the row set.
void resync_rows(rs::EmbedForm& f, store::RagMode mode, bool advanced) {
    const auto* focused = f.form.focused();
    const std::string focused_id = focused ? focused->id : std::string{};
    f.cfg = rs::config_from_form(f.cfg, f.form);

    const bool was_dirty = f.form.dirty;
    f.form = rs::build_form(f.cfg, mode, deps().load_settings(), advanced);
    f.form.dirty = was_dirty;
    for (std::size_t i = 0; i < f.form.fields.size(); ++i)
        if (f.form.fields[i].id == focused_id) {
            f.form.cursor = static_cast<int>(i);
            break;
        }
}

// Pull edits out of the rows and back into cfg (without rebuilding rows).
void sync_cfg(rs::EmbedForm& f) {
    f.cfg = rs::config_from_form(f.cfg, f.form);
}

// Any edit invalidates a previous probe result: the endpoint that answered
// may not be the endpoint now configured.
void invalidate_probe(rs::EmbedForm& f) {
    // Also supersede any probe IN FLIGHT: bumping the generation makes a
    // worker completion that races this edit land as a no-op instead of
    // stamping Ok onto a config it never tested.
    ++f.probe_gen;
    if (!std::holds_alternative<rs::EmbedForm::Idle>(f.probe))
        f.probe = rs::EmbedForm::Idle{};
}

// Project the probe state onto the Action row + footer note, so the outcome
// renders where the action lives. One place, called after every transition.
void refresh_status(rs::EmbedForm& f) {
    using A = form::field::Action;
    if (auto* row = f.form.find(rs::kFieldTest))
        if (auto* act = std::get_if<A>(&row->value)) {
            std::visit([&](const auto& p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, rs::EmbedForm::Testing>) {
                    act->status = "testing";
                    act->tone   = A::Tone::Busy;
                } else if constexpr (std::is_same_v<T, rs::EmbedForm::Ok>) {
                    act->status = "ready · " + std::to_string(p.dim) + "d · "
                                + std::to_string(p.latency_ms) + "ms";
                    act->tone   = A::Tone::Good;
                } else if constexpr (std::is_same_v<T, rs::EmbedForm::Failed>) {
                    act->status = p.why;
                    act->tone   = A::Tone::Bad;
                } else {
                    act->status.clear();
                    act->tone = A::Tone::Neutral;
                }
            }, f.probe);
        }

    f.form.subtitle = eb::describe(f.cfg);

    // Validation is a VALUE: surface it inline rather than refusing silently
    // at save time.
    if (auto v = eb::validate(f.cfg); const auto* bad = std::get_if<eb::Invalid>(&v))
        f.form.note = bad->why;
    else if (eb::is_in_process(f.cfg.backend))
        f.form.note = "no daemon required \xe2\x80\x94 the model runs in-process";
    else if (f.form.dirty)
        f.form.note = "unsaved \xe2\x80\x94 ^T test, ^S save";
    else
        // The RESTING note advertises the advanced key. A bare letter, not ^A:
        // that chord is the form layer's caret-home and tmux's default prefix,
        // so it never reaches the app. Shown only when there is nothing more
        // urgent to say.
        f.form.note = "a  advanced";
}

// Build the pane's form for `mode`, seeded with what the live retriever
// already knows so opening it immediately shows whether embeddings work
// rather than an empty "untested" state.
[[nodiscard]] rs::EmbedForm make_embed_form(store::RagMode mode,
                                            bool advanced = false) {
    rs::EmbedForm f;
    f.cfg  = current_embed_config();
    f.form = rs::build_form(f.cfg, mode, deps().load_settings(), advanced);
    const auto st = tools::rag_embed_status();
    using S = tools::RagEmbedStatus::State;
    if (st.state == S::Ready)
        f.probe = rs::EmbedForm::Ok{st.dim, st.latency_ms};
    else if (st.state == S::Unavailable)
        f.probe = rs::EmbedForm::Failed{st.reason};
    refresh_status(f);
    return f;
}

[[nodiscard]] rs::EmbedForm* form_of(Model& m) {
    auto* o = m.ui.panel.get<pn::Rag>();
    return o ? &o->embed : nullptr;
}

} // namespace

Step rag_settings_update(Model m, msg::RagMsg rm) {
    return std::visit(overload{
        [&](OpenRag) -> Step {
            // ONE pane. The overlay used to open a three-row mode list, with
            // the embedder settings hidden behind a second keypress that
            // nothing advertised. Mode and embedder are two halves of one
            // question, so they are two rows of one form.
            //
            // The form is built SYNCHRONOUSLY. It used to be deferred through
            // a zero-delay Cmd, which meant the overlay existed for at least
            // one frame holding no form — and a form pane with no form claims
            // every key and answers none of them, so the app looked frozen.
            // An overlay must never be representable in a state where it owns
            // the keyboard but cannot act on it.
            const auto s = deps().load_settings();
            const auto mode = s.rag.configured ? s.rag.mode : store::RagMode::On;
            m.ui.panel.descend(
                pn::Rag{{mode, mode, make_embed_form(mode)}});
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](CloseRag) -> Step {
            // Esc unwinds ONE level: the parent snapshot (palette or settings
            // list, full state intact) or the thread when there is none.
            // (Selecting a mode, below, commits and drops to the thread —
            // that's "done", not "back".)
            ascend(m);
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagMove& e) -> Step {
            // Legacy entry point. The pane's own keys arrive as RagEmbedKey;
            // this survives only for callers that synthesise a move.
            if (auto* f = form_of(m)) form::move(f->form, e.delta);
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagAdjust&) -> Step {
            if (auto* f = form_of(m)) (void)form::activate(f->form);
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagAdvanced) -> Step {
            // Reveal/hide the Tier::Advanced rows. The form is rebuilt because
            // the row SET changes; the cursor is kept where it was so the
            // toggle does not also move the selection out from under the user.
            if (auto* o = m.ui.panel.get<pn::Rag>()) {
                o->advanced = !o->advanced;
                const int cursor = o->embed.form.cursor;
                o->embed.form = rs::build_form(o->embed.cfg, o->cursor,
                                               deps().load_settings(),
                                               o->advanced);
                // Clamp: hiding rows can leave the cursor past the end.
                const int n = static_cast<int>(o->embed.form.fields.size());
                o->embed.form.cursor = n > 0 ? std::min(cursor, n - 1) : 0;
            }
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagReset) -> Step {
            commit_mode(store::RagMode::On);
            if (auto* o = m.ui.panel.get<pn::Rag>()) {
                o->cursor = store::RagMode::On;
                o->active = store::RagMode::On;
                o->embed  = make_embed_form(store::RagMode::On, o->advanced);
            }
            return {std::move(m), Cmd<Msg>::none()};
        },

        // ── Embeddings rows ─────────────────────────────────────────
        [&](RagEmbedOpen) -> Step {
            // Idempotent re-seed. Nothing defers to this any more (the pane is
            // built on open), but a caller that wants a fresh probe state can
            // still ask for one.
            if (auto* o = m.ui.panel.get<pn::Rag>())
                o->embed = make_embed_form(o->cursor, o->advanced);
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagEmbedClose) -> Step {
            // Esc unwinds one level at a time (menu → field → pane); the form
            // layer owns that ordering so every pane behaves identically.
            //
            // At the OUTERMOST level this closes the whole overlay. It used to
            // clear `embed` instead, which left the pane open holding no form
            // — a state that swallows every key including the Esc that would
            // have escaped it.
            if (auto* o = m.ui.panel.get<pn::Rag>())
                if (!form::escape(o->embed.form))
                    return {std::move(m), Cmd<Msg>::none()};   // unwound a level
            return agentty::app::update(std::move(m), Msg{CloseRag{}});
        },

        // Every navigation/editing key arrives here. The shared reducer does
        // the work; this arm only handles what is genuinely pane-specific:
        // a backend change alters WHICH rows exist, and firing the Action row
        // means "probe".
        [&](RagEmbedKey& e) -> Step {
            auto* f = form_of(m);
            if (!f) return {std::move(m), Cmd<Msg>::none()};

            const auto* before = f->form.focused();
            const bool on_backend = before && before->id == rs::kFieldBackend;
            const bool on_mode    = before && before->id == rs::kFieldMode;

            const auto applied = form::keys::apply(f->form, e.action);

            if (applied.changed) {
                // The mode row commits immediately — it is a policy switch with
                // nothing to validate and no probe to invalidate, so making the
                // user press ^S for it would be ceremony.
                if (on_mode) {
                    if (auto* o = m.ui.panel.get<pn::Rag>()) {
                        o->cursor = rs::mode_from_form(f->form, o->cursor);
                        o->active = o->cursor;
                        commit_mode(o->cursor);
                    }
                } else {
                    invalidate_probe(*f);
                    if (on_backend) {
                        auto* o = m.ui.panel.get<pn::Rag>();
                        const auto mode = o ? o->cursor : store::RagMode::On;
                        resync_rows(*f, mode, o && o->advanced);
                    } else {
                        sync_cfg(*f);
                    }
                }
            }
            refresh_status(*f);

            if (applied.fired)
                return {std::move(m),
                        Cmd<Msg>::after(std::chrono::milliseconds{0}, Msg{RagEmbedTest{}})};
            if (applied.save)
                return {std::move(m),
                        Cmd<Msg>::after(std::chrono::milliseconds{0}, Msg{RagEmbedSave{}})};
            if (applied.close)
                return {std::move(m),
                        Cmd<Msg>::after(std::chrono::milliseconds{0}, Msg{RagEmbedClose{}})};
            return {std::move(m), Cmd<Msg>::none()};
        },

        [&](RagEmbedPaste& e) -> Step {
            auto* f = form_of(m);
            if (!f) return {std::move(m), Cmd<Msg>::none()};
            auto* row = f->form.focused();
            // Only while actually EDITING: the router targets pastes by a
            // snapshot that can go stale (same rule as the editing-intent
            // guards), so the reducer re-checks the true mode.
            if (!f->form.editing() || !row || !row->editable() || row->locked)
                return {std::move(m), Cmd<Msg>::none()};
            agentty::form::paste(row->value, e.text);
            f->form.dirty = true;
            invalidate_probe(*f);
            refresh_status(*f);
            return {std::move(m), Cmd<Msg>::none()};
        },

        // Run the probe on a worker: it dials a network endpoint (or loads a
        // model file) and must never block the UI thread.
        [&](RagEmbedTest) -> Step {
            auto* f = form_of(m);
            if (!f) return {std::move(m), Cmd<Msg>::none()};
            sync_cfg(*f);
            if (auto v = eb::validate(f->cfg); auto* bad = std::get_if<eb::Invalid>(&v)) {
                f->probe = rs::EmbedForm::Failed{bad->why};
                return {std::move(m), Cmd<Msg>::none()};
            }
            f->probe = rs::EmbedForm::Testing{};
            const std::uint64_t gen = ++f->probe_gen;

            store::RagConfig probe_cfg = deps().load_settings().rag;
            write_embed_into(probe_cfg, f->cfg);
            std::string key = f->cfg.api_key;
            // task_isolated: the probe dials a network endpoint or memory-maps
            // a model file, either of which can block for seconds. Keeping it
            // off the shared BG pool means a wedged endpoint cannot starve
            // other background work.
            return {std::move(m),
                    Cmd<Msg>::task_isolated(
                        [probe_cfg = std::move(probe_cfg), key = std::move(key),
                         gen]
                        (std::function<void(Msg)> dispatch) {
                            const auto r = tools::rag_probe_embedder(probe_cfg, key);
                            dispatch(Msg{RagEmbedTestDone{r.ok, r.dim,
                                                          r.latency_ms, r.error,
                                                          gen}});
                        })};
        },
        [&](RagEmbedTestDone& e) -> Step {
            auto* f = form_of(m);
            if (!f) return {std::move(m), Cmd<Msg>::none()};
            // STALENESS GATE (same shape as login's attempt_id): only the
            // completion of the LATEST launch may land. An edit or a re-test
            // bumped probe_gen, making this answer about a config that no
            // longer exists — adopting its dim / Ok would verify bytes the
            // probe never saw.
            if (e.gen != f->probe_gen)
                return {std::move(m), Cmd<Msg>::none()};
            if (e.ok) {
                // Adopt the MEASURED dimension. This is the only place `dim`
                // is ever set: a user-supplied value would be silently
                // dropped by rag-cpp's HNSW on mismatch.
                f->cfg.dim = e.dim;
                f->probe   = rs::EmbedForm::Ok{e.dim, e.latency_ms};
                f->form.dirty = false;
            } else {
                f->probe = rs::EmbedForm::Failed{e.error.empty() ? "probe failed" : e.error};
            }
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](RagEmbedSave) -> Step {
            auto* f = form_of(m);
            if (!f) return {std::move(m), Cmd<Msg>::none()};
            sync_cfg(*f);
            if (auto v = eb::validate(f->cfg); auto* bad = std::get_if<eb::Invalid>(&v)) {
                f->probe = rs::EmbedForm::Failed{bad->why};
                return {std::move(m), Cmd<Msg>::none()};
            }

            // The credential goes to the keystore (or a sealed file), keyed by
            // endpoint — never into settings.json.
            std::string note;
            if (eb::needs_api_key(f->cfg.backend)) {
                const auto slot = eb::endpoint_key(f->cfg);
                if (!eb::store_key(slot, f->cfg.api_key) && !f->cfg.api_key.empty())
                    note = " (key not saved: no secure store)";
            }

            auto s = deps().load_settings();
            s.rag.configured = true;
            write_embed_into(s.rag, f->cfg);
            // The pipeline knobs, read back through the same table that
            // generated their rows.
            rs::apply_form_to_settings(f->form, s);
            deps().save_settings(s);
            tools::rag_apply_settings(s.rag);

            const std::string label = eb::describe(f->cfg);
            return {std::move(m),
                    set_status_toast(m, "Embeddings: " + label + note,
                                     std::chrono::seconds{4})};
        },
        [&](RagEmbedRevert) -> Step {
            if (auto* o = m.ui.panel.get<pn::Rag>()) {
                auto& f = o->embed;
                f.cfg   = current_embed_config();
                f.form  = rs::build_form(f.cfg, o->cursor, deps().load_settings());
                f.probe = rs::EmbedForm::Idle{};
                refresh_status(f);
            }
            return {std::move(m), Cmd<Msg>::none()};
        },
    }, rm);
}

} // namespace agentty::app::detail
