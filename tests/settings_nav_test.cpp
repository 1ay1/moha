// settings_nav_test — Esc is BACK ONE LEVEL, not "exit".
//
// The settings panes are reachable three ways: a direct chord (^S), a Ctrl+K
// palette row, and the settings list (Ctrl+K → Settings → Retrieval / Smart
// Mode). Where Esc should land depends on which of those you used, so a pane
// cannot answer it from a constant — and each close handler used to try:
// CloseRagSettings always reopened the palette, CloseSmartMode always dropped
// to the thread. Each was right for one entry point and wrong for the rest,
// and the settings-list hand-offs were outright trapdoors (they closed the
// list before opening the target, so Esc lost your place entirely).
//
// Each pane now CARRIES a snapshot of the panel it was opened over
// (pn::From), so "return to the wrong place" stops being representable — and
// the return is the FULL state, query and cursor, not a reconstruction.
// These tests walk each entry point in and press Esc, through the real
// app::update reducer.

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/runtime/panel/settings/categories.hpp"
#include "agentty/runtime/panel/settings/items.hpp"   // items_for, Action
#include "agentty/runtime/view/palette.hpp"   // ui::palette_context

#include <string>
#include <vector>

namespace pn = agentty::ui::panel;

using namespace agentty;

namespace {

store::Settings g_settings;

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& s) { g_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-nav"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

// Esc, as the user presses it: through the real key→intent→reducer path.
//
// The RAG pane defers its close through a Cmd (the form layer's `applied.close`
// becomes a RagEmbedClose message on the next tick), which a synchronous test
// never runs — so the follow-up is dispatched here. That is a property of the
// pane's async plumbing, not of the navigation being tested.
Model press_escape(Model m) {
    const auto esc = form::keys::Action{form::keys::Intent::Close};
    if (m.ui.panel.is<pn::SmartMode>())
        return app::update(std::move(m), Msg{SmartModeKey{esc}}).first;
    if (m.ui.panel.is<pn::RagSettings>()) {
        m = app::update(std::move(m), Msg{RagEmbedKey{esc}}).first;
        if (m.ui.panel.is<pn::RagSettings>())
            m = app::update(std::move(m), Msg{RagEmbedClose{}}).first;
        return m;
    }
    return m;
}

// Open the palette and run `row` the way a user does: type enough of its
// label to leave it the single match, then Enter.
//
// Deliberately NOT "compute the index" — the cursor indexes the FILTERED list,
// and filtering depends on a PaletteContext derived from the live Model
// (pending changes, code blocks, an available update all gate rows). A test
// that builds its own context indexes a different list than the reducer does
// and silently selects the wrong command, which is exactly what this helper
// did on its first attempt.
Model run_from_palette(Model m, std::string_view query, Command expect) {
    m = app::update(std::move(m), Msg{OpenCommandPalette{}}).first;
    for (char c : query)
        m = app::update(std::move(m),
                        Msg{CommandPaletteInput{static_cast<char32_t>(c)}}).first;
    (void)expect;   // named at the call site for readability
    return app::update(std::move(m), Msg{CommandPaletteSelect{}}).first;
}

} // namespace

TEST_CASE("settings nav: ^S then Esc closes to the thread") {
    // A direct chord is not navigation — you did not descend from anywhere, so
    // there is nothing to go back to and Esc means "done".
    install_stub_deps();
    Model m;
    m = app::update(std::move(m), Msg{OpenSmartMode{}}).first;
    REQUIRE(m.ui.panel.is<pn::SmartMode>());

    m = press_escape(std::move(m));
    CHECK(m.ui.panel.is<pn::None>());
}

TEST_CASE("settings nav: palette → pane → Esc returns to the palette") {
    // …with the FULL palette state the user left: the query they typed and
    // the cursor on the matching row. (The old origin-reconstruction reset
    // the query to "" and recomputed a cursor; the snapshot just restores.)
    install_stub_deps();
    {
        Model m = run_from_palette(Model{}, "smart mode", Command::SmartMode);
        REQUIRE(m.ui.panel.is<pn::SmartMode>());

        m = press_escape(std::move(m));
        REQUIRE(m.ui.panel.is<pn::CommandPalette>());
        const auto* p = m.ui.panel.get<pn::CommandPalette>();
        CHECK(p->query == "smart mode");
        // The cursor must still land on the command that was run — resolved
        // through the SAME filtered list the view renders, against the LIVE
        // context (revalidated by ascend(), in case gating changed while the
        // pane was open).
        const auto matches = filtered_commands(p->query, ui::palette_context(m));
        REQUIRE(!matches.empty());
        REQUIRE(p->index >= 0);
        REQUIRE(p->index < static_cast<int>(matches.size()));
        CHECK(matches[static_cast<std::size_t>(p->index)]->id
              == Command::SmartMode);
    }
    {
        Model m = run_from_palette(Model{}, "retrieval", Command::OpenRagSettings);
        REQUIRE(m.ui.panel.is<pn::RagSettings>());

        m = press_escape(std::move(m));
        REQUIRE(m.ui.panel.is<pn::CommandPalette>());
        const auto* p = m.ui.panel.get<pn::CommandPalette>();
        CHECK(p->query == "retrieval");
        const auto matches = filtered_commands(p->query, ui::palette_context(m));
        REQUIRE(!matches.empty());
        REQUIRE(p->index >= 0);
        REQUIRE(p->index < static_cast<int>(matches.size()));
        CHECK(matches[static_cast<std::size_t>(p->index)]->id
              == Command::OpenRagSettings);
    }
}

