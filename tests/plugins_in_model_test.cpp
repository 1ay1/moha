// plugins_in_model_test — the Plugins panel is a pure function of the Model.
//
// This locks the architectural contract that replaced the recurring
// "stuck on connecting… / laggy panel" bugs:
//
//   1. Opening the Plugins panel sets plugins_loading and returns a Cmd
//      (the connect is DRIVEN BY THE UPDATE LOOP, not a lazy side effect).
//   2. PluginsUpdated{snapshot} stores the snapshot in m.ui.plugins and
//      clears the loading flag — the reducer, not a render-time global,
//      owns the truth.
//   3. items_for(Plugins) renders m.ui.plugins verbatim (a pure projection):
//      an empty model + loading → "connecting…"; a populated model → the
//      servers/tools; nothing calls the live pool.
//
// Because the snapshot lives IN the Model, visual_hash covers it (see
// visual_hash_coverage_test) and the panel repaints on every change with no
// nonce hack — that invariant is what makes the old bug class impossible.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/mcp/plugin_model.hpp"

#include <string>

namespace pn = agentty::ui::panel;

using namespace agentty;


TEST_CASE("plugins in model") {
    // ── 1: opening Plugins arms loading + returns a connect Cmd ──
    {
        Model m;
        auto [m2, cmd] = app::update(
            std::move(m), Msg{OpenSettingsList{settings::Category::Plugins}});
        check(m2.ui.panel.is<pn::SettingsList>(),
              "open: Plugins panel is open");
        check(m2.ui.plugins_loading,
              "open: plugins_loading armed (connect is loop-driven)");
        check(!cmd.is_none(),
              "open: a connect Cmd was returned (not a lazy side effect)");
    }

    // ── 2: PluginsUpdated stores the snapshot + clears loading ──
    {
        Model m;
        m.ui.panel = pn::SettingsList{{settings::Category::Plugins, 0}};
        m.ui.plugins_loading = true;

        mcp::PluginModel snap;
        mcp::ServerState s;
        s.name = "date";
        s.connected = true;
        s.tools.push_back({"current_date", "today's date", true, false});
        s.tools.push_back({"days_between", "day math", true, false});
        snap.servers.push_back(std::move(s));

        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{PluginsUpdated{std::move(snap)}});
        check(!m2.ui.plugins_loading, "updated: loading flag cleared");
        check(m2.ui.plugins.servers.size() == 1,
              "updated: snapshot stored in m.ui.plugins");
        check(m2.ui.plugins.servers[0].connected,
              "updated: connected state preserved");
        check(m2.ui.plugins.servers[0].tools.size() == 2,
              "updated: advertised tools preserved");
    }

    // ── 3: items_for is a pure projection of m.ui.plugins ──
    {
        // Empty + loading → a "connecting…" row.
        Model loading;
        loading.ui.plugins_loading = true;
        auto rows_loading = settings::items_for(loading, settings::Category::Plugins);
        check(rows_loading.size() == 1
              && rows_loading[0].primary.find("connecting") != std::string::npos,
              "projection: empty+loading renders 'connecting…'");

        // Empty + not loading → the "no plugins" hint.
        Model empty;
        auto rows_empty = settings::items_for(empty, settings::Category::Plugins);
        check(rows_empty.size() == 1
              && rows_empty[0].primary.find("no plugins") != std::string::npos,
              "projection: empty+idle renders the 'no plugins' hint");

        // Populated → a server header + its tools; nothing reads the live pool.
        Model populated;
        mcp::ServerState s;
        s.name = "date";
        s.connected = true;
        s.tools.push_back({"current_date", "d", true, false});
        populated.ui.plugins.servers.push_back(std::move(s));
        auto rows = settings::items_for(populated, settings::Category::Plugins);
        bool saw_date = false, saw_tool = false;
        for (const auto& r : rows) {
            if (r.primary.find("date") != std::string::npos) saw_date = true;
            if (r.primary.find("current_date") != std::string::npos) saw_tool = true;
        }
        check(saw_date, "projection: populated model renders the server");
        check(saw_tool, "projection: populated model renders its tools");

        // A CONNECTED server row must read as healthy (Status::Ok), NOT wear
        // the remove-action badge that looked like an error.
        for (const auto& r : rows) {
            if (r.primary == "date") {
                check(r.status == settings::Item::Status::Ok,
                      "badge: connected server is Status::Ok (healthy, not a red ✕)");
                // Enter TOGGLES the whole plugin on/off (reversible), it does
                // NOT remove — destructive delete is the deliberate `d` key.
                check(r.action == settings::Action::TogglePlugin,
                      "interaction: Enter on a plugin toggles it on/off");
                check(r.on && r.hint == "disable",
                      "interaction: an enabled plugin shows on + 'disable'");
            }
        }

        // A DISABLED server reads as off (Neutral, not an error), offers to
        // re-enable, and KEEPS its tools listed — but marks the whole subtree
        // inactive (present-but-can't-run), with no per-tool toggle hint.
        Model off;
        mcp::ServerState d;
        d.name = "date";
        d.disabled = true;
        d.tools.push_back({"current_date", "", true, false});
        d.tools.push_back({"days_between", "", true, false});
        off.ui.plugins.servers.push_back(std::move(d));
        int tools_seen = 0, inactive_seen = 0;
        for (const auto& r : settings::items_for(off, settings::Category::Plugins)) {
            if (r.primary == "date") {
                check(!r.on && r.hint == "enable",
                      "interaction: a disabled plugin shows off + 'enable'");
                check(r.status == settings::Item::Status::Neutral,
                      "badge: a disabled plugin is Neutral (off on purpose, not an error)");
            }
            if (r.indented) {   // a tool row under the disabled plugin
                ++tools_seen;
                if (r.inactive) ++inactive_seen;
                check(r.hint.empty(),
                      "interaction: a tool under a disabled plugin has no toggle hint");
            }
        }
        check(tools_seen == 2,
              "UX: a disabled plugin STILL lists its tools (got "
              + std::to_string(tools_seen) + ")");
        check(inactive_seen == 2,
              "UX: every tool under a disabled plugin is marked inactive (dimmed)");

        // An ENABLED, connected plugin's tools are ACTIVE (not dimmed) and
        // toggleable.
        Model live;
        mcp::ServerState ls;
        ls.name = "date";
        ls.connected = true;
        ls.tools.push_back({"current_date", "", true, false});
        live.ui.plugins.servers.push_back(std::move(ls));
        for (const auto& r : settings::items_for(live, settings::Category::Plugins)) {
            if (r.indented) {
                check(!r.inactive,
                      "UX: a tool under a connected plugin is active (not dimmed)");
                check(r.hint == "disable",
                      "interaction: an active enabled tool toggles to 'disable'");
            }
        }

        // A FAILED server reads as Bad.
        Model failed;
        mcp::ServerState bad;
        bad.name = "broken";
        bad.connected = false;
        bad.error = "spawn failed: no such file";
        failed.ui.plugins.servers.push_back(std::move(bad));
        for (const auto& r : settings::items_for(failed, settings::Category::Plugins)) {
            if (r.primary == "broken")
                check(r.status == settings::Item::Status::Bad,
                      "badge: a failed server is Status::Bad (⚠)");
        }
    }

    // ── 4: scope provenance, trust, and HTTP rows (post-scope-migration) ──
    {
        // A PROJECT-scope server is badged with its scope; a USER server is
        // not (the common case reads clean).
        Model m;
        mcp::ServerState proj;
        proj.name = "repo-tool";
        proj.connected = true;
        proj.origin = mcp::Origin::Project;
        proj.config_dir = ".agentty";
        proj.tools.push_back({"t", "", true, false});
        m.ui.plugins.servers.push_back(std::move(proj));

        mcp::ServerState usr;
        usr.name = "my-tool";
        usr.connected = true;
        usr.origin = mcp::Origin::User;
        usr.config_dir = "/home/u/.agentty";
        m.ui.plugins.servers.push_back(std::move(usr));

        for (const auto& r : settings::items_for(m, settings::Category::Plugins)) {
            if (r.primary == "repo-tool") {
                check(r.scope_label == "project", "scope: project row labelled 'project'");
                check(r.config_dir == ".agentty",
                      "scope: project row carries its config dir (edit routing)");
                check(r.secondary.find("project") != std::string::npos,
                      "scope: project badge shows in the row text");
            }
            if (r.primary == "my-tool") {
                check(r.secondary.find("project") == std::string::npos
                      && r.secondary.find("user") == std::string::npos,
                      "scope: a user row is NOT badged (clean common case)");
            }
        }

        // An UNTRUSTED project server: Enter APPROVES (not toggles), the row
        // is flagged untrusted, and the hint invites trust.
        Model u;
        mcp::ServerState untrusted;
        untrusted.name = "risky";
        untrusted.origin = mcp::Origin::Project;
        untrusted.untrusted = true;
        untrusted.error = "untrusted project config — approve to enable";
        untrusted.config_dir = ".agentty";
        u.ui.plugins.servers.push_back(std::move(untrusted));
        for (const auto& r : settings::items_for(u, settings::Category::Plugins)) {
            if (r.primary == "risky") {
                check(r.untrusted, "trust: untrusted flag set on the row");
                check(r.action == settings::Action::ApprovePlugin,
                      "trust: Enter on an untrusted server APPROVES, not toggles");
                check(r.hint == "trust & enable",
                      "trust: hint invites approval");
                check(r.status == settings::Item::Status::Bad,
                      "trust: untrusted reads as attention (Bad)");
            }
        }

        // An HTTP/SSE server (url, no command) connects fine and is NOT
        // flagged 'no command' — the false-error fix.
        Model h;
        mcp::ServerState http;
        http.name = "remote";
        http.url = "https://mcp.example.com";
        http.connected = true;
        h.ui.plugins.servers.push_back(std::move(http));
        for (const auto& r : settings::items_for(h, settings::Category::Plugins)) {
            if (r.primary == "remote") {
                check(r.status == settings::Item::Status::Ok,
                      "http: a connected url-only server is healthy");
                check(r.secondary.find("no \"command\"") == std::string::npos,
                      "http: a url-only server is NOT flagged 'no command'");
            }
        }
    }
}

