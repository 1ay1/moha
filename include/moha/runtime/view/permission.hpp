#pragma once
#include <maya/maya.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Inline permission footer attached underneath the tool call awaiting it.
[[nodiscard]] maya::Element render_inline_permission(const PendingPermission& pp,
                                                     const ToolUse& tc);

[[nodiscard]] maya::Element render_checkpoint_divider();

} // namespace moha::ui
