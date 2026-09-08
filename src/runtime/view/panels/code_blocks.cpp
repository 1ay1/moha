// code_blocks.cpp — the ^G code-block picker + the run-result card.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {


// Ctrl+G — code blocks from the newest assistant reply. Row = ①-style
// index + language tag + first-line preview + line count. The digit
// shortcut in the key handler maps 1-based onto these rows, so the
// leading number is the affordance that teaches the fast path.
Element code_blocks(const Model& m) {
    auto* o = m.ui.panel.get<pn::CodeBlocks>();
    if (!o) return nothing();

    Panel::Config cfg;
    cfg.title      = " Run Code Block ";
    cfg.accent     = success;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.code_blocks_scroll;
    cfg.selected   = o->blocks.empty() ? -1 : o->index;

    cfg.items.reserve(o->blocks.size());
    // First pass width: the badge column is padded to a common width so the
    // preview text of every row starts at the same column (a ▶ run badge and
    // a `python` tag badge would otherwise misalign). Mirrors the tool
    // output viewer's badge alignment.
    int badge_w = 0;
    for (const auto& b : o->blocks) {
        const bool runnable = code_blocks::is_shell_language(b.language);
        const std::string lang = b.language.empty() ? std::string{"sh"} : b.language;
        const std::string bd = runnable ? std::string{" \xe2\x96\xb6 "}
                                        : " " + lang + " ";
        badge_w = std::max(badge_w, string_width(bd));
    }
    for (int i = 0; i < static_cast<int>(o->blocks.size()); ++i) {
        const auto& b = o->blocks[static_cast<std::size_t>(i)];
        // First non-blank line of the (cleaned) body as the preview — what
        // the user visually matches against the reply on screen. Skipping
        // leading blanks means a block that opens with a comment/newline
        // still previews its first real command instead of an empty row.
        std::string_view body{b.body};
        while (!body.empty() && (body.front() == '\n' || body.front() == '\r'))
            body.remove_prefix(1);
        auto eol = body.find('\n');
        std::string preview{body.substr(0, eol == std::string_view::npos
                                             ? body.size() : eol)};
        const bool runnable = code_blocks::is_shell_language(b.language);
        const std::string lang = b.language.empty() ? std::string{"sh"}
                                                    : b.language;

        Panel::Item row;
        // Badge = the run affordance, a stable colour anchor (NOT dimmed on
        // the selected row, so "which of these actually runs" reads at a
        // glance): a ` ▶ ` play glyph in the success hue for a runnable
        // block, or the language tag in muted for one we can only edit/copy.
        if (runnable) {
            row.badge       = " \xe2\x96\xb6 ";           // ▶
            row.badge_style = Style{}.with_fg(success).with_bold();
        } else {
            row.badge       = " " + lang + " ";
            row.badge_style = fg_dim(muted);
        }
        if (int bw = string_width(row.badge); bw < badge_w)
            row.badge.append(static_cast<std::size_t>(badge_w - bw), ' ');
        // Leading: the 1-9 fast-path number (the affordance that teaches the
        // shortcut) + the first-line preview.
        row.leading = std::to_string(i + 1) + "  " + preview;
        row.leading_style  = runnable ? fg_of(fg) : fg_dim(muted);
        // Trailing: language · line count. For a runnable block the language
        // moved into meaning (the ▶ badge), so the tag here is just the
        // dialect; for a non-runnable one the badge already carries it, so
        // show only the size to avoid repeating the tag.
        row.trailing = (runnable ? lang + " \xc2\xb7 " : std::string{})
                     + std::to_string(b.line_count)
                     + (b.line_count == 1 ? " line" : " lines");
        row.trailing_style = fg_dim(muted);
        cfg.items.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"Enter/1-9", "run", 3},
        {"e", "edit", 4},
        {"y", "copy", 4},
        {"Esc", "close", 4},
    }));

    return Panel{std::move(cfg)}.build();
}

