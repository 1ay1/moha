#include "moha/runtime/view/view.hpp"

#include "moha/runtime/login.hpp"
#include "moha/runtime/view/changes.hpp"
#include "moha/runtime/view/composer.hpp"
#include "moha/runtime/view/diff_review.hpp"
#include "moha/runtime/view/login.hpp"
#include "moha/runtime/view/pickers.hpp"
#include "moha/runtime/view/statusbar.hpp"
#include "moha/runtime/view/thread.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element view(const Model& m) {
    auto base = (v(
        v(thread_panel(m)) | grow(1.0f),
        changes_strip(m),
        composer(m),
        status_bar(m)
    ) | pad<1> | grow(1.0f)).build();

    Element overlay;
    bool has_overlay = false;

    // Login modal precedes the others — auth is the gating step, no
    // other UI should appear over it.
    if      (login::is_open(m.ui.login))       { overlay = login_modal(m);  has_overlay = true; }
    else if (pick::is_open(m.ui.model_picker)) { overlay = model_picker(m);  has_overlay = true; }
    else if (pick::is_open(m.ui.thread_list))  { overlay = thread_list(m);   has_overlay = true; }
    else if (is_open(m.ui.command_palette))    { overlay = command_palette(m);has_overlay = true; }
    else if (pick::is_open(m.ui.diff_review))  { overlay = diff_review(m);   has_overlay = true; }
    else if (pick::is_open(m.ui.todo.open))    { overlay = todo_modal(m);    has_overlay = true; }

    if (has_overlay)
        return zstack({std::move(base),
            vstack().align_items(Align::Center).justify(Justify::End)(
                vstack().bg(Color::default_color())(std::move(overlay)))});

    return base;
}

} // namespace moha::ui
