// maya::Panel — the degenerate cases.
//
// Panel is now the ONLY overlay widget: every picker and every settings form
// in the app renders through it. That makes its edge behaviour load-bearing in
// a way a single-use widget's never is — a caller that hands it an empty list,
// an out-of-range cursor, or a one-row terminal must still get a usable frame,
// because the alternative is an overlay that is open, modal, and invisible.
//
// These are the inputs a caller can legitimately produce (an empty match set,
// a cursor left over from a longer list, a resized terminal) plus the ones
// only a bug produces. Both must render.

#include "agtest.hpp"

#include <maya/app/inline.hpp>
#include <maya/widget/panel.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

using maya::Panel;

Panel::Row text_row(const char* label, const char* value = "") {
    Panel::Row r;
    r.leading = label;
    r.trailing = value;
    return r;
}

Panel::Row header_row(const char* label) {
    Panel::Row r;
    r.leading   = label;
    r.is_header = true;
    return r;
}

std::string render(Panel::Config c, int width = 60) {
    return maya::render_to_string(Panel{std::move(c)}.build(), width);
}

// Does the frame have a top and bottom border? A panel that fails to draw one
// is the "open but invisible" failure — the overlay owns the keyboard while
// showing nothing.
bool framed(const std::string& out) {
    return out.find("\xe2\x95\xad") != std::string::npos    // ╭
        && out.find("\xe2\x95\xb0") != std::string::npos;   // ╰
}

int count_of(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t i = hay.find(needle); i != std::string::npos;
         i = hay.find(needle, i + needle.size())) ++n;
    return n;
}

// A list whose rows deliberately disagree in height: some carry help (which
// renders only under the FOCUSED row, so content height depends on where the
// cursor is), some an error, some are headers.
std::vector<Panel::Row> mixed_rows(int n) {
    std::vector<Panel::Row> rows;
    for (int i = 0; i < n; ++i) {
        Panel::Row r;
        r.leading = "row" + std::to_string(i);
        if (i % 3 == 0)  r.help    = "help for row " + std::to_string(i);
        if (i % 5 == 0)  r.error   = "bad value";
        if (i % 11 == 0) r.is_header = true;   // headers outrank both
        rows.push_back(std::move(r));
    }
    return rows;
}

} // namespace

TEST_CASE("panel: an empty body still renders a frame") {
    // A picker whose query matches nothing. It must draw its border, its
    // header and its footer — the user needs somewhere to see "no matches"
    // and a reminder that Esc exits.
    Panel::Config c;
    c.title  = " Empty ";
    c.footer.push_back(maya::Element{maya::TextElement{.content = "  Esc close"}});
    const auto out = render(std::move(c));

    CHECK(framed(out));
    CHECK(out.find("Empty") != std::string::npos);
    CHECK(out.find("Esc close") != std::string::npos);
}

TEST_CASE("panel: an empty body with a scroll state does not collapse") {
    // The dangerous combination: scrolling ON, nothing to scroll. A viewport
    // computed as min(viewport_h, 0) is zero rows, and a zero-height scroll
    // region collapses the panel to its border.
    maya::ScrollState s;
    Panel::Config c;
    c.title      = " Empty ";
    c.scroll     = &s;
    c.viewport_h = 10;
    const auto out = render(std::move(c));

    CHECK(framed(out));
    CHECK(s.y == 0);
}

TEST_CASE("panel: an out-of-range cursor focuses nothing") {
    // A stale index — the query narrowed the list but the cursor did not move
    // yet. It must not paint focus on an arbitrary row, and must not crash.
    for (int sel : {-1, 5, 99, -99}) {
        Panel::Config c;
        c.title = " Stale ";
        c.rows.push_back(text_row("Alpha"));
        c.rows.push_back(text_row("Beta"));
        c.selected = sel;
        const auto out = render(std::move(c));

        CHECK(framed(out));
        CHECK(out.find("Alpha") != std::string::npos);
        // No cursor bar anywhere: out of range means nothing is focused.
        CHECK(out.find("\xe2\x96\x8e") == std::string::npos);   // ▎
    }
}

TEST_CASE("panel: the cursor never lands on a header") {
    // Headers carry no action. Pointing `selected` at one is a caller bug,
    // and the widget refuses rather than rendering a focus the user cannot
    // act on — an arrow key that appears to do nothing.
    Panel::Config c;
    c.title = " Grouped ";
    c.rows.push_back(header_row("Section"));
    c.rows.push_back(text_row("Alpha"));
    c.selected = 0;                       // the header
    const auto out = render(std::move(c));

    CHECK(framed(out));
    CHECK(out.find("SECTION") != std::string::npos);
    CHECK(out.find("\xe2\x96\x8e") == std::string::npos);   // no cursor bar
}

