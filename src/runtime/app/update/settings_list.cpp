// settings_list_update — reducer for the settings pickers (Ctrl+K →
// Plugins/Commands/Agents/Hooks). One list modal parameterised by the
// concern it was opened with. ↑↓ move over the live rows; Enter runs the
// focused row's Action; Esc closes. Row resolution is index-into-live
// (settings::items_for), never a stale enum — the same correctness rule
// the command palette uses.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"   // load_plugins_async

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/runtime/view/helpers.hpp"   // utf8_encode / utf8_prev
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/registry.hpp"   // reload_mcp_plugins

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;
namespace se = agentty::settings;
namespace cmdf = agentty::app::cmd;   // cmd_factory (local vars named `cmd` shadow the ns)

namespace {
// The mcp.json an edit on THIS row should write. A server row carries the
// .agentty dir it was read from (scope provenance from PluginModel), so a
// remove/toggle lands in the RIGHT file — a project server edits the project
// config, a user server the user config. Empty config_dir (add-mode, or a
// pre-provenance row) falls back to the user config, the historical default.
[[nodiscard]] std::filesystem::path edit_target(const se::Item& row) {
    if (!row.config_dir.empty())
        return std::filesystem::path{row.config_dir} / "mcp.json";
    return tools::plugin::config_path(/*project=*/false);
}

// True when row `i` is one the user can act on (Enter/toggle/etc).
[[nodiscard]] bool actionable_at(const std::vector<se::Item>& rows, int i) {
    return i >= 0 && i < static_cast<int>(rows.size())
        && rows[static_cast<std::size_t>(i)].action != se::Action::None;
}

// Cursor-landing SSOT for the settings list: the index of the first row the
// user can ACT on, scanning from `start` in `dir` (+1 down / -1 up), `start`
// included. Skips Action::None rows (headers, built-in listings, empty-state
// placeholders). If nothing actionable exists that way, returns `start`
// clamped — so a wholly-informational pane (e.g. Agents built-ins) still has
// a valid, stable cursor and never lands out of range. Used by open, move,
// and the plugins-refresh reconciler so "where the cursor sits" is decided in
// ONE place.
[[nodiscard]] int first_actionable(const std::vector<se::Item>& rows,
                                   int start, int dir) {
    const int n = static_cast<int>(rows.size());
    if (n <= 0) return 0;
    start = std::clamp(start, 0, n - 1);
    const int step = dir < 0 ? -1 : 1;
    for (int i = start; i >= 0 && i < n; i += step)
        if (rows[static_cast<std::size_t>(i)].action != se::Action::None)
            return i;
    return start;
}
}  // namespace

