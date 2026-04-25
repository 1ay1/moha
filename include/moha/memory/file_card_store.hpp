#pragma once
// moha::memory::FileCardStore — per-file LLM-distilled knowledge cards.
//
// The third pillar of moha's "warm cache" strategy (memos + repo-index
// being the other two). Where the symbol index tells the model
// *what's in* a file (function names, class names) and memos tell it
// *what we discussed*, knowledge cards tell it *what the file does*
// and *how it fits* — distilled into ~80 words by a Haiku call,
// cached on disk, refreshed on mtime change.
//
// The compound effect: the repo-map injected into the system prompt
// goes from this:
//
//   src/auth.cpp  [authenticate(), refresh_token(), validate_session()]
//
// to this:
//
//   src/auth.cpp  [authenticate(), refresh_token(), validate_session()]
//        // OAuth+API-key entry point. Holds the credential cache;
//        // refresh_token() runs after any 401 then retries the
//        // request once. Consumed by src/io/http.cpp's request builder.
//
// — and the model can answer "where does X happen?" / "what does Y
// do?" without any tool calls, because the cached prefix tells it.
//
// Generation policy: lazy + async. Cards are requested by `investigate`
// (for every file_ref) and by future hooks; a background worker drains
// the queue with bounded concurrency. The parent never waits.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace moha::memory {

struct FileCard {
    std::string path;                                          // workspace-relative
    std::filesystem::file_time_type mtime{};                   // file mtime at generation
    std::string summary;                                       // ~80 words, markdown
    std::chrono::system_clock::time_point generated_at{};
    std::string source;                                        // "auto" | "memo" | "explicit"
    int         confidence = 50;                               // 0-100
};

class FileCardStore {
public:
    ~FileCardStore();

    // Bind to a workspace directory. Lazy-loads the per-card JSON
    // files from <workspace>/.moha/cards/. Idempotent. The first call
    // also spins up the background generation worker.
    void set_workspace(const std::filesystem::path& workspace);

    // Bind the auth handle the worker uses for Haiku calls. Called
    // once at startup right after install_deps().
    void set_auth(std::string header, std::string style);

    // Look up the cached card for `path`. Returns the card if cached
    // AND fresh (cached mtime == current mtime). Returns nullopt when
    // not cached or stale.
    [[nodiscard]] std::optional<FileCard>
    get(const std::filesystem::path& path) const;

    // Queue an async generation request. No-op if a fresh card
    // already exists, or if `path` isn't a regular text file. Returns
    // immediately — the worker picks up the request on its own.
    void request(const std::filesystem::path& path);

    // Return all currently-cached cards. Used by the repo-map
    // renderer to surface card text inline. Snapshot copy; safe to
    // hold without a lock.
    [[nodiscard]] std::vector<FileCard> all_cards() const;

    // Process-lifetime stats — useful for the `cards` tool.
    [[nodiscard]] std::size_t card_count() const noexcept;
    [[nodiscard]] std::size_t pending_jobs() const noexcept;
    [[nodiscard]] std::size_t generated_this_session() const noexcept;

    // Drop a card by path. Re-issues the file to be re-summarised on
    // next request(). Used by an explicit "refresh" flow.
    void drop(const std::filesystem::path& path);

    [[nodiscard]] bool ready() const noexcept;

private:
    mutable std::mutex                                        mu_;
    std::filesystem::path                                     workspace_;
    std::filesystem::path                                     cards_dir_;
    std::unordered_map<std::string, FileCard>                 by_path_;       // path → card
    std::string                                               auth_header_;
    std::string                                               auth_style_;
    bool                                                      loaded_ = false;
    std::atomic<std::size_t>                                  generated_{0};

    // Background worker — single thread, drains a job queue. We don't
    // need parallelism here (Haiku already amortises the latency); a
    // single worker keeps the total request rate gentle on Anthropic
    // and prevents card generation from saturating the parent's
    // worker pool. mutable so the const accessors (pending_jobs())
    // can lock it without lying about the API surface.
    mutable std::mutex                                        jobs_mu_;
    mutable std::condition_variable                           jobs_cv_;
    std::deque<std::filesystem::path>                         jobs_;
    std::thread                                               worker_;
    std::atomic<bool>                                         shutdown_{false};

    void load_locked_();
    void persist_locked_(const FileCard& c) const;

    // Worker-thread entry point: pops jobs, generates cards, persists.
    void worker_loop_();

    // Synchronous Haiku call to produce a card for `path`. May fail;
    // returns nullopt and the worker silently retries next session.
    [[nodiscard]] std::optional<FileCard>
    distill_(const std::filesystem::path& path) const;
};

[[nodiscard]] FileCardStore& shared_cards();

} // namespace moha::memory
