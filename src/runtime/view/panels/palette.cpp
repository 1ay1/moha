// palette.cpp — the command palette (^K) + the ONE palette_context().
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {

Element palette_panel(const Model& m) {
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
