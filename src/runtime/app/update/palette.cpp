// palette_update + todo_update — reducers for the command palette and
// the todo modal. Both are simple list-modals; the palette is the
// dispatcher for action commands (NewThread, ReviewChanges, etc.), so it
// re-enters the top-level update() to fan a Command::* into the matching
// domain Msg.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/code_block_picker.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace ov = agentty::ui::overlay;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

// Build the live visibility context for the palette from the current Model,
// so conditionally-dead rows (Accept-all with no diff, Run-code-block with no
// fenced reply, Update with no release) never render. One place, consulted by
// every filtered_commands() call in this reducer + the view.
[[nodiscard]] inline PaletteContext palette_ctx(const Model& m) {
    PaletteContext ctx;
    ctx.update_available    = !m.s.update_latest.empty();
    ctx.has_pending_changes = !m.d.pending_changes.empty();
    ctx.has_code_block      = [&] {
        for (auto it = m.d.current.messages.rbegin();
             it != m.d.current.messages.rend(); ++it) {
            if (it->role != Role::Assistant || it->text.empty()) continue;
            if (!code_block_picker::extract_code_blocks(it->text).empty())
                return true;
        }
        return false;
    }();
    return ctx;
}

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
// emit_settings<Msg, Overlay>(row): open a settings pane and record that the
// PALETTE opened it, at `row`.
//
// The Open* messages mean "opened from the thread" — that is what ^S and ^K's
// own chord do — so a palette row has to say otherwise, or Esc would close to
// the thread and throw away the palette the user just navigated. Stamping it
// here keeps the Open* arms honest about their default instead of teaching
// them about every possible caller.
template <class T, class Ov>
[[nodiscard]] CommandHandler emit_settings(Command row) {
    return [row](Model m) {
        auto st = agentty::app::update(std::move(m), Msg{T{}});
        if (auto* o = st.first.ui.overlay.template get<Ov>())
            o->from = ui::settings_origin::Palette{row};
        return st;
    };
}
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
        add(Command::ForkThread,       emit<OpenForkPicker>());
        add(Command::CompactContext,   emit<CompactContext>());
        add(Command::RewindCheckpoint, emit<OpenCheckpointPicker>());
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
        add(Command::InspectToolOutputs, emit<OpenToolOutputViewer>());
        add(Command::RunCodeBlock,     emit<OpenCodeBlockPicker>());
        // ── Config ──
        add(Command::CycleProfile,     emit<CycleProfile>());
        add(Command::OpenModels,       emit<OpenFusedPicker>());
        add(Command::SwapModel,        emit<SwitchToPreviousModel>());
        add(Command::OpenProviders,    emit<OpenProviderPicker>());
        add(Command::SmartMode,
            emit_settings<OpenSmartMode, ov::SmartMode>(Command::SmartMode));
        add(Command::OpenRagSettings,
            emit_settings<OpenRagSettings, ov::RagSettings>(
                Command::OpenRagSettings));
        add(Command::OpenPlugins,      emit_val<OpenSettingsList>(settings::Category::Plugins));
        add(Command::OpenCommands,     emit_val<OpenSettingsList>(settings::Category::Commands));
        add(Command::OpenAgents,       emit_val<OpenSettingsList>(settings::Category::Agents));
        add(Command::OpenHooks,        emit_val<OpenSettingsList>(settings::Category::Hooks));
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

Step palette_update(Model m, msg::CommandPaletteMsg pm) {
    return std::visit(overload{
        [&](OpenCommandPalette) -> Step {
            m.ui.overlay = ov::CommandPalette{};
            return done(std::move(m));
        },
        [&](CloseCommandPalette) -> Step {
            m.ui.overlay.close<ov::CommandPalette>();
            return done(std::move(m));
        },
        [&](CommandPaletteInput& e) -> Step {
            auto* o = m.ui.overlay.get<ov::CommandPalette>();
            if (o && static_cast<uint32_t>(e.ch) < 0x80) {
                o->query.push_back(static_cast<char>(e.ch));
                // Reset cursor to the top of the (newly filtered) list so
                // the previous index doesn't point at a now-hidden row.
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](CommandPaletteBackspace) -> Step {
            auto* o = m.ui.overlay.get<ov::CommandPalette>();
            if (o && !o->query.empty()) {
                o->query.pop_back();
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](CommandPaletteMove& e) -> Step {
            auto* o = m.ui.overlay.get<ov::CommandPalette>();
            if (!o) return done(std::move(m));
            // Clamp against the *visible* row count, not kCommands.size().
            // Without the upper bound the cursor used to walk off-screen
            // and Enter would silently fall through to the no-match path.
            int sz = static_cast<int>(filtered_commands(
                o->query, palette_ctx(m)).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](CommandPaletteSelect) -> Step {
            auto* o = m.ui.overlay.get<ov::CommandPalette>();
            if (!o) return done(std::move(m));
            // Resolve cursor → typed Command via the SAME filtered list
            // the view rendered. The previous design switched on the raw
            // o->index against the unfiltered enum, which silently fired
            // the wrong command whenever any query was active.
            auto matches = filtered_commands(o->query, palette_ctx(m));
            m.ui.overlay.close<ov::CommandPalette>();
            if (matches.empty()
                || o->index < 0
                || o->index >= static_cast<int>(matches.size()))
                return done(std::move(m));
            const Command sel = matches[static_cast<std::size_t>(o->index)]->id;
            // Behaviour lives in the command registry (dispatch_command), not
            // an inline switch — one declarative table, no drift, and adding a
            // command never touches this arm.
            return dispatch_command(sel, std::move(m));
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
        [&](UpdateTodos& e) -> Step {
            m.ui.todo.items = std::move(e.items);
            return done(std::move(m));
        },
    }, tm);
}

} // namespace agentty::app::detail