Step settings_list_update(Model m, msg::SettingsListMsg sm) {
    return std::visit(overload{
        [&](OpenSettingsList& e) -> Step {
            // Land the cursor on the first ACTIONABLE row, not a leading
            // header (the Plugins pane opens with a "N tools on the wire"
            // info row). first_actionable clamps to a valid index for a
            // wholly-informational / empty pane too.
            const int start =
                first_actionable(se::items_for(m, e.concern), 0, +1);
            m.ui.panel = pn::SettingsList{{e.concern, start}};
            // A fresh open starts at the top — don't inherit the scroll
            // offset of the last visit (the widget's keep-selection-in-view
            // would fight the stale offset for a frame). PR #34.
            m.ui.settings_list_scroll.y = 0;
            // Opening the Plugins panel is what CONNECTS the servers (and
            // refreshes the snapshot): the connection is driven by the update
            // loop, not a lazy side effect of the first tool call. Without
            // this the panel showed "connecting…" forever until you happened
            // to send a turn. Fire the async connect; PluginsUpdated lands
            // the result in m.ui.plugins.
            if (e.concern == se::Category::Plugins) {
                // If the startup connect (from init) is still in flight, don't
                // fire a SECOND one — just show its spinner. Redundant
                // concurrent connects were the "date connected 4×" symptom
                // and the precondition for the pool-swap race. When idle,
                // opening the panel is what (re)connects.
                if (!m.ui.plugins_loading) {
                    m.ui.plugins_loading = true;
                    return {std::move(m), cmdf::load_plugins_async(/*reconnect=*/true)};
                }
            }
            return done(std::move(m));
        },
        [&](PluginsUpdated& e) -> Step {
            // The connect/reload finished on a worker. Store the snapshot
            // (this IS the model delta that repaints the panel — visual_hash
            // covers m.ui.plugins, so no nonce hack) and clear the spinner.
            //
            // Cursor IDENTITY, not index: a reload can add/remove/reorder
            // rows, so a raw index clamp would silently slide the cursor to a
            // neighbour — and a later Enter/`d` would act on the wrong server.
            // Remember which row (by server+tool+action) was selected BEFORE
            // the swap, then restore the cursor to that same row after.
            std::optional<std::tuple<std::string, std::string, se::Action>> sel;
            if (auto* o = m.ui.panel.get<pn::SettingsList>();
                o && o->concern == se::Category::Plugins) {
                auto before = se::items_for(m, o->concern);
                if (o->index >= 0 && o->index < static_cast<int>(before.size())) {
                    const auto& r = before[static_cast<std::size_t>(o->index)];
                    sel = std::make_tuple(r.arg, r.arg2, r.action);
                }
            }

            m.ui.plugins = std::move(e.model);
            m.ui.plugins_loading = false;

            if (auto* o = m.ui.panel.get<pn::SettingsList>();
                o && o->concern == se::Category::Plugins) {
                auto after = se::items_for(m, o->concern);
                const int cnt = static_cast<int>(after.size());
                int restored = -1;
                if (sel) {
                    for (int i = 0; i < cnt; ++i)
                        if (after[static_cast<std::size_t>(i)].arg == std::get<0>(*sel)
                            && after[static_cast<std::size_t>(i)].arg2 == std::get<1>(*sel)
                            && after[static_cast<std::size_t>(i)].action == std::get<2>(*sel)) {
                            restored = i; break;
                        }
                }
                // Same row found → follow it; otherwise re-land on the
                // first actionable row at/after the old index (never a stale
                // header) — the row it named is gone (e.g. removed server).
                o->index = restored >= 0
                    ? restored
                    : first_actionable(after,
                          std::clamp(o->index, 0, std::max(0, cnt - 1)), +1);
            }
            return done(std::move(m));
        },
        [&](CloseSettingsList) -> Step {
            // Esc unwinds one level: the palette snapshot stashed at open
            // (query and cursor intact), or the thread when ^S-style direct
            // entry left no parent. The old hand-reconstruction mapped the
            // category back to a palette command; the snapshot just IS the
            // palette the user left.
            ascend(m);
            return done(std::move(m));
        },
        [&](SettingsListMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return done(std::move(m));
            o->confirm_remove.clear();   // moving off a row disarms a pending `d`
            auto rows = se::items_for(m, o->concern);
            const int n = static_cast<int>(rows.size());
            if (n <= 0) { o->index = 0; return done(std::move(m)); }

            // Seamless nav: hop OVER purely informational rows (Action::None
            // — the plugins "N tools on the wire" header, the agents built-in
            // list, empty-state placeholders) so ↑↓ lands only on rows the
            // user can actually act on. Each unit of delta advances one
            // ACTIONABLE row via the shared first_actionable SSOT; when no
            // actionable row remains that way we take a plain clamped step so
            // an all-informational pane still scrolls and never gets stuck.
            const int dir = e.delta > 0 ? 1 : (e.delta < 0 ? -1 : 0);
            const int steps = dir == 0 ? 0 : std::abs(e.delta);
            int idx = std::clamp(o->index, 0, n - 1);
            const bool on_actionable = actionable_at(rows, idx);
            for (int s = 0; s < steps; ++s) {
                const int probe = idx + dir;
                if (probe < 0 || probe >= n) break;   // at the edge
                const int landed = first_actionable(rows, probe, dir);
                if (actionable_at(rows, landed)) {
                    idx = landed;                     // hopped onto a real row
                } else if (on_actionable) {
                    // We started on an actionable row and there's nothing
                    // actionable ahead (only a header/footer). DON'T slide
                    // onto that dead row — hold position; that's the seamless
                    // feel (↑ at the top row is a no-op, not a jump to a
                    // non-interactive header).
                    break;
                } else {
                    // Started on an informational row (wholly-inert pane):
                    // plain-step so the user can still scroll through it.
                    idx = std::clamp(idx + dir, 0, n - 1);
                }
            }
            o->index = idx;
            return done(std::move(m));
        },
        [&](SettingsListActivate) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return done(std::move(m));
            o->confirm_remove.clear();   // any Enter action disarms a pending `d`
            auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return done(std::move(m));
            const se::Item& row = rows[static_cast<std::size_t>(o->index)];

            switch (row.action) {
                case se::Action::CycleProfile:
                    return agentty::app::update(std::move(m), Msg{CycleProfile{}});
                case se::Action::OpenRag: {
                    // Descending, not jumping: OpenRag runs with the
                    // list still open, so descend() stashes it — category,
                    // cursor and its own parent chain — as the pane's Esc
                    // target. No hand-stamped origin, nothing to forget.
                    return agentty::app::update(std::move(m),
                                                Msg{OpenRag{}});
                }
                case se::Action::OpenSmart: {
                    return agentty::app::update(std::move(m),
                                                Msg{OpenSmartMode{}});
                }
                // (There is deliberately NO Activate-arm for plugin removal.
                // Removal is `d` → SettingsListRemove, which is TWO-step —
                // arm, then confirm. An Enter-fired remove here would be a
                // one-press destructive action; the old arm was unreachable
                // — no row builder ever emitted it — but unreachable code
                // with live side effects is a trap, not a feature.)
                case se::Action::TogglePlugin: {
                    // Ignore a toggle while a connect/reload is already in
                    // flight — the snapshot (and this row's on/off) is mid-
                    // change, so acting on it could write a stale intent.
                    if (m.ui.plugins_loading)
                        return done(std::move(m));
                    // Enter on a plugin row flips the WHOLE server on/off — a
                    // reversible `disabled` flag in mcp.json, NOT a delete.
                    // Enabling spawns + handshakes; disabling drops the
                    // connection. Either way it changes the live process set,
                    // so reconnect=true, off the UI thread.
                    auto path = edit_target(row);
                    const bool want_disabled = row.on;   // on → turn off
                    auto r = tools::plugin::set_server_disabled(
                        path, row.arg, want_disabled);
                    maya::Cmd<Msg> cmd;
                    if (r == tools::plugin::EditResult::Ok) {
                        m.ui.plugins_loading = true;
                        cmd = maya::Cmd<Msg>::batch(std::vector<maya::Cmd<Msg>>{
                            cmdf::load_plugins_async(/*reconnect=*/true),
                            set_status_toast(m,
                                (want_disabled ? "disabled plugin '"
                                               : "enabled plugin '")
                                + row.arg + "'")});
                    } else {
                        cmd = set_status_toast(m,
                                  "could not toggle '" + row.arg + "'");
                    }
                    return {std::move(m), std::move(cmd)};
                }
                case se::Action::ToggleTool: {
                    if (m.ui.plugins_loading)
                        return done(std::move(m));
                    // Toggling one tool of a DISABLED/disconnected plugin is
                    // meaningless — the whole plugin runs nothing. Guide the
                    // user to enable the plugin (Enter on the parent) instead
                    // of silently editing an exclude that has no effect.
                    if (row.inactive)
                        return {std::move(m), set_status_toast(m,
                            "'" + row.arg + "' is disabled — enable the plugin "
                            "first (Enter on it), then toggle its tools")};
                    // Enable/disable one tool of a plugin: persist to
                    // mcp.json's tools.exclude, then invalidate the wire
                    // catalog so it re-projects with the new filter. NO
                    // server re-spawn, NO background thread — the server
                    // stays connected and project_tools re-reads the live
                    // exclude. This is synchronous + race-free (the earlier
                    // reload-on-toggle re-spawned the server and hung on
                    // rapid disable→re-enable).
                    auto path = edit_target(row);
                    const bool want_enabled = !row.on;   // toggle
                    auto r = tools::plugin::set_tool_enabled(
                        path, row.arg, row.arg2, want_enabled);
                    maya::Cmd<Msg> cmd;
                    if (r == tools::plugin::EditResult::Ok) {
                        tools::invalidate_mcp_catalog();
                        // No respawn (only the exclude filter changed), but
                        // the Model snapshot must reflect the new enabled set
                        // — re-snapshot the live pool (reconnect=false).
                        cmd = maya::Cmd<Msg>::batch(std::vector<maya::Cmd<Msg>>{
                            cmdf::load_plugins_async(/*reconnect=*/false),
                            set_status_toast(m,
                                (want_enabled ? "enabled tool '" : "disabled tool '")
                                + row.arg2 + "' on " + row.arg)});
                    } else {
                        cmd = set_status_toast(m,
                            "could not toggle '" + row.arg2 + "'");
                    }
                    // Keep the cursor on the same row after the list rebuilds.
                    if (auto* oo = m.ui.panel.get<pn::SettingsList>()) {
                        const int n = static_cast<int>(
                            se::items_for(m, oo->concern).size());
                        oo->index = std::clamp(oo->index, 0, std::max(0, n - 1));
                    }
                    return {std::move(m), std::move(cmd)};
                }
                case se::Action::ApprovePlugin: {
                    // Enter on an untrusted project server = a deliberate
                    // "trust this server" — the row is explicitly labelled
                    // "trust & enable", so the keypress IS the consent (unlike
                    // hooks, which needs a y/N review). Approve just THIS
                    // server's spec (not the whole file), then reconnect so it
                    // spawns; a later-added server stays pending.
                    if (m.ui.plugins_loading)
                        return done(std::move(m));
                    const bool ok = tools::plugin::approve_server(
                        edit_target(row), row.arg);
                    maya::Cmd<Msg> cmd;
                    if (ok) {
                        m.ui.plugins_loading = true;
                        cmd = maya::Cmd<Msg>::batch(std::vector<maya::Cmd<Msg>>{
                            cmdf::load_plugins_async(/*reconnect=*/true),
                            set_status_toast(m,
                                "trusted project config — connecting…")});
                    } else {
                        cmd = set_status_toast(m,
                            "could not record approval (no project mcp.json?)");
                    }
                    return {std::move(m), std::move(cmd)};
                }
                case se::Action::ApproveHooks:
                    // Consent MUST be a deliberate terminal action — the
                    // picker can't safely own the y/N prompt while it holds
                    // the screen. Point at the CLI.
                    return {std::move(m), set_status_toast(m,
                        "run `agentty hooks approve` in a shell to review "
                        "+ approve (consent is deliberate by design)")};
                case se::Action::None:
                default:
                    return done(std::move(m));
            }
        },
        [&](SettingsListRemove) -> Step {
            // `d` deletes the highlighted plugin — the deliberate, destructive
            // counterpart to Enter's reversible on/off toggle. Only applies
            // to a server row (Plugins concern, TogglePlugin action); ignored
            // on tool sub-rows and every other category.
            if (m.ui.plugins_loading) return done(std::move(m));
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || o->concern != se::Category::Plugins)
                return done(std::move(m));
            auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return done(std::move(m));
            const se::Item& row = rows[static_cast<std::size_t>(o->index)];
            // A server row is either TogglePlugin (normal) or ApprovePlugin
            // (untrusted project) — both are removable with `d`. Tool sub-rows
            // and every other category are not.
            if ((row.action != se::Action::TogglePlugin
                 && row.action != se::Action::ApprovePlugin)
                || row.arg.empty())
                return done(std::move(m));   // not a server row

            // Two-step: the first `d` on a row ARMS (stores the name); the
            // view then paints "press d again to remove". Only a second `d` on
            // the SAME row commits — so a stray keystroke never deletes a
            // hand-tuned mcp.json entry with no undo.
            if (o->confirm_remove != row.arg) {
                o->confirm_remove = row.arg;
                return done(std::move(m));
            }
            o->confirm_remove.clear();

            auto path = edit_target(row);
            auto r = tools::plugin::remove_server(path, row.arg);
            maya::Cmd<Msg> cmd;
            if (r == tools::plugin::EditResult::Ok) {
                m.ui.plugins_loading = true;
                cmd = maya::Cmd<Msg>::batch(std::vector<maya::Cmd<Msg>>{
                    cmdf::load_plugins_async(/*reconnect=*/true),
                    set_status_toast(m, "removed plugin '" + row.arg + "'")});
            } else {
                cmd = set_status_toast(m, "could not remove '" + row.arg + "'");
            }
            if (auto* oo = m.ui.panel.get<pn::SettingsList>()) {
                const int n = static_cast<int>(se::items_for(m, oo->concern).size());
                oo->index = std::clamp(oo->index, 0, std::max(0, n - 1));
            }
            return {std::move(m), std::move(cmd)};
        },
        [&](SettingsListEditOpen& e) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || o->input_active) return done(std::move(m));
            if (o->concern != se::Category::Plugins) {
                // Other concerns have no editor form; `a` degrades to the
                // one-line starter prompt they already use, `e` is a no-op.
                if (e.add)
                    return agentty::app::update(std::move(m),
                                                Msg{SettingsListAddStart{}});
                return done(std::move(m));
            }
            if (e.add) {
                // Add form: descend keeps this list as the Esc target.
                return agentty::app::update(std::move(m),
                                            Msg{OpenPluginEdit{{}, false}});
            }
            // Detail form for the highlighted server. Tool rows carry the
            // server in `arg` too, so `e` works from anywhere in a subtree.
            const auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return done(std::move(m));
            const auto& row = rows[static_cast<std::size_t>(o->index)];
            if (row.arg.empty()) return done(std::move(m));   // header rows
            return agentty::app::update(std::move(m),
                                        Msg{OpenPluginEdit{row.arg, false}});
        },
        [&](SettingsListAddStart) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return done(std::move(m));
            // Add is only meaningful for the file/config-backed concerns.
            if (o->concern != se::Category::Plugins
                && o->concern != se::Category::Commands
                && o->concern != se::Category::Agents)
                return done(std::move(m));
            o->input_active = true;
            o->input.clear();
            o->cursor = 0;
            return done(std::move(m));
        },
        [&](SettingsListChar& e) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active) return done(std::move(m));
            const std::string utf8 = ui::utf8_encode(e.ch);
            o->input.insert(static_cast<std::size_t>(o->cursor), utf8);
            o->cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](SettingsListPaste& e) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active) return done(std::move(m));
            // The add-prompt is a single line (a plugin's "name command
            // args…" spec, or a new file's name). Flatten any newlines/tabs
            // in the paste to spaces so a multi-line clipboard can't smuggle
            // a line break into the one-line field or split the arg vector.
            std::string clean;
            clean.reserve(e.text.size());
            for (char c : e.text)
                clean += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
            o->input.insert(static_cast<std::size_t>(o->cursor), clean);
            o->cursor += static_cast<int>(clean.size());
            return done(std::move(m));
        },
        [&](SettingsListBackspace) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active || o->cursor <= 0)
                return done(std::move(m));
            int p = ui::utf8_prev(o->input, o->cursor);
            o->input.erase(static_cast<std::size_t>(p),
                           static_cast<std::size_t>(o->cursor - p));
            o->cursor = p;
            return done(std::move(m));
        },
        [&](SettingsListCancelInput) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return done(std::move(m));
            o->input_active = false;
            o->input.clear();
            o->cursor = 0;
            return done(std::move(m));
        },
        [&](SettingsListSubmitInput) -> Step {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active) return done(std::move(m));
            const std::string line = o->input;
            const se::Category concern = o->concern;
            o->input_active = false;
            o->input.clear();
            o->cursor = 0;
            if (line.empty()) return done(std::move(m));   // empty = cancel

            se::AddResult r = (concern == se::Category::Plugins)
                ? se::add_plugin_from_line(line)
                : se::create_starter(concern, line);

            // Make it USABLE NOW, not after a restart. For Plugins the
            // add already wrote mcp.json; reload the live pool OFF the UI
            // thread (the connect handshake must never freeze the TUI —
            // the bridge bounds it with a deadline). Commands are loaded
            // fresh per use (create_starter invalidated the cache) and
            // agents are scanned per task-tool call, so both are already
            // live — no reload needed.
            maya::Cmd<Msg> reload = maya::Cmd<Msg>::none();
            if (r.ok && concern == se::Category::Plugins) {
                m.ui.plugins_loading = true;
                reload = cmdf::load_plugins_async(/*reconnect=*/true);
                r.message += " — connecting…";
            }
            // Re-clamp the (possibly grown) list to the top of the new row.
            if (auto* oo = m.ui.panel.get<pn::SettingsList>()) {
                const int cnt =
                    static_cast<int>(se::items_for(m, oo->concern).size());
                oo->index = std::clamp(oo->index, 0, std::max(0, cnt - 1));
            }
            return {std::move(m), maya::Cmd<Msg>::batch(
                std::vector<maya::Cmd<Msg>>{
                    std::move(reload), set_status_toast(m, r.message)})};
        },
    }, sm);
}

} // namespace agentty::app::detail
