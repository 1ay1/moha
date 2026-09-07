// tool_pickers.cpp — tool & code overlays: the code-block picker (^G), the
// post-run code-block result card, and the tool-output viewer. Split out of
// the former monolithic pickers.cpp; shared scaffolding lives in
// pickers_prologue.hpp / pickers_common.hpp.
//
// Pure adapter: builds maya::Panel::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements and the typed cursor index.

#include "pickers_prologue.hpp"

namespace agentty::ui {

// Ctrl+G — code blocks from the newest assistant reply. Row = ①-style
// index + language tag + first-line preview + line count. The digit
// shortcut in the key handler maps 1-based onto these rows, so the
// leading number is the affordance that teaches the fast path.
Element code_block_picker(const Model& m) {
    auto* o = m.ui.overlay.get<ov::CodeBlocks>();
    if (!o) return nothing();

    Panel::Config cfg;
    cfg.title      = " Run Code Block ";
    cfg.accent     = success;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.code_blocks_scroll;
    cfg.selected   = o->blocks.empty() ? -1 : o->index;

    cfg.items.reserve(o->blocks.size());
    // First pass width: the badge column is padded to a common width so the
    // preview text of every row starts at the same column (a ▶ run badge and
    // a `python` tag badge would otherwise misalign). Mirrors the tool
    // output viewer's badge alignment.
    int badge_w = 0;
    for (const auto& b : o->blocks) {
        const bool runnable = code_block_picker::is_shell_language(b.language);
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
        const bool runnable = code_block_picker::is_shell_language(b.language);
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
    auto* r = m.ui.overlay.get<ov::CodeBlockResult>();
    if (!r) return nothing();

    const bool ok_exit = !r->timed_out && r->exit_code == 0;

    Panel::Config cfg;
    cfg.title      = " Run Result ";
    cfg.accent     = ok_exit ? success : danger;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = picker_viewport_h();
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
Element tool_output_viewer(const Model& m) {
    const auto* o = m.ui.overlay.get<ov::ToolViewer>();
    if (!o) return nothing();

    const int sz = static_cast<int>(o->entries.size());
    const int cur = std::clamp(o->index, 0, std::max(0, sz - 1));

    Panel::Config cfg;
    // Deliberately NOT one of the shared kPanel* floors. Overlay stretch
    // supplies all available columns, and a large minimum used to force the
    // picker past phone/SSH terminal bounds and clip its right side; keep only
    // the border's structural floor and let row flex do the rest. Captured
    // output is arbitrary-width content, so "how narrow may this get" is
    // answered by the terminal, not by the shape of a label column.
    cfg.min_width  = 1;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.tool_viewer_scroll;

    // "n/N" position — shown in both stages so the user always knows
    // where they are in the output stack.
    const std::string pos =
        std::to_string(cur + 1) + "/" + std::to_string(sz);

    if (!o->viewing) {
        // ── LIST stage ──
        cfg.title    = " Tool Outputs \xc2\xb7 " + std::to_string(sz) + " ";
        cfg.accent   = highlight;
        cfg.selected = sz == 0 ? -1 : cur;
        // LIST chrome is border+padding (4) plus blank+hint footer (2).
        // At truly tiny heights, drop the optional footer and devote every
        // remaining row to selectable outputs.
        const int list_term_rows = picker_terminal_rows();
        const bool compact_list = list_term_rows < 7;
        cfg.viewport_h = std::clamp(
            list_term_rows - (compact_list ? 4 : 6), 1, kViewportH);

        // Badge column width: longest display name across the entries,
        // so every detail line starts at the same column and the badge
        // hues read as a vertical colour strip.
        int badge_w = 0;
        for (const auto& e : o->entries)
            badge_w = std::max(badge_w, string_width(e.title));

        cfg.items.reserve(o->entries.size());
        for (int i = 0; i < sz; ++i) {
            const auto& e = o->entries[static_cast<std::size_t>(i)];
            Panel::Item row;
            const Color cat_hue = tool_category_color(e.name);
            if (e.is_live) {
                // The currently-running tool, pinned to the top. A bright
                // "● LIVE" badge in the tool's hue reads as "streaming now";
                // the detail line carries the tool name so the row is still
                // self-describing.
                std::string badge = "\xe2\x97\x8f LIVE";
                const int live_w = string_width(badge);
                if (live_w < badge_w)
                    badge.append(static_cast<std::size_t>(badge_w - live_w), ' ');
                row.badge          = std::move(badge);
                row.badge_style    = fg_bold(cat_hue);
                row.leading        = e.title + (e.detail.empty() ? "" : "  " + e.detail);
                row.leading_style  = fg_of(fg);
                row.trailing       = e.trailing;
                row.trailing_style = fg_dim(cat_hue);
                cfg.items.push_back(std::move(row));
                continue;
            }
            row.badge = e.title;
            const int title_w = string_width(e.title);
            if (title_w < badge_w)
                row.badge.append(static_cast<std::size_t>(badge_w - title_w), ' ');
            // Category hue — the same colour identity the transcript
            // card used, so "which tool was that?" is answered by hue
            // before the label is even read. Failures go red on the
            // badge too: status outranks category.
            row.badge_style    = e.failed ? fg_bold(danger)
                                          : fg_bold(cat_hue);
            row.leading        = e.detail.empty() ? std::string{"\xe2\x80\xa6"}
                                                  : e.detail;
            row.leading_style  = e.failed ? fg_of(danger) : fg_of(fg);
            row.trailing       = e.trailing;
            row.trailing_style = e.failed ? fg_of(danger) : fg_dim(muted);
            cfg.items.push_back(std::move(row));
        }
        if (!compact_list) {
            cfg.footer.push_back(text(""));
            cfg.footer.push_back(key_hints({
                {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
                {"Enter", "view", 6},
                {"y", "copy", 4},
                {"Esc", "close", 3},
            }));
        }
        return Panel{std::move(cfg)}.build();
    }

    // ── BODY stage ──
    const auto& e = o->entries[static_cast<std::size_t>(cur)];
    const Color tool_hue = e.failed ? danger : tool_category_color(e.name);
    cfg.title    = (e.is_live ? std::string(" \xe2\x97\x8f LIVE \xc2\xb7 ") + e.title
                              : " " + e.title)
                 + " \xc2\xb7 " + pos + " ";
    cfg.accent   = tool_hue;
    cfg.selected = -1;   // read-only — no cursor row; manual scroll rules
    // Normal BODY chrome is 9 rows: border+padding (4), header+separator
    // (2), blank+range+hints footer (3). Below 10 terminal rows there is
    // room for none of that optional chrome, so the border title carries
    // identity and every remaining row goes to output. This keeps the modal
    // inside all viable terminal heights (5+ rows) instead of inheriting the
    // shared picker's four-row viewport floor.
    const int body_term_rows = picker_terminal_rows();
    const bool compact_body = body_term_rows < 10;
    cfg.viewport_h = std::clamp(
        body_term_rows - (compact_body ? 4 : 9), 1, kViewportH);

    // Header: coloured tool name + detail, then the status line — the
    // user never loses track of WHICH output they're reading, and ←/→
    // visibly swaps this header as they hop entries. Full-width hstack
    // so the position indicator pins right even on narrow terminals.
    if (!compact_body) {
        cfg.header.push_back(
            hstack().width(Dimension::percent(100))(
                text(" " + e.title, fg_bold(tool_hue))
                    | clip | shrink(1.0f),
                text(e.detail.empty() ? "" : "  " + e.detail, fg_of(fg))
                    | clip | grow(1.0f) | shrink(3.0f),
                text(e.trailing + " ", e.failed ? fg_of(danger) : fg_dim(muted))
                    | clip | shrink(2.0f)
            ).build());
        cfg.header.push_back(sep);
    }

    // Body rows — built ONCE per viewed entry, then windowed per frame.
    //
    // Why: a 256 KiB output renders to thousands of row Elements. The
    // previous shape rebuilt the full ToolBodyPreview tree (deep-copying
    // the output through its Config) on EVERY key repeat, and handed the
    // whole thing to a scroll container that laid out ALL rows to paint
    // ~20 — the "viewer lags/hangs" report. Now:
    //   * the rows are materialised once into a function-local cache
    //     keyed by (entries identity, index, output bytes identity);
    //   * each frame we copy only the visible [y, y+vh) slice into the
    //     picker — no scroll container, no full-content layout — so a
    //     frame costs O(viewport) regardless of output size;
    //   * scroll bounds are written to the host ScrollState here (the
    //     reducer's clamp reads max_y), replacing the widget writeback
    //     the scroll container used to do.
    //
    // The cache is safe across frames: entries are snapshotted at open
    // (immutable while Open), the vector buffer is stable under Model
    // moves, and the output-bytes key guards against allocator reuse
    // after a close/reopen.
    struct BodyCache {
        const void* entries_key = nullptr;
        int         index       = -1;
        const void* bytes_key   = nullptr;
        std::size_t bytes_len   = 0;
        std::uint64_t call_key  = 0;
        std::vector<Element> rows;
    };
    static BodyCache cache;   // UI thread only — same discipline as pickers’ statics

    const void* entries_key = static_cast<const void*>(o->entries.data());
    const void* bytes_key   = static_cast<const void*>(e.output.data());
    const auto call_key     = e.call.compute_render_key();
    if (cache.entries_key != entries_key || cache.index != cur
        || cache.bytes_key != bytes_key || cache.bytes_len != e.output.size()
        || cache.call_key != call_key) {
        cache.entries_key = entries_key;
        cache.index       = cur;
        cache.bytes_key   = bytes_key;
        cache.bytes_len   = e.output.size();
        cache.call_key    = call_key;
        cache.rows.clear();

        // For a command-running tool (shell / diagnostics / process_*), show
        // the FULL command at the top of the body — the one-line `detail` in
        // the header is clipped, so a long or multi-line command was otherwise
        // unreadable. The command is WRAPPED (not clipped): a long single-line
        // one-liner flows onto as many rows as it needs, indented under the
        // `$ ` gutter. A separator then divides it from the output.
        if (e.call.args.is_object()) {
            std::string command;
            if (auto it = e.call.args.find("command");
                it != e.call.args.end() && it->is_string())
                command = it->get<std::string>();
            if (!command.empty()) {
                const Color cmd_hue = e.failed ? danger : tool_hue;
                // hstack: a fixed 2-col "$ " gutter + the command as a WRAPPING
                // text that takes the remaining width. maya grows the row to
                // however many visual lines the wrap produces; continuation
                // lines sit under the gutter. Embedded newlines wrap too.
                cache.rows.push_back(
                    hstack()(
                        text("$ ", fg_bold(cmd_hue)),
                        maya::Element{maya::TextElement{
                            .content = command,
                            .style   = fg_of(fg),
                            .wrap    = maya::TextWrap::Wrap,
                        }} | grow(1.0f)));
                cache.rows.push_back(sep);
            }
        }

        using Kind = maya::ToolBodyPreview::Kind;
        auto bp = tool_body_preview_config(e.call);
        const bool structured =
            !e.failed && (bp.kind == Kind::EditDiff || bp.kind == Kind::GitDiff
                       || bp.kind == Kind::FileRead || bp.kind == Kind::FileWrite
                       || bp.kind == Kind::TodoList);

        if (structured) {
            bp.show_all   = true;    // no "⋯ N more" elision — full output
            bp.tail_only  = e.is_live;  // live: newest pinned bottom; settled: from top
            bp.show_streaming_placeholder = false;
            Element body = maya::ToolBodyPreview{std::move(bp)}.build();
            // The preview renders as one vstack of row Elements. Explode
            // it so the window slice below can address individual rows;
            // the wrapper vstack carries no styling of its own.
            if (auto* box = maya::as_box(body);
                box && box->layout.direction == maya::FlexDirection::Column
                && !box->children.empty()) {
                cache.rows.reserve(box->children.size());
                for (auto& child : box->children) {
                    // Manual viewport accounting below is row-based. Keep
                    // every structured preview child to exactly one visual
                    // row just like the plain-text fallback; otherwise a
                    // narrow FileWrite/Todo label can wrap while max_y still
                    // counts it as one, making later rows unreachable.
                    cache.rows.push_back(
                        std::move(child)
                        | height(1)
                        | overflow(Overflow::Hidden));
                }
            } else {
                cache.rows.push_back(
                    std::move(body)
                    | height(1)
                    | overflow(Overflow::Hidden));
            }
        } else if (e.output.empty()) {
            cache.rows.push_back(
                text("  (no output captured)", fg_italic(muted))
                | height(1)
                | overflow(Overflow::Hidden));
        } else {
            // Line-numbered fallback: right-aligned gutter + dim pipe in
            // the tool's category hue (red pipe on failure), then the
            // raw line. Every line of every plain-text output numbered.
            const Color pipe_hue = e.failed ? danger : tool_hue;
            std::vector<std::string_view> lines;
            {
                std::string_view b{e.output};
                std::size_t p = 0;
                while (p <= b.size()) {
                    std::size_t nl = b.find('\n', p);
                    std::size_t len = (nl == std::string_view::npos ? b.size() : nl) - p;
                    lines.push_back(b.substr(p, len));
                    if (nl == std::string_view::npos) break;
                    p = nl + 1;
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
                           e.failed ? fg_of(danger) : fg_of(muted))
                          | clip | grow(1.0f) | shrink(1.0f)
                    ).build()
                    | height(1) | overflow(Overflow::Hidden));
            }
        }
    }

    // Window the cached rows to the viewport. No scroll container — the
    // picker paints the slice inline; scroll bounds are maintained here
    // so the reducer's clamp (ToolViewerMove) stays correct.
    const int total_rows = static_cast<int>(cache.rows.size());
    const int vh = std::max(1, cfg.viewport_h);
    auto& sc = m.ui.tool_viewer_scroll;
    sc.max_y = std::max(0, total_rows - vh);
    // Live entry tails by default: pin to the newest output (bottom) unless
    // the user has scrolled up to read earlier lines. `auto_tail` re-engages
    // when they scroll back to the bottom (maintained in the reducer). For a
    // settled entry the saved scroll position rules.
    if (e.is_live && m.ui.tool_viewer_tail)
        sc.y = sc.max_y;
    sc.y     = std::clamp(sc.y, 0, sc.max_y);
    cfg.scroll = nullptr;
    const int first = sc.y;
    const int last  = std::min(total_rows, first + vh);
    for (int i = first; i < last; ++i)
        cfg.prebuilt.push_back(cache.rows[static_cast<std::size_t>(i)]);

    if (total_rows == 0)
        cfg.prebuilt.push_back(
            text("  waiting for output\xe2\x80\xa6", fg_italic(muted))
            | height(1)
            | overflow(Overflow::Hidden));

    if (!compact_body) {
        cfg.footer.push_back(text(""));
        // Position line: which rows of the output are on screen — the manual
        // window has no scrollbar, so this is the scroll affordance.
        if (total_rows > vh) {
            std::string pos_line =
                "  " + std::to_string(first + 1) + "\xe2\x80\x93"      // –
                     + std::to_string(last) + " / "
                     + std::to_string(total_rows) + " rows";
            if (e.is_live && m.ui.tool_viewer_tail)
                pos_line += "  \xc2\xb7 tailing";
            cfg.footer.push_back(text(pos_line,
                fg_dim(e.is_live && m.ui.tool_viewer_tail ? tool_hue : muted)));
        }
        std::vector<Hint> viewer_hints;
        if (e.is_live) {
            viewer_hints = {
                {"\xe2\x86\x91\xe2\x86\x93", "scroll", 5},        // ↑↓
                {"End", "tail", 4},
                {"\xe2\x86\x90\xe2\x86\x92", "prev/next", 4},     // ←→
                {"Esc", "back", 3},
            };
        } else {
            viewer_hints = {
                {"\xe2\x86\x91\xe2\x86\x93", "scroll", 5},        // ↑↓
                {"\xe2\x86\x90\xe2\x86\x92", "prev/next", 4},     // ←→
                {"y", "copy", 4},
                {"Esc", "back", 3},
            };
        }
        cfg.footer.push_back(key_hints(std::move(viewer_hints)));
    }
    return Panel{std::move(cfg)}.build();
}

} // namespace agentty::ui
