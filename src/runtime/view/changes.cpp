#include "moha/runtime/view/changes.hpp"

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

maya::ChangesStrip::Config changes_strip_config(const Model& m) {
    maya::ChangesStrip::Config cfg;
    cfg.border_color = warn;
    cfg.text_color   = fg;
    cfg.accept_color = success;
    cfg.reject_color = danger;
    if (m.d.pending_changes.empty()) return cfg;

    cfg.changes.reserve(m.d.pending_changes.size());
    for (const auto& c : m.d.pending_changes) {
        cfg.changes.push_back({
            .path          = c.path,
            .kind          = c.original_contents.empty()
                               ? maya::FileChangeKind::Created
                               : maya::FileChangeKind::Modified,
            .lines_added   = c.added,
            .lines_removed = c.removed,
        });
    }
    return cfg;
}

} // namespace moha::ui
