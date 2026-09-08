// Tests for the form component layer: the shared state model, the one key
// map, and the pure projection onto maya::Panel::Config.
//
// The properties worth pinning are the ones that were bugs in the five
// hand-rolled pickers this replaces:
//
//   * exactly one mode owns the keyboard (a keystroke can never leak from a
//     text field into global navigation),
//   * Esc from a dropdown is a true CANCEL (the value is untouched until
//     commit),
//   * a Secret's plaintext never reaches the rendering layer at all — not
//     "is masked", but structurally absent from what maya is handed.

#include <doctest/doctest.h>

#include <maya/app/inline.hpp>

#include "agentty/runtime/panel/form.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/runtime/view/form_panel.hpp"

using namespace agentty::form;
namespace keys = agentty::form::keys;

namespace {

maya::KeyEvent key(maya::SpecialKey k) {
    maya::KeyEvent ev;
    ev.key = k;
    return ev;
}

maya::KeyEvent chr(char32_t c, bool ctrl = false) {
    maya::KeyEvent ev;
    ev.key = maya::CharKey{c};
    ev.mods.ctrl = ctrl;
    return ev;
}

Form demo() {
    return Builder{"Demo"}
        .choice("backend", "Backend", {"Auto", "Ollama", "Custom"}, {"auto", "ollama", "openai"}, "auto")
        .text("host", "Host", "127.0.0.1")
        .number("port", "Port", 11434, 1, 65535)
        .slider("weight", "Weight", 0.65, 0.0, 1.0, 0.05)
        .secret("key", "API key", "sk-super-secret")
        .action("test", "Test connection")
        .build();
}

// Drive one key through the router + reducer, as the pane does.
keys::Applied press(Form& f, const maya::KeyEvent& ev) {
    auto a = keys::translate(f, ev);
    if (!a) return {};
    return keys::apply(f, *a);
}

} // namespace

TEST_CASE("form: builder produces addressable rows") {
    auto f = demo();
    CHECK(f.fields.size() == 6);
    REQUIRE(f.find("host") != nullptr);
    CHECK(f.find("host")->label == "Host");
    CHECK(f.find("nope") == nullptr);
    // Choice selects by stored id, not display label.
    CHECK(std::get<field::Choice>(f.find("backend")->value).id() == "auto");
}

TEST_CASE("form: exactly one mode owns the keyboard") {
    auto f = demo();
    CHECK(std::holds_alternative<focus::Browsing>(f.focus));

    // Enter on a text row engages editing.
    f.cursor = 1;
    press(f, key(maya::SpecialKey::Enter));
    CHECK(f.editing());
    CHECK_FALSE(f.choosing());

    // While editing, 'j' is a character — NOT a move. This is the leak the
    // hand-rolled inputs kept springing.
    const int before = f.cursor;
    press(f, chr(U'j'));
    CHECK(f.cursor == before);
    CHECK(std::get<field::Text>(f.find("host")->value).value.find('j') != std::string::npos);

    // Esc leaves the field but not the form.
    auto r = press(f, key(maya::SpecialKey::Escape));
    CHECK_FALSE(r.close);
    CHECK_FALSE(f.editing());

    // Esc again closes.
    r = press(f, key(maya::SpecialKey::Escape));
    CHECK(r.close);
}

TEST_CASE("form: dropdown opens, commits, and cancels non-destructively") {
    auto f = demo();
    f.cursor = 0;

    press(f, key(maya::SpecialKey::Enter));
    CHECK(f.choosing());
    // Opening must not touch the value.
    CHECK(std::get<field::Choice>(f.find("backend")->value).id() == "auto");

    press(f, key(maya::SpecialKey::Down));
    // Still untouched — the highlight moved, not the field.
    CHECK(std::get<field::Choice>(f.find("backend")->value).id() == "auto");

    // Esc is a TRUE cancel.
    press(f, key(maya::SpecialKey::Escape));
    CHECK_FALSE(f.choosing());
    CHECK(std::get<field::Choice>(f.find("backend")->value).id() == "auto");

    // Reopen, move, commit.
    press(f, key(maya::SpecialKey::Enter));
    press(f, key(maya::SpecialKey::Down));
    auto r = press(f, key(maya::SpecialKey::Enter));
    CHECK(r.changed);
    CHECK_FALSE(f.choosing());
    CHECK(std::get<field::Choice>(f.find("backend")->value).id() == "ollama");
    CHECK(f.dirty);
}

