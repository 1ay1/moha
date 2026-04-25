#include "moha/memory/file_card_store.hpp"

#include "moha/auth/auth.hpp"
#include "moha/provider/anthropic/transport.hpp"
#include "moha/runtime/msg.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <variant>

#include <nlohmann/json.hpp>

namespace moha::memory {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

[[nodiscard]] std::string sha_path(const fs::path& p) {
    // Cheap FNV-1a hex of the path string. We don't need crypto, just
    // a stable filename keyed by the file path. 64-bit hex = 16 chars
    // — collisions across one workspace are vanishingly unlikely for
    // any sane file count.
    std::string s = p.generic_string();
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return buf;
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

[[nodiscard]] std::chrono::system_clock::time_point
parse_iso_8601(std::string_view s) {
    if (s.size() < 19) return {};
    std::tm tm_buf{};
    try {
        tm_buf.tm_year = std::stoi(std::string{s.substr(0, 4)}) - 1900;
        tm_buf.tm_mon  = std::stoi(std::string{s.substr(5, 2)}) - 1;
        tm_buf.tm_mday = std::stoi(std::string{s.substr(8, 2)});
        tm_buf.tm_hour = std::stoi(std::string{s.substr(11, 2)});
        tm_buf.tm_min  = std::stoi(std::string{s.substr(14, 2)});
        tm_buf.tm_sec  = std::stoi(std::string{s.substr(17, 2)});
    } catch (...) { return {}; }
#if defined(_WIN32)
    auto t = _mkgmtime(&tm_buf);
#else
    auto t = timegm(&tm_buf);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

[[nodiscard]] std::int64_t mtime_to_int(fs::file_time_type t) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        t.time_since_epoch()).count();
}

[[nodiscard]] fs::file_time_type int_to_mtime(std::int64_t s) {
    return fs::file_time_type{std::chrono::seconds{s}};
}

[[nodiscard]] bool is_summarisable(const fs::path& p) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    static const std::unordered_set<std::string> ok_exts = {
        ".cpp", ".cc", ".cxx", ".c++",
        ".hpp", ".hh", ".hxx", ".h++",
        ".c",   ".h",  ".m",   ".mm",
        ".py",  ".pyi",
        ".js",  ".jsx", ".ts",  ".tsx", ".mjs", ".cjs",
        ".go",  ".rs",
        ".java", ".kt", ".rb", ".swift",
        ".md",  ".rst", ".txt",
        ".cmake", ".toml", ".yaml", ".yml",
        ".json",
    };
    if (e.empty() && (p.filename() == "CMakeLists.txt"
                   || p.filename() == "Makefile"
                   || p.filename() == "Dockerfile"))
        return true;
    return ok_exts.contains(e);
}

constexpr const char* kCardSystemPrompt =
    "You distill source files into compact knowledge cards for an "
    "AI coding agent's persistent context. Write ONE paragraph, "
    "60-100 words, plain prose. Cover: (1) what the file *does* in "
    "one phrase, (2) the key entry points / public API surface, "
    "(3) any non-obvious responsibility, contract, or invariant the "
    "agent needs to know. Skip restating the function names that are "
    "already in the agent's symbol index — focus on PURPOSE and "
    "RELATIONSHIPS. No preamble, no markdown headings, no bullet "
    "lists — one tight paragraph that the agent can splice into a "
    "table of contents.";

constexpr std::size_t kMaxFileBytesForCard = 64 * 1024;
constexpr std::size_t kMaxCards            = 256;

} // namespace

// ── FileCardStore ─────────────────────────────────────────────────

