#pragma once
#include <cstddef>
#include <maya/widget/turn.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Build the Turn config for one message — header + typed body slots
// (PlainText / MarkdownText / AgentTimeline / Permission / cached
// streaming-markdown Element).
[[nodiscard]] maya::Turn::Config turn_config(const Message& msg,
                                             std::size_t msg_idx,
                                             int turn_num,
                                             const Model& m);

// Build a single Turn config for a run of consecutive assistant
// messages [start_idx, end_idx).  Merges every message's text into
// successive body slots and concatenates every message's tool_calls
// into ONE agent_timeline panel.  This is what the user actually
// sees as "the agent doing things" — three API responses with one
// tool each look like three turns to the model but should look like
// one block of work to the human.
//
// `turn_num` is the GROUP's number (incremented once per group, not
// once per message).  `m` provides the spinner state and per-message
// caches.  Pre-condition: every message in [start_idx, end_idx) has
// role == Role::Assistant.
[[nodiscard]] maya::Turn::Config turn_config_assistant_group(
    std::size_t start_idx,
    std::size_t end_idx,
    int turn_num,
    const Model& m);

} // namespace moha::ui
