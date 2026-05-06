#include "moha/runtime/view/thread/conversation.hpp"

#include <algorithm>
#include <cstddef>

#include "moha/runtime/view/thread/activity_indicator.hpp"
#include "moha/runtime/view/thread/turn/turn.hpp"

namespace moha::ui {

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // Virtualize: older messages live in the terminal's native scrollback
    // (committed via maya::Cmd::commit_scrollback). Preserve absolute
    // turn numbering by reading the running turn count that maybe_virtualize
    // maintains alongside thread_view_start — O(1), regardless of how
    // many turns the session has accumulated.  Previously we walked
    // messages[0..start) here every frame, which was O(thread_view_start)
    // and grew linearly with the conversation; on a long-running session
    // that became the dominant per-frame view cost.
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1 + m.ui.thread_view_start_turn;

    // Per-message turns.  Each Anthropic API response (one Message in
    // m.d.current.messages) gets its own Turn.  An earlier attempt
    // fused consecutive assistant messages into one Turn for visual
    // density — the trade-off was non-determinism mid-stream: while
    // the group was still extending, every frame produced a different
    // body slot count and a different total height.  Maya's render
    // guarantees are about cell-level correctness given a deterministic
    // tree; they can't undo application-level layout drift.  Rendering
    // each message as its own Turn keeps every frame's tree shape
    // stable for any given message state, so the cell diff has a
    // consistent prev/cur to compare and there's no overlap.
    cfg.turns.reserve(total - start);
    int turn_num = turn;
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];
        cfg.turns.push_back(turn_config(msg, i, turn_num, m));
        if (msg.role == Role::Assistant) ++turn_num;
    }

    cfg.in_flight = activity_indicator_config(m);
    return cfg;
}

} // namespace moha::ui
