#include "moha/runtime/view/composer.hpp"

#include <algorithm>
#include <string>

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element composer(const Model& m) {
    std::string display = m.composer.text;
    std::string placeholder;
    if (display.empty()) {
        placeholder = m.stream.is_streaming()
            ? "streaming \u2014 type to queue\u2026"
            : "ask a question, or give an instruction\u2026";
    }

    // Thin vertical bar cursor — less visually noisy than a full block,
    // reads as a caret rather than a character.
    std::string with_cursor = display;
    int cur = std::min<int>(m.composer.cursor, static_cast<int>(display.size()));
    with_cursor.insert(cur, "\u258E");

    int rows = m.composer.expanded ? 8 : 3;

    // Leading ❯ in accent color — same affordance as a shell prompt, makes
    // the input area read as "type here" before the user has to think.
    auto prompt_glyph = m.stream.is_awaiting_permission()
        ? text("\u276F ", fg_bold(danger))
        : text("\u276F ", fg_bold(accent));

    auto body = !display.empty()
        ? text(with_cursor, fg_of(fg))
        : text(placeholder, fg_italic(muted));

    auto inner = v(
        h(prompt_glyph, body)
    ) | padding(0, 1) | height(rows);

    // Only escalate the border on AwaitingPermission — the streaming/working
    // state is already communicated by the activity bar's phase pill, so the
    // composer chrome stays calm.
    auto bdr_color = m.stream.is_awaiting_permission() ? danger : muted;

    auto sep = text(" \u00B7 ", fg_dim(muted));
    auto hint = h(
        text("Enter",     fg_bold(fg)),     text(" send",    fg_dim(muted)),
        sep,
        text("Alt+Enter", fg_bold(fg)),     text(" newline", fg_dim(muted)),
        sep,
        text("Ctrl+E",    fg_bold(fg)),     text(" expand",  fg_dim(muted))
    );

    return (v(inner.build(), hint.build())
            | border(BorderStyle::Round)
            | bcolor(bdr_color)
            | grow(1.0f)
           ).build();
}

} // namespace moha::ui
