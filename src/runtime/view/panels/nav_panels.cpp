// nav_panels.cpp — navigation & command overlays: the thread list, the
// Smart Mode config overlay, the command palette (^K), the @-mention file
// palette, and the symbol palette. Split out of the former monolithic
// panels.cpp; shared scaffolding lives in panels_prologue.hpp /
// panels_common.hpp.
//
// Pure adapter: builds maya::Panel::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements and the typed cursor index.

#include "panels_prologue.hpp"

#include "agentty/runtime/view/form_panel.hpp"
#include <maya/widget/panel.hpp>

namespace agentty::ui {

Element thread_list(const Model& m) {
    auto* picker = m.ui.panel.get<pn::ThreadList>();
    if (!picker) return nothing();

    Panel::Config cfg;
    cfg.title      = " Threads ";
    cfg.accent     = info;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.thread_list_scroll;
    cfg.selected   = picker->index;

    if (m.d.threads.empty()) {
        cfg.prebuilt.push_back(text(
            m.s.threads_loading ? "  Loading conversations…"
                                : "  No threads yet.",
            fg_italic(muted)));
    } else {
        cfg.items.reserve(m.d.threads.size());
        for (const auto& t : m.d.threads) {
            const bool is_current = (t.id == m.d.current.id);
            const bool confirming = (picker->confirm_remove == t.id.value);
            Panel::Item row;
            // "● " marks the thread you're IN — the anchor for both the
            // picker and the ^←→ / Alt+←→ quick-cycle. Non-current rows
            // get a two-space gutter so titles stay column-aligned.
            row.leading        = (is_current ? "\xe2\x97\x8f " : "  ")
                               + (t.title.empty() ? "(untitled)" : t.title);
            // Thread TITLES are what you are choosing between — full
            // foreground. Same hierarchy rule as the model/provider pickers.
            row.leading_style  = is_current ? fg_bold(info) : fg_of(fg);
            if (confirming) {
                row.badge       = "\xe2\x9a\xa0";           // ⚠
                row.badge_style = fg_of(warn);
                row.leading_style = fg_bold(warn);
                row.trailing       = "press d again to confirm";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = timestamp_full(t.updated_at);
                row.trailing_style = fg_dim(muted);
            }
            // The TITLE is what you are choosing; the timestamp is reference
            // data and yields first on a narrow terminal.
            row.trailing_secondary = true;
            cfg.items.push_back(std::move(row));
        }
    }

    cfg.footer.push_back(text(""));
    // Positional readout — same "k/N" the ^←→ / Alt+←→ toast shows, so
    // the two navigation surfaces speak one coordinate system.
    if (!m.d.threads.empty()) {
        cfg.footer.push_back(text(
            "  " + std::to_string(picker->index + 1) + "/"
                + std::to_string(m.d.threads.size()),
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"PgUp/PgDn", "page", 2},
        {"Enter", "open", 5},
        {"N", "new", 3},
        {"D", picker->confirm_remove.empty() ? "remove" : "confirm", 3},
        {"^/Alt+\xe2\x86\x90\xe2\x86\x92", "cycle", 1},   // ^←→ / Alt+←→
        {"Esc", "close", 4},
    }));

    return Panel{std::move(cfg)}.build();
}

// Smart Mode config overlay: a master Enabled toggle + the three role slots,
// each showing its RESOLVED model (pinned, or the auto-fill). See
// docs/design/smart-mode.md.
Element smart_mode_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::SmartMode>();
    if (!o) return nothing();
    // Every glyph belongs to maya::Panel; this host only projects state onto
    // its Config, exactly as the Retrieval pane does.
    return maya::Panel{form_config(o->form, success,
                                  &m.ui.smart_mode_scroll,
                                  panel_viewport_h(),
                                  panel_terminal_cols())}.build();
}

