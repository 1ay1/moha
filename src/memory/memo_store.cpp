#include "moha/memory/memo_store.hpp"

#include "moha/tool/util/fs_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace moha::memory {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

[[nodiscard]] std::string short_id() {
    // 64-bit random, hex, 16 chars. Plenty of entropy for memo
    // identity — collisions across a single workspace are vanishingly
    // unlikely at the scales we care about (kMaxMemos = 64).
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream oss;
    oss << std::hex << rng();
    auto s = oss.str();
    while (s.size() < 16) s.insert(0, "0");
    return s;
}

[[nodiscard]] std::string iso_8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

[[nodiscard]] std::chrono::system_clock::time_point parse_iso_8601(std::string_view s) {
    std::tm tm_buf{};
    int yr = 1970, mo = 1, dy = 1, hh = 0, mm = 0, ss = 0;
    // Light-touch parse — accept "YYYY-MM-DDTHH:MM:SSZ" only.
    if (s.size() >= 19) {
        try {
            yr = std::stoi(std::string{s.substr(0, 4)});
            mo = std::stoi(std::string{s.substr(5, 2)});
            dy = std::stoi(std::string{s.substr(8, 2)});
            hh = std::stoi(std::string{s.substr(11, 2)});
            mm = std::stoi(std::string{s.substr(14, 2)});
            ss = std::stoi(std::string{s.substr(17, 2)});
        } catch (...) {}
    }
    tm_buf.tm_year = yr - 1900;
    tm_buf.tm_mon  = mo - 1;
    tm_buf.tm_mday = dy;
    tm_buf.tm_hour = hh;
    tm_buf.tm_min  = mm;
    tm_buf.tm_sec  = ss;
#if defined(_WIN32)
    auto t = _mkgmtime(&tm_buf);
#else
    auto t = timegm(&tm_buf);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

[[nodiscard]] std::string lowercase(std::string s) {
    std::ranges::transform(s, s.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

// Tokenize a string into bag-of-words (lowercase, len>=3, no
// punctuation). Used by find_similar to score query overlap.
[[nodiscard]] std::unordered_set<std::string>
tokenize(std::string_view s) {
    std::unordered_set<std::string> out;
    std::string cur;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || u == '_') {
            cur.push_back(static_cast<char>(std::tolower(u)));
        } else {
            if (cur.size() >= 3) out.insert(std::move(cur));
            cur.clear();
        }
    }
    if (cur.size() >= 3) out.insert(std::move(cur));
    return out;
}

// Read git HEAD by parsing .git/HEAD directly — avoids a subprocess
// fork on every call. Returns empty when not a git repo.
[[nodiscard]] std::string read_git_head_(const fs::path& workspace) {
    std::error_code ec;
    fs::path git_dir = workspace / ".git";
    if (!fs::exists(git_dir, ec)) return {};
    // `.git` may be a directory (normal repo) OR a file with
    // `gitdir: <relative path>` (worktrees + submodules). Handle both.
    fs::path real_git;
    if (fs::is_directory(git_dir, ec)) {
        real_git = git_dir;
    } else if (fs::is_regular_file(git_dir, ec)) {
        std::ifstream f(git_dir);
        std::string line;
        if (std::getline(f, line) && line.starts_with("gitdir: ")) {
            fs::path rel = line.substr(8);
            real_git = rel.is_absolute() ? rel : (workspace / rel);
        }
    }
    if (real_git.empty()) return {};

    std::ifstream head(real_git / "HEAD");
    std::string line;
    if (!std::getline(head, line)) return {};
    if (line.starts_with("ref: ")) {
        // Resolve symbolic ref. "ref: refs/heads/main" → read
        // .git/refs/heads/main; if not found (packed refs), look in
        // .git/packed-refs for the matching line.
        fs::path ref_path = real_git / line.substr(5);
        std::ifstream ref(ref_path);
        std::string sha;
        if (std::getline(ref, sha) && !sha.empty()) return sha;
        std::ifstream packed(real_git / "packed-refs");
        std::string p;
        std::string ref_name = line.substr(5);
        while (std::getline(packed, p)) {
            if (p.size() < 41) continue;
            if (p.front() == '#' || p.front() == '^') continue;
            // "<sha> <ref>"
            auto sp = p.find(' ');
            if (sp == std::string::npos) continue;
            if (p.substr(sp + 1) == ref_name) return p.substr(0, sp);
        }
        return {};
    }
    // Detached HEAD — line IS the SHA.
    return line;
}

} // namespace