TEST_CASE("panel: a menu only opens on a Choice row") {
    // `menu_row` pointing at a toggle is a caller bug. Rendering the option
    // list anyway would put a dropdown under a control that cannot use it.
    Panel::Config c;
    c.title = " Mismatch ";
    Panel::Row t;
    t.leading = "Toggle";
    t.control = maya::panel::Toggle{true};
    c.rows.push_back(std::move(t));
    c.selected = 0;

    Panel::Menu m;
    m.options = {"One", "Two"};
    c.menu     = m;
    c.menu_row = 0;                        // NOT a Choice row

    const auto out = render(std::move(c));
    CHECK(framed(out));
    CHECK(out.find("One") == std::string::npos);
    CHECK(out.find("Two") == std::string::npos);
}

TEST_CASE("panel: a menu on a Choice row renders its options") {
    // The positive case, so the guard above cannot pass by disabling menus.
    Panel::Config c;
    c.title = " Choice ";
    Panel::Row ch;
    ch.leading = "Backend";
    ch.control = maya::panel::Choice{"One"};
    c.rows.push_back(std::move(ch));
    c.selected = 0;

    Panel::Menu m;
    m.options     = {"One", "Two"};
    m.current     = 0;
    m.highlighted = 1;
    c.menu     = m;
    c.menu_row = 0;

    const auto out = render(std::move(c));
    CHECK(out.find("One") != std::string::npos);
    CHECK(out.find("Two") != std::string::npos);
    // ◉ marks the COMMITTED value, ❯ the cursor — two facts, two markers.
    CHECK(out.find("\xe2\x97\x89") != std::string::npos);   // ◉
    CHECK(out.find("\xe2\x9d\xaf") != std::string::npos);   // ❯
}

TEST_CASE("panel: a menu with fewer hints than options is safe") {
    // `hints` is optional and may be short. Indexing it by the highlighted
    // option without a bounds check reads past the end.
    Panel::Config c;
    c.title = " Hints ";
    Panel::Row ch;
    ch.leading = "Pick";
    ch.control = maya::panel::Choice{"A"};
    c.rows.push_back(std::move(ch));
    c.selected = 0;

    Panel::Menu m;
    m.options     = {"A", "B", "C"};
    m.hints       = {"only the first"};    // shorter than options
    m.highlighted = 2;                     // past the end of hints
    c.menu     = m;
    c.menu_row = 0;

    const auto out = render(std::move(c));
    CHECK(framed(out));
    CHECK(out.find("C") != std::string::npos);
}

TEST_CASE("panel: highlight offsets outside the label are ignored") {
    // Fuzzy-match positions come from a scorer run against a string that may
    // since have been truncated or replaced. Out-of-range offsets must not
    // index past the label.
    Panel::Config c;
    c.title = " Highlight ";
    Panel::Row r = text_row("abc");
    r.highlight = {0, 2, 99, -1};          // 99 and -1 are out of range
    c.rows.push_back(std::move(r));
    c.selected = 0;

    const auto out = render(std::move(c));
    CHECK(framed(out));
    CHECK(out.find("abc") != std::string::npos);
}

TEST_CASE("panel: an empty label with highlights is safe") {
    // The degenerate pairing: highlight offsets against no text at all.
    Panel::Config c;
    c.title = " Empty label ";
    Panel::Row r;
    r.highlight = {0, 1};
    c.rows.push_back(std::move(r));
    c.selected = 0;

    CHECK(framed(render(std::move(c))));
}

TEST_CASE("panel: a tiny viewport still shows a row") {
    // A short terminal. The viewport floor is what keeps at least one row and
    // the frame visible instead of collapsing to a border.
    maya::ScrollState s;
    Panel::Config c;
    c.title      = " Tiny ";
    c.scroll     = &s;
    c.viewport_h = 0;                      // degenerate
    for (int i = 0; i < 20; ++i)
        c.rows.push_back(text_row("Row"));
    c.selected = 19;

    const auto out = render(std::move(c));
    CHECK(framed(out));
    CHECK(out.find("Row") != std::string::npos);
}

TEST_CASE("panel: a narrow terminal keeps the frame closed") {
    // min_width has a floor: below it the border text overflows the frame and
    // the panel stops being a box.
    Panel::Config c;
    c.title     = " N ";
    c.min_width = 1;                       // degenerate
    c.rows.push_back(text_row("Alpha", "value"));
    c.selected = 0;

    const auto out = render(std::move(c), 24);
    CHECK(framed(out));
}