TEST_CASE("form: an open dropdown swallows stray keys") {
    auto f = demo();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    REQUIRE(f.choosing());

    // A key the menu doesn't use must NOT fall through to the form beneath.
    const int cursor_before = f.cursor;
    press(f, key(maya::SpecialKey::Right));
    CHECK(f.cursor == cursor_before);
    CHECK(f.choosing());
}

TEST_CASE("form: a dropdown never filters — j/k always navigate") {
    // The dropdown holds an ENUM. It deliberately has no query state, so j/k
    // are unconditionally navigation and can never be swallowed as filter
    // text. A candidate set big enough to need searching is a `Pick` row.
    auto f = demo();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    REQUIRE(f.choosing());

    press(f, key(maya::SpecialKey::Down));
    CHECK(f.dropdown()->highlighted == 1);
    press(f, key(maya::SpecialKey::Up));
    CHECK(f.dropdown()->highlighted == 0);

    // Even a long list navigates rather than filters — the option count does
    // not change the interaction model.
    std::vector<std::string> many;
    for (int i = 0; i < 12; ++i) many.push_back("model-" + std::to_string(i));
    auto g = Builder{"Long"}.choice("m", "Model", many).build();
    press(g, key(maya::SpecialKey::Enter));
    REQUIRE(g.choosing());
    press(g, key(maya::SpecialKey::Down));
    CHECK(g.dropdown()->highlighted == 1);
}

TEST_CASE("form: a Pick row hands off instead of opening a dropdown") {
    // The escape hatch that keeps Choice honest: large/dynamic candidate sets
    // open a real picker, and the form never learns about them.
    auto f = Builder{"Hand-off"}
        .pick("model", "Model", "claude-opus-4-5")
        .build();
    f.cursor = 0;

    const auto r = press(f, key(maya::SpecialKey::Enter));
    CHECK(r.hand_off);
    CHECK_FALSE(f.choosing());     // no inline list
    CHECK_FALSE(f.editing());      // and not an edit either
    CHECK_FALSE(r.changed);
}

TEST_CASE("form: dropdown commit resolves through the same list the view shows") {
    // The reducer and the view resolve options with the same function, so the
    // highlighted row and the committed value cannot diverge.
    std::vector<std::string> many;
    for (int i = 0; i < 12; ++i) many.push_back("model-" + std::to_string(i));
    auto f = Builder{"Long"}.choice("m", "Model", many).build();

    press(f, key(maya::SpecialKey::Enter));
    for (int i = 0; i < 3; ++i) press(f, key(maya::SpecialKey::Down));
    const auto& c = std::get<field::Choice>(f.find("m")->value);
    REQUIRE(visible_options(c).labels.size() == 12);
    press(f, key(maya::SpecialKey::Enter));
    CHECK(std::get<field::Choice>(f.find("m")->value).label() == "model-3");
}

TEST_CASE("form: number and slider clamp on every mutation") {
    auto f = demo();
    f.cursor = 2;                               // port
    press(f, key(maya::SpecialKey::Enter));
    for (char32_t c : {U'9', U'9', U'9', U'9', U'9', U'9'}) press(f, chr(c));
    CHECK(std::get<field::Number>(f.find("port")->value).value == 65535);

    press(f, key(maya::SpecialKey::Escape));
    f.cursor = 3;                               // weight slider
    for (int i = 0; i < 20; ++i) press(f, key(maya::SpecialKey::Right));
    CHECK(std::get<field::Slider>(f.find("weight")->value).value == doctest::Approx(1.0));
    for (int i = 0; i < 40; ++i) press(f, key(maya::SpecialKey::Left));
    CHECK(std::get<field::Slider>(f.find("weight")->value).value == doctest::Approx(0.0));
}

TEST_CASE("form: slider steps stay on the grid") {
    // Repeated ±step must not accumulate float drift into 0.6500000000000001.
    auto f = demo();
    f.cursor = 3;
    for (int i = 0; i < 5; ++i) press(f, key(maya::SpecialKey::Right));
    for (int i = 0; i < 5; ++i) press(f, key(maya::SpecialKey::Left));
    CHECK(std::get<field::Slider>(f.find("weight")->value).value == doctest::Approx(0.65));
}

