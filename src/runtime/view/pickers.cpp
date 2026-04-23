#include "moha/runtime/view/pickers.hpp"

#include <vector>

#include <maya/widget/plan_view.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element model_picker(const Model& m) {
    if (!m.ui.model_picker.open) return text("");
    std::vector<Element> rows;
    if (m.d.available_models.empty()) {
        rows.push_back(text("  Loading models\u2026", fg_italic(muted)));
    }
    int i = 0;
    for (const auto& mi : m.d.available_models) {
        bool sel    = i == m.ui.model_picker.index;
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
    if (!m.ui.thread_list.open) return text("");
    std::vector<Element> rows;
    if (m.d.threads.empty()) {
        rows.push_back(text("  No threads yet.", fg_italic(muted)));
    }
    int i = 0;
    for (const auto& t : m.d.threads) {
        bool sel = i == m.ui.thread_list.index;
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
    if (!m.ui.command_palette.open) return text("");

    std::vector<Element> rows;
    rows.push_back(h(text("\u203A ", fg_bold(highlight)),
        text(m.ui.command_palette.query.empty() ? "type to filter\u2026"
                                              : m.ui.command_palette.query,
             m.ui.command_palette.query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    rows.push_back(sep);

    int i = 0;
    for (const auto& cmd : kCommands) {
        std::string_view name{cmd.label};
        std::string_view desc{cmd.description};
        if (!m.ui.command_palette.query.empty()
            && name.find(m.ui.command_palette.query) == std::string_view::npos)
            continue;
        bool sel = i == m.ui.command_palette.index;
        auto prefix = sel ? text("\u203A ", fg_bold(highlight)) : text("  ");
        rows.push_back(h(prefix,
            text(std::string{name}, sel ? fg_bold(fg) : fg_of(muted)),
            spacer(),
            text(std::string{desc}, fg_dim(muted))).build());
        ++i;
    }
    if (i == 0) {
        rows.push_back(text("  no matches", fg_italic(muted)));
    }

    auto content = (v(std::move(rows)) | padding(1, 2) | width(70));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(highlight)
            | btext(" Command Palette ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

Element todo_modal(const Model& m) {
    if (!m.ui.todo.open) return text("");

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
