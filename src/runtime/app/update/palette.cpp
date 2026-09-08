// palette_update + todo_update — reducers for the command palette and
// the todo modal. Both are simple list-modals; the palette is the
// dispatcher for action commands (NewThread, ReviewChanges, etc.), so it
// re-enters the top-level update() to fan a Command::* into the matching
// domain Msg.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/view/palette.hpp"   // ui::palette_context

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

// Build the live visibility context for the palette from the current Model,
// so conditionally-dead rows (Accept-all with no diff, Run-code-block with no
// fenced reply, Update with no release) never render. One place, consulted by
// every filtered_commands() call in this reducer + the view.
// Defined in meta.cpp (declared in internal.hpp) so the view shares it.

// ── Command dispatch driver ───────────────────────────────────────────────
// Each palette Command declares ITS OWN behaviour in one place: the registry
// below is a flat table of {Command, handler}. Adding a command is one row
// here (plus its metadata row in command_palette.hpp) — no growing switch, no
// four-file edit. The vast majority just re-enter the reducer with a Msg, so
// `emit<T>()` builds that handler generically; the few commands with bespoke
// side effects (learning reset, self-update) get a named lambda.
//
// A handler is Model-in → Step-out; the driver has already closed the palette
// before calling it, so handlers only describe the action.
using CommandHandler = std::function<Step(Model)>;

namespace {
// emit<T>: the handler for a command that simply dispatches Msg{T{}} back
// through the top-level reducer. Covers ~20 of the rows.
template <class T>
[[nodiscard]] CommandHandler emit() {
    return [](Model m) { return agentty::app::update(std::move(m), Msg{T{}}); };
}
// emit_val<T>(v): same, for a Msg that carries a payload (settings category).
template <class T, class V>
[[nodiscard]] CommandHandler emit_val(V v) {
    return [v](Model m) { return agentty::app::update(std::move(m), Msg{T{v}}); };
}
// NOTE on Esc chains: the select arm below snapshots the palette and
// `adopt()`s it onto WHATEVER the command opened — generically, after
// dispatch. The old emit_settings<> wrapper (which hand-stamped an origin
// per settings command) is gone: commands need no per-caller knowledge,
// and every palette-opened overlay now unwinds back to the palette.
} // namespace

// The registry: Command → what it does. Ordered for readability, not lookup
// (the driver does a linear find over 25 entries — trivially cheap, and keeps
// the table declarative). One entry per Command in the enum; a missing entry
// is a no-op (handled by the driver), so the build can't silently mis-wire.
[[nodiscard]] const std::vector<std::pair<Command, CommandHandler>>& command_registry() {
    static const std::vector<std::pair<Command, CommandHandler>> reg = [] {
        std::vector<std::pair<Command, CommandHandler>> r;
        r.reserve(25);
        auto add = [&](Command c, CommandHandler h) { r.emplace_back(c, std::move(h)); };

        // ── Thread ──
        add(Command::NewThread,        emit<NewThread>());
        add(Command::ForkThread,       emit<OpenFork>());
        add(Command::CompactContext,   emit<CompactContext>());
        add(Command::RewindCheckpoint, emit<OpenCheckpoints>());
        // ── Changes ──
        add(Command::ReviewChanges,    emit<OpenDiffReview>());
        add(Command::ToggleChangesStrip, [](Model m) -> Step {
            m.d.show_changes_strip = !m.d.show_changes_strip;
            // Persist so it survives restarts.
            auto s = deps().load_settings();
            s.show_changes_strip = m.d.show_changes_strip;
            deps().save_settings(s);
            auto cmd = set_status_toast(m, m.d.show_changes_strip
                ? "changes strip: shown" : "changes strip: hidden (Ctrl+R still reviews)");
            return {std::move(m), std::move(cmd)};
        });
        add(Command::AcceptAll,        emit<AcceptAllChanges>());
        add(Command::RejectAll,        emit<RejectAllChanges>());
        // ── Go ──
        add(Command::OpenThreads,      emit<OpenThreadList>());
        add(Command::OpenPlan,         emit<OpenTodoModal>());
        add(Command::InspectToolOutputs, emit<OpenToolOutput>());
        add(Command::RunCodeBlock,     emit<OpenCodeBlocks>());
        // ── Config ──
        add(Command::CycleProfile,     emit<CycleProfile>());
        add(Command::OpenModels,       emit<OpenModels>());
        add(Command::SwapModel,        emit<SwitchToPreviousModel>());
        add(Command::OpenProviders,    emit<OpenProviders>());
        add(Command::SmartMode,        emit<OpenSmartMode>());
        add(Command::OpenRag,  emit<OpenRag>());
        add(Command::OpenPlugins,      emit_val<OpenSettingsList>(settings::Category::Plugins));
        add(Command::OpenCommands,     emit_val<OpenSettingsList>(settings::Category::Commands));
        add(Command::OpenAgents,       emit_val<OpenSettingsList>(settings::Category::Agents));
        add(Command::OpenHooks,        emit_val<OpenSettingsList>(settings::Category::Hooks));
        add(Command::OpenGeneralSettings,
            emit_val<OpenSettingsList>(settings::Category::General));
        // ── Account ──
        add(Command::OpenLogin,        emit<OpenLogin>());
        add(Command::SignOut,          emit<SignOut>());
        // ── General ──
        add(Command::UpdateAgentty, [](Model m) -> Step {
            if (m.s.update_latest.empty() || m.s.update_in_flight)
                return done(std::move(m));
            m.s.update_in_flight = true;
            std::string v = m.s.update_latest;
            m.s.status = "\xe2\xac\x86 downloading agentty v" + v + "\xe2\x80\xa6";
            m.s.status_until = {};
            return {std::move(m), cmd::perform_self_update(std::move(v))};
        });
        add(Command::Quit,             emit<Quit>());
        return r;
    }();
    return reg;
}

