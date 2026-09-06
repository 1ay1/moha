// Navigation through a grouped picker.
//
// Section headers are NON-SELECTABLE: they carry no action, so a cursor
// landing on one is a dead keypress. The reducer indexes the header-FREE
// match list and the view maps that onto a display position — which means
// nothing in the reducer has to know headers exist, and no arrow key can
// stop on one.
//
// This pins that end to end, because the failure is silent: a cursor on a
// header still renders (the row just looks unselected), so the only symptom
// is an arrow press that appears to do nothing.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/pickers.hpp"
#include "agentty/runtime/command_palette.hpp"

#include <maya/app/inline.hpp>

#include <string>
#include <vector>

namespace ov = agentty::ui::overlay;
using namespace agentty;

namespace {

// The rendered frame, minus styling.
std::string frame(const Model& m) {
    return maya::render_to_string(ui::command_palette(m), 82);
}

// Which line carries the cursor bar, or -1. The bar is the ONE marker for
// "you are here", so asking the rendered output is the honest check —
// inspecting model state would only prove the model agrees with itself.
int cursor_line(const std::string& out) {
    int line = 0;
    std::size_t start = 0;
    while (start <= out.size()) {
        const std::size_t nl = out.find('\n', start);
        const std::string s = out.substr(start, nl - start);
        // U+258E, in the row's marker lane (past the panel border + padding).
        if (s.find("\xe2\x96\x8e") != std::string::npos) return line;
        if (nl == std::string::npos) break;
        start = nl + 1;
        ++line;
    }
    return -1;
}

// Is this rendered line a section header? Headers are the only rows drawn as
// a rule running to the right edge.
bool is_header_line(const std::string& out, int want) {
    int line = 0;
    std::size_t start = 0;
    while (start <= out.size()) {
        const std::size_t nl = out.find('\n', start);
        const std::string s = out.substr(start, nl - start);
        if (line == want)
            return s.find("\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80") != std::string::npos;
        if (nl == std::string::npos) break;
        start = nl + 1;
        ++line;
    }
    return false;
}

store::Settings g_settings;

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

TEST_CASE("palette nav: the cursor never lands on a section header") {
    install_stub_deps();

    Model m;
    m.ui.overlay = ov::CommandPalette{{}};
    m.d.pending_changes.push_back(FileChange{});

    // Walk the whole list. Every stop must be a real, runnable row — the
    // grouped view has headers interleaved, and an arrow that stops on one
    // would look like a key that did nothing.
    const int rows = static_cast<int>(filtered_commands("").size());
    REQUIRE(rows > 0);

    for (int step = 0; step < rows + 2; ++step) {
        const auto out = frame(m);
        const int cl = cursor_line(out);
        CHECK(cl >= 0);
        CHECK_FALSE(is_header_line(out, cl));

        auto [next, cmd] = app::update(std::move(m), Msg{CommandPaletteMove{+1}});
        m = std::move(next);
    }
}

TEST_CASE("palette nav: moving up also skips headers") {
    install_stub_deps();

    Model m;
    m.ui.overlay = ov::CommandPalette{{}};
    m.d.pending_changes.push_back(FileChange{});

    const int rows = static_cast<int>(filtered_commands("").size());
    REQUIRE(rows > 0);

    for (int step = 0; step < rows + 2; ++step) {
        auto [next, cmd] = app::update(std::move(m), Msg{CommandPaletteMove{-1}});
        m = std::move(next);

        const auto out = frame(m);
        const int cl = cursor_line(out);
        CHECK(cl >= 0);
        CHECK_FALSE(is_header_line(out, cl));
    }
}

TEST_CASE("palette nav: the cursor index addresses the header-free list") {
    // The reducer must stay header-UNAWARE. If it ever indexed the display
    // rows instead, adding a section would silently shift every selection —
    // and the symptom would be "Enter ran the wrong command", not a crash.
    install_stub_deps();

    Model m;
    m.ui.overlay = ov::CommandPalette{{}};
    auto* o = m.ui.overlay.get<ov::CommandPalette>();
    REQUIRE(o != nullptr);

    const auto cmds = filtered_commands("");
    REQUIRE(!cmds.empty());

    // Index 0 is the FIRST COMMAND, not the first display row (which is a
    // header in the grouped view).
    CHECK(o->index == 0);
    const auto out = frame(m);
    const int cl = cursor_line(out);
    CHECK(cl >= 0);
    CHECK_FALSE(is_header_line(out, cl));
    // …and the row under the cursor is that first command.
    CHECK(out.find(std::string{cmds.front()->label}) != std::string::npos);
}