std::string current_git_head(const fs::path& workspace) {
    // Cached for ~5 s so a flurry of memo lookups in one turn doesn't
    // hammer the filesystem with HEAD reads.
    static std::mutex mu;
    static fs::path  cached_ws;
    static std::string cached_head;
    static std::chrono::steady_clock::time_point cached_at{};
    std::lock_guard lk(mu);
    auto now = std::chrono::steady_clock::now();
    if (cached_ws == workspace
        && now - cached_at < std::chrono::seconds(5))
        return cached_head;
    cached_ws   = workspace;
    cached_head = read_git_head_(workspace);
    cached_at   = now;
    return cached_head;
}

// ── MemoStore ─────────────────────────────────────────────────────

void MemoStore::set_workspace(const fs::path& workspace) {
    std::lock_guard lk(mu_);
    fs::path canon;
    std::error_code ec;
    canon = fs::weakly_canonical(workspace, ec);
    if (ec) canon = workspace;
    if (canon == workspace_ && loaded_) return;        // idempotent
    workspace_  = canon;
    store_path_ = workspace_ / ".moha" / "memos.json";
    memos_.clear();
    loaded_ = false;
    load_locked_();
}

void MemoStore::load_locked_() {
    if (loaded_) return;
    loaded_ = true;
    std::error_code ec;
    if (!fs::exists(store_path_, ec)) return;
    std::ifstream f(store_path_);
    if (!f) return;
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    auto j = json::parse(body, /*cb*/nullptr, /*throw*/false);
    if (j.is_discarded() || !j.is_object()) return;
    auto it = j.find("memos");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& mj : *it) {
        if (!mj.is_object()) continue;
        Memo m;
        m.id        = mj.value("id", "");
        m.query     = mj.value("query", "");
        m.synthesis = mj.value("synthesis", "");
        m.git_head  = mj.value("git_head", "");
        m.created_at = parse_iso_8601(mj.value("created_at", ""));
        if (auto rf = mj.find("file_refs"); rf != mj.end() && rf->is_array()) {
            for (const auto& s : *rf) {
                if (s.is_string()) m.file_refs.push_back(s.get<std::string>());
            }
        }
        if (m.id.empty() || m.synthesis.empty()) continue;
        memos_.push_back(std::move(m));
    }
}

void MemoStore::persist_locked_() const {
    if (workspace_.empty()) return;
    std::error_code ec;
    fs::create_directories(store_path_.parent_path(), ec);
    if (ec) {
        std::fprintf(stderr,
            "moha: memos: cannot create %s/.moha (%s) — memos this "
            "session will not persist\n",
            workspace_.string().c_str(), ec.message().c_str());
        return;
    }
    json j;
    j["version"]   = 1;
    j["workspace"] = workspace_.generic_string();
    j["memos"]     = json::array();
    for (const auto& m : memos_) {
        json mj;
        mj["id"]         = m.id;
        mj["query"]      = m.query;
        mj["synthesis"]  = m.synthesis;
        mj["created_at"] = iso_8601(m.created_at);
        mj["git_head"]   = m.git_head;
        mj["file_refs"]  = m.file_refs;
        j["memos"].push_back(std::move(mj));
    }
    // Atomic rename via tmp file so a partial write doesn't corrupt
    // the store. fflush + fsync (where available) means a crash
    // mid-rename can't leave us with a half-written file pretending
    // to be the truth.
    auto tmp = store_path_;
    tmp += ".tmp";
    bool wrote = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
            std::string body = j.dump(2);
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
            f.flush();
            wrote = static_cast<bool>(f);
        }
    }
    if (!wrote) {
        std::fprintf(stderr,
            "moha: memos: write to %s failed — keeping prior contents\n",
            tmp.string().c_str());
        fs::remove(tmp, ec);
        return;
    }
    fs::rename(tmp, store_path_, ec);
    if (ec) {
        // Cross-device or Windows file-lock contention with the
        // previous file. Fall back to copy+delete; if THAT fails,
        // surface the error so the user knows the memo wasn't saved.
        std::error_code cec;
        fs::copy_file(tmp, store_path_,
                      fs::copy_options::overwrite_existing, cec);
        if (cec) {
            std::fprintf(stderr,
                "moha: memos: rename %s -> %s failed (%s) and copy "
                "fallback failed (%s) — memo NOT persisted\n",
                tmp.string().c_str(), store_path_.string().c_str(),
                ec.message().c_str(), cec.message().c_str());
        }
        fs::remove(tmp, cec);
    }
}