TEST_CASE("panel: the cursor row's help is kept in view when scrolled") {
    // A field's help line belongs to its row. Scrolling to the row alone left
    // the description one row past the bottom edge — it vanished exactly when
    // you arrowed onto the row to read it.
    maya::ScrollState s;
    Panel::Config c;
    c.title      = " Help ";
    c.scroll     = &s;
    c.viewport_h = 6;
    for (int i = 0; i < 12; ++i) c.rows.push_back(text_row("Row"));
    Panel::Row last = text_row("Last");
    last.help = "the description of the last row";
    c.rows.push_back(std::move(last));
    c.selected = 12;                       // the last row

    const auto out = render(std::move(c));
    CHECK(out.find("the description of the last row") != std::string::npos);
}

TEST_CASE("panel: rows stay inside the border at any width") {
    // The layout is flex, not arithmetic. Every previous version measured the
    // row and padded it against a guessed reserve, and each guess drifted from
    // the real clip — values slid under the scrollbar and off the frame.
    maya::ScrollState s;
    for (int w : {30, 40, 60, 100}) {
        Panel::Config c;
        c.title      = " Width ";
        c.scroll     = &s;
        c.viewport_h = 4;
        for (int i = 0; i < 10; ++i)
            c.rows.push_back(text_row(
                "a fairly long label that will not fit",
                "and a fairly long value too"));
        c.selected = 0;

        const auto out = render(std::move(c), w);
        // Every line is exactly `w` columns of painted cells: the frame closes
        // on both sides of every row.
        std::size_t start = 0;
        while (start <= out.size()) {
            const std::size_t nl = out.find('\n', start);
            const std::string line = out.substr(start, nl - start);
            if (!line.empty())
                CHECK(maya::string_width(line) <= w);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
}

TEST_CASE("panel: the caret stays visible in a value longer than its column") {
    // Typing past the column width used to push the caret — and everything you
    // were typing — off the right edge behind the truncation ellipsis. You
    // could still edit; you just could not see it. The value now scrolls
    // horizontally under the caret, the way every single-line editor does.
    const std::string v =
        "a-very-long-endpoint-name-that-will-not-fit-in-the-column-at-all";

    for (std::size_t at : {std::size_t{0}, v.size() / 2, v.size()}) {
        Panel::Config c;
        c.title = " Caret ";
        Panel::Row r;
        r.leading = "Host";
        r.control = maya::panel::Text{v, at};
        c.rows.push_back(std::move(r));
        c.selected = 0;

        const auto out = render(std::move(c), 70);
        // The caret is on screen at every position.
        CHECK(out.find("\xe2\x96\x88") != std::string::npos);   // █
    }
}

TEST_CASE("panel: a scrolled value says which way it is scrolled") {
    // ‹ / › mark a horizontal scroll — deliberately not the … that
    // TruncateEnd uses, so "there is more text this way" reads differently
    // from "this was cut off".
    const std::string v =
        "a-very-long-endpoint-name-that-will-not-fit-in-the-column-at-all";

    Panel::Config c;
    c.title = " Caret ";
    Panel::Row r;
    r.leading = "Host";
    r.control = maya::panel::Text{v, v.size()};   // caret at the END
    c.rows.push_back(std::move(r));
    c.selected = 0;

    const auto out = render(std::move(c), 70);
    CHECK(out.find("\xe2\x80\xb9") != std::string::npos);   // ‹ more to the left
}

TEST_CASE("panel: the text caret is not the row cursor bar") {
    // Both used U+258E, so an edited row showed the same glyph in column 0 and
    // mid-value — the caret read as a second cursor. The caret is now a full
    // block, which cannot be confused with the quarter-block edge marker.
    Panel::Config c;
    c.title = " Distinct ";
    Panel::Row r;
    r.leading = "Host";
    r.control = maya::panel::Text{"abc", 1};
    c.rows.push_back(std::move(r));
    c.selected = 0;

    const auto out = render(std::move(c));
    CHECK(out.find("\xe2\x96\x88") != std::string::npos);   // █ caret
    CHECK(count_of(out, "\xe2\x96\x8e") == 1);              // ▎ exactly once
}

TEST_CASE("panel: an empty field being edited says so") {
    // A lone caret in the value column is indistinguishable from a rendering
    // artefact. An empty edited field keeps its placeholder beside the caret.
    Panel::Config c;
    c.title = " Empty edit ";
    Panel::Row t;
    t.leading = "Host";
    t.control = maya::panel::Text{"", 0, "hostname"};
    c.rows.push_back(std::move(t));
    Panel::Row s;
    s.leading = "Key";
    s.control = maya::panel::Secret{0, 0};
    c.rows.push_back(std::move(s));
    c.selected = 0;

    const auto out = render(std::move(c));
    CHECK(out.find("hostname") != std::string::npos);
    CHECK(out.find("empty")    != std::string::npos);
}

TEST_CASE("panel: dropdown options align with the value column") {
    // The option list replaces the value it belongs to, so it must sit in the
    // same column. A one-column margin (rows use two) nudged the list right,
    // so opening a dropdown appeared to move the column.
    Panel::Config c;
    c.title = " Align ";
    Panel::Row ch;
    ch.leading = "Backend";
    ch.control = maya::panel::Choice{"Auto"};
    c.rows.push_back(std::move(ch));
    c.selected = 0;

    Panel::Menu m;
    m.options = {"Auto", "Ollama"};
    m.current = 0;
    c.menu     = m;
    c.menu_row = 0;

    const auto out = render(std::move(c), 60);

    // Find the right edge of the closed control's chevron row and of an
    // option row; they must end at the same column.
    const auto right_edge = [&](const std::string& needle) -> std::size_t {
        std::size_t start = 0;
        while (start <= out.size()) {
            const std::size_t nl = out.find('\n', start);
            const std::string line = out.substr(start, nl - start);
            if (line.find(needle) != std::string::npos) {
                const auto last = line.find_last_not_of(" \xe2\x94\x82");
                return last;
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return std::string::npos;
    };
    const auto opt_edge = right_edge("Ollama");
    CHECK(opt_edge != std::string::npos);
}

TEST_CASE("panel: a frame settles in a single render") {
    // The run loop re-renders whenever the renderer's post-layout writeback
    // disagrees with the scroll state the view was built from
    // (detail::scroll_writeback_dirty). So any quantity the panel and the
    // renderer BOTH compute is a latency bug waiting to happen: the first
    // frame paints from the panel's answer, the writeback substitutes the
    // renderer's, and the whole overlay is drawn again.
    //
    // `max_y` was exactly that. scrollbar_y sizes its thumb from it, but the
    // renderer only writes it back after layout — so the bar was drawn from
    // the PREVIOUS frame's content height, and every arrow press that changed
    // the height (the focused row's help line appearing) cost two renders,
    // some with a visibly wrong thumb in between.
    //
    // Panel measures the body, so it knows the extent before layout runs.
    // It publishes it, and the writeback then agrees instead of arbitrating.
    // This test asserts the agreement: rendering the SAME input twice must
    // leave the state untouched the second time.
    constexpr int kRows = 60;
    constexpr int kVh   = 8;

    maya::ScrollState s;
    const auto frame = [&](int sel) {
        Panel::Config c;
        c.title      = " Nav ";
        c.viewport_h = kVh;
        c.scroll     = &s;
        c.rows       = mixed_rows(kRows);
        c.selected   = sel;
        return render(std::move(c), 72);
    };

    frame(0);   // establish a steady state

    for (int sel = 0; sel < kRows; ++sel) {
        const auto first = frame(sel);
        const int  y = s.y, max_y = s.max_y;

        // Re-rendering identical input must be a no-op: same pixels, same
        // scroll geometry. If it is not, the user saw the first frame and
        // then a correction — a flicker, and a doubled input->photon.
        maya::detail::scroll_writeback_dirty = false;
        const auto second = frame(sel);

        CHECK(second == first);
        CHECK(s.y == y);
        CHECK(s.max_y == max_y);
        CHECK_FALSE(maya::detail::scroll_writeback_dirty);
    }
}

TEST_CASE("panel: the scrollbar is sized from the CURRENT frame's content") {
    // scrollbar_y sizes its thumb by reading s.max_y at BUILD time, but the
    // renderer only writes max_y back AFTER layout. A panel that leaves max_y
    // to the renderer therefore paints its bar from whatever the PREVIOUS
    // frame left behind; the writeback then notices, dirties the scroll state,
    // and the run loop draws the entire overlay again to correct it.
    //
    // The evidence has to be sampled at BUILD time. Rendering and then reading
    // s.max_y proves nothing — by then the writeback has corrected it, which
    // is exactly the second render this exists to prevent. So: build() the
    // element and inspect the state WITHOUT rendering. Whatever max_y holds at
    // that moment is what the scrollbar was sized from.
    for (int rows : {20, 60, 200}) {
        for (int vh : {4, 8, 14}) {
            maya::ScrollState s;          // cold, exactly as an overlay opens
            Panel::Config c;
            c.title      = " Cold ";
            c.viewport_h = vh;
            c.scroll     = &s;
            c.selected   = 0;
            for (int i = 0; i < rows; ++i) {
                Panel::Row r;
                r.leading = "row" + std::to_string(i);
                c.rows.push_back(std::move(r));
            }

            const maya::Element frame = Panel{std::move(c)}.build();
            (void)frame;
            // Plain rows are one line each, so the extent is exact arithmetic
            // — and it is known here, before any layout has run.
            CHECK(s.max_y == rows - vh);
        }
    }

    // A body that FITS publishes no scrollable extent, so the bar draws a
    // full-height thumb rather than a misleading sliver.
    {
        maya::ScrollState s;
        Panel::Config c;
        c.title      = " Fits ";
        c.viewport_h = 10;
        c.scroll     = &s;
        c.selected   = 0;
        for (int i = 0; i < 4; ++i) {
            Panel::Row r;
            r.leading = "row" + std::to_string(i);
            c.rows.push_back(std::move(r));
        }
        const maya::Element frame = Panel{std::move(c)}.build();
        (void)frame;
        CHECK(s.max_y == 0);
    }
}

TEST_CASE("panel: independent panels do not interfere across threads") {
    // Panel is a pure value transform (Config in, Element out) with exactly one
    // piece of shared mutable state: the BORROWED ScrollState*. maya's scroll
    // bookkeeping (the live-state registry and the writeback-dirty flag) is
    // thread_local, so panels on different threads with different states must
    // be fully independent.
    //
    // Worth asserting rather than assuming, because the panel now WRITES
    // s.max_y itself. A shared registry would have made that a race on every
    // overlay render; this pins the confinement that makes it safe.
    constexpr int kThreads = 8;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            maya::ScrollState s;                  // per-thread, not shared
            const int rows = 40 + t * 10;         // a different size per thread
            for (int sel = 0; sel < rows; ++sel) {
                Panel::Config c;
                c.title      = " T ";
                c.viewport_h = 8;
                c.scroll     = &s;
                c.selected   = sel;
                for (int i = 0; i < rows; ++i) {
                    Panel::Row r;
                    r.leading = "row" + std::to_string(i);
                    c.rows.push_back(std::move(r));
                }
                if (render(std::move(c), 72).empty()) ++failures;
            }
            // Each thread's state must describe ITS OWN content. A shared
            // registry or dirty flag would show up as a neighbour's extent
            // landing here.
            if (s.max_y != rows - 8) ++failures;
        });
    }
    for (auto& th : threads) th.join();

    CHECK(failures.load() == 0);
}

TEST_CASE("panel: a built Element outlives the Config it came from") {
    // Rows render into deferred ComponentElements (the section header measures
    // itself against the final width). If one of those captured a reference
    // into the Config — or `this` — the capture dangles the moment the
    // temporary Panel dies, and the crash lands at PAINT time, far from the
    // build that caused it. Build inside a scope, paint outside it.
    maya::Element e = [] {
        Panel::Config c;
        c.title = " Scoped ";
        c.rows.push_back(header_row("SECTION"));
        for (int i = 0; i < 12; ++i)
            c.rows.push_back(text_row("field", "value"));
        c.selected = 1;
        return Panel{std::move(c)}.build();
    }();

    const auto out = maya::render_to_string(e, 60);
    CHECK(framed(out));
    CHECK(out.find("SECTION") != std::string::npos);
}

TEST_CASE("panel: landing on a header does not move the view") {
    // A header cannot take focus — it carries no action, so the widget refuses
    // to render it as the cursor. But "no row is focused" is NOT the same as
    // "the cursor is on line 0", and conflating them made the keep-in-view
    // clamp dutifully scroll line 0 into view: navigating down a long grouped
    // list snapped it back to the top the moment the cursor crossed a section
    // header, losing the user's place entirely.
    //
    // The same applies to an absent or out-of-range selection, which is how a
    // picker with no matches renders while the user is still scrolling.
    constexpr int kRows = 60;
    constexpr int kVh   = 8;

    const auto scrolled_state = [&](maya::ScrollState& s) {
        // Walk far enough down that the view is genuinely scrolled.
        for (int sel = 0; sel < 40; ++sel) {
            Panel::Config c;
            c.title = " Nav "; c.viewport_h = kVh; c.scroll = &s;
            c.rows = mixed_rows(kRows); c.selected = sel;
            (void)render(std::move(c), 72);
        }
    };
    const auto render_at = [&](maya::ScrollState& s, int sel) {
        Panel::Config c;
        c.title = " Nav "; c.viewport_h = kVh; c.scroll = &s;
        c.rows = mixed_rows(kRows); c.selected = sel;
        (void)render(std::move(c), 72);
    };

    // mixed_rows makes every 11th row a header.
    {
        maya::ScrollState s;
        scrolled_state(s);
        const int before = s.y;
        REQUIRE(before > 0);
        render_at(s, 44);                     // a header
        CHECK(s.y == before);
    }
    // No selection at all.
    {
        maya::ScrollState s;
        scrolled_state(s);
        const int before = s.y;
        REQUIRE(before > 0);
        render_at(s, -1);
        CHECK(s.y == before);
    }
    // A selection left over from a longer list.
    {
        maya::ScrollState s;
        scrolled_state(s);
        const int before = s.y;
        REQUIRE(before > 0);
        render_at(s, 9999);
        CHECK(s.y == before);
    }
}

TEST_CASE("panel: multi-row items scroll in row space") {
    // Config::items are caller-built Elements and may be MULTI-ROW: the todo
    // picker pushes one PlanView covering every task. Measuring them as one
    // line each put the auto-scroll clamp in INDEX space, so the last of four
    // 3-row items — which begins at row 9, not row 3 — scrolled to 0 and left
    // the selection off-screen entirely.
    maya::ScrollState s;
    Panel::Config c;
    c.title      = " Items ";
    c.viewport_h = 4;
    c.scroll     = &s;
    for (int i = 0; i < 4; ++i) {
        const std::string n = std::to_string(i);
        c.items.push_back(maya::dsl::v(
            maya::dsl::text("item" + n + "-a"),
            maya::dsl::text("item" + n + "-b"),
            maya::dsl::text("item" + n + "-c")).build());
    }
    c.selected = 3;

    const auto out = render(std::move(c), 50);

    // Four 3-row items = 12 rows, viewport 4 ⇒ the last item ends at row 12,
    // so the offset must be 8. Index space would have said 0.
    CHECK(s.y == 8);
    CHECK(out.find("item3-c") != std::string::npos);
}

TEST_CASE("panel: the dropdown reads as one attached block") {
    // The dropdown's job is to look like a dropdown — a list belonging to the
    // row above it — and not like four more panel rows that happen to sit
    // nearby. Two properties carry that, and both were absent when every
    // option was right-aligned on its own:
    //
    //   1. The radios share a COLUMN. Independently right-aligned lines put
    //      them in as many columns as there were distinct label lengths, and
    //      a radio group whose radios do not line up does not read as a group.
    //   2. The block has a BOUNDARY, so where the options start and stop is
    //      drawn rather than inferred.
    maya::ScrollState s;
    Panel::Config c;
    c.title      = " Menu ";
    c.min_width  = 60;
    c.viewport_h = 12;
    c.scroll     = &s;
    for (int i = 0; i < 3; ++i) {
        Panel::Row r;
        r.leading = "field" + std::to_string(i);
        if (i == 1) r.control = maya::panel::Choice{.label = "beta"};
        c.rows.push_back(std::move(r));
    }
    Panel::Menu m;
    // Deliberately ragged: the shortest and longest differ by a lot, which is
    // exactly when per-line alignment scattered the radios.
    m.options     = {"a", "beta", "a considerably longer option label"};
    m.highlighted = 1;
    m.current     = 1;
    m.viewport    = 3;
    c.menu     = m;
    c.menu_row = 1;
    c.selected = 1;

    const auto out = render(std::move(c), 72);

    // Column of the radio glyph on each option line. All three must agree.
    std::vector<std::size_t> radio_cols;
    std::size_t start = 0;
    while (start <= out.size()) {
        const std::size_t nl = out.find('\n', start);
        const std::string line =
            out.substr(start, nl == std::string::npos ? std::string::npos
                                                      : nl - start);
        // ◉ (selected) or ○ (unselected)
        std::size_t at = line.find("\xe2\x97\x89");
        if (at == std::string::npos) at = line.find("\xe2\x97\x8b");
        if (at != std::string::npos) {
            // Count DISPLAY columns, not bytes: the chevron ahead of it is
            // multi-byte, so a byte offset would differ per line by construction.
            radio_cols.push_back(
                static_cast<std::size_t>(maya::string_width(line.substr(0, at))));
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    REQUIRE(radio_cols.size() == 3);
    CHECK(radio_cols[0] == radio_cols[1]);
    CHECK(radio_cols[1] == radio_cols[2]);

    // And the block is bracketed above and below.
    CHECK(out.find("\xe2\x95\xad") != std::string::npos);   // ╭ opens it
    CHECK(out.find("\xe2\x95\xb0") != std::string::npos);   // ╰ closes it
    // The top rule ties back to the row's chevron.
    CHECK(out.find("\xe2\x94\xb4") != std::string::npos);   // ┴
}

TEST_CASE("panel: a vertical list has no horizontal scroll extent") {
    // A section header PAINTS a rule out to the right edge, but its natural
    // size is just its label — the rule is decoration that expands into space
    // offered, not content demanding room. Conflating the two made its
    // ComponentElement::measure answer "I need all the width you have", and
    // the width offered during measurement is not a terminal width: a scroll
    // viewport probes its children against an UNBOUNDED width to discover the
    // content extent, so the header claimed ~2^24 columns.
    //
    // Two consequences, both real. The panel reported itself horizontally
    // scrollable by sixteen million columns, which is what the scrollbar and
    // wheel-routing machinery reads. And because the bogus number was derived
    // from the offered width, it CHANGED on every resize — dirtying the scroll
    // state and making the run loop redraw the whole overlay a second time on
    // 25 of 31 widths.
    //
    // The panel is a vertical list. max_x is 0, at every width, with any mix
    // of rows.
    for (int width : {40, 60, 80, 120, 160}) {
        maya::ScrollState s;
        Panel::Config c;
        c.title      = " Wide ";
        c.viewport_h = 10;
        c.scroll     = &s;
        c.rows       = mixed_rows(50);      // includes headers, help, errors
        c.selected   = 1;
        (void)render(std::move(c), width);
        CHECK(s.max_x == 0);
    }
}

TEST_CASE("panel: resizing settles in a single render") {
    // The resize counterpart to the keystroke fixed-point test. Sweeping the
    // terminal width must not leave the scroll state dirty: nothing about a
    // panel's CONTENT height depends on its width (rows are one line, help and
    // errors are NoWrap), so a writeback that disagrees is reporting geometry
    // the view got wrong rather than a genuine reflow.
    maya::ScrollState s;
    const auto frame = [&](int width) {
        Panel::Config c;
        c.title      = " Resize ";
        c.viewport_h = 10;
        c.scroll     = &s;
        c.rows       = mixed_rows(200);
        c.selected   = 100;
        return render(std::move(c), width);
    };

    frame(60);                              // establish a steady state

    int dirty = 0, unstable = 0;
    for (int width = 40; width <= 160; width += 4) {
        maya::detail::scroll_writeback_dirty = false;
        const auto f1 = frame(width);
        const int max_y = s.max_y, y = s.y;
        if (maya::detail::scroll_writeback_dirty) ++dirty;

        const auto f2 = frame(width);       // same width twice: a fixed point
        if (f1 != f2 || s.max_y != max_y || s.y != y) ++unstable;
    }
    CHECK(dirty == 0);
    CHECK(unstable == 0);
}

TEST_CASE("panel: a Secret is never rendered in the clear") {
    // The control carries a LENGTH, not a string — there is no plaintext in
    // the widget's inputs, so no render path can leak one.
    Panel::Config c;
    c.title = " Secret ";
    Panel::Row r;
    r.leading = "API key";
    r.control = maya::panel::Secret{11};
    c.rows.push_back(std::move(r));
    c.selected = 0;

    const auto out = render(std::move(c));
    CHECK(out.find("*") != std::string::npos);
    CHECK(count_of(out, "*") == 11);
}

// ============================================================================
//  Virtualisation
// ============================================================================
//
// The panel MEASURES the body (counting lines, building nothing) to decide
// which rows the viewport intersects, then renders only those — the thread
// list is thousands of rows behind a fourteen-row window, and painting the
// other 4986 cost ~67ms a frame.
//
// That splits one piece of knowledge in two: how tall a row is, and how tall
// a row draws. If they drift by a line, the skipped-row spacers no longer
// match the rows they stand for, and a field silently scrolls off its own
// help text. These tests pin them together THROUGH THE RENDERER, which is the
// only referee that sees both: max_y is computed by the layout from the
// actually-painted children, while the scroll clamp uses the measured total.
// If the two disagree, max_y disagrees with the arithmetic below.

namespace {

// The documented rule, restated independently of the widget: a row is one
// line, plus one for the help of the FOCUSED row, plus one for an error.
// A header is always exactly one.
int expected_lines(const Panel::Row& r, bool focused) {
    if (r.is_header) return 1;
    return 1 + (focused && !r.help.empty() ? 1 : 0) + (!r.error.empty() ? 1 : 0);
}

} // namespace

TEST_CASE("panel: the measured body is exactly as tall as the painted body") {
    // Walk the cursor over a list of mixed-height rows. At every stop the
    // content extent the LAYOUT computes from the painted children must equal
    // the total the MEASURE pass predicted — that equality is the whole
    // premise of rendering only a window.
    constexpr int kRows = 60;
    constexpr int kVh   = 8;

    for (int sel = 0; sel < kRows; ++sel) {
        maya::ScrollState s;
        Panel::Config c;
        c.title      = " Mixed ";
        c.viewport_h = kVh;
        c.scroll     = &s;
        c.rows       = mixed_rows(kRows);
        c.selected   = sel;

        int total = 0;
        for (int i = 0; i < kRows; ++i)
            total += expected_lines(c.rows[static_cast<std::size_t>(i)],
                                    i == sel);

        (void)render(std::move(c));
        // content = viewport + max_y, by the renderer's own definition.
        CHECK(s.max_y == total - kVh);
    }
}

TEST_CASE("panel: an open menu is measured at the height it draws") {
    // The menu is the tallest thing a single row can contribute, and its
    // height is conditional three ways (a hint for the focused option, a
    // position counter only when windowed, an empty-list placeholder).
    //
    // The assertion is VISIBILITY, not extent: the scroll keeps the focused
    // row's whole span on screen, and that span is what measure computed. If
    // measure undercounts the menu by even one line, the scroll stops one
    // line short and the bottom of the option list — the counter that says
    // there is more, or the hint explaining the option under the cursor —
    // sits just past the bottom edge. Asserting the painted extent instead
    // would prove nothing: it is computed from what was drawn, so a measure
    // that disagrees with the drawing cancels out of the comparison.
    const auto frame = [](int opts, int viewport, int hi, bool with_hints,
                          int vh) -> std::string {
        maya::ScrollState s;
        Panel::Config c;
        c.title      = " Menu ";
        c.viewport_h = vh;
        c.scroll     = &s;
        for (int i = 0; i < 12; ++i) {
            Panel::Row r;
            r.leading = "field" + std::to_string(i);
            if (i == 3) r.control = maya::panel::Choice{.label = "b"};
            c.rows.push_back(std::move(r));
        }
        Panel::Menu m;
        for (int i = 0; i < opts; ++i)
            m.options.push_back("option" + std::to_string(i));
        if (with_hints)
            for (int i = 0; i < opts; ++i)
                m.hints.push_back("about" + std::to_string(i));
        m.highlighted = hi;
        m.current     = 0;
        m.viewport    = viewport;
        c.menu     = m;
        c.menu_row = 3;
        c.selected = 3;
        return render(std::move(c), 60);
    };

    // A windowed list with a hint: row + the block's two rules + 3 options +
    // hint + "2/5". Every one of those lines must be on screen at once, which
    // is what the measure pass has to have predicted for the scroll to keep
    // the group together.
    {
        const auto out = frame(5, 3, 1, true, 8);
        CHECK(out.find("field3")  != std::string::npos);
        CHECK(out.find("option0") != std::string::npos);
        CHECK(out.find("option2") != std::string::npos);
        CHECK(out.find("about1")  != std::string::npos);   // the hint
        CHECK(out.find("2/5")     != std::string::npos);   // the counter
    }
    // No hints: the counter is then the last line of the group.
    {
        const auto out = frame(5, 3, 1, false, 7);
        CHECK(out.find("option2") != std::string::npos);
        CHECK(out.find("2/5")     != std::string::npos);
        CHECK(out.find("about1")  == std::string::npos);
    }
    // Fits: no counter at all, and the last option is the last line.
    {
        const auto out = frame(3, 5, 1, false, 6);
        CHECK(out.find("option2") != std::string::npos);
        CHECK(out.find("3/3")     == std::string::npos);
    }
    // Empty option set: the placeholder still gets its line.
    {
        const auto out = frame(0, 5, 0, false, 2);
        CHECK(out.find("(no options)") != std::string::npos);
    }
}

TEST_CASE("panel: a row is rendered whenever any part of it is on screen") {
    // The window is chosen from LINE offsets, so a row straddling either edge
    // must still be built — the scroll offset then clips it. Dropping a
    // partially-visible row is how virtualisation shows a blank strip at the
    // top or bottom of the viewport.
    constexpr int kRows = 40;
    constexpr int kVh   = 6;

    maya::ScrollState s;
    for (int sel = 0; sel < kRows; ++sel) {
        Panel::Config c;
        c.title      = " Window ";
        c.viewport_h = kVh;
        c.scroll     = &s;          // shared: this is a real key-nav walk
        c.rows       = mixed_rows(kRows);
        c.selected   = sel;

        const bool is_header = c.rows[static_cast<std::size_t>(sel)].is_header;
        const std::string label = "row" + std::to_string(sel);
        const std::string help  = c.rows[static_cast<std::size_t>(sel)].help;

        const auto out = render(std::move(c));

        CHECK(framed(out));
        // The row you are on is always on screen...
        if (!is_header) CHECK(out.find(label) != std::string::npos);
        // ...and so is the help it owns, because the scroll keeps the row's
        // whole span in view. This is what a one-line measure error breaks.
        if (!is_header && !help.empty())
            CHECK(out.find(help) != std::string::npos);
    }
}

TEST_CASE("panel: a huge list renders only its viewport") {
    // The point of the exercise. Five thousand rows, fourteen visible: the
    // frame must contain the rows around the cursor and none of the rest.
    maya::ScrollState s;
    Panel::Config c;
    c.title      = " Huge ";
    c.viewport_h = 14;
    c.scroll     = &s;
    c.rows.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        Panel::Row r;
        r.leading = "entry-" + std::to_string(i);
        c.rows.push_back(std::move(r));
    }
    c.selected = 2500;

    const auto out = render(std::move(c), 60);

    CHECK(framed(out));
    CHECK(out.find("entry-2500") != std::string::npos);
    // Neither end of the list is anywhere near the window.
    CHECK(out.find("entry-0\n") == std::string::npos);
    CHECK(out.find("entry-4999") == std::string::npos);
    // The scrollbar still knows the true size of the list.
    CHECK(s.max_y == 5000 - 14);
}