FileCardStore::~FileCardStore() {
    {
        std::lock_guard lk(jobs_mu_);
        shutdown_.store(true);
    }
    jobs_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FileCardStore::set_workspace(const fs::path& workspace) {
    std::lock_guard lk(mu_);
    fs::path canon;
    std::error_code ec;
    canon = fs::weakly_canonical(workspace, ec);
    if (ec) canon = workspace;
    if (canon == workspace_ && loaded_) return;
    workspace_ = canon;
    cards_dir_ = workspace_ / ".moha" / "cards";
    by_path_.clear();
    loaded_ = false;
    load_locked_();
    // Spin up the worker on first bind. Idempotent — if it's already
    // running (re-bind to same workspace), it stays alive.
    if (!worker_.joinable()) {
        worker_ = std::thread([this]{ worker_loop_(); });
    }
}

void FileCardStore::set_auth(std::string header, std::string style) {
    std::lock_guard lk(mu_);
    auth_header_ = std::move(header);
    auth_style_  = std::move(style);
}

void FileCardStore::load_locked_() {
    if (loaded_) return;
    loaded_ = true;
    std::error_code ec;
    if (!fs::exists(cards_dir_, ec)) return;
    for (const auto& entry : fs::directory_iterator(cards_dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f) continue;
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        auto j = json::parse(body, /*cb*/nullptr, /*throw*/false);
        if (j.is_discarded() || !j.is_object()) continue;
        FileCard c;
        c.path        = j.value("path", "");
        c.summary     = j.value("summary", "");
        c.source      = j.value("source", "auto");
        c.confidence  = j.value("confidence", 50);
        c.generated_at = parse_iso_8601(j.value("generated_at", ""));
        c.mtime        = int_to_mtime(j.value("mtime_s", std::int64_t{0}));
        if (c.path.empty() || c.summary.empty()) continue;
        by_path_.emplace(c.path, std::move(c));
    }
}

void FileCardStore::persist_locked_(const FileCard& c) const {
    if (workspace_.empty()) return;
    std::error_code ec;
    fs::create_directories(cards_dir_, ec);
    if (ec) return;
    json j;
    j["path"]         = c.path;
    j["summary"]      = c.summary;
    j["source"]       = c.source;
    j["confidence"]   = c.confidence;
    j["generated_at"] = iso_8601(c.generated_at);
    j["mtime_s"]      = mtime_to_int(c.mtime);
    auto file = cards_dir_ / (sha_path(c.path) + ".json");
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        std::string body = j.dump(2);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        std::error_code cec;
        fs::copy_file(tmp, file, fs::copy_options::overwrite_existing, cec);
        fs::remove(tmp, cec);
    }
}

std::optional<FileCard>
FileCardStore::get(const fs::path& path) const {
    std::lock_guard lk(mu_);
    if (workspace_.empty()) return std::nullopt;
    std::error_code ec;
    auto rel = fs::relative(path, workspace_, ec).generic_string();
    if (ec) rel = path.generic_string();
    auto it = by_path_.find(rel);
    if (it == by_path_.end()) return std::nullopt;
    auto cur_mt = fs::last_write_time(path, ec);
    if (ec) return it->second;     // can't stat; trust the cache
    if (mtime_to_int(cur_mt) != mtime_to_int(it->second.mtime))
        return std::nullopt;       // stale
    return it->second;
}

void FileCardStore::request(const fs::path& path) {
    if (!is_summarisable(path)) return;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    auto sz = fs::file_size(path, ec);
    if (ec || sz == 0 || sz > kMaxFileBytesForCard) return;
    if (get(path)) return;                  // already cached + fresh
    {
        std::lock_guard lk(jobs_mu_);
        // Dedupe — if path already queued, skip.
        for (const auto& q : jobs_) if (q == path) return;
        jobs_.push_back(path);
    }
    jobs_cv_.notify_one();
}

std::vector<FileCard> FileCardStore::all_cards() const {
    std::lock_guard lk(mu_);
    std::vector<FileCard> out;
    out.reserve(by_path_.size());
    for (const auto& [_, c] : by_path_) out.push_back(c);
    return out;
}

std::size_t FileCardStore::card_count() const noexcept {
    std::lock_guard lk(mu_);
    return by_path_.size();
}

std::size_t FileCardStore::pending_jobs() const noexcept {
    std::lock_guard lk(jobs_mu_);
    return jobs_.size();
}

std::size_t FileCardStore::generated_this_session() const noexcept {
    return generated_.load(std::memory_order_relaxed);
}

void FileCardStore::drop(const fs::path& path) {
    std::lock_guard lk(mu_);
    std::error_code ec;
    auto rel = fs::relative(path, workspace_, ec).generic_string();
    if (ec) rel = path.generic_string();
    by_path_.erase(rel);
    if (!workspace_.empty()) {
        std::error_code rec;
        fs::remove(cards_dir_ / (sha_path(rel) + ".json"), rec);
    }
}

bool FileCardStore::ready() const noexcept {
    std::lock_guard lk(mu_);
    return loaded_ && !workspace_.empty();
}