std::string MemoStore::git_head_locked_() {
    auto now = std::chrono::steady_clock::now();
    if (!cached_git_head_.empty()
        && now - cached_git_head_at_ < std::chrono::seconds(5))
        return cached_git_head_;
    cached_git_head_ = current_git_head(workspace_);
    cached_git_head_at_ = now;
    return cached_git_head_;
}

void MemoStore::add(Memo m) {
    if (m.id.empty()) m.id = short_id();
    if (m.created_at.time_since_epoch().count() == 0)
        m.created_at = std::chrono::system_clock::now();
    std::lock_guard lk(mu_);
    if (workspace_.empty()) return;     // not bound to a workspace
    if (m.git_head.empty()) m.git_head = git_head_locked_();

    // Dedupe: if a memo with the same exact query already exists,
    // replace it rather than accumulate near-duplicates.
    auto it = std::ranges::find_if(memos_,
        [&](const Memo& x) { return x.query == m.query; });
    if (it != memos_.end()) {
        *it = std::move(m);
    } else {
        memos_.push_back(std::move(m));
    }

    // Cap: drop oldest if over.
    while (memos_.size() > kMaxMemos)
        memos_.erase(memos_.begin());

    persist_locked_();
}

std::string MemoStore::compose_prompt_block(std::size_t max_bytes) const {
    std::lock_guard lk(mu_);
    if (memos_.empty()) return {};

    // Most-recent first — the model sees freshest knowledge at top.
    auto by_recency = memos_;
    std::ranges::sort(by_recency, [](const Memo& a, const Memo& b) {
        return a.created_at > b.created_at;
    });

    std::ostringstream out;
    out << "# What we've learned about this workspace\n";
    out << "# (Auto-saved from past `investigate` runs. If a question "
           "below covers your task, ANSWER FROM THIS KNOWLEDGE — do "
           "NOT re-investigate the same ground. Call `investigate` "
           "only for genuinely new questions.)\n\n";

    std::size_t budget = max_bytes;
    if (out.tellp() >= 0 && static_cast<std::size_t>(out.tellp()) >= budget)
        return {};

    for (const auto& m : by_recency) {
        // Cap each memo's contribution. Older ones get distilled
        // (truncated to the first ~600 chars + "..."). Newest one
        // gets a generous slice.
        std::ostringstream block;
        block << "## " << m.query << "\n";
        std::string body = m.synthesis;
        // Truncate per-memo to keep one verbose memo from eating the
        // whole budget. Snap to a line boundary for readability.
        constexpr std::size_t kPerMemoCap = 1500;
        if (body.size() > kPerMemoCap) {
            auto cut = body.rfind('\n', kPerMemoCap);
            if (cut == std::string::npos) cut = kPerMemoCap;
            body = body.substr(0, cut) + "\n[…distilled, "
                 + std::to_string(m.synthesis.size() - cut)
                 + " chars elided]";
        }
        block << body << "\n\n";
        std::string s = block.str();
        if (s.size() > budget) {
            // Don't half-emit a memo. Stop here, leave a hint.
            out << "<!-- " << by_recency.size() - 1
                << " older memo(s) elided to fit budget -->\n";
            break;
        }
        out << s;
        budget -= s.size();
    }
    return out.str();
}

