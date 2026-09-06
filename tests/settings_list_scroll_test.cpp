// settings_list_scroll_test — the settings list (Ctrl+K → Plugins etc.)
// CLIPS its list to the viewport and SCROLLS, like every other picker.
//
// This pins the fix for the "/mcp servers list paints off-screen" bug: the
// picker used to pass scroll = nullptr + a fixed viewport_h = 14 to maya's
// Picker widget. By contract that paints the list as a STATIC block —
// allowed only when items.size() ≤ viewport_h is a structural invariant.
// The Plugins pane lists every advertised tool (125 tools ⇒ 135+ rows), so
// the overflow ran past the modal border, under the footer, and off the
// terminal; rows beyond the screen were unreachable (no scrollbar, no
// wheel, no PageUp/End clamp — the widget drops selection-follow entirely
// without a ScrollState).
//
// The test renders the real view through maya (same harness as
// picker_sections_render_test) and asserts against pixels, not source text:
//
//   1. Footer-on-top: the picker must END (bottom border) inside the
//      canvas — the old build painted list rows below the footer hint.
//   2. Clipped: a big list renders only ~viewport_h rows; the row count
//      between the borders stays bounded instead of growing with n.
//   3. Scrollable: with the selection driven deep into the list, the
//      viewport window moves — an early row leaves the canvas while a
//      late row becomes visible. That is the ScrollState wiring doing
//      its job (widget-side keep-selection-in-view clamp).

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/settings_list.hpp"
#include "agentty/runtime/view/pickers.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ov = agentty::ui::overlay;

using namespace agentty;

namespace {

// A Plugins snapshot with `n` enabled tools on one connected server —
// enough rows to blow past any viewport.
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
// approach as picker_sections_render_test.cpp). The canvas is deliberately
// SHORT (48 rows): the bug overflowed the modal past the terminal bottom,
// so a short canvas makes the overflow visible as paint beyond the border.
std::string render_picker(const Model& m, int width = 100, int height = 48) {
    auto root = ui::settings_list_picker(m);
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
    return out;
}

// Count rendered list rows: lines that carry a tool name in the flattened
// text. Tool names are unique, so this is a conservative, cheap row census.
int count_tool_rows(const std::string& screen, int n) {
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        const std::string name = "tool_" + std::to_string(i);
        if (screen.find(name) != std::string::npos) ++hits;
    }
    return hits;
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const auto nl = s.find('\n', pos);
        lines.push_back(s.substr(pos, nl == std::string::npos
                                         ? std::string::npos
                                         : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

} // namespace

TEST_CASE("settings list scrolls instead of painting off-screen") {
    // The scroll state must live on the Model (routed_scroll pattern,
    // auto_dispatch = false — the widget clamps it on paint).
    Model m = big_plugins_model(120);
    m.ui.overlay = ov::SettingsList{{settings::Category::Plugins, 0}};
    m.ui.settings_list_scroll.y = 0;

    const std::string screen = render_picker(m);
    INFO(screen);

    // ── 1: the modal ends INSIDE the canvas ──
    // The last canvas row is padding, the bottom border above it. If the
    // list paints past the border again, the border row vanishes under
    // item text and the footer keys line disappears entirely.
    CHECK(has(screen, "move"), "footer key hints are on screen");
    CHECK(has(screen, "close"), "footer close hint is on screen");
    {
        // Bottom border: UTF-8 border glyphs fold to '?' in the flattened
        // text, so the modal's bottom edge is an all-'?' line — and it must
        // sit BELOW the footer key-hint line (the old build painted list
        // rows under the footer, past the border, off the canvas).
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
        const bool all_border = !bottom.empty() &&
            bottom.find_first_not_of("? ") == std::string::npos;
        CHECK(all_border,
              "canvas ends on the modal's bottom border, not overflow rows");
        CHECK(bottom.find("tool_") == std::string::npos,
              "no list row is painted at/below the bottom border");
        int footer_row = -1;
        for (int y = 0; y < last_nonblank; ++y)
            if (lines[static_cast<std::size_t>(y)].find("esc close") !=
                std::string::npos)
                footer_row = y;
        CHECK(footer_row >= 0, "footer key hints are inside the modal");
        CHECK(footer_row < last_nonblank,
              "footer renders above the bottom border (no underpaint)");
    }

    // ── 2: the list is CLIPPED to the viewport ──
    // 120 tools ⇒ ~127 rows in the old build; the widget now shows only
    // the visible window (well under 40 of 120 names).
    const int seen = count_tool_rows(screen, 120);
    CHECK(seen > 0, "some tool rows render");
    CHECK(seen < 40, "list is clipped to the viewport, not painted whole");

    // ── 3: scrolling moves the window ──
    // Drive the selection deep (PageUp/End map onto Move in NavSpec) and
    // re-render: an early row must leave the canvas and a late row enter
    // it — proof the ScrollState-backed viewport actually scrolls.
    // Row layout of the Plugins pane: 0 = info row, 1 = server row,
    // 2..n+1 = tool_i — so cursor index 118 sits on tool_116.
    const bool early_before = screen.find("tool_0") != std::string::npos;
    m.ui.overlay = ov::SettingsList{{settings::Category::Plugins, 118}};
    const std::string deep = render_picker(m);
    INFO(deep);
    CHECK(has(deep, "tool_116"), "deep row is reachable and rendered");
    if (early_before)
        CHECK(deep.find("tool_0") == std::string::npos,
              "early row scrolled out of the viewport window");
}