TEST_CASE("form: locked rows refuse edits") {
    auto f = Builder{"Locked"}
        .text("host", "Host", "127.0.0.1")
        .lock("env: AGENTTY_EMBED_ENDPOINT")
        .build();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    CHECK_FALSE(f.editing());                   // never engages
    press(f, key(maya::SpecialKey::Right));
    CHECK(std::get<field::Text>(f.find("host")->value).value == "127.0.0.1");
    CHECK_FALSE(f.dirty);
}

TEST_CASE("form: an action row reports rather than mutating") {
    auto f = demo();
    f.cursor = 5;                               // "Test connection"
    auto r = press(f, key(maya::SpecialKey::Enter));
    CHECK(r.fired);
    CHECK_FALSE(r.changed);
    CHECK_FALSE(f.editing());
}

TEST_CASE("form: no save chord — Esc commits; ^C stays ambient") {
    auto f = demo();
    // ^S is NOT a form key: leaving a field commits it, and Esc leaves.
    // A second key for the same act is a second thing to keep in sync.
    CHECK_FALSE(keys::translate(f, chr(U's', /*ctrl=*/true)).has_value());
    // ^C must NOT be claimed — the app quits from anywhere.
    CHECK_FALSE(keys::translate(f, chr(U'c', true)).has_value());
}

// ── The projection ───────────────────────────────────────────────────────

TEST_CASE("form view: a Secret's plaintext never reaches the widget") {
    // Not "is masked" — structurally absent. maya::panel::Secret has no field
    // that could hold it, so no present or future render path can leak it.
    auto f = demo();
    const auto cfg = agentty::ui::form_config(f, maya::Color::blue());

    bool saw_secret = false;
    for (const auto& row : cfg.items) {
        if (const auto* s = std::get_if<maya::panel::Secret>(&row.control)) {
            saw_secret = true;
            CHECK(s->filled == 15);             // "sk-super-secret"
        }
        // No control anywhere carries the plaintext.
        if (const auto* t = std::get_if<maya::panel::Text>(&row.control))
            CHECK(t->value.find("sk-") == std::string::npos);
    }
    CHECK(saw_secret);
}

TEST_CASE("form view: menu is present only while choosing") {
    auto f = demo();
    f.cursor = 0;
    CHECK_FALSE(agentty::ui::form_config(f, maya::Color::blue()).menu.has_value());

    press(f, key(maya::SpecialKey::Enter));
    const auto cfg = agentty::ui::form_config(f, maya::Color::blue());
    REQUIRE(cfg.menu.has_value());
    CHECK(cfg.menu_row == 0);
    CHECK(cfg.menu->options.size() == 3);
    CHECK(cfg.menu->options[0] == "Auto");
}

TEST_CASE("form view: the widget renders every control kind") {
    // A smoke test that the maya widget builds without throwing for each
    // control, including with an open menu (the z-stacked float path).
    auto f = demo();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    auto cfg = agentty::ui::form_config(f, maya::Color::blue());
    cfg.items[1].origin = "settings.json";
    cfg.items[2].error  = "port must be 1-65535";
    maya::Element el = maya::Panel{std::move(cfg)}.build();
    CHECK(true);   // built without throwing
}