void FileCardStore::worker_loop_() {
    for (;;) {
        fs::path next;
        {
            std::unique_lock lk(jobs_mu_);
            jobs_cv_.wait(lk, [this]{
                return shutdown_.load() || !jobs_.empty();
            });
            if (shutdown_.load() && jobs_.empty()) return;
            next = std::move(jobs_.front());
            jobs_.pop_front();
        }
        // Re-check freshness — between the request enqueue and now,
        // someone else may have cached the same file.
        if (get(next)) continue;
        try {
            if (auto card = distill_(next)) {
                std::lock_guard lk(mu_);
                // Cap eviction: if we're at the cap, drop the oldest
                // by generated_at (LRU-ish).
                while (by_path_.size() >= kMaxCards) {
                    auto oldest = by_path_.begin();
                    for (auto it = by_path_.begin(); it != by_path_.end(); ++it)
                        if (it->second.generated_at < oldest->second.generated_at)
                            oldest = it;
                    std::error_code rec;
                    fs::remove(cards_dir_ / (sha_path(oldest->first) + ".json"), rec);
                    by_path_.erase(oldest);
                }
                by_path_[card->path] = *card;
                persist_locked_(*card);
                generated_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            // Swallow — card generation is best-effort.
        }
    }
}

std::optional<FileCard>
FileCardStore::distill_(const fs::path& path) const {
    // Read the file content (cap at kMaxFileBytesForCard).
    auto content = tools::util::read_file(path);
    if (content.empty()) return std::nullopt;
    if (content.size() > kMaxFileBytesForCard)
        content.resize(kMaxFileBytesForCard);

    std::string header, style;
    {
        std::lock_guard lk(mu_);
        header = auth_header_;
        style  = auth_style_;
    }
    if (header.empty()) return std::nullopt;

    // Build the request — short Haiku prompt, capped output.
    Message instr;
    instr.role = Role::User;
    {
        std::ostringstream o;
        std::error_code ec;
        auto rel = fs::relative(path, workspace_, ec).generic_string();
        if (ec) rel = path.generic_string();
        o << "Path: " << rel << "\n\n```\n" << content << "\n```\n\n"
          << "Distill this file into a 60-100 word knowledge card per "
          << "the system instructions.";
        instr.text = o.str();
    }

    provider::anthropic::Request req;
    req.model         = "claude-haiku-4-5-20251001";
    req.system_prompt = kCardSystemPrompt;
    req.messages      = std::vector<Message>{std::move(instr)};
    req.tools         = {};
    req.max_tokens    = 256;
    req.auth_header   = header;
    req.auth_style    = (style == "Bearer") ? auth::Style::Bearer
                                            : auth::Style::ApiKey;

    std::string summary;
    bool finished = false;
    std::string err;
    try {
        provider::anthropic::run_stream_sync(std::move(req), [&](Msg m) {
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamTextDelta>) {
                    summary += e.text;
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    finished = true;
                } else if constexpr (std::same_as<T, StreamError>) {
                    err = e.message;
                    finished = true;
                }
            }, m);
        }, /*cancel*/{});
    } catch (...) { return std::nullopt; }
    if (!err.empty() || summary.empty()) return std::nullopt;

    // Trim whitespace.
    while (!summary.empty()
           && (summary.front() == ' ' || summary.front() == '\n'
            || summary.front() == '\r' || summary.front() == '\t'))
        summary.erase(0, 1);
    while (!summary.empty()
           && (summary.back() == ' ' || summary.back() == '\n'
            || summary.back() == '\r' || summary.back() == '\t'))
        summary.pop_back();
    if (summary.empty()) return std::nullopt;
    summary = tools::util::to_valid_utf8(std::move(summary));

    FileCard c;
    std::error_code ec;
    auto rel = fs::relative(path, workspace_, ec).generic_string();
    c.path         = ec ? path.generic_string() : rel;
    c.mtime        = fs::last_write_time(path, ec);
    c.summary      = std::move(summary);
    c.generated_at = std::chrono::system_clock::now();
    c.source       = "auto";
    c.confidence   = static_cast<int>(std::min<std::size_t>(
        90, 30 + content.size() / 200));     // bigger files → higher confidence
    return c;
}

FileCardStore& shared_cards() {
    static FileCardStore g;
    return g;
}

} // namespace moha::memory
