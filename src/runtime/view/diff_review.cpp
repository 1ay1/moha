#include "moha/runtime/view/diff_review.hpp"

#include <algorithm>
#include <format>
#include <vector>

#include <maya/widget/diff_view.hpp>

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {
const char* hunk_status_tag(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return "[\u2713 accepted]";
        case Hunk::Status::Rejected: return "[\u2717 rejected]";
        case Hunk::Status::Pending:  return "[ pending ]";
    }
    return "";
}

maya::Color hunk_status_color(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return success;
        case Hunk::Status::Rejected: return danger;
        case Hunk::Status::Pending:  return warn;
    }
    return muted;
}
} // namespace

Element diff_review(const Model& m) {
    if (!m.ui.diff_review.open || m.d.pending_changes.empty()) return text("");
    const auto idx = std::min<int>(m.ui.diff_review.file_index,
                                   static_cast<int>(m.d.pending_changes.size()) - 1);
    const auto& fc = m.d.pending_changes[idx];

    std::vector<Element> rows;
    rows.push_back(h(
        text(fc.path, fg_bold(fg)),
        spacer(),
        text(std::format("+{}", fc.added), fg_of(success)),
        text(" "),
        text(std::format("-{}", fc.removed), fg_of(danger)),
        text("  "),
        text(std::format("file {}/{}", m.ui.diff_review.file_index + 1,
                         m.d.pending_changes.size()), fg_dim(muted))
    ).build());
    rows.push_back(sep);

    int hi = 0;
    for (const auto& h_ : fc.hunks) {
        bool sel = hi == m.ui.diff_review.hunk_index;
        rows.push_back(h(
            sel ? text("\u203A ", fg_bold(accent)) : text("  "),
            text(std::format("@@ -{},{} +{},{}", h_.old_start, h_.old_len,
                             h_.new_start, h_.new_len), fg_of(muted)),
            text("  "),
            text(hunk_status_tag(h_.status), fg_of(hunk_status_color(h_.status)))
        ).build());
        DiffView dv(fc.path, h_.patch);
        rows.push_back((v(dv.build()) | padding(0, 0, 0, 2)).build());
        ++hi;
    }
    rows.push_back(sep);
    rows.push_back(h(
        text("\u2191\u2193", fg_of(fg)), text(" hunk  ", fg_dim(muted)),
        text("\u2190\u2192", fg_of(fg)), text(" file  ", fg_dim(muted)),
        text("Y", fg_of(success)), text(" accept  ", fg_dim(muted)),
        text("N", fg_of(danger)), text(" reject  ", fg_dim(muted)),
        text("A", fg_of(success)), text(" all  ", fg_dim(muted)),
        text("X", fg_of(danger)), text(" none  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());
    auto content = (v(std::move(rows)) | padding(1, 2));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(muted)
            | btext(" Review Changes ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

} // namespace moha::ui
