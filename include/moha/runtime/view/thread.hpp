#pragma once
#include <maya/maya.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element thread_panel(const Model& m);

// Lower-level helpers (exposed for testability and reuse).
[[nodiscard]] maya::Element render_message(const Message& msg, std::size_t msg_idx,
                                           int turn_num, const Model& m);
[[nodiscard]] maya::Element render_tool_call(const ToolUse& tc);

} // namespace moha::ui