TEST_CASE("form view: one setting is one row") {
    // Regression: every row with `help` painted a second, dim sub-line, so a
    // pane's height was 2x its setting count. The RAG pane has 22 settings
    // across 5 group headers -- over 50 rows in a 14-row viewport, which
    // turned a settings list into a wall of prose you had to scroll to reach
    // a single toggle.
    //
    // Help belongs to the row the cursor is ON: that is the only row it can
    // be describing, and it costs one line instead of N.
    auto f = Builder{"Helpy"}
        .toggle("a", "Alpha", true,  "the first thing")
        .toggle("b", "Beta",  false, "the second thing")
        .toggle("c", "Gamma", true,  "the third thing")
        .build();

    const auto height = [&](const Form& form) {
        auto cfg = agentty::ui::form_config(form, maya::Color::blue());
        cfg.viewport_h = 100;            // no clamping: measure the content
        const auto out = maya::render_to_string(
            maya::Panel{std::move(cfg)}.build(), 80);
        int n = 0;
        for (char ch : out) if (ch == '\n') ++n;
        return n;
    };

    f.cursor = 0;
    const int h0 = height(f);
    f.cursor = 1;
    const int h1 = height(f);

    // Moving the cursor must not change the pane's height: exactly one help
    // line exists either way. Before the fix all three were always painted.
    CHECK(h0 == h1);

    // And the help shown is the one under the cursor, not all of them.
    const auto text_at = [&](int cursor) {
        f.cursor = cursor;
        auto cfg = agentty::ui::form_config(f, maya::Color::blue());
        cfg.viewport_h = 100;
        return maya::render_to_string(maya::Panel{std::move(cfg)}.build(), 80);
    };
    const std::string on_first  = text_at(0);
    const std::string on_second = text_at(1);
    CHECK(on_first.find("the first thing")  != std::string::npos);
    CHECK(on_first.find("the second thing") == std::string::npos);
    CHECK(on_second.find("the second thing") != std::string::npos);
    CHECK(on_second.find("the first thing")  == std::string::npos);
}

TEST_CASE("form view: headers cost one row, not two") {
    // Regression: every section header emitted a blank spacer line above
    // itself. At one per group that was 5 wasted rows of a 14-row viewport in
    // the Retrieval pane -- in a list you scroll, a row that says nothing is
    // a row that hides one that does. A header is already distinct (bold,
    // dim, no value column), so the blank bought nothing.
    //
    // Headers render UPPER-CASE with a trailing rule, so they cannot be
    // mistaken for a locked row (which is what "Endpoint  auto-detected"
    // looked like when both were merely dim).
    auto f = Builder{"Grouped"}
        .header("Sources")
        .toggle("a", "Alpha", true)
        .header("Pipeline")
        .toggle("b", "Beta", false)
        .build();

    auto cfg = agentty::ui::form_config(f, maya::Color::blue());
    cfg.viewport_h = 100;
    const auto out = maya::render_to_string(
        maya::Panel{std::move(cfg)}.build(), 80);

    // No blank line directly above either header.
    const auto no_blank_before = [&](const std::string& label) {
        const auto at = out.find(label);
        REQUIRE(at != std::string::npos);
        const auto line_start = out.rfind('\n', at);
        REQUIRE(line_start != std::string::npos);
        const auto prev_start = out.rfind('\n', line_start - 1);
        const std::string prev = out.substr(
            prev_start + 1, line_start - prev_start - 1);
        // The previous line must carry content, not just frame + spaces.
        return prev.find_first_not_of(" \u2502") != std::string::npos;
    };
    CHECK(no_blank_before("PIPELINE"));
    // And the header is visually a header: caps plus a rule to the edge.
    CHECK(out.find("SOURCES") != std::string::npos);
    CHECK(out.find("\xe2\x94\x80\xe2\x94\x80") != std::string::npos);
}

TEST_CASE("form view: the current row is tinted, the others are not") {
    // With help gone from unfocused rows, a single accent cell at the far
    // left was the only focus signal -- easy to lose when your eye is on the
    // value column at the far right. The current row now carries a subtle
    // wash across its whole span.
    //
    // The wash must be a real colour, not ANSI `black`: black IS the
    // terminal background on most dark themes, so the first attempt at this
    // was invisible by construction. It uses the same value nib's code_view
    // uses for its current-line shade.
    auto f = Builder{"Tinted"}
        .toggle("a", "Alpha", true)
        .toggle("b", "Beta", false)
        .build();
    f.cursor = 0;

    auto cfg = agentty::ui::form_config(f, maya::Color::blue());
    cfg.viewport_h = 100;
    const auto ansi = maya::render_to_string_ansi(
        maya::Panel{std::move(cfg)}.build(), 80);

    const auto line_with = [&](const std::string& label) {
        const auto at = ansi.find(label);
        REQUIRE(at != std::string::npos);
        const auto from = ansi.rfind('\n', at);
        const auto to   = ansi.find('\n', at);
        return ansi.substr(from + 1, to - from - 1);
    };

    // 0x232634 as an SGR true-colour background. Asserting the emitted code
    // rather than "it looks nice" is the only version of this that can fail
    // for the right reason -- and it is what caught the invisible black.
    const std::string wash = "48;2;35;38;52";
    CHECK(line_with("Alpha").find(wash) != std::string::npos);
    CHECK(line_with("Beta").find(wash)  == std::string::npos);
}

