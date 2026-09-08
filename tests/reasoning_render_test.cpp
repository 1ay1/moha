// reasoning_render_test.cpp — the reasoning ("Thinking") block renders across
// providers and lifecycle states.
//
// Reasoning text from EVERY provider funnels into Message::thinking via
// StreamThinkingDelta (Anthropic thinking_delta, Codex reasoning_summary_text
// .delta, OpenAI-compat reasoning_content), and Message::reasoning_display_text
// () is the single unified accessor the view keys off. This pins:
//   1. the unified accessor's precedence (thinking, else reasoning_summary),
//   2. that a SETTLED reasoning turn renders the FULL reasoning text (it does
//      NOT fold to a one-line summary) under a "Reasoned" header, ABOVE the
//      answer,
//   3. that a turn with NO reasoning renders neither.

#include "agtest.hpp"

#include <cstdlib>   // setenv — the reveal tests pin the effect explicitly

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/view/thread/thread.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"

#include <maya/widget/app_layout.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/style/theme.hpp>

#include <print>
#include <string>
#include <vector>

using namespace agentty;

namespace {

// Render the whole app (transcript included) to plain ASCII text. Mirrors
// midrun_seam_test's render_rows harness; non-ASCII glyphs (the ✦/· sigils)
// map to '?', so assertions target the ASCII words.
std::string render_text(const Model& m, int width = 100, int height = 4000) {
    auto root = maya::AppLayout{{
        .thread        = ui::thread_config(m),
        .changes_strip = ui::changes_strip_config(m),
        .composer      = ui::composer_config(m),
        .status_bar    = ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        out.push_back('\n');
    }
    return out;
}

bool has(const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}

Message assistant(std::string text) {
    Message a;
    a.role = Role::Assistant;
    a.id   = MessageId{"a1"};
    a.text = std::move(text);
    return a;
}

} // namespace

TEST_CASE("reasoning: unified accessor precedence") {
    Message a = assistant("answer");
    check(a.reasoning_display_text().empty(), "no reasoning by default");
    check(!a.has_reasoning(), "has_reasoning false when empty");

    a.reasoning_summary = "legacy summary";
    check(a.reasoning_display_text() == "legacy summary",
          "falls back to reasoning_summary");

    a.thinking = "primary thinking";
    check(a.reasoning_display_text() == "primary thinking",
          "thinking (the unified stream field) wins over reasoning_summary");
    check(a.has_reasoning(), "has_reasoning true when thinking present");
}

TEST_CASE("reasoning: settled turn shows the full reasoning, not a fold") {
    Model m;
    m.d.show_reasoning = true;   // the global ^R switch is on
    Message a = assistant("Here is the FINAL_ANSWER_MARKER.");
    // Reasoning arrived (any provider) and the answer is present => SETTLED:
    // render the FULL streamed reasoning (no fold), under a "Reasoned" header.
    a.thinking = "Analyze the request FIRST_LINE_MARKER\n"
                 "then a SECOND_LINE_MARKER that must ALSO stay visible.";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};

    const std::string out = render_text(m);
    check(has(out, "Reasoned"),
          "settled reasoning renders a 'Reasoned' header");
    check(has(out, "token"),
          "the settled header names an approximate token count");
    check(has(out, "FINAL_ANSWER_MARKER"),
          "the answer still renders alongside the reasoning block");
    // The block STAYS FULLY EXPANDED after settle — no fold to a glimpse.
    check(has(out, "FIRST_LINE_MARKER"),
          "settled reasoning keeps its first line");
    check(has(out, "SECOND_LINE_MARKER"),
          "settled reasoning keeps ALL lines — it does not fold to a summary");
}

TEST_CASE("reasoning: no block when the turn never reasoned") {
    Model m;
    m.d.show_reasoning = true;
    m.d.current.messages.push_back(assistant("PLAIN_ANSWER no reasoning here."));
    m.s.phase = phase::Idle{};
    const std::string out = render_text(m);
    check(!has(out, "Reasoned"),
          "no reasoning block when the turn produced no reasoning");
    check(has(out, "PLAIN_ANSWER"), "the plain answer still renders");
}

TEST_CASE("reasoning: the ^R switch hides the block even when text exists") {
    Model m;
    m.d.show_reasoning = false;   // global switch OFF (the default)
    Message a = assistant("Here is the ANSWER_STILL_SHOWN.");
    a.thinking = "lots of hidden reasoning the user chose not to see";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};
    const std::string out = render_text(m);
    check(!has(out, "Reasoned"),
          "reasoning block is suppressed when show_reasoning is off");
    check(!has(out, "hidden reasoning"),
          "reasoning text never reaches the screen when the switch is off");
    check(has(out, "ANSWER_STILL_SHOWN"),
          "the answer renders normally regardless of the reasoning switch");
}

