// thread_list.cpp — the thread switcher panel (^T): pick, d-to-delete.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

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
    // Armed delete is a MODE: hint it in the note line, exactly like the
    // account list and the settings add-prompt, so every two-step confirm
    // in the app reads the same way.
    if (!picker->confirm_remove.empty())
        cfg.note = "d confirm delete \xc2\xb7 any other key cancels";
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


} // namespace agentty::ui
