#pragma once
// View-side render cache.
//
// Keeps mutable UI state out of the pure domain types (Message, ToolUse).
// The domain describes *what* a conversation is; this cache describes
// *what we've already painted for it* so we can skip rebuilding identical
// Elements every frame.
//
//   • Message markdown — finalized assistant messages whose `text` is
//     immutable. Keyed by (thread_id, msg_idx). Streaming messages carry a
//     separate `StreamingMarkdown` that caches block boundaries so each
//     delta costs O(new_chars) instead of O(total).

#include <cstddef>
#include <memory>

#include "moha/domain/id.hpp"

namespace maya {
    struct Element;
    class  StreamingMarkdown;
}

namespace moha::ui {

struct MessageMdCache {
    std::shared_ptr<maya::Element>            finalized;
    std::shared_ptr<maya::StreamingMarkdown>  streaming;
};

[[nodiscard]] MessageMdCache& message_md_cache(const ThreadId& tid,
                                               std::size_t msg_idx);

} // namespace moha::ui
