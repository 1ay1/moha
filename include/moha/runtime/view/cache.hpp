#pragma once
// View-side render cache.
//
// Keeps mutable UI state out of the pure domain types (Message, ToolUse).
// The domain describes *what* a conversation is; this cache describes
// *what we've already painted for it* so we can skip rebuilding identical
// Elements every frame.
//
// Cache policy per entry kind:
//   • Tool cards — terminal-state tool calls (Done/Failed/Rejected) only.
//     Keyed by ToolCallId; the cached bundle carries a content hash so a
//     retry that reuses the same id but produces different output still
//     invalidates. Running / pending tools rebuild every frame so the live
//     elapsed counter keeps ticking.
//
//   • Message markdown — finalized assistant messages whose `text` is
//     immutable. Keyed by (thread_id, msg_idx). Streaming messages carry a
//     separate `StreamingMarkdown` that caches block boundaries so each
//     delta costs O(new_chars) instead of O(total).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "moha/domain/id.hpp"

namespace maya {
    struct Element;
    class  StreamingMarkdown;
}

namespace moha::ui {

struct ToolCardCache {
    std::shared_ptr<maya::Element> element;
    std::uint64_t key = 0;
};

struct MessageMdCache {
    std::shared_ptr<maya::Element>            finalized;
    std::shared_ptr<maya::StreamingMarkdown>  streaming;
};

// Keyed lookups. Creates an empty slot on first touch so callers can
// populate in place. All lookups run on the UI thread; the maps are
// thread_local and need no synchronization.
[[nodiscard]] ToolCardCache&   tool_card_cache(const ToolCallId& id);
[[nodiscard]] MessageMdCache&  message_md_cache(const ThreadId& tid,
                                                std::size_t msg_idx);

} // namespace moha::ui
