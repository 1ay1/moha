#include "moha/runtime/view/cache.hpp"

#include <string>
#include <unordered_map>

namespace moha::ui {

namespace {

// thread_local because the view renders on a single thread.
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

MessageMdCache& message_md_cache(const ThreadId& tid, std::size_t msg_idx) {
    return g_message_cache[message_key(tid, msg_idx)];
}

} // namespace moha::ui
