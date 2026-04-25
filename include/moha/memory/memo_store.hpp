#pragma once
// moha::memory::MemoStore — persistent "what we've learned about this
// workspace" cache. Every successful `investigate` run drops a memo
// here; the system-prompt builder injects the accumulated memos as a
// `<learned-about-this-workspace>` block so EVERY future request — by
// the parent agent or any sub-agent — sees the prior knowledge. The
// cached prefix means the cost amortises: pay once on the turn that
// adds a memo, get it free on every turn after.
//
// Three-layer design (see chat for the fuller rationale):
//   Layer 1: Memos in system prompt (always-on context).
//   Layer 2: Investigation cache — investigate() consults memos before
//            spawning a sub-agent and short-circuits on a fresh hit.
//   Layer 3: file_refs cross-index — repo_map surfaces "this file was
//            touched in investigation X" annotations.
//
// Storage: <workspace>/.moha/memos.json  (single JSON file, simple,
// hand-editable, easy to git-ignore).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace moha::memory {

struct Memo {
    std::string id;                                            // short hex id
    std::string query;                                         // the question
    std::string synthesis;                                     // the answer
    std::chrono::system_clock::time_point created_at{};        // wall-clock
    std::string git_head;                                      // SHA at creation
    std::vector<std::string> file_refs;                        // workspace-relative

    // Provenance — used to compute the runtime confidence score the
    // composer applies to decide between "show full body" vs "show
    // topic only." Set by the writer (investigate fills `model`;
    // remember tool can override).
    std::string  model      = "";        // "haiku" | "sonnet" | "opus" (free-form)
    std::string  source     = "auto";    // "auto" (investigate) | "manual" (remember) | "adr" | "long_term"
    int          base_score = 50;        // 0-100, set at write time

    // Runtime confidence: combines base_score, freshness against
    // current file mtimes, and age decay. Computed on demand —
    // cheaper than persisting and more accurate (mtimes drift).
    [[nodiscard]] int effective_confidence(
        const std::filesystem::path& workspace) const;
};

class MemoStore {
public:
    // Bind the store to a workspace directory. Lazy-loads memos.json
    // from <workspace>/.moha/memos.json on first call. Idempotent —
    // calling with the same path is a no-op; calling with a different
    // path swaps the active memo set (used when the user changes -w).
    void set_workspace(const std::filesystem::path& workspace);

    // Append a memo + persist. Trims the memo list if it grows past
    // the soft cap (oldest evicted). Thread-safe.
    void add(Memo m);

    // Batch append: same semantics as N back-to-back add() calls but
    // takes the lock + persists exactly ONCE. The remember tool calls
    // this when the model passes a `memos: [...]` array — a turn that
    // banks 5 memos becomes 1 disk write instead of 5, and 1 queued
    // tool slot instead of 5 (each remember is WriteFs-exclusive, so
    // they can't run in parallel anyway). No-op if the input is empty.
    void add_batch(std::vector<Memo> memos);

    // Render the "what we know" block for system-prompt injection.
    // Format: short markdown bullet list of (Q, A-summary) pairs,
    // most-recent first, capped at `max_bytes`. Returns empty when
    // there are no memos.
    [[nodiscard]] std::string compose_prompt_block(std::size_t max_bytes = 8192) const;

    // Find the best-matching memo for a query. Match score uses
    // case-insensitive bag-of-words overlap; ties broken by recency.
    // Returns nullopt when nothing scores above the minimum threshold.
    [[nodiscard]] std::optional<Memo> find_similar(std::string_view query) const;

    // Has the memo's view of the world held up since it was written?
    // True iff (a) git HEAD is unchanged OR (b) every file in
    // file_refs has an mtime <= memo.created_at. This is conservative
    // — for a non-git workspace, falls back to mtime comparison.
    [[nodiscard]] bool is_fresh(const Memo& m) const;

    // Map of workspace-relative file path → list of memo ids that
    // touch it. Used by repo_map to annotate files with "discussed
    // in investigation X". Computed on demand from the memo set.
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
    file_to_memo_ids() const;

    // Look up a memo by id. Used by the file→memo cross-index to
    // resolve back to the original query for display.
    [[nodiscard]] std::optional<Memo> by_id(std::string_view id) const;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::filesystem::path workspace() const;

    // Drop every memo whose `id` exactly matches `target` OR whose
    // `query` contains `target` as a case-insensitive substring.
    // Returns the number of memos removed and re-persists. Used by
    // the `forget` tool.
    std::size_t forget(std::string_view target);

    // Snapshot of all memos (deep copy). Used by the `memos` tool to
    // render a listing without needing to lock the store throughout.
    [[nodiscard]] std::vector<Memo> list_memos() const;

    // Block until any async write triggered by add/add_batch/forget has
    // completed. No-op when no write is pending. Call from app shutdown
    // so the latest in-memory state lands on disk before the process
    // exits — without it, a remember fired in the final turn could be
    // lost to a racing process exit.
    void flush() const;

private:
    mutable std::mutex          mu_;
    std::filesystem::path       workspace_;
    std::filesystem::path       store_path_;
    std::vector<Memo>           memos_;
    bool                        loaded_ = false;
    std::string                 cached_git_head_;          // current HEAD
    std::chrono::steady_clock::time_point cached_git_head_at_{};

    // ── Async-persist machinery ────────────────────────────────────
    // The whole point: tool foreground (add/add_batch/forget) finishes
    // in microseconds — lock, mutate in-memory, signal, return. The
    // disk write happens on a single detached writer thread. Multiple
    // adds in flight coalesce into a single write of the latest state
    // (the writer always serializes the freshest snapshot, never a
    // stale intermediate). Lock ordering: any caller that holds `mu_`
    // and then takes `persist_mu_` is fine; the writer never takes
    // `mu_`, so no inversion is possible.
    mutable std::mutex                       persist_mu_;
    mutable std::condition_variable          persist_cv_;
    mutable std::optional<std::vector<Memo>> pending_snapshot_;
    mutable std::filesystem::path            pending_store_path_;
    mutable bool                             writer_active_ = false;

    // Cap on number of memos stored. Older ones evicted in FIFO order
    // when adding past the cap. Keeping recency over comprehensiveness
    // assumes the user's recent investigations are more topical.
    static constexpr std::size_t kMaxMemos = 64;

    void load_locked_();
    void insert_locked_(Memo m);
    // Called with `mu_` held. Snapshots the current memos + path,
    // queues them for the writer, spawns the writer if not running.
    void schedule_persist_locked_();
    // Pure write op: takes everything by value/const-ref so the writer
    // can run without touching `mu_` or any class state besides the
    // async-persist fields it owns. Static-like; "const" only because
    // it does no mutation of MemoStore-visible state.
    void persist_to_disk_(const std::vector<Memo>& memos,
                          const std::filesystem::path& store_path) const;
    [[nodiscard]] std::string git_head_locked_();
};

// Process-wide singleton — investigate.cpp adds, transport.cpp reads.
[[nodiscard]] MemoStore& shared();

// Read the workspace's current git HEAD (full SHA). Empty string when
// the workspace isn't a git repo or HEAD is unreadable. Cached per
// process for ~5 s so it doesn't fork a subprocess on every call.
[[nodiscard]] std::string current_git_head(const std::filesystem::path& workspace);

} // namespace moha::memory
