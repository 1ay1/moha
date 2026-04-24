#pragma once
#include <maya/maya.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Render the in-app login modal. Returns an empty text element when
// `m.ui.login` is Closed; the caller's overlay logic uses
// `ui::login::is_open` to decide whether to draw it.
[[nodiscard]] maya::Element login_modal(const Model& m);

} // namespace moha::ui