TEST_CASE("form view: the panel is FRAMED like every other picker") {
    // Regression: Form::build() originally returned a bare vstack with no
    // border, padding or min-width. A frameless overlay paints transparently
    // over the thread, which on screen reads as "the key did nothing" / "the
    // picker closed" — the pane was open and invisible. A form is a
    // picker-family overlay and must sit in the same frame.
    auto f = demo();
    const auto out = maya::render_to_string(
        maya::Panel{agentty::ui::form_config(f, maya::Color::blue())}.build(), 80);

    // Rounded border corners, exactly as Picker draws them.
    CHECK(out.find("\xe2\x95\xad") != std::string::npos);   // ╭
    CHECK(out.find("\xe2\x95\xb0") != std::string::npos);   // ╰
    // The title rides the top border.
    CHECK(out.find("Demo") != std::string::npos);
    // And the rows are actually painted.
    CHECK(out.find("Backend") != std::string::npos);
    CHECK(out.find("Host")    != std::string::npos);
}

TEST_CASE("form view: an open dropdown paints its options over the panel") {
    // The float must actually reach the screen — a menu that is state-only
    // but never painted is the same invisible-overlay failure one level down.
    auto f = demo();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    REQUIRE(f.choosing());

    const auto out = maya::render_to_string(
        maya::Panel{agentty::ui::form_config(f, maya::Color::blue())}.build(), 80);
    // Every backend option is on screen, including ones that are not the
    // current selection (i.e. this is the list, not the closed control).
    CHECK(out.find("Ollama") != std::string::npos);
    CHECK(out.find("Custom") != std::string::npos);
}