Element command_palette(const Model& m) {
    auto* o = m.ui.panel.get<pn::Palette>();
    if (!o) return nothing();

    // Live visibility context — literally the same function the reducer uses,
    // so a row the dispatcher would reject never renders (no dead Accept-all)
    // and the cursor the view shows indexes the list the reducer resolves.
    const PaletteContext pctx = ui::palette_context(m);
    auto scored = match_commands(o->query, pctx);
    std::vector<const CommandDef*> matches;
    matches.reserve(scored.size());
    for (const auto& s : scored) matches.push_back(s.cmd);

    Panel::Config cfg;
    cfg.title      = " Command Palette ";
    cfg.accent     = highlight;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.command_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(
        o->query.empty()
            ? h(text("\xe2\x8c\x98 ", fg_bold(highlight)),   // ⌘
                query_caret(highlight),
                text("type to filter\xe2\x80\xa6", fg_italic(muted))
              ).build()
            : h(text("\xe2\x8c\x98 ", fg_bold(highlight)),
                text(o->query, fg_of(fg)),
                query_caret(highlight)
              ).build());
    // Rule under the filter — the same `sep` every other picker draws, so the
    // query box is separated from its results identically everywhere.
    cfg.header.push_back(sep);

    // Each category owns a hue so the flat list reads as coloured bands; the
    // badge keeps its hue on the selected row (Picker contract), so the
    // grouping survives the cursor. General rows carry no badge (Quit/Update
    // don't need a section chip).
    auto category_hue = [](Category c) -> Color {
        switch (c) {
            case Category::Thread:   return info;
            case Category::Changes:  return success;
            case Category::Navigate: return highlight;
            case Category::Config:   return warn;
            case Category::Account:  return muted;
            case Category::General:  return muted;
        }
        return muted;
    };

    if (matches.empty()) {
        cfg.prebuilt.push_back(text(
            o->query.empty() ? "  no commands available"
                             : "  no command matches \"" + o->query + "\"",
            fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size() + 6);
        // On the EMPTY query we render real SECTION HEADERS between category
        // groups — true nesting, VS Code / Raycast style. The moment the user
        // types, headers vanish and the list goes flat-with-ranking (empty
        // categories would be noise, and label-hit ranking reorders anyway).
        // Headers are non-selectable rows; the cursor (o->index) indexes the
        // header-FREE `matches`, so we only set Item::selected on real rows and
        // point cfg.selected at the header-adjusted display position — the
        // reducer/dispatch stay entirely header-unaware.
        const bool show_headers = o->query.empty();
        Category   last_cat     = Category::General;
        bool       first_group  = true;
        int        display_row  = 0;   // position INCLUDING headers, for scroll
        int        sel_display  = -1;

        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];

            const bool group_start =
                show_headers && (first_group || cmd.category != last_cat);
            if (group_start) {
                if (auto lab = category_label(cmd.category); !lab.empty()) {
                    // The SAME helper every other grouped picker uses. Hand-
                    // rolling a header here is how the palette ended up with
                    // its own bracket-and-spine grouping while the model
                    // picker had plain rules — two designs for one idea.
                    cfg.items.push_back(section_header(
                        SectionHeader{std::string{lab}, category_hue(cmd.category)}));
                    ++display_row;
                }
                last_cat    = cmd.category;
                first_group = false;
            }

            Panel::Item row;

            // ── Label, with live toggle/mode state folded in ──
            std::string label{cmd.label};
            if (cmd.id == Command::SmartMode)
                label += m.d.smart.enabled ? "  (on)" : "  (off)";
            else if (cmd.id == Command::ToggleChangesStrip)
                label += m.d.show_changes_strip ? "  (shown)" : "  (hidden)";
            row.leading = std::move(label);
            row.leading_style = cmd.danger ? fg_of(danger) : fg_of(fg);
            // Highlight the fuzzy-matched characters (Raycast-style) so the
            // ranking is legible: with "re" typed, the "Re" in Review/Reject
            // lights up. Positions came from the scored matcher.
            if (!o->query.empty()) {
                row.highlight    = scored[static_cast<std::size_t>(i)].positions;
                row.highlight_fg = cmd.danger ? danger : highlight;
            }

            // ── Description UNDER the focused row (Item::help), rag-style,
            // not crammed into the same line. Prose belongs below — it gets
            // the panel's full width and only renders where the cursor is.
            // Trailing keeps only the SHORTCUT: short reference data that
            // survives a narrow (phone/SSH) terminal, marked SECONDARY so it
            // yields before the label does.
            row.help = cmd.description;
            if (cmd.shortcut && *cmd.shortcut) {
                row.trailing           = cmd.shortcut;
                row.trailing_style     = fg_dim(muted);
                row.trailing_secondary = true;
            }
            if (i == o->index) sel_display = display_row;
            cfg.items.push_back(std::move(row));
            ++display_row;
        }
        // Scroll tracks the header-adjusted cursor position.
        cfg.selected = sel_display;
    }

    cfg.footer.push_back(text(""));
    // Count anchor when the list is scrolled — same grammar as the @ / # pickers.
    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()) + " commands",
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"type", "filter", 3},
        {"Enter", "run", 6},
        {"Esc", "close", 3},
    }));

    return Panel{std::move(cfg)}.build();
}

