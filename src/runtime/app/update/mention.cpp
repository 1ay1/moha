// mention_update — reducer for `msg::MentionPaletteMsg`. The @file
// picker captures a snapshot of the workspace's files at open time and
// then filters that snapshot per-keystroke. On select, an Attachment
// of kind FileRef is appended to composer.attachments and the inline
// SOH placeholder is inserted at the cursor; the file's bytes are
// loaded later, at submit time (modal.cpp), so an edited file is read
// with its latest contents.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/panel/mention.hpp"
#include "agentty/workspace/files.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Step mention_update(Model m, msg::MentionPaletteMsg mm) {
    return std::visit(overload{
        [&](OpenMentionPalette) -> Step {
            // Capture the workspace listing once on open. Re-walking
            // disk on every keystroke would dominate frame cost on
            // larger repos and produce visible lag in the picker.
            mention::Open o;
            o.files = list_workspace_files();
            m.ui.panel = pn::Mention{std::move(o)};
            return done(std::move(m));
        },
        [&](CloseMentionPalette) -> Step {
            m.ui.panel.close<pn::Mention>();
            return done(std::move(m));
        },
        [&](MentionPaletteInput& e) -> Step {
            auto* o = m.ui.panel.get<pn::Mention>();
            if (o && static_cast<uint32_t>(e.ch) < 0x80
                  && e.ch >= 0x20) {
                // Picker opened cold (index still warming) → empty snapshot.
                // Fill it the moment the background walk has published, so
                // typing progressively reveals results without a blocking
                // open. One-shot: once files are in, this is a no-op.
                if (o->files.empty() && files_ready())
                    o->files = list_workspace_files();
                o->query.push_back(static_cast<char>(e.ch));
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](MentionPaletteBackspace) -> Step {
            auto* o = m.ui.panel.get<pn::Mention>();
            if (!o) return done(std::move(m));
            if (o->query.empty()) {
                // Backspace on empty query closes the picker — same
                // affordance as command palette + most chat apps.
                m.ui.panel.close<pn::Mention>();
                return done(std::move(m));
            }
            o->query.pop_back();
            o->index = 0;
            return done(std::move(m));
        },
        [&](MentionPaletteMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::Mention>();
            if (!o) return done(std::move(m));
            int sz = static_cast<int>(mention_filtered(*o).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](MentionPaletteSelect) -> Step {
            auto* o = m.ui.panel.get<pn::Mention>();
            if (!o) return done(std::move(m));
            const auto& matches = mention_filtered(*o);
            if (matches.empty()
                || o->index < 0
                || o->index >= static_cast<int>(matches.size())) {
                m.ui.panel.close<pn::Mention>();
                return done(std::move(m));
            }
            std::string path = std::move(o->files[matches[
                static_cast<std::size_t>(o->index)]]);
            m.ui.panel.close<pn::Mention>();

            // Frecency: this path just got referenced — rank it near the
            // top of the next `@`. Cheap, bounded recency window.
            note_file_referenced(path);

            // Append a FileRef attachment + insert its placeholder at
            // the composer cursor. Body is filled at submit time
            // (modal.cpp) so a file edited between selection and send
            // reaches the model with its current bytes.
            Attachment att;
            att.kind = Attachment::Kind::FileRef;
            att.path = std::move(path);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));

            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            return done(std::move(m));
        },
    }, mm);
}

} // namespace agentty::app::detail
