#include "moha/runtime/view/changes.hpp"

#include <format>

#include <maya/widget/file_changes.hpp>

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element changes_strip(const Model& m) {
    if (m.pending_changes.empty()) return text("");
    FileChanges fc;
    for (const auto& c : m.pending_changes) {
        auto kind = c.original_contents.empty()
            ? FileChangeKind::Created
            : FileChangeKind::Modified;
        fc.add(c.path, kind, c.added, c.removed);
    }
    auto summary = (v(
        h(text("Changes ", fg_bold(warn)),
          text(std::format("({} files)", m.pending_changes.size()), fg_of(muted)),
          spacer(),
          text("Ctrl+R", fg_of(fg)), text(" review  ", fg_dim(muted)),
          text("A", fg_of(success)), text(" accept  ", fg_dim(muted)),
          text("X", fg_of(danger)), text(" reject", fg_dim(muted))
        ).build(),
        fc.build()
    ) | border(BorderStyle::Round) | bcolor(warn) | padding(0, 1));
    return summary.build();
}

} // namespace moha::ui
