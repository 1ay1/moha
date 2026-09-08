// smart_mode.cpp — the Smart Mode pane's reducer: open/close (descend/
// ascend), the advanced-rows toggle, key dispatch through the shared form
// layer, the slot hand-off to the model picker, and slot reset.
//
// build_smart_form / apply_smart stay in meta.cpp (shared with models.cpp
// and ascend()); declared in internal.hpp.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/core/cmd.hpp>

#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/settings_registry.hpp"   // tuning row write-back

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;
using maya::Cmd;

Step smart_mode_update(Model m, msg::SmartModeMsg sm) {
    return std::visit(overload{
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
        [&](SmartModePaste& e) -> Step {
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (o) form::paste_into(o->form, e.text);   // SSOT: guard + dirty
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
                // descend(): the ENTIRE SmartMode pane — form, cursor,
                // advanced flag, its own parent chain — rides in the
                // picker's `from` snapshot; Esc or a completed pin restores
                // it verbatim. Nothing is parked on Model::UI any more.
                pn::Models picker{{0, ""}, {}, *role};
                m.ui.panel.descend(std::move(picker));
                return agentty::app::update(std::move(m), Msg{OpenModels{}});
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
    }, sm);
}

} // namespace agentty::app::detail
