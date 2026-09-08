// mention.cpp — the @-file palette.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {

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


} // namespace agentty::ui
