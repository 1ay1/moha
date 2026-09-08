// tool_output.cpp — the tool-output viewer (^O): list + body stages.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {

Element tool_output_panel(const Model& m) {
    const auto* o = m.ui.panel.get<pn::ToolOutput>();
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
    cfg.viewport_h = panel_viewport_h();
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
        const int list_term_rows = panel_terminal_rows();
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
                {"^Y", "copy", 4},
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
    const int body_term_rows = panel_terminal_rows();
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
    // so the reducer's clamp (ToolOutputMove) stays correct.
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
                {"^Y", "copy", 4},
                {"Esc", "back", 3},
            };
        }
        cfg.footer.push_back(key_hints(std::move(viewer_hints)));
    }
    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
