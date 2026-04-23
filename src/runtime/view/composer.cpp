#include "moha/runtime/view/composer.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

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

} // namespace

Element composer(const Model& m) {
    const std::string& display = m.ui.composer.text;

    // ── State-driven colors ───────────────────────────────────────────
    // Pro TUIs (Helix / k9s / Lazygit) use restrained color: a single
    // dim/muted border by default, escalating ONLY on real states that
    // demand attention (permission wait → danger). Streaming and
    // "has text" don't change the border — that signal lives elsewhere
    // (activity row, send affordance) and adding more chrome here just
    // makes the input feel toyish and over-styled.
    bool has_text = !display.empty();
    Color prompt_color = m.s.is_awaiting_permission() ? danger : accent;
    Color border_color = m.s.is_awaiting_permission() ? danger : muted;

    // ── Cursor injection ──────────────────────────────────────────────
    // Insert a thin vertical bar at the byte cursor position; this lives
    // inside the text so when we split on '\n' the cursor naturally lands
    // on the right line.
    std::string with_cursor = display;
    int cur = std::min<int>(m.ui.composer.cursor, static_cast<int>(display.size()));
    with_cursor.insert(cur, "\u258E");                                // ▎

    // ── Body: one row per visual line ────────────────────────────────
    // Empty buffer → italic placeholder. Otherwise, split on \n and
    // render each line. The first line gets a plain bold ❯ prompt
    // (no inverse video — that's what made it look like a sticker);
    // continuation lines get a dim ┊ so wrapped input visually attaches
    // to the prompt without screaming.
    auto prompt_chip  = text("\u276F ", fg_bold(prompt_color));       // ❯
    auto continuation = text("\u250a ", fg_dim(muted));               // ┊
    auto blank_pre    = text("  ", {});

    std::vector<Element> body_rows;
    if (!has_text) {
        std::string placeholder = m.s.is_streaming()
            ? "streaming \u2014 type to queue\u2026"
            : m.s.is_awaiting_permission()
              ? "awaiting permission \u2014 respond above\u2026"
              : "type a message\u2026";
        // Cursor inline before the placeholder so the user sees their
        // caret even on an empty buffer. Dim cursor to match placeholder.
        body_rows.push_back(h(
            prompt_chip,
            text("\u258E", fg_dim(muted)),
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
    int row_count = static_cast<int>(body_rows.size());
    int max_rows  = m.ui.composer.expanded ? 16 : 8;
    int min_rows  = 3;
    int rows      = std::clamp(row_count, min_rows, max_rows);

    auto inner = (v(std::move(body_rows)) | padding(0, 1) | height(rows)).build();

    // ── Hint row ──────────────────────────────────────────────────────
    // Helix / Lazygit / k9s style: bold key in default fg, dim label,
    // dim · separators. No inverse video, no per-key color category —
    // that's what made the row read as stickers. Restraint is the look.
    auto kbd = [](const char* k) { return text(k, fg_bold(fg)); };
    auto lbl = [](const char* l) { return text(l, fg_dim(muted)); };
    auto sep = text("  \xc2\xb7  ", fg_dim(muted));                   // ·

    auto hint = h(
        kbd("\xe2\x86\xb5"),         lbl(" send"),                    // ↵
        sep,
        // Show Shift+Enter as the primary; Alt+Enter as the fallback so
        // users on terminals that don't disambiguate Shift+Enter still
        // discover a working binding without having to read docs.
        kbd("\xe2\x87\xa7\xe2\x86\xb5 / \xe2\x8c\xa5\xe2\x86\xb5"),    // ⇧↵ / ⌥↵
                                       lbl(" newline"),
        sep,
        kbd("^E"),                   lbl(" expand"),
        spacer(),
        // Live counters — words / lines / chars. Helps the user keep
        // a feel for long prompts without scrolling. All dim so they
        // don't compete with the input itself.
        text(has_text
                 ? std::to_string(word_count(display)) + " words  \xc2\xb7  "
                   + std::to_string(static_cast<int>(split_lines(display).size())) + " lines  \xc2\xb7  "
                   + std::to_string(display.size()) + " chars"
                 : "",
             fg_dim(muted))
    );

    // ── Title strip ──────────────────────────────────────────────────
    // A small "▎ Compose" leading mark gives the box a labelled feel
    // No "Compose" title chip — pro TUIs trust the layout; the ❯
    // prompt is the affordance. For long buffers we surface the line
    // count as a quiet border-text caption (bottom-right of the box)
    // so the info stays available without becoming a sticker.
    int line_count = static_cast<int>(split_lines(display).size());

    auto box = v(inner, hint.build())
               | border(BorderStyle::Round)
               | bcolor(border_color);
    if (line_count > 1) {
        box = std::move(box) | btext(
            " " + std::to_string(line_count) + " lines ",
            BorderTextPos::Bottom, BorderTextAlign::End);
    }
    return (std::move(box) | grow(1.0f)).build();
}

} // namespace moha::ui