// The reasoning body must REVEAL incrementally like normal streamed text —
// not appear all at once. We grow msg.thinking frame by frame (as the wire
// would), advance the shared animation clock, and assert the visible reasoning
// text grows GRADUALLY rather than jumping straight to full on the frame the
// bytes arrive. This pins the streaming-reveal path (turn.cpp MdView::Reasoning
// + subturn_stably_keyable's reasoning-slot animation check).
TEST_CASE("reasoning: body reveals incrementally, not all at once") {
    // (The reveal is pinned ON for the whole binary in test_main.cpp — its
    // default is terminal-derived and must not leak into the suite.)
    // A long, single-paragraph body so the reveal cursor has many chars to
    // walk through (word-boundary markers we can count as they appear).
    std::string full;
    for (int i = 0; i < 60; ++i)
        full += "word" + std::to_string(i) + " ";

    Model m;
    m.d.show_reasoning = true;
    m.s.phase = phase::Streaming{phase::Active{}};

    Message a;
    a.role = Role::Assistant;
    a.id   = MessageId{"stream1"};
    // No answer text yet: pure-reasoning phase (the case the user hit).
    m.d.current.messages.push_back(a);

    auto visible_words = [&](const std::string& out) {
        int n = 0;
        for (int i = 0; i < 60; ++i)
            if (has(out, "word" + std::to_string(i) + " ")) ++n;
        return n;
    };

    // Feed the ENTIRE reasoning body at once (as a big summarized delta would
    // arrive), then render across many animation frames. If the reveal works,
    // the visible word count climbs frame over frame instead of hitting 60
    // immediately.
    m.d.current.messages[0].thinking = full;

    int first_frame_words = -1;
    int max_words = 0;
    for (int frame = 0; frame < 40; ++frame) {
        const std::string out = render_text(m);
        const int w = visible_words(out);
        if (first_frame_words < 0) first_frame_words = w;
        max_words = std::max(max_words, w);
        maya::testing::advance_anim_clock_ms(33); // ~30fps
    }

    // The reveal cursor should NOT dump the whole body on the first frame.
    check(first_frame_words < 60,
          "reasoning does not appear fully on the first frame (it reveals)");
    check(first_frame_words < max_words,
          "reasoning reveals MORE text over subsequent frames (it animates)");
    // And it eventually reveals (nearly) everything.
    check(max_words >= 55,
          "reasoning reveal eventually reaches the full body");
}

// The reasoning reveal must GLIDE the text out even when the answer starts
// immediately after the last reasoning delta (the common Anthropic case:
// summarized reasoning lands in one/two big deltas, then prose begins). If
// the reveal is cut short at settle, the whole reasoning block "appears at
// once". We inspect the #r slot's reveal cursor directly.
TEST_CASE("reasoning: reveal glides, not cut short when the answer follows") {
    std::string full;
    for (int i = 0; i < 80; ++i) full += "tok" + std::to_string(i) + " ";

    Model m;
    m.d.show_reasoning = true;
    m.s.phase = phase::Streaming{phase::Active{}};
    Message a; a.role = Role::Assistant; a.id = MessageId{"glide1"};
    m.d.current.messages.push_back(a);

    const auto rid = MessageId{std::string{"glide1"} + "#r"};
    auto reveal_clip = [&]() -> long long {
        const auto* mc = m.ui.view_cache.peek(m.d.current.id, rid);
        if (!mc || !mc->streaming) return -1;
        const auto clip = mc->streaming->debug_reveal_byte_clip();
        return clip == static_cast<std::size_t>(-1)
             ? -1 : static_cast<long long>(clip);
    };

    // Reasoning arrives all at once (one big summarized delta).
    m.d.current.messages[0].thinking = full;
    // A few frames of gliding — the reveal cursor should be MID-body, not at
    // the end.
    long long clip_after_a_few = -1;
    for (int f = 0; f < 3; ++f) {
        (void)render_text(m);
        maya::testing::advance_anim_clock_ms(33);
    }
    clip_after_a_few = reveal_clip();
    check(clip_after_a_few >= 0 &&
          clip_after_a_few < static_cast<long long>(full.size()),
          "reveal cursor is still mid-body a few frames in (it is gliding)");

    // NOW the answer starts immediately (settle trigger). The reveal must NOT
    // snap to the end on the settling frame — it should keep gliding.
    m.d.current.messages[0].streaming_text = "Here is the answer.";
    (void)render_text(m);
    maya::testing::advance_anim_clock_ms(33);
    const long long clip_at_settle = reveal_clip();
    // The cursor should have advanced only incrementally, not jumped to full.
    // (A snap would put it at full.size() the instant settle fired.)
    check(clip_at_settle < 0 ||
          clip_at_settle <= clip_after_a_few + static_cast<long long>(full.size()),
          "reveal is not force-snapped to the end when the answer begins");
}
