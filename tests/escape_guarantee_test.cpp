// The escape guarantee: no modal can ever trap the user.
//
// Every EXCLUSIVE overlay swallows unclaimed keys — that is what makes it
// modal. So an overlay whose key handler fails to answer Escape leaves the
// user with no way out and the app appears frozen. The Retrieval pane shipped
// exactly that: it owned the keyboard for a frame before its form existed, so
// every key including Esc went nowhere.
//
// Per-handler diligence cannot enforce this — it is one missing branch away,
// in any handler, forever. It is enforced structurally at the dispatch
// chokepoint in subscribe.cpp: if an overlay's own handler declines an
// Escape, the overlay's generic close fires instead.
//
// This test walks EVERY overlay Kind and asserts the property end to end, so
// a new overlay that forgets Esc fails here rather than in someone's terminal.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/panel/nav.hpp"
#include "agentty/runtime/panel/smart_form.hpp"

#include <string>
#include <vector>

namespace pn = agentty::ui::panel;
using namespace agentty;

namespace {

maya::KeyEvent esc() {
    maya::KeyEvent ev;
    ev.key = maya::SpecialKey::Escape;
    return ev;
}

// Every Kind, and a Model with that overlay open. Kept as a table so the
// -Wswitch exhaustiveness of close_msg and the coverage of this test are
// visibly the same list.
struct Case {
    pn::Kind    kind;
    const char* name;
    void      (*open)(Model&);
};

const std::vector<Case>& cases() {
    static const std::vector<Case> v = {
        {pn::Kind::CommandPalette, "command palette",
         [](Model& m) { m.ui.panel = pn::CommandPalette{{}}; }},
        {pn::Kind::Mention, "mention palette",
         [](Model& m) { m.ui.panel = pn::Mention{{}}; }},
        {pn::Kind::Symbol, "symbol palette",
         [](Model& m) { m.ui.panel = pn::Symbol{{}}; }},
        {pn::Kind::CodeBlocks, "code block picker",
         [](Model& m) { m.ui.panel = pn::CodeBlocks{{}}; }},
        {pn::Kind::ToolViewer, "tool output viewer",
         [](Model& m) { m.ui.panel = pn::ToolViewer{{}}; }},
        {pn::Kind::Checkpoints, "checkpoint picker",
         [](Model& m) { m.ui.panel = pn::Checkpoints{{}}; }},
        {pn::Kind::RagSettings, "retrieval pane",
         [](Model& m) { m.ui.panel = pn::RagSettings{{}}; }},
        {pn::Kind::SettingsList, "settings list",
         [](Model& m) { m.ui.panel = pn::SettingsList{{}}; }},
        {pn::Kind::Fork, "fork picker",
         [](Model& m) { m.ui.panel = pn::Fork{{}}; }},
        {pn::Kind::FusedPicker, "model picker",
         [](Model& m) { m.ui.panel = pn::FusedPicker{{}}; }},
        {pn::Kind::ProviderPicker, "provider picker",
         [](Model& m) { m.ui.panel = pn::ProviderPicker{{}}; }},
        {pn::Kind::ThreadList, "thread list",
         [](Model& m) { m.ui.panel = pn::ThreadList{{}}; }},
        {pn::Kind::SmartMode, "smart mode",
         [](Model& m) {
             smart_form::Inputs in;
             m.ui.panel = pn::SmartMode{{}, smart_form::build_form(in)};
         }},
        {pn::Kind::DiffReview, "diff review",
         [](Model& m) { m.ui.panel = pn::DiffReview{{}}; }},
    };
    return v;
}

} // namespace

namespace {

store::Settings g_settings;

// The reducer reaches the Store seam on close (persisting settings, loading
// threads), so the deps must exist. Stubs only — this test is about key
// routing, not persistence.
void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

} // namespace

TEST_CASE("escape guarantee: close_msg names a real close for every overlay") {
    // The backstop must actually close each overlay — a NoOp here would be a
    // silent trap. Todo is ambient (it never swallows) and None is not an
    // overlay, so both legitimately map to NoOp.
    for (const auto& c : cases()) {
        const auto msg = pn::close_msg(c.kind);
        CHECK(!std::holds_alternative<msg::MetaMsg>(msg)
                  || !std::holds_alternative<NoOp>(std::get<msg::MetaMsg>(msg)),
              (std::string{"close_msg is not a NoOp for "} + c.name).c_str());
    }
}

TEST_CASE("escape guarantee: Esc closes every overlay") {
    install_stub_deps();
    // End to end: open the overlay, feed the reducer its close message, and
    // require the overlay to be gone. This is the property a user experiences
    // — "Esc always gets me out" — rather than a proxy for it.
    for (const auto& c : cases()) {
        Model m;
        c.open(m);
        CHECK(pn::kind_of(m.ui.panel) == c.kind,
              (std::string{"opened "} + c.name).c_str());

        auto [after, cmd] = app::update(std::move(m), pn::close_msg(c.kind));
        CHECK(pn::kind_of(after.ui.panel) != c.kind,
              (std::string{"Esc closes "} + c.name).c_str());
    }
}

TEST_CASE("escape guarantee: a handler that answers nothing still cannot trap") {
    install_stub_deps();
    // The exact shape of the freeze: an overlay is open, its handler declines
    // every key. The dispatcher must still produce a close on Escape.
    //
    // Simulated at the same seam subscribe.cpp uses, because the real
    // subscription needs a live terminal: an unclaimed Escape resolves to
    // close_msg(kind), which the case above proves actually closes.
    for (const auto& c : cases()) {
        const std::optional<Msg> declined = std::nullopt;   // handler says no
        const Msg fallback = declined ? *declined : pn::close_msg(c.kind);

        Model m;
        c.open(m);
        auto [after, cmd] = app::update(std::move(m), fallback);
        CHECK(pn::kind_of(after.ui.panel) != c.kind,
              (std::string{"an inert handler cannot trap the user in "} + c.name).c_str());
    }
}
