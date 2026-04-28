#pragma once
#include <maya/widget/permission.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Build the Permission widget config from the pending permission +
// the tool call it's gating. Pure data extraction — no Element work.
[[nodiscard]] maya::Permission::Config inline_permission_config(
    const PendingPermission& pp, const ToolUse& tc);

} // namespace moha::ui