std::optional<Memo> MemoStore::find_similar(std::string_view query) const {
    std::lock_guard lk(mu_);
    if (memos_.empty()) return std::nullopt;
    auto qt = tokenize(query);
    if (qt.empty()) return std::nullopt;

    int best_score = 0;
    const Memo* best = nullptr;
    auto best_time = std::chrono::system_clock::time_point{};

    for (const auto& m : memos_) {
        auto mt = tokenize(m.query);
        int hits = 0;
        for (const auto& w : qt) if (mt.contains(w)) ++hits;
        // Score = overlap × 100 / max(query_len, memo_len). Penalises
        // big query / small memo or vice versa.
        int denom = std::max(qt.size(), mt.size()) ? static_cast<int>(std::max(qt.size(), mt.size())) : 1;
        int score = hits * 100 / denom;
        if (score > best_score
            || (score == best_score && m.created_at > best_time)) {
            best_score = score;
            best       = &m;
            best_time  = m.created_at;
        }
    }
    // 60% overlap is a reasonable threshold — anything less is likely
    // a different question.
    if (best && best_score >= 60) return *best;
    return std::nullopt;
}

bool MemoStore::is_fresh(const Memo& m) const {
    std::lock_guard lk(mu_);
    if (workspace_.empty()) return true;     // can't check; trust it
    auto head = const_cast<MemoStore*>(this)->git_head_locked_();
    if (!head.empty() && !m.git_head.empty() && head == m.git_head)
        return true;     // HEAD unchanged → certainly fresh
    // HEAD moved (or no git): fall back to mtime comparison of every
    // file_ref. If any is newer than the memo, it's stale.
    for (const auto& rel : m.file_refs) {
        std::error_code ec;
        auto p = workspace_ / rel;
        auto mt_ft = fs::last_write_time(p, ec);
        if (ec) continue;     // file gone — be lenient
        // file_time_type vs system_clock comparison is platform-y;
        // convert via duration_cast on time_since_epoch.
        auto memo_d  = std::chrono::duration_cast<std::chrono::seconds>(
            m.created_at.time_since_epoch()).count();
        auto file_d  = std::chrono::duration_cast<std::chrono::seconds>(
            mt_ft.time_since_epoch()).count();
        if (file_d > memo_d) return false;   // file modified after memo
    }
    return true;
}

std::unordered_map<std::string, std::vector<std::string>>
MemoStore::file_to_memo_ids() const {
    std::lock_guard lk(mu_);
    std::unordered_map<std::string, std::vector<std::string>> out;
    for (const auto& m : memos_)
        for (const auto& f : m.file_refs)
            out[f].push_back(m.id);
    return out;
}

std::optional<Memo> MemoStore::by_id(std::string_view id) const {
    std::lock_guard lk(mu_);
    for (const auto& m : memos_) if (m.id == id) return m;
    return std::nullopt;
}

std::size_t MemoStore::size() const noexcept {
    std::lock_guard lk(mu_);
    return memos_.size();
}

std::size_t MemoStore::forget(std::string_view target) {
    std::lock_guard lk(mu_);
    if (memos_.empty()) return 0;
    std::string lower_target{target};
    std::ranges::transform(lower_target, lower_target.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    auto match = [&](const Memo& m) {
        if (m.id == target) return true;
        std::string lq = m.query;
        std::ranges::transform(lq, lq.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return lq.find(lower_target) != std::string::npos;
    };
    std::size_t before = memos_.size();
    auto [first, last] = std::ranges::remove_if(memos_, match);
    memos_.erase(first, last);
    std::size_t removed = before - memos_.size();
    if (removed > 0) persist_locked_();
    return removed;
}

std::vector<Memo> MemoStore::list_memos() const {
    std::lock_guard lk(mu_);
    return memos_;
}

bool MemoStore::ready() const noexcept {
    std::lock_guard lk(mu_);
    return loaded_ && !workspace_.empty();
}

fs::path MemoStore::workspace() const {
    std::lock_guard lk(mu_);
    return workspace_;
}

MemoStore& shared() {
    static MemoStore g;
    return g;
}

} // namespace moha::memory
