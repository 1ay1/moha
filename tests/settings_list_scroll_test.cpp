// settings_list_scroll_test — the settings list is a REAL scrolling picker.
//
// The Plugins pane (Ctrl+K → Plugins, or /mcp servers) is built from live
// server snapshots; a single server advertising 125 tools produces 135+
// rows. The list used to be the ONE picker built with scroll = nullptr —
// by maya's widget contract that paints the body as a static block, so the
// rows drew under the footer, past the modal border, off the screen.
//
// Pins three properties by rendering the REAL view through maya (canvas
// flatten — same approach as panel_sections_render_test):
//   1. Bounded: the canvas ends on the modal's bottom border; no row
//      paints below it.
//   2. Clipped: a big list renders only ~viewport_h rows.
//   3. Scrollable: driving the selection deep moves the window — an early
//      row leaves the canvas, a late row becomes visible.
//
// Ported from PR #34 (davidwed) across the panel refactor: ov::→pn::,
// settings_list_view→settings_list_panel, Row→Item vocabulary.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/settings/list.hpp"
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pn = agentty::ui::panel;

using namespace agentty;

namespace {

// A Plugins snapshot with `n` enabled tools on one connected server.
Model big_plugins_model(int n) {
    Model m;
    mcp::PluginModel snap;
    mcp::ServerState s;
    s.name      = "big";
    s.connected = true;
    for (int i = 0; i < n; ++i)
        s.tools.push_back({"tool_" + std::to_string(i),
                           "test tool " + std::to_string(i), true, false});
    snap.servers.push_back(std::move(s));
    m.ui.plugins = std::move(snap);
    return m;
}

// Render ONLY the picker element and flatten the canvas to text (same
// approach as panel_sections_render_test: render_tree + character fold;
// non-ASCII folds to '?'). The canvas is deliberately SHORT (48 rows): the
// bug overflowed the modal past the terminal bottom, so a short canvas
// makes overflow visible as painted rows beyond the border.
std::string render_picker(const Model& m, int width = 100, int height = 48) {
    auto root = ui::settings_list_panel(m);
    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out.push_back('\n');
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= s.size()) {
        auto nl = s.find('\n', at);
        if (nl == std::string::npos) { lines.push_back(s.substr(at)); break; }
        lines.push_back(s.substr(at, nl - at));
        at = nl + 1;
    }
    return lines;
}

bool has(const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}

int count_tool_rows(const std::string& screen, int n) {
    int seen = 0;
    for (int i = 0; i < n; ++i)
        if (has(screen, "tool_" + std::to_string(i))) ++seen;
    return seen;
}

} // namespace

TEST_CASE("settings list: bounded, clipped, and scrollable") {
    Model m = big_plugins_model(120);
    m.ui.panel = pn::SettingsList{{settings::Category::Plugins, 2}};

    const std::string screen = render_picker(m);
    INFO(screen);

    // ── 1: bounded — nothing paints below the modal's bottom border ──
    {
        const auto lines = split_lines(screen);
        int last_nonblank = -1;
        for (int y = static_cast<int>(lines.size()) - 1; y >= 0; --y)
            if (!lines[static_cast<std::size_t>(y)].empty()) {
                last_nonblank = y;
                break;
            }
        REQUIRE(last_nonblank >= 0);
        const std::string& bottom =
            lines[static_cast<std::size_t>(last_nonblank)];
        CHECK_MESSAGE(bottom.find("tool_") == std::string::npos,
                      "no list row is painted at/below the bottom border");
        int footer_row = -1;
        for (int y = 0; y < last_nonblank; ++y)
            if (lines[static_cast<std::size_t>(y)].find("esc close")
                    != std::string::npos
                || lines[static_cast<std::size_t>(y)].find("esc back")
                    != std::string::npos)
                footer_row = y;
        CHECK_MESSAGE(footer_row >= 0, "footer key hints are inside the modal");
        CHECK_MESSAGE(footer_row < last_nonblank,
                      "footer renders above the bottom border (no underpaint)");
    }

    // ── 2: clipped — only the viewport window renders ──
    const int seen = count_tool_rows(screen, 120);
    CHECK_MESSAGE(seen > 0, "some tool rows render");
    CHECK_MESSAGE(seen < 40, "list is clipped to the viewport, not painted whole");

    // ── 3: scrolling — a deep selection moves the window ──
    // Row 0 = info row, 1 = server row, 2..n+1 = tool_i — cursor 118 sits
    // on tool_116.
    const bool early_before = has(screen, "tool_0\xe2\x80\xa6") || has(screen, " tool_0");
    m.ui.panel = pn::SettingsList{{settings::Category::Plugins, 118}};
    const std::string deep = render_picker(m);
    INFO(deep);
    CHECK_MESSAGE(has(deep, "tool_116"), "deep row is reachable and rendered");
    if (early_before)
        CHECK_MESSAGE(deep.find(" tool_0 ") == std::string::npos,
                      "early row scrolled out of the viewport window");
}
