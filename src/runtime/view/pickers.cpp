#include "moha/runtime/view/pickers.hpp"

#include <vector>

#include <maya/widget/plan_view.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element model_picker(const Model& m) {
    auto* picker = pick::opened(m.ui.model_picker);
    // Zero-row when not active.  The previous `text("")` left a 1-row
    // blank in the parent's region — when the modal dismissed, that
    // ghost row briefly held stale prior content until the next full
    // layout pass.  `nothing()` is the canonical zero-height placeholder.
    if (!picker) return nothing();
    std::vector<Element> rows;
    if (m.d.available_models.empty()) {
        rows.push_back(text("  Loading models\u2026", fg_italic(muted)));
    }
    int i = 0;
    for (const auto& mi : m.d.available_models) {
        bool sel    = i == picker->index;
        bool active = mi.id == m.d.model_id;
        auto prefix = sel ? text("\u203A ", fg_bold(accent)) : text("  ");
        auto star   = mi.favorite ? text("\u2605 ", fg_of(warn)) : text("  ");
        auto active_mark = active ? text(" \u2713", fg_of(success)) : text("");
        rows.push_back(h(prefix, star,
            text(mi.display_name,
                 sel ? fg_bold(fg) : fg_of(muted)),
            active_mark).build());
        ++i;
    }
    rows.push_back(text(""));
    rows.push_back(h(
        text("\u2191\u2193", fg_of(fg)), text(" move  ", fg_dim(muted)),
        text("Enter", fg_of(fg)), text(" select  ", fg_dim(muted)),
        text("F", fg_of(fg)), text(" favorite  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());
    auto content = (v(std::move(rows)) | padding(1, 2) | width(50));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(accent)
            | btext(" Models ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

Element thread_list(const Model& m) {
    auto* picker = pick::opened(m.ui.thread_list);
    if (!picker) return nothing();
    std::vector<Element> rows;
    if (m.d.threads.empty()) {
        rows.push_back(text("  No threads yet.", fg_italic(muted)));
    }
    int i = 0;
    for (const auto& t : m.d.threads) {
        bool sel = i == picker->index;
        auto prefix = sel ? text("\u203A ", fg_bold(info)) : text("  ");
        rows.push_back(h(prefix,
            text(t.title.empty() ? "(untitled)" : t.title,
                 sel ? fg_of(fg) : fg_of(muted)),
            spacer(),
            text(timestamp_hh_mm(t.updated_at), fg_dim(muted))
        ).build());
        if (++i > 15) break;
    }
    rows.push_back(text(""));
    rows.push_back(h(
        text("\u2191\u2193", fg_of(fg)), text(" move  ", fg_dim(muted)),
        text("Enter", fg_of(fg)), text(" open  ", fg_dim(muted)),
        text("N", fg_of(fg)), text(" new  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());
    auto content = (v(std::move(rows)) | padding(1, 2) | width(60));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(info)
            | btext(" Threads ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

Element command_palette(const Model& m) {
    auto* o = opened(m.ui.command_palette);
    if (!o) return nothing();

    std::vector<Element> rows;
    rows.push_back(h(text("\u203A ", fg_bold(highlight)),
        text(o->query.empty() ? "type to filter\u2026" : o->query,
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    rows.push_back(sep);

    // Same filter the dispatcher uses (case-insensitive substring) — see
    // command_palette.hpp. Sharing one helper means the row at visible
    // index N here is *exactly* the row CommandPaletteSelect resolves;
    // we used to filter independently and the dispatcher fired the wrong
    // command whenever any query was active.
    auto matches = filtered_commands(o->query);
    if (matches.empty()) {
        rows.push_back(text("  no matches", fg_italic(muted)));
    } else {
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];
            bool sel = i == o->index;
            auto prefix = sel ? text("\u203A ", fg_bold(highlight)) : text("  ");
            rows.push_back(h(prefix,
                text(std::string{cmd.label}, sel ? fg_bold(fg) : fg_of(muted)),
                spacer(),
                text(std::string{cmd.description}, fg_dim(muted))).build());
        }
    }

    auto content = (v(std::move(rows)) | padding(1, 2) | width(70));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(highlight)
            | btext(" Command Palette ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

Element todo_modal(const Model& m) {
    if (!pick::is_open(m.ui.todo.open)) return nothing();

    std::vector<Element> rows;

    if (m.ui.todo.items.empty()) {
        rows.push_back(text("  No tasks yet.", fg_italic(muted)));
        rows.push_back(text("  The agent will create tasks as it works.", fg_dim(muted)));
    } else {
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
        rows.push_back(plan.build());

        int total = static_cast<int>(m.ui.todo.items.size());
        int done_count = 0;
        for (const auto& item : m.ui.todo.items)
            if (item.status == TodoStatus::Completed) ++done_count;
        rows.push_back(text(""));
        rows.push_back(h(
            text("  " + std::to_string(done_count) + "/" + std::to_string(total),
                 fg_bold(done_count == total ? success : info)),
            text(" completed", fg_dim(muted))
        ).build());
    }

    rows.push_back(text(""));
    rows.push_back(h(
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    auto content = (v(std::move(rows)) | padding(1, 2) | width(60));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(info)
            | btext(" Plan ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

} // namespace moha::ui