TEST_CASE("settings nav: settings list → pane → Esc returns to the list") {
    // The trapdoor. Activating "Retrieval" or "Smart Mode" from the settings
    // list closed the list and opened the target with no memory of it, so Esc
    // dropped two levels to the thread — the one behaviour a user reading it
    // as a stack would not expect.
    install_stub_deps();

    const auto descend = [](Command row) {
        Model m;
        m = app::update(std::move(m),
                        Msg{OpenSettingsList{settings::Category::General}}).first;
        REQUIRE(m.ui.panel.is<pn::SettingsList>());

        // Walk to the row and activate it, rather than reaching into the
        // reducer — this is the path a user takes.
        auto* o = m.ui.panel.get<pn::SettingsList>();
        const auto rows = settings::items_for(m, o->concern);
        int idx = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if ((row == Command::OpenRagSettings
                    && rows[static_cast<std::size_t>(i)].action
                           == settings::Action::OpenRag)
             || (row == Command::SmartMode
                    && rows[static_cast<std::size_t>(i)].action
                           == settings::Action::OpenSmart))
                idx = i;
        REQUIRE(idx >= 0);
        // A row that is not the first, or "remembered the row" and "reset to
        // the top" would be the same assertion.
        REQUIRE(idx > 0);
        m.ui.panel.get<pn::SettingsList>()->index = idx;
        m = app::update(std::move(m), Msg{SettingsListActivate{}}).first;
        return std::pair{std::move(m), idx};
    };

    {
        auto [m, idx] = descend(Command::SmartMode);
        REQUIRE(m.ui.panel.is<pn::SmartMode>());
        m = press_escape(std::move(m));
        REQUIRE(m.ui.panel.is<pn::SettingsList>());
        CHECK(m.ui.panel.get<pn::SettingsList>()->concern
              == settings::Category::General);
        CHECK(m.ui.panel.get<pn::SettingsList>()->index == idx);
    }
    {
        auto [m, idx] = descend(Command::OpenRagSettings);
        REQUIRE(m.ui.panel.is<pn::RagSettings>());
        m = press_escape(std::move(m));
        REQUIRE(m.ui.panel.is<pn::SettingsList>());
        CHECK(m.ui.panel.get<pn::SettingsList>()->concern
              == settings::Category::General);
        CHECK(m.ui.panel.get<pn::SettingsList>()->index == idx);
    }
}

TEST_CASE("settings nav: the origin survives a slot round trip") {
    // Assigning a model slot destroys the Smart Mode overlay (it hands off to
    // the model picker and comes back). The origin has to be parked across
    // that, or a pane entered from the settings list returns from the picker
    // believing it was opened by ^S — and the next Esc drops to the thread.
    install_stub_deps();
    Model m = run_from_palette(Model{}, "smart mode", Command::SmartMode);
    REQUIRE(m.ui.panel.is<pn::SmartMode>());

    // Descend into the picker, then back out without choosing.
    m.ui.smart_assign_slot     = smart::ModelRole::Strategic;
    m.ui.smart_assign_advanced = m.ui.panel.get<pn::SmartMode>()->advanced;
    m.ui.smart_assign_from     = m.ui.panel.get<pn::SmartMode>()->from;
    m.ui.panel.close<pn::SmartMode>();
    m = app::update(std::move(m), Msg{OpenFusedPicker{}}).first;
    REQUIRE(m.ui.panel.is<pn::FusedPicker>());
    m = app::update(std::move(m), Msg{CloseFusedPicker{}}).first;

    // Back on the pane — and Esc must still know it came from the palette.
    REQUIRE(m.ui.panel.is<pn::SmartMode>());
    m = press_escape(std::move(m));
    CHECK(m.ui.panel.is<pn::CommandPalette>());
}
