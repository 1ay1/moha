#include "moha/runtime/view/cache.hpp"

#include <atomic>
#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>

namespace moha::ui {

namespace {

// LRU-bounded thread_local cache.  Both the markdown render and the
// turn-config caches share one entry per (thread, msg) pair so they
// live and die together — half-evictions force a rebuild anyway, so
// keeping them coupled simplifies invalidation.

struct Entry {
    MessageMdCache  md;
    TurnConfigCache cfg;
    std::list<std::string>::iterator lru_it;
};

thread_local std::unordered_map<std::string, Entry> g_entries;
thread_local std::list<std::string>                 g_lru;
std::atomic<std::size_t>                            g_cap{4096};

std::string message_key(const ThreadId& tid, std::size_t msg_idx) {
    std::string k;
    k.reserve(tid.value.size() + 16);
    k.append(tid.value);
    k.push_back(':');
    k.append(std::to_string(msg_idx));
    return k;
}

Entry& touch(const std::string& key) {
    auto it = g_entries.find(key);
    if (it != g_entries.end()) {
        g_lru.splice(g_lru.begin(), g_lru, it->second.lru_it);
        it->second.lru_it = g_lru.begin();
        return it->second;
    }

    // Evict the oldest until we're under the cap before inserting.
    const std::size_t cap = g_cap.load(std::memory_order_relaxed);
    while (g_entries.size() >= cap && !g_lru.empty()) {
        const std::string& victim = g_lru.back();
        g_entries.erase(victim);
        g_lru.pop_back();
    }

    g_lru.push_front(key);
    auto [ins, _] = g_entries.emplace(key, Entry{});
    ins->second.lru_it = g_lru.begin();
    return ins->second;
}

} // namespace

MessageMdCache& message_md_cache(const ThreadId& tid, std::size_t msg_idx) {
    return touch(message_key(tid, msg_idx)).md;
}

TurnConfigCache& turn_config_cache(const ThreadId& tid, std::size_t msg_idx) {
    return touch(message_key(tid, msg_idx)).cfg;
}

void evict_thread(const ThreadId& tid) {
    const std::string prefix = tid.value + ":";
    for (auto it = g_entries.begin(); it != g_entries.end(); ) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            g_lru.erase(it->second.lru_it);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void evict_message(const ThreadId& tid, std::size_t msg_idx) {
    auto it = g_entries.find(message_key(tid, msg_idx));
    if (it == g_entries.end()) return;
    g_lru.erase(it->second.lru_it);
    g_entries.erase(it);
}

void set_cache_capacity(std::size_t max_entries) noexcept {
    g_cap.store(max_entries == 0 ? 1 : max_entries, std::memory_order_relaxed);
}

} // namespace moha::ui
