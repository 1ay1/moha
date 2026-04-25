#pragma once
#include <maya/maya.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element thread_panel(const Model& m);

// Lower-level helpers (exposed for testability and reuse).
[[nodiscard]] maya::Element render_message(const Message& msg, std::size_t msg_idx,
                                           int turn_num, const Model& m);
[[nodiscard]] maya::Element render_tool_call(const ToolUse& tc);

// Inline-timeline body for one tool event — the compact rendering placed
// under the timeline event's `│` connector. Lives in the same TU as
// `render_tool_call` (src/runtime/view/tool_card.cpp) so per-tool
// rendering is single-source-of-truth: the rich card and the compact
// body for any given tool sit next to each other, not in separate files.
[[nodiscard]] maya::Element render_tool_compact(const ToolUse& tc);

} // namespace moha::ui