// Run the command the cursor landed on. The palette is already closed by the
// caller; unknown commands (no registry entry) are a safe no-op.
[[nodiscard]] Step dispatch_command(Command sel, Model m) {
    for (const auto& [id, handler] : command_registry())
        if (id == sel) return handler(std::move(m));
    return done(std::move(m));
}

Step palette_update(Model m, msg::PaletteMsg pm) {
    return std::visit(overload{
        [&](OpenPalette) -> Step {
            m.ui.panel = pn::Palette{};
            return done(std::move(m));
        },
        [&](ClosePalette) -> Step {
            // Esc unwinds one level — a palette opened over another panel
            // (rare but possible via chords) restores it; over the thread,
            // closes.
            ascend(m);
            return done(std::move(m));
        },
        [&](PanelFilterPaste& e) -> Step {
            // ONE arm for every filter panel: replay the paste through the
            // open panel's OWN typed-input message, one char at a time — so
            // paste has exactly typing's semantics (ASCII gate, cursor
            // reset, the models panel's re-rank) with no duplicated logic
            // and no cross-TU coupling. Control chars are dropped here so a
            // multi-line clipboard can't smuggle newlines into a filter.
            // Bounded by clipboard size; each step is the cheap typed path.
            Step st{std::move(m), maya::Cmd<Msg>::none()};
            for (char c : e.text) {
                const auto u = static_cast<unsigned char>(c);
                if (u < 0x20 || u >= 0x7f) continue;
                const auto ch = static_cast<char32_t>(u);
                Msg per_char =
                    st.first.ui.panel.is<pn::Palette>()   ? Msg{PaletteInput{ch}}
                  : st.first.ui.panel.is<pn::Models>()    ? Msg{ModelsFilterInput{ch}}
                  : st.first.ui.panel.is<pn::Providers>() ? Msg{ProvidersFilterInput{ch}}
                  : st.first.ui.panel.is<pn::Mention>()   ? Msg{MentionInput{ch}}
                  : st.first.ui.panel.is<pn::Symbol>()    ? Msg{SymbolInput{ch}}
                                                          : Msg{NoOp{}};
                st = agentty::app::update(std::move(st.first),
                                          std::move(per_char));
            }
            return st;
        },
        [&](PaletteInput& e) -> Step {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (o && static_cast<uint32_t>(e.ch) < 0x80) {
                o->query.push_back(static_cast<char>(e.ch));
                // Reset cursor to the top of the (newly filtered) list so
                // the previous index doesn't point at a now-hidden row.
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](PaletteBackspace) -> Step {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (o && !o->query.empty()) {
                o->query.pop_back();
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](PaletteMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (!o) return done(std::move(m));
            // Clamp against the *visible* row count, not kCommands.size().
            // Without the upper bound the cursor used to walk off-screen
            // and Enter would silently fall through to the no-match path.
            int sz = static_cast<int>(filtered_commands(
                o->query, ui::palette_context(m)).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](PaletteSelect) -> Step {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (!o) return done(std::move(m));
            // Resolve cursor → typed Command via the SAME filtered list
            // the view rendered. The previous design switched on the raw
            // o->index against the unfiltered enum, which silently fired
            // the wrong command whenever any query was active.
            auto matches = filtered_commands(o->query, ui::palette_context(m));
            // Copy out BEFORE closing: `o` points into the variant, and
            // close() destroys that alternative (the old code read o->index
            // through the dangling pointer afterwards).
            const int idx = o->index;
            if (matches.empty()
                || idx < 0
                || idx >= static_cast<int>(matches.size())) {
                m.ui.panel.close<pn::Palette>();
                return done(std::move(m));
            }
            const Command sel = matches[static_cast<std::size_t>(idx)]->id;
            // Snapshot the palette — query, cursor, its own parent chain —
            // then dispatch and let WHATEVER the command opened adopt it as
            // its Esc target. Generic: the registry needs no per-command
            // origin plumbing, and a command that opens nothing leaves the
            // slot None, where adopt() is a no-op.
            auto parent = pn::From::of(pn::Snapshot{m.ui.panel.raw()});
            m.ui.panel.close<pn::Palette>();
            // Behaviour lives in the command registry (dispatch_command), not
            // an inline switch — one declarative table, no drift, and adding a
            // command never touches this arm.
            auto st = dispatch_command(sel, std::move(m));
            st.first.ui.panel.adopt(std::move(parent));
            return st;
        },
    }, pm);
}

Step todo_update(Model m, msg::TodoMsg tm) {
    return std::visit(overload{
        [&](OpenTodoModal) -> Step {
            m.ui.todo.open = pick::OpenModal{};
            return done(std::move(m));
        },
        [&](CloseTodoModal) -> Step {
            m.ui.todo.open = pick::Closed{};
            return done(std::move(m));
        },
        // (No UpdateTodos arm: the agent's todo writes land via
        // stream_preview's direct sync — see sync_todos there. A message
        // nobody sent.)
    }, tm);
}

} // namespace agentty::app::detail
