#include "moha/runtime/view/composer.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// Cheap line splitter — keeps empty lines (so the cursor on a fresh \n
// renders at column 0 of the next visual row instead of getting lost).
std::vector<std::string_view> split_lines(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.emplace_back(s.data() + start, i - start);
            start = i + 1;
        }
    }
    out.emplace_back(s.data() + start, s.size() - start);
    return out;
}

// Approximate word count (whitespace-separated runs). Matches the user's
// intuition for "how long is this prompt" without parsing markdown.
int word_count(std::string_view s) {
    int n = 0;
    bool in_word = false;
    for (char c : s) {
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in_word) { ++n; in_word = true; }
        else if (ws)         { in_word = false; }
    }
    return n;
}

// Approximate token count using Claude's ~4-bytes-per-token heuristic.
// Good enough for the input box's "how big is this prompt" indicator —
// no need to round-trip through a real tokenizer for live counter UI.
int approx_tokens(std::string_view s) {
    return static_cast<int>((s.size() + 3) / 4);
}

} // namespace

Element composer(const Model& m) {
    const std::string& display = m.ui.composer.text;
    const bool has_text       = !display.empty();
    const bool is_awaiting    = m.s.is_awaiting_permission();
    const bool is_streaming   = m.s.is_streaming();
    const bool is_executing   = m.s.is_executing_tool();
    const bool active         = is_streaming || is_executing;
    const std::size_t queued  = m.ui.composer.queued.size();

    // ── State-driven colors ───────────────────────────────────────────
    // The border + prompt color *is* the input box's state indicator —
    // no extra chrome needed. Same palette the status bar uses, so
    // reading either one tells you what's happening:
    //
    //   awaiting permission → warn  (user must act before more input lands)
    //   streaming/executing → phase color (highlight / success)
    //   idle, has text      → accent (primed; this will fire on Enter)
    //   idle, empty         → muted dim (quiet, waiting)
    //
    // The "ready" treatment when idle+text uses the brand accent — same
    // signal you see on a ❯ prompt that's about to do something. It's
    // the visual analogue of an iOS button lighting up when its action
    // becomes available.
    Color box_color =
        is_awaiting ? warn :
        active      ? phase_color(m.s.phase) :
        has_text    ? accent :
                      muted;
    Color prompt_color = box_color;

    // ── Cursor injection ──────────────────────────────────────────────
    // Insert a thin vertical bar at the byte cursor position; this lives
    // inside the text so when we split on '\n' the cursor naturally lands
    // on the right line.
    std::string with_cursor = display;
    int cur = std::min<int>(m.ui.composer.cursor, static_cast<int>(display.size()));
    with_cursor.insert(cur, "\u258E");                                // ▎

    // ── Body: one row per visual line ────────────────────────────────
    // ❯ prompt for the first line; dim ┊ for continuation rows so
    // wrapped input visually attaches without screaming. The prompt's
    // boldness reflects state: bright on "ready" / "active", dim on
    // empty-idle so the box feels relaxed at rest.
    Style prompt_style = (active || has_text || is_awaiting)
        ? Style{}.with_fg(prompt_color).with_bold()
        : Style{}.with_fg(prompt_color).with_dim();
    auto prompt_chip  = text("\u276F ", prompt_style);                // ❯
    auto continuation = text("\u250a ", fg_dim(muted));               // ┊
    auto blank_pre    = text("  ", {});

    std::vector<Element> body_rows;
    if (!has_text) {
        // Placeholder text reflects the precise state — and the
        // streaming case nudges the user toward the queueing affordance
        // rather than just stating the obvious.
        std::string placeholder =
            is_awaiting  ? "awaiting permission \u2014 respond above\u2026" :
            is_executing ? "running tool \u2014 type to queue\u2026"        :
            is_streaming ? "streaming \u2014 type to queue\u2026"           :
                           "type a message\u2026";
        body_rows.push_back(h(
            prompt_chip,
            text("\u258E", fg_dim(muted)),                            // dim cursor
            text(placeholder, fg_italic(muted))
        ).build());
    } else {
        auto lines = split_lines(with_cursor);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            Element prefix = (i == 0) ? prompt_chip
                                      : (lines.size() > 1 ? continuation : blank_pre);
            body_rows.push_back(h(
                prefix,
                text(std::string{lines[i]}, fg_of(fg))
            ).build());
        }
    }

    // ── Sizing ───────────────────────────────────────────────────────
    // Grow with content but cap at a sensible max. Empty / short input
    // shows 3 rows so the box doesn't feel cramped; long input grows up
    // to `expanded ? 16 : 8` rows. Beyond that the inner area scrolls.
    //
    // While the model is actively streaming we PIN the composer height
    // at its current size so new rows rendered by the assistant above
    // don't cause the composer's top edge to bob up and down each
    // frame. Without this, every time a wrapped line count changed in
    // the thread view, the composer would shift — reads as vertical
    // jitter of the whole input box during streaming.
    int row_count = static_cast<int>(body_rows.size());
    int max_rows  = m.ui.composer.expanded ? 16 : 8;
    int min_rows  = 3;
    // When the model is streaming/executing, pin the composer height
    // to `min_rows` (3) so the box can't bounce as the user types
    // queued messages or as layout re-measures mid-frame. At rest the
    // box still grows with content up to `max_rows`.
    int rows      = active ? min_rows
                           : std::clamp(row_count, min_rows, max_rows);

    auto inner = (v(std::move(body_rows)) | padding(0, 1) | height(rows)).build();

    // ── Hint row ──────────────────────────────────────────────────────
    // Helix / Lazygit / k9s style: bold key in default fg, dim label,
    // dim · separators. No inverse video, no per-key color category —
    // that's what made the row read as stickers.
    //
    // The hint row is also the one place we surface ambient info that
    // the user wants to glance at without looking elsewhere: the
    // current profile (so they know their permission tier), the queued-
    // message count when streaming with input pending, and a quick
    // counter (words / tokens) for long prompts.
    auto kbd = [](const char* k) { return text(k, fg_bold(fg)); };
    auto lbl = [](const char* l) { return text(l, fg_dim(muted)); };
    auto dot = []() { return text("  \xc2\xb7  ", fg_dim(muted)); };   // ·

    // Build the hint row as a ComponentElement so we can measure
    // available width and drop lower-priority items on narrow terminals.
    // Priority order (highest first): send, profile chip, counters,
    // newline, expand. On very narrow widths (< 60 cols) we keep only
    // send + profile; medium (< 90) drops expand; full shows everything.
    //
    // Capture BY VALUE ([=]): kbd/lbl/dot are stateless lambdas
    // (empty closures); the ComponentElement outlives composer()'s
    // stack frame, so a reference capture [&] would dangle when maya
    // re-invokes the render callback on a later frame — visible as
    // the hint row going blank or corrupting intermittently. Copies
    // of stateless lambdas are zero-size and free.
    auto hint_left_builder = [=](int avail_width) {
        std::vector<Element> out;
        out.push_back(kbd("\xe2\x86\xb5"));           // ↵
        out.push_back(lbl(" send"));
        // Show newline + expand hints only when there's room.
        // ~60 cols is the threshold below which the row feels cramped.
        if (avail_width >= 60) {
            out.push_back(dot());
            out.push_back(kbd("\xe2\x87\xa7\xe2\x86\xb5 / \xe2\x8c\xa5\xe2\x86\xb5"));
            out.push_back(lbl(" newline"));
        }
        if (avail_width >= 90) {
            out.push_back(dot());
            out.push_back(kbd("^E"));
            out.push_back(lbl(" expand"));
        }
        return out;
    };

    // ── Right-side ambient indicators ────────────────────────────────
    // Order: queue (only if non-zero), counters (only if has_text),
    // profile (always). Each is dim so it doesn't compete with the
    // input itself; the profile chip uses its color for the leading
    // ▎ rail so the eye lands on the tier at a glance.
    std::vector<Element> hint_right;

    // Queue depth — when streaming and the user has queued messages,
    // show "N queued" so they know their typing is going somewhere
    // useful, not into the void. Bold to draw the eye when present.
    // Count is right-aligned to 2 display columns so 1→9→99 doesn't
    // shove the surrounding chips left/right as messages queue up
    // during a stream (the worst kind of jitter — movement caused by
    // user actions they weren't directly looking at).
    if (queued > 0) {
        hint_right.push_back(text("\xe2\x9d\x9a ", fg_of(highlight)));     // ❚
        hint_right.push_back(text(
            tabular_int(static_cast<int>(queued), 2) + " queued",
            Style{}.with_fg(highlight).with_bold()));
        hint_right.push_back(dot());
    }

    // Live counters — words + tokens. Words for "how long is this
    // prompt" intuition; tokens for "how much budget will this cost"
    // intuition. Skip when empty so the row reads cleanly at rest.
    //
    // Both numbers use a fixed 4-column right-aligned field so the
    // profile chip to the right of them stays pinned as the user types
    // — with variable-width ints the label would bob every keystroke.
    // The "words" / "word" suffix always renders plural ("words") so
    // the label itself doesn't flip width at n==1.
    if (has_text) {
        int words = word_count(display);
        int toks  = approx_tokens(display);
        hint_right.push_back(text(
            tabular_int(words, 4) + " words",
            fg_dim(muted)));
        hint_right.push_back(text("  \xc2\xb7  ", fg_dim(muted)));
        hint_right.push_back(text(
            "~" + tabular_int(toks, 4) + " tok",
            fg_dim(muted)));
        hint_right.push_back(dot());
    }

    // Profile chip — always-on so the user always knows their
    // permission tier. Leading ▎ rail in profile color matches the
    // status bar's design language; the label is small-caps so it
    // reads as a quiet identifier, not free text.
    Color prof_c = profile_color(m.d.profile);
    hint_right.push_back(text("\xe2\x96\x8e", fg_of(prof_c)));            // ▎
    hint_right.push_back(text(" ", {}));
    hint_right.push_back(text(small_caps(profile_label(m.d.profile)),
                              Style{}.with_fg(prof_c).with_bold()));

    // Wrap the hint row in a ComponentElement so we can measure available
    // width and drop lower-priority items on narrow terminals.
    //
    // The lambda captures by VALUE (no `mutable` + move-out of captured
    // state). maya's layout engine may invoke render() more than once
    // per frame (measure + paint); the earlier version moved out of
    // `hint_right` on the first call, so a second call rendered an
    // empty right group — visible as the profile chip / counters
    // flickering in and out of existence during streaming.
    auto hint_element = Element{ComponentElement{
        .render = [hint_left_builder, hint_right](
                      int w, int /*h*/) -> Element {
            auto left = hint_left_builder(w);
            return h(
                h(left),
                spacer(),
                h(hint_right),
                text(" ", {})
            ).build();
        }
    }};

    // ── Box composition ──────────────────────────────────────────────
    // Round border in state color. Bottom-right caption surfaces line
    // count when multi-line (no need to scroll the buffer to see how
    // much you've written). When there's a "Send" affordance, the
    // border title carries it on the bottom edge so the eye picks it
    // up without scanning the hint row.
    int line_count = static_cast<int>(split_lines(display).size());

    auto box = v(inner, std::move(hint_element))
               | border(BorderStyle::Round)
               | bcolor(box_color);

    // Bottom-right caption: line count when wrapped, with a leading
    // glyph for visual rhythm. Stays subtle (it's chrome on chrome).
    if (line_count > 1) {
        box = std::move(box) | btext(
            " " + std::to_string(line_count) + " lines ",
            BorderTextPos::Bottom, BorderTextAlign::End);
    }
    return (std::move(box) | grow(1.0f)).build();
}

} // namespace moha::ui
