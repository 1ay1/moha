#include "moha/runtime/view/cache.hpp"

namespace moha::ui {

namespace {

// thread_local because the view renders on a single thread. Avoiding the
// sync cost + `static` initialization ordering pitfalls is worth more
// than the memory savings a global would offer for a one-thread app.
thread_local std::unordered_map<std::string, ToolCardCache>  g_tool_cache;
thread_local std::unordered_map<std::string, MessageMdCache> g_message_cache;

std::string message_key(const ThreadId& tid, std::size_t msg_idx) {
    std::string k;
    k.reserve(tid.value.size() + 16);
    k.append(tid.value);
    k.push_back(':');
    k.append(std::to_string(msg_idx));
    return k;
}

} // namespace

ToolCardCache& tool_card_cache(const ToolCallId& id) {
    return g_tool_cache[id.value];
}

MessageMdCache& message_md_cache(const ThreadId& tid, std::size_t msg_idx) {
    return g_message_cache[message_key(tid, msg_idx)];
}

} // namespace moha::ui
