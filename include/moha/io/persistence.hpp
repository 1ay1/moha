#pragma once
// Filesystem adapter for the store domain.  Lives in `io/` because it
// talks to the filesystem; the concept it satisfies lives in
// `moha/store/store.hpp`.  Exposed as free functions plus an `FsStore`
// thin wrapper so tests can drop in an alternative without touching
// the rest of the app.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "moha/domain/conversation.hpp"
#include "moha/store/store.hpp"

namespace moha::persistence {

[[nodiscard]] std::filesystem::path data_dir();
[[nodiscard]] std::filesystem::path threads_dir();

[[nodiscard]] std::vector<Thread> load_all_threads();
void save_thread(const Thread& t);
void delete_thread(const ThreadId& id);

[[nodiscard]] store::Settings load_settings();
void save_settings(const store::Settings& s);

[[nodiscard]] ThreadId new_id();
[[nodiscard]] std::string title_from_first_message(std::string_view text);

} // namespace moha::persistence

namespace moha::io {

// Filesystem-backed store satisfying moha::store::Store.
class FsStore {
public:
    [[nodiscard]] std::vector<Thread> load_threads() {
        return persistence::load_all_threads();
    }
    void save_thread(const Thread& t)            { persistence::save_thread(t); }
    [[nodiscard]] store::Settings load_settings()          { return persistence::load_settings(); }
    void save_settings(const store::Settings& s) { persistence::save_settings(s); }
    [[nodiscard]] ThreadId new_id()              { return persistence::new_id(); }
    [[nodiscard]] std::string title_from(std::string_view text) {
        return persistence::title_from_first_message(text);
    }
};

static_assert(store::Store<FsStore>);

} // namespace moha::io
