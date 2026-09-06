// settings_cache.cpp — write-behind for the settings seam. See the header.

#include "agentty/runtime/app/settings_cache.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace agentty::app::settings_cache {

namespace {

struct State {
    std::mutex              mu;
    std::condition_variable cv;

    // The authoritative in-memory value. Once seeded, `load` never touches
    // disk again — which is what gives read-your-writes and also removes a
    // disk read from every reducer that does load-modify-save.
    std::optional<store::Settings> cached;

    // The newest value not yet written. Consecutive saves COALESCE here: the
    // intermediate states of a held-down key are not interesting, only where
    // it stopped. This also bounds the queue at one regardless of how fast
    // the UI produces writes.
    std::optional<store::Settings> pending;

    bool        writing  = false;   // a write is in flight right now
    bool        stopping = false;
    std::thread worker;

    std::function<store::Settings()>            load_from_disk;
    std::function<void(const store::Settings&)> save_to_disk;
};

State& state() {
    static State s;
    return s;
}

// Drain loop. Runs on its own thread; the ONLY place settings IO happens.
void run(State& s) {
    std::unique_lock lock(s.mu);
    for (;;) {
        s.cv.wait(lock, [&] { return s.pending.has_value() || s.stopping; });
        if (!s.pending) {
            if (s.stopping) return;
            continue;
        }

        auto value = std::move(*s.pending);
        s.pending.reset();
        s.writing = true;

        // IO with the lock RELEASED: a reader must never wait on a disk write,
        // which is the entire point of this file.
        lock.unlock();
        try {
            if (s.save_to_disk) s.save_to_disk(value);
        } catch (...) { /* best-effort: a failed save must not kill the app */ }
        lock.lock();

        s.writing = false;
        // flush() waits on this: "queue empty AND nothing in flight".
        s.cv.notify_all();
    }
}

void ensure_worker(State& s) {
    if (s.worker.joinable() || s.stopping) return;
    s.worker = std::thread([&s] { run(s); });
}

} // namespace

Seam wrap(std::function<store::Settings()> load_from_disk,
          std::function<void(const store::Settings&)> save_to_disk) {
    auto& s = state();
    {
        std::lock_guard lock(s.mu);
        s.load_from_disk = std::move(load_from_disk);
        s.save_to_disk   = std::move(save_to_disk);
        // A re-install (provider switch rebuilds Deps) must not resurrect a
        // stale cache from the previous store.
        s.cached.reset();
        s.stopping = false;
    }

    Seam out;

    out.load = [] {
        auto& st = state();
        std::unique_lock lock(st.mu);
        if (st.cached) return *st.cached;

        // First read: fault in from disk with the lock released, then publish.
        auto loader = st.load_from_disk;
        lock.unlock();
        store::Settings fresh;
        try {
            if (loader) fresh = loader();
        } catch (...) { /* defaults stand */ }
        lock.lock();
        // Another thread may have seeded (or a save may have published a
        // newer value) while we were off the lock — theirs wins, because it
        // is at least as new as what we just read.
        if (!st.cached) st.cached = std::move(fresh);
        return *st.cached;
    };

    out.save = [](const store::Settings& value) {
        auto& st = state();
        {
            std::lock_guard lock(st.mu);
            // Publish to readers FIRST. A reducer that saves then loads must
            // see its own write, and it must see it without waiting for disk.
            st.cached  = value;
            st.pending = value;
            ensure_worker(st);
        }
        st.cv.notify_one();
    };

    return out;
}

void flush() noexcept {
    auto& s = state();
    std::unique_lock lock(s.mu);
    if (!s.worker.joinable()) {
        // No worker ever started, but a value may have been published before
        // one existed. Write it here — this path is only reached at exit.
        if (s.pending && s.save_to_disk) {
            auto value = std::move(*s.pending);
            s.pending.reset();
            lock.unlock();
            try { s.save_to_disk(value); } catch (...) {}
        }
        return;
    }
    s.cv.wait(lock, [&] { return !s.pending && !s.writing; });
}

void shutdown() noexcept {
    // Drain BEFORE stopping the worker: a value published moments before quit
    // must still reach disk, otherwise the last thing the user changed is the
    // one thing that does not persist.
    flush();

    auto& s = state();
    std::thread worker;
    {
        std::lock_guard lock(s.mu);
        s.stopping = true;
        if (!s.worker.joinable()) return;
        worker = std::move(s.worker);
    }
    s.cv.notify_all();
    // The worker only ever holds the lock briefly or does one write, so this
    // join is bounded by a single settings save.
    if (worker.joinable()) worker.join();
}

} // namespace agentty::app::settings_cache
