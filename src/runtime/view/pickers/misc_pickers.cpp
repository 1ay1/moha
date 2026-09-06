// misc_pickers.cpp — the remaining overlays: the rewind checkpoint picker and
// the todo modal. Split out of the former monolithic pickers.cpp; shared
// scaffolding lives in pickers_prologue.hpp / pickers_common.hpp.
//
// Pure adapter: builds maya::Panel::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements and the typed cursor index.

#include "pickers_prologue.hpp"

namespace agentty::ui {

// Rewind checkpoint picker. One row per checkpointed user turn (oldest at
// the top, newest at the bottom nearest the composer — same spatial order
// as the transcript). Each row: turn number + one-line prompt preview
// (leading), and "Nm ago · diffstat" (trailing) where the diffstat shows
// what the worktree has changed SINCE that point so the rewind is never
// blind. Enter rewinds; the destructive files+transcript revert is the
// existing RestoreCheckpoint flow.
Element checkpoint_picker(const Model& m) {
    auto* o = m.ui.overlay.get<ov::Checkpoints>();
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
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.checkpoints_scroll;
    cfg.selected   = o->entries.empty() ? -1 : o->index;

    cfg.rows.reserve(o->entries.size());
    for (int i = 0; i < static_cast<int>(o->entries.size()); ++i) {
        const auto& e = o->entries[static_cast<std::size_t>(i)];
        Panel::Row row;
        row.leading = "#" + std::to_string(e.turn) + "  " + e.preview;
        row.leading_style = fg_of(fg);

        // Trailing: "<time> · <diffstat>". The diffstat is filled in async;
        // until then a subtle ellipsis so the row doesn't jump.
        std::string when = ago(e.timestamp_ms);
        std::string stat;
        switch (e.diff_state) {
            case checkpoint_picker::Entry::DiffState::Loading:
                stat = "\xe2\x80\xa6";   // …
                break;
            case checkpoint_picker::Entry::DiffState::Failed:
                stat = "";
                break;
            case checkpoint_picker::Entry::DiffState::Ready:
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
            e.diff_state == checkpoint_picker::Entry::DiffState::Ready && !e.clean;
        row.trailing_style = has_changes ? fg_of(success) : fg_dim(muted);
        cfg.rows.push_back(std::move(row));
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

Element todo_modal(const Model& m) {
    if (!pick::is_open(m.ui.todo.open)) return nothing();

    Panel::Config cfg;
    cfg.title      = " Plan ";
    cfg.accent     = info;
    cfg.min_width  = kPanelNarrow;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.todo_scroll;
    // No selection cursor in the todo modal — read-only. Pass -1
    // so the auto-scroll-to-selection is a no-op and the user's
    // manual scroll position is fully respected.
    cfg.selected   = -1;

    if (m.ui.todo.items.empty()) {
        cfg.items.push_back(text("  No tasks yet.", fg_italic(muted)));
        cfg.items.push_back(text("  The agent will create tasks as it works.", fg_dim(muted)));
    } else {
        // PlanView returns one Element with all tasks. It lives in
        // the scrollable region so a long task list pages cleanly
        // when it overflows the viewport.
        maya::PlanView plan;
        for (const auto& item : m.ui.todo.items) {
            maya::TaskStatus ts;
            switch (item.status) {
                case TodoStatus::Pending:    ts = maya::TaskStatus::Pending; break;
                case TodoStatus::InProgress: ts = maya::TaskStatus::InProgress; break;
                case TodoStatus::Completed:  ts = maya::TaskStatus::Completed; break;
            }
            plan.add(item.content, ts);
        }
        cfg.items.push_back(plan.build());

        int total = static_cast<int>(m.ui.todo.items.size());
        int done_count = 0;
        for (const auto& item : m.ui.todo.items)
            if (item.status == TodoStatus::Completed) ++done_count;
        cfg.footer.push_back(text(""));
        cfg.footer.push_back(h(
            text("  " + std::to_string(done_count) + "/" + std::to_string(total),
                 fg_bold(done_count == total ? success : info)),
            text(" completed", fg_dim(muted))
        ).build());
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    return Panel{std::move(cfg)}.build();
}

} // namespace agentty::ui