TEST_CASE("form view: the option list is INLINE, inside the panel") {
    // The list was once a z-stacked float, which had to invent an opaque
    // background, a width, a flip and a clamp — and every one of those was a
    // bug (bleed-through, overflow, a theme-coloured slab). It now expands in
    // the panel's own flow, so the only thing to assert is that each option
    // line stays INSIDE the panel border.
    auto f = demo();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    REQUIRE(f.choosing());

    const auto out = maya::render_to_string(
        maya::Panel{agentty::ui::form_config(f, maya::Color::blue())}.build(), 80);

    std::size_t start = 0;
    int checked = 0;
    while (start <= out.size()) {
        const std::size_t nl = out.find('\n', start);
        const std::string line = out.substr(start, nl - start);
        // These strings only ever appear as option rows (the closed control
        // shows "Custom endpoint", and while open it shows only the chevron).
        if (line.find("Ollama") != std::string::npos
            || line.find("llama.cpp server") != std::string::npos) {
            // Exactly two vertical bars: the panel's left and right border.
            // A third would mean a box crept back in; fewer would mean the
            // line escaped the frame.
            int bars = 0;
            for (std::size_t i = 0;
                 (i = line.find("\xe2\x94\x82", i)) != std::string::npos; i += 3)
                ++bars;
            CHECK(bars == 2);
            ++checked;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    CHECK(checked > 0);   // we actually inspected a row
}

TEST_CASE("form view: a Secret is never painted in the clear") {
    // The end-to-end version of the type-level guarantee: render the whole
    // widget and confirm the plaintext appears nowhere in the output.
    auto f = demo();
    const auto out = maya::render_to_string(
        maya::Panel{agentty::ui::form_config(f, maya::Color::blue())}.build(), 80);
    CHECK(out.find("sk-super-secret") == std::string::npos);
    CHECK(out.find("sk-")             == std::string::npos);
    CHECK(out.find("*")               != std::string::npos);   // masked instead
}

TEST_CASE("form: an open pane ALWAYS answers Esc") {
    // Regression (the "input stops getting through" freeze): the Retrieval
    // overlay was created holding no form, filled in by a deferred Cmd. For at
    // least one frame the pane owned the keyboard and could act on nothing —
    // every key, INCLUDING the Esc that would have escaped it, was swallowed.
    //
    // The structural fix is that the form is built at construction and is not
    // optional, so "open but inert" is unrepresentable. This pins the
    // behavioural half: from every focus mode, Esc must eventually close.
    auto f = demo();

    // Browsing → closes immediately.
    CHECK(press(f, key(maya::SpecialKey::Escape)).close);

    // Editing → first Esc leaves the field, second closes.
    f = demo();
    f.cursor = 1;
    press(f, key(maya::SpecialKey::Enter));
    REQUIRE(f.editing());
    CHECK_FALSE(press(f, key(maya::SpecialKey::Escape)).close);
    CHECK(press(f, key(maya::SpecialKey::Escape)).close);

    // Choosing → first Esc cancels the list, second closes.
    f = demo();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    REQUIRE(f.choosing());
    CHECK_FALSE(press(f, key(maya::SpecialKey::Escape)).close);
    CHECK(press(f, key(maya::SpecialKey::Escape)).close);
}

TEST_CASE("form: an empty form still answers Esc") {
    // The degenerate case the freeze actually hit: a form with no rows must
    // not become a keyboard black hole.
    Form empty;
    auto a = keys::translate(empty, key(maya::SpecialKey::Escape));
    REQUIRE(a.has_value());
    CHECK(keys::apply(empty, *a).close);
}

TEST_CASE("form: the key router depends only on focus, not on the rows") {
    // Regression (input lag, twice). `subscribe()` rebuilds its key handler
    // EVERY FRAME. A router that needs the Form makes each frame deep-copy
    // every row's strings on the input path — which is what made the settings
    // pane feel laggy the moment it grew past a handful of rows.
    //
    // The flags overload is the contract: three bools decide the whole key
    // map (editing, choosing, text_row — focus-shaped facts, not rows). If
    // someone reintroduces a Form-shaped dependency, this stops compiling
    // rather than quietly costing a copy per keystroke.
    const auto esc = key(maya::SpecialKey::Escape);
    const auto j   = chr(U'j');

    // Browsing a NON-text row: bare letters do NOTHING — the one key
    // policy (actions are chords/Enter/Esc/arrows; letters only type).
    {
        auto a = keys::translate(/*editing=*/false, /*choosing=*/false, j);
        CHECK_FALSE(a.has_value());
    }
    // Browsing a TEXT row: the same key TYPES — type-to-edit, no Enter
    // modality. TypeToEdit (not Insert) so a stale batch can't re-enter.
    {
        auto a = keys::translate(false, false, j, /*text_row=*/true);
        REQUIRE(a.has_value());
        CHECK(a->intent == keys::Intent::TypeToEdit);
        CHECK(a->ch == U'j');
    }
    // Editing: the same key is text, never navigation.
    {
        auto a = keys::translate(/*editing=*/true, /*choosing=*/false, j);
        REQUIRE(a.has_value());
        CHECK(a->intent == keys::Intent::Insert);
        CHECK(a->ch == U'j');
    }
    // Choosing: an open dropdown swallows printables (an enum list has no
    // filter; leaking the key to the form under it would be a mis-target).
    {
        auto a = keys::translate(/*editing=*/false, /*choosing=*/true, j);
        REQUIRE(a.has_value());
        CHECK(a->intent == keys::Intent::None);
    }
    // And every mode answers Esc — the escape guarantee, at the router level.
    for (auto [e, c] : {std::pair{false, false}, {true, false}, {false, true}}) {
        auto a = keys::translate(e, c, esc);
        REQUIRE(a.has_value());
        CHECK(a->intent != keys::Intent::None);
    }
}

TEST_CASE("form: caret is reported in characters, not bytes") {
    // maya renders characters; the model stores bytes. A multi-byte value
    // would draw its caret in the wrong column if this were confused.
    auto f = Builder{"UTF"}.text("t", "T", "").build();
    f.cursor = 0;
    press(f, key(maya::SpecialKey::Enter));
    press(f, chr(U'é'));
    press(f, chr(U'漢'));
    CHECK(std::get<field::Text>(f.find("t")->value).value.size() == 5);  // bytes
    CHECK(caret_chars(f.find("t")->value, true) == 2);                   // chars
}
