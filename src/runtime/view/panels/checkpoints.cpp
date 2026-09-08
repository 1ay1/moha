// checkpoints.cpp — the rewind checkpoint picker.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {


// Rewind checkpoint picker. One row per checkpointed user turn (oldest at
// the top, newest at the bottom nearest the composer — same spatial order
// as the transcript). Each row: turn number + one-line prompt preview
// (leading), and "Nm ago · diffstat" (trailing) where the diffstat shows
// what the worktree has changed SINCE that point so the rewind is never
// blind. Enter rewinds; the destructive files+transcript revert is the
// existing RestoreCheckpoint flow.
Element checkpoints_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::Checkpoints>();
    if (!o) return nothing();

    // Relative "time ago" from a wall-clock ms stamp — local to the view;
    // no shared helper exists and the grammar here is picker-specific.
    auto ago = [](std::int64_t ts_ms) -> std::string {
        if (ts_ms <= 0) return {};
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::int64_t s = (now - ts_ms) / 1000;
        if (s < 0)     s = 0;
        if (s < 45)    return "just now";
        if (s < 3600)  return std::to_string(s / 60)   + "m ago";
        if (s < 86400) return std::to_string(s / 3600) + "h ago";
        return std::to_string(s / 86400) + "d ago";
    };

    Panel::Config cfg;
    cfg.title      = " Rewind to Checkpoint ";
    cfg.accent     = warn;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.checkpoints_scroll;
    cfg.selected   = o->entries.empty() ? -1 : o->index;

    cfg.items.reserve(o->entries.size());
    for (int i = 0; i < static_cast<int>(o->entries.size()); ++i) {
        const auto& e = o->entries[static_cast<std::size_t>(i)];
        Panel::Item row;
        row.leading = "#" + std::to_string(e.turn) + "  " + e.preview;
        row.leading_style = fg_of(fg);

        // Trailing: "<time> · <diffstat>". The diffstat is filled in async;
        // until then a subtle ellipsis so the row doesn't jump.
        std::string when = ago(e.timestamp_ms);
        std::string stat;
        switch (e.diff_state) {
            case checkpoints::Entry::DiffState::Loading:
                stat = "\xe2\x80\xa6";   // …
                break;
            case checkpoints::Entry::DiffState::Failed:
                stat = "";
                break;
            case checkpoints::Entry::DiffState::Ready:
                if (e.clean) {
                    stat = "no changes";
                } else {
                    stat = std::to_string(e.files_changed)
                         + (e.files_changed == 1 ? " file" : " files");
                    if (e.insertions > 0) stat += " +" + std::to_string(e.insertions);
                    if (e.deletions  > 0) stat += " \xe2\x88\x92" + std::to_string(e.deletions); // −
                }
                break;
        }
        std::string trailing = when;
        if (!stat.empty())
            trailing += (when.empty() ? "" : " \xc2\xb7 ") + stat;
        row.trailing = std::move(trailing);
        // Green when a real rewind (has changes), dim when clean/no-stat.
        const bool has_changes =
            e.diff_state == checkpoints::Entry::DiffState::Ready && !e.clean;
        row.trailing_style = has_changes ? fg_of(success) : fg_dim(muted);
        cfg.items.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(text(
        "  Restores files and rewinds the transcript here.",
        fg_dim(muted)));
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"Enter", "rewind", 3},
        {"Esc", "cancel", 4},
    }));

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