TEST_CASE("settings nav skips non-actionable rows") {
    using settings::Action;
    auto opened = [](const Model& mm) {
        return mm.ui.panel.get<pn::SettingsList>();
    };

    // Build a populated Plugins model: row 0 is the "N tools on the wire"
    // header (Action::None), the server + its tools are actionable.
    auto make_model = [] {
        Model m;
        mcp::ServerState s;
        s.name = "date";
        s.connected = true;
        s.tools.push_back({"current_date", "d", true, false});
        s.tools.push_back({"days_between", "m", true, false});
        m.ui.plugins.servers.push_back(std::move(s));
        return m;
    };

    // Opening lands on the FIRST actionable row, not the info header at 0.
    {
        auto [m, cmd] = app::update(make_model(),
            Msg{OpenSettingsList{settings::Category::Plugins}});
        auto rows = settings::items_for(m, settings::Category::Plugins);
        auto* o = opened(m);
        REQUIRE(o != nullptr);
        check(rows[0].action == Action::None,
              "row 0 is the informational header");
        check(o->index > 0 && rows[o->index].action != Action::None,
              "open lands on the first ACTIONABLE row, skipping the header");
    }

    // Moving up from the first actionable row does NOT drop back onto the
    // header (no actionable row above → stays put or plain-steps, never lands
    // on a dead header as a resting spot when it started actionable).
    {
        auto [m0, c0] = app::update(make_model(),
            Msg{OpenSettingsList{settings::Category::Plugins}});
        auto rows = settings::items_for(m0, settings::Category::Plugins);
        auto [m1, c1] = app::update(std::move(m0), Msg{SettingsListMove{-1}});
        auto* o = opened(m1);
        REQUIRE(o != nullptr);
        check(rows[o->index].action != Action::None,
              "moving up from the top actionable row stays on an actionable row");
    }

    // Moving down hops between actionable rows (header never a landing spot).
    {
        auto [m0, c0] = app::update(make_model(),
            Msg{OpenSettingsList{settings::Category::Plugins}});
        auto rows = settings::items_for(m0, settings::Category::Plugins);
        Model m = std::move(m0);
        for (int i = 0; i < 4; ++i) {
            auto [mn, cn] = app::update(std::move(m), Msg{SettingsListMove{1}});
            m = std::move(mn);
            auto* o = opened(m);
            REQUIRE(o != nullptr);
            check(rows[o->index].action != Action::None,
                  "every down-step lands on an actionable row");
        }
    }

    // A wholly-informational pane (Agents = built-ins, all Action::None) must
    // still give a valid, in-range cursor and never crash on move.
    {
        auto [m0, c0] = app::update(Model{},
            Msg{OpenSettingsList{settings::Category::Agents}});
        auto rows = settings::items_for(m0, settings::Category::Agents);
        auto* o = opened(m0);
        REQUIRE(o != nullptr);
        check(o->index >= 0 && o->index < static_cast<int>(rows.size()),
              "agents pane: cursor in range even when all rows are informational");
        auto [m1, c1] = app::update(std::move(m0), Msg{SettingsListMove{1}});
        auto* o1 = opened(m1);
        REQUIRE(o1 != nullptr);
        auto rows1 = settings::items_for(m1, settings::Category::Agents);
        check(o1->index >= 0 && o1->index < static_cast<int>(rows1.size()),
              "agents pane: move keeps the cursor in range (scrolls, no stick)");
    }
}