// Post-run result card. The user already watched the full output live
// on the real terminal (and it remains in native scrollback above the
// TUI); this card shows the summary — command, exit code, size — plus
// the LAST few lines (errors live at the end) and the decision keys.
Element code_block_result_card(const Model& m) {
    auto* r = m.ui.panel.get<pn::CodeBlockResult>();
    if (!r) return nothing();

    const bool ok_exit = !r->timed_out && r->exit_code == 0;

    Panel::Config cfg;
    cfg.title      = " Run Result ";
    cfg.accent     = ok_exit ? success : danger;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.code_blocks_scroll;
    cfg.selected   = -1;   // read-only — no cursor row

    // Header: "$ command" + status line.
    {
        std::string cmd_line = r->command;
        // First line only — a multi-line block reads badly in a header.
        if (auto eol = cmd_line.find('\n'); eol != std::string::npos) {
            cmd_line.resize(eol);
            cmd_line += " \xe2\x80\xa6";   // …
        }
        cfg.header.push_back(h(text("$ ", fg_bold(cfg.accent)),
                               text(std::move(cmd_line), fg_of(fg))).build());
        std::size_t lines = r->output.empty() ? 0 : 1;
        for (char c : r->output) if (c == '\n') ++lines;
        std::string status = r->timed_out
            ? "timed out"
            : "exit " + std::to_string(r->exit_code);
        status += " \xc2\xb7 " + std::to_string(lines) + " lines \xc2\xb7 ";
        status += (r->output.size() >= 1024)
            ? std::to_string(r->output.size() / 1024) + " KB"
            : std::to_string(r->output.size()) + " B";
        cfg.header.push_back(text("  " + status,
            ok_exit ? fg_of(muted) : fg_bold(danger)));
        cfg.header.push_back(sep);
    }

    // Full capture, line-numbered like the tool output viewer: a right-
    // aligned gutter + a status-hued │ pipe (red on a failed/timed-out run,
    // muted otherwise), then the raw line. The numbers make it easy to say
    // "line 42 is where it broke" and match the Ctrl+O viewer so the two
    // output surfaces read the same.
    //
    // Manual windowing (same discipline as tool_output_viewer): the capture
    // is capped at 2 MB — up to tens of thousands of lines — so building an
    // Element per line into a scroll container that lays out ALL of them
    // would hitch on every keypress. Instead materialise the rows ONCE into
    // a static cache keyed by the output bytes, then feed only the visible
    // [y, y+vh) slice to the picker (cfg.scroll = nullptr), so a frame costs
    // O(viewport) regardless of capture size.
    struct ResultBodyCache {
        const void*          bytes_key = nullptr;
        std::size_t          bytes_len = 0;
        bool                 ok        = false;
        std::vector<Element> rows;
    };
    static ResultBodyCache cache;   // UI thread only — same discipline as the pickers' statics

    const void* bytes_key = static_cast<const void*>(r->output.data());
    if (cache.bytes_key != bytes_key || cache.bytes_len != r->output.size()
        || cache.ok != ok_exit) {
        cache.bytes_key = bytes_key;
        cache.bytes_len = r->output.size();
        cache.ok        = ok_exit;
        cache.rows.clear();

        std::string_view out{r->output};
        if (out.empty()) {
            cache.rows.push_back(
                text("  (no output captured)", fg_italic(muted))
                | height(1) | overflow(Overflow::Hidden));
        } else {
            const Color pipe_hue = ok_exit ? muted : danger;
            std::vector<std::string_view> lines;
            {
                std::size_t pos = 0;
                while (pos <= out.size()) {
                    std::size_t eol = out.find('\n', pos);
                    std::size_t len =
                        (eol == std::string_view::npos ? out.size() : eol) - pos;
                    lines.push_back(out.substr(pos, len));
                    if (eol == std::string_view::npos) break;
                    pos = eol + 1;
                }
            }
            const int gutter_w = static_cast<int>(
                std::to_string(std::max<std::size_t>(1, lines.size())).size());
            cache.rows.reserve(lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i) {
                std::string num = std::to_string(i + 1);
                if (static_cast<int>(num.size()) < gutter_w)
                    num.insert(0, gutter_w - num.size(), ' ');
                cache.rows.push_back(
                    hstack().width(Dimension::percent(100))(
                      text("  " + num + " ", fg_dim(warn)),
                      text("\xe2\x94\x82 ", fg_dim(pipe_hue)),   // │
                      text(std::string{lines[i]},
                           ok_exit ? fg_of(muted) : fg_of(fg))
                          | clip | grow(1.0f) | shrink(1.0f)
                    ).build()
                    | height(1) | overflow(Overflow::Hidden));
            }
        }
    }

    // Window the cached rows to the viewport; maintain scroll bounds here so
    // the reducer's clamp stays correct, and feed only the visible slice.
    const int total_rows = static_cast<int>(cache.rows.size());
    const int vh = std::max(1, cfg.viewport_h);
    auto& sc = m.ui.code_blocks_scroll;
    sc.max_y = std::max(0, total_rows - vh);
    sc.y     = std::clamp(sc.y, 0, sc.max_y);
    cfg.scroll = nullptr;
    const int first = sc.y;
    const int last  = std::min(total_rows, first + vh);
    for (int i = first; i < last; ++i)
        cfg.prebuilt.push_back(cache.rows[static_cast<std::size_t>(i)]);

    cfg.footer.push_back(text(""));
    // Scroll-position readout: which lines of the capture are on screen —
    // the manual window has no scrollbar, so this is the affordance that
    // there is more output above/below (same grammar as the tool viewer).
    if (total_rows > vh) {
        cfg.footer.push_back(text(
            "  " + std::to_string(first + 1) + "\xe2\x80\x93"   // –
                 + std::to_string(last) + " / "
                 + std::to_string(total_rows) + " lines",
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"a", "attach to composer", 6},
        {"y", "copy", 4},
        {"Esc", "discard", 4},
    }));

    return Panel{std::move(cfg)}.build();
}

// Ctrl+O tool-output viewer. Two stages inside one Picker chrome:
//
//   LIST  — one row per settled tool call (newest first): a category-
//           coloured tool badge ("Read", "Bash", "Edit" — same hue as
//           the transcript card), the detail line, and "ok · 1.2s ·
//           48 KB" trailing. Enter opens the body.
//   BODY  — the FULL stored output of the selected call in the
//           scrollable region (the timeline card elides long bodies;
//           this is where the elided middle lives). ←/→ hop straight
//           to the previous/next output; Esc returns to the list.
//
// An overlay — not in-place card expansion — because the transcript's
// committed rows are immutable native scrollback; growing a card there
// would rewrite committed rows (HardReset corruption class). The overlay
// paints strictly over the live viewport, same as every other picker.

} // namespace agentty::ui
