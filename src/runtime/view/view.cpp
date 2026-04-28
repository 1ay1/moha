#include "moha/runtime/view/view.hpp"

#include <optional>

#include <maya/widget/app_layout.hpp>

#include "moha/runtime/login.hpp"
#include "moha/runtime/view/changes.hpp"
#include "moha/runtime/view/composer.hpp"
#include "moha/runtime/view/diff_review.hpp"
#include "moha/runtime/view/login.hpp"
#include "moha/runtime/view/pickers.hpp"
#include "moha/runtime/view/statusbar.hpp"
#include "moha/runtime/view/thread.hpp"

namespace moha::ui {

namespace {

// Pick the active overlay, if any. Login modal has highest priority —
// auth gates everything else.
std::optional<maya::Element> pick_overlay(const Model& m) {
    if (login::is_open(m.ui.login))        return login_modal(m);
    if (pick::is_open(m.ui.model_picker))  return model_picker(m);
    if (pick::is_open(m.ui.thread_list))   return thread_list(m);
    if (is_open(m.ui.command_palette))     return command_palette(m);
    if (pick::is_open(m.ui.diff_review))   return diff_review(m);
    if (pick::is_open(m.ui.todo.open))     return todo_modal(m);
    return std::nullopt;
}

} // namespace

maya::Element view(const Model& m) {
    return maya::AppLayout{{
        .thread        = thread_config(m),
        .changes_strip = changes_strip_config(m),
        .composer      = composer_config(m),
        .status_bar    = status_bar_config(m),
        .overlay       = pick_overlay(m),
    }}.build();
}

} // namespace moha::ui