Element mention(const Model& m) {
    auto* o = m.ui.panel.get<pn::Mention>();
    if (!o) return nothing();

    const auto& matches = mention_filtered(*o);

    Panel::Config cfg;
    cfg.title      = " Mention File ";
    cfg.accent     = info;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.mention_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("@", fg_bold(info)),
        text(o->query.empty() ? " your changed files first · type to filter…"
                              : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->files.empty()) {
        // Distinguish "still indexing" from "genuinely empty" — the walk
        // runs on a background thread; if it hasn't landed the picker
        // opened with an empty snapshot. files_ready() tells them apart.
        cfg.prebuilt.push_back(text(
            files_ready() ? "  workspace empty (or no readable files)"
                          : "  indexing workspace… (type to filter as it fills)",
            fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.prebuilt.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& path = o->files[matches[static_cast<std::size_t>(i)]];
            auto [name, dir] = split_name_dir(path);
            Panel::Item row;
            // Git-status badge — the working-set signal, colour-coded so the
            // file you're editing is unmistakable at a glance. Padded to a
            // fixed width so leading text aligns across rows.
            if (auto tag = file_git_tag(path); tag != GitTag::None) {
                auto label = git_tag_label(tag);
                row.badge = "● " + std::string{label};
                row.badge_style =
                    tag == GitTag::Modified          ? fg_of(maya::Color::yellow())
                  : tag == GitTag::Staged            ? fg_of(maya::Color::green())
                  : tag == GitTag::Untracked         ? fg_of(info)
                  : /* RecentlyCommitted */            fg_dim(muted);
            }
            row.leading        = std::string{name};
            row.leading_style  = fg_of(fg);
            // Light up the matched characters of the filename so the fuzzy
            // rank is legible (re-score the name in-view against the query;
            // the workspace scorer ranks but doesn't return positions).
            if (!o->query.empty()) {
                auto fm = fuzzy::score(name, o->query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            cfg.items.push_back(std::move(row));
        }
    }

    // Position indicator: still useful as a textual N/total anchor even
    // though the scrollbar shows the same thing visually.
    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Panel{std::move(cfg)}.build();
}

Element symbol(const Model& m) {
    auto* o = m.ui.panel.get<pn::Symbol>();
    if (!o) return nothing();

    const auto& matches = symbol_filtered(*o);

    Panel::Config cfg;
    cfg.title      = " Symbol ";
    cfg.accent     = highlight;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.symbol_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("#", fg_bold(highlight)),
        text(o->query.empty() ? " type to filter symbols…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->entries.empty()) {
        cfg.prebuilt.push_back(text(
            symbols_ready() ? "  no symbols indexed"
                            : "  indexing symbols… (type to filter as it fills)",
            fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.prebuilt.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& sym = o->entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            Panel::Item row;
            // Combine symbol name + locus into the leading cell so a
            // long parent-dir trailing still has room to render; the
            // "name  file:line" pair is what the user is scanning.
            row.leading        = sym.name + "  " + std::string{fname}
                               + ":" + std::to_string(sym.line_number);
            row.leading_style  = fg_of(fg);
            // Highlight the matched chars of the symbol NAME (which is the
            // leading segment, so its offsets map directly onto row.leading).
            if (!o->query.empty()) {
                auto fm = fuzzy::score(sym.name, o->query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            cfg.items.push_back(std::move(row));
        }
    }

    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Panel{std::move(cfg)}.build();
}

// The ONE row-visibility predicate. Declared in view/palette.hpp; called by
// this view to render the list, by the palette reducer to resolve a cursor to
// a command, and by back_to() to restore a cursor after Esc. Those three must
// index the same list — see the header for what goes wrong when they do not.
PaletteContext palette_context(const Model& m) {
    PaletteContext ctx;
    ctx.update_available    = !m.s.update_latest.empty();
    ctx.has_pending_changes = !m.d.pending_changes.empty();
    ctx.has_code_block      = [&] {
        for (auto it = m.d.current.messages.rbegin();
             it != m.d.current.messages.rend(); ++it) {
            if (it->role != Role::Assistant || it->text.empty()) continue;
            if (!code_blocks::extract_code_blocks(it->text).empty())
                return true;
        }
        return false;
    }();
    return ctx;
}

} // namespace agentty::ui
