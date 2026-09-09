// agentty::rag adapter — the retrieval engine, powered by rag-cpp (rag::Engine).
//
// This is the ONLY translation unit in agentty that includes <rag/rag.hpp>.
// It maps agentty's compact retrieval boundary (rag_adapter.hpp) onto rag-cpp's
// production hybrid engine: contextual chunking, BM25 + dense/HNSW, RRF fusion,
// rerank, GraphRAG, and .ragdb persistence — so `search_docs`, `search_code`
// and the proactive pre-turn path "just work" and stay fast.
//
// Knowledge sources are unified into ONE Engine and distinguished by a URI
// prefix so provenance survives search():
//     docs://<rel-path>      the docs / knowledge folder (code-aware chunking)
//     skill://<name>         installed Agent-Skills bodies
//     memory://<id>          learned facts (JSONL memory store)
//     mcp://<uri>            this session's MCP resources (opt-in)

#include "agentty/rag/rag_adapter.hpp"

#include "agentty/util/logx.hpp"

// agentty's retrieval engine is rag-cpp. On platforms where rag-cpp isn't
// buildable yet (MSVC — see CMakeLists.txt), AGENTTY_HAS_RAGCPP is 0 and this
// TU compiles to a tiny no-op implementation of the public boundary so the
// rest of agentty links and every non-retrieval feature works. The full engine
// ships in the Linux/macOS binaries.
#ifndef AGENTTY_HAS_RAGCPP
#  define AGENTTY_HAS_RAGCPP 1
#endif

#if AGENTTY_HAS_RAGCPP

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rag/rag.hpp>

#include "agentty/io/http.hpp"
#include "agentty/mcp/client.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/memory_store.hpp"
#include "agentty/util/dbglog.hpp"

namespace fs = std::filesystem;

namespace agentty::rag {
namespace {

bool truthy_default_on(const char* var) {
    const char* v = std::getenv(var);
    if (!v || !v[0]) return true;                 // unset ⇒ ON
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

bool truthy_default_off(const char* var) {
    const char* v = std::getenv(var);
    if (!v || !v[0]) return false;                // unset ⇒ OFF
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

float env_float(const char* var, float dflt) {
    if (const char* v = std::getenv(var); v && v[0]) {
        try { return std::stof(v); } catch (...) {}
    }
    return dflt;
}

fs::path resolve_docs_root(const std::string& configured) {
    // Env wins and is re-read EVERY call: AGENTTY_DOCS_DIR can change between
    // retrievals (tests, or a user who repoints it), and a cached root would
    // silently serve the wrong corpus.
    if (const char* d = std::getenv("AGENTTY_DOCS_DIR"); d && d[0])
        return fs::path{d};
    if (!configured.empty()) return fs::path{configured};
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return {};
    auto docs = cwd / "docs";
    if (fs::is_directory(docs, ec)) return docs;
    auto kb = cwd / ".agentty" / "knowledge";
    if (fs::is_directory(kb, ec)) return kb;
    return {};
}

// Split a "docs://path" uri back into (source-tag, bare-path).
std::pair<std::string, std::string> split_uri(const std::string& uri) {
    auto pos = uri.find("://");
    if (pos == std::string::npos) return {"docs", uri};
    return {uri.substr(0, pos), uri.substr(pos + 3)};
}

std::size_t retrieval_output_budget() {
    std::size_t value = 12 * 1024; // about 3k tokens across the whole result
    if (const char* v = std::getenv("AGENTTY_RAG_OUTPUT_BYTES"); v && v[0]) {
        try { value = std::clamp<std::size_t>(std::stoull(v), 2048, 64 * 1024); }
        catch (...) {}
    }
    return value;
}

// Fraction of the TOP score below which a tail passage is dropped rather than
// compressed into the context. The low-confidence tail is what the model
// ignores anyway, so paying flagship input price for it is pure waste. A hit at
// score s survives only if s >= floor_frac * top_score. 0 disables the floor
// (keep every hit); the default trims the obvious tail without touching the
// meat of a good retrieval. Tunable via AGENTTY_RAG_RELEVANCE_FLOOR.
float relevance_floor_frac() {
    float f = 0.30f;
    if (const char* v = std::getenv("AGENTTY_RAG_RELEVANCE_FLOOR"); v && v[0]) {
        try { f = std::clamp(std::stof(v), 0.0f, 0.95f); } catch (...) {}
    }
    return f;
}

// Water-filling budget allocator: split `total` bytes across passages in
// proportion to their score^gamma (gamma>1 concentrates budget on the
// confident head), but never below `floor` per passage so even a kept tail hit
// stays a legible excerpt. This replaces the flat total/n split, which handed a
// rank-8 hit at confidence 0.11 the same byte allowance as the rank-1 hit at
// 0.88 — i.e. spent the same tokens on signal and noise. Returns one allowance
// per input score, summing to <= total. Tunable via AGENTTY_RAG_BUDGET_GAMMA.
std::vector<std::size_t> allocate_budget(const std::vector<double>& scores,
                                         std::size_t total,
                                         std::size_t floor = 768) {
    const std::size_t n = scores.size();
    std::vector<std::size_t> out(n, 0);
    if (n == 0 || total == 0) return out;
    float gamma = 1.5f;
    if (const char* v = std::getenv("AGENTTY_RAG_BUDGET_GAMMA"); v && v[0]) {
        try { gamma = std::clamp(std::stof(v), 0.5f, 4.0f); } catch (...) {}
    }
    // If every passage can't get the floor, fall back to an even split so we
    // don't starve later passages to overfeed the first.
    if (total < floor * n) {
        std::size_t each = total / n;
        for (auto& a : out) a = each;
        return out;
    }
    std::vector<double> weight(n);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double s = std::clamp(scores[i], 0.0, 1.0);
        weight[i] = std::pow(s + 1e-3, static_cast<double>(gamma));
        sum += weight[i];
    }
    if (sum <= 0.0) { for (auto& w : weight) w = 1.0; sum = static_cast<double>(n); }
    // Reserve the floor for everyone, distribute the remainder by weight.
    const std::size_t reserved = floor * n;
    const std::size_t extra = total - reserved;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = floor + static_cast<std::size_t>(
                             (weight[i] / sum) * static_cast<double>(extra));
    return out;
}

// Scale the TOTAL byte budget by retrieval confidence. A retrieval that barely
// clears the relevance bar (conf ~0.3) shouldn't reserve the same ~3k tokens as
// a slam-dunk (conf ~0.9) — the marginal passages it would spend that budget on
// are exactly the ones the model is least likely to use. We linearly ramp the
// budget from a floor fraction at conf=floor_conf to 100% at conf=1.0, so weak
// retrievals inject a tight, cheap block and strong ones get full room. conf<0
// (unknown) keeps the full budget — we don't penalise a path that never graded.
// Tunable via AGENTTY_RAG_CONF_BUDGET_FLOOR (min fraction, default 0.45).
std::size_t confidence_scaled_budget(std::size_t total, double conf) {
    if (conf < 0.0) return total;                 // ungraded → full budget
    float floor_frac = 0.45f;
    if (const char* v = std::getenv("AGENTTY_RAG_CONF_BUDGET_FLOOR"); v && v[0]) {
        try { floor_frac = std::clamp(std::stof(v), 0.1f, 1.0f); } catch (...) {}
    }
    const double c = std::clamp(conf, 0.0, 1.0);
    // frac = floor + (1-floor) * c, clamped to [floor, 1].
    const double frac = std::clamp(
        static_cast<double>(floor_frac) + (1.0 - floor_frac) * c,
        static_cast<double>(floor_frac), 1.0);
    return std::max<std::size_t>(2048,
        static_cast<std::size_t>(static_cast<double>(total) * frac));
}

std::vector<std::string> query_terms(std::string_view query) {
    std::vector<std::string> terms;
    std::string word;
    auto flush = [&] {
        if (word.size() >= 2
            && std::find(terms.begin(), terms.end(), word) == terms.end())
            terms.push_back(word);
        word.clear();
    };
    for (unsigned char c : query) {
        if (std::isalnum(c)) word.push_back(static_cast<char>(std::tolower(c)));
        else flush();
    }
    flush();
    return terms;
}

std::size_t utf8_boundary(std::string_view text, std::size_t at) {
    at = std::min(at, text.size());
    while (at > 0 && at < text.size()
           && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
    return at;
}

// Re-read an exact 1-based inclusive line range [start,end] from live disk.
// Powers RAG result verification: the index can lag an edit, but disk is
// always current — so search_code re-reads the cited lines and serves the
// disk truth. Returns "" on any error (missing file, out-of-range) so the
// caller falls back to the indexed text.
std::string read_disk_lines(const std::string& path, int start, int end) {
    if (start < 1 || end < start) return {};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string line, out;
    int n = 0;
    while (std::getline(f, line)) {
        ++n;
        if (n < start) continue;
        if (n > end) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out += line;
        out += '\n';
    }
    return (n >= start) ? out : std::string{};
}

std::string compress_passage(std::string_view query, std::string_view text,
                             std::size_t budget) {
    if (text.size() <= budget) return std::string{text};
    const auto terms = query_terms(query);

    // Extractive sentence selection (model-free LLMLingua-style) for PROSE.
    // Score each sentence by rare-term-weighted query overlap, greedily keep
    // the highest-scoring ones until the budget fills, then emit them in
    // original order. This drops irrelevant filler BETWEEN relevant sentences
    // — something the contiguous line-window below cannot do — so the same
    // budget carries more answer-bearing text. We only take this path for
    // natural-language passages (few newlines relative to length); code and
    // config keep the contiguous window path, where inter-line contiguity is
    // load-bearing for readability. Disable via AGENTTY_RAG_EXTRACTIVE=0.
    auto extractive_enabled = [] {
        const char* v = std::getenv("AGENTTY_RAG_EXTRACTIVE");
        return !(v && (v[0] == '0' || v[0] == 'f' || v[0] == 'F'));
    };
    const std::size_t newline_count =
        static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    const bool prose_shaped =
        !terms.empty() && text.size() > 0
        && newline_count * 40 < text.size();   // avg line > 40 chars ⇒ prose
    if (extractive_enabled() && prose_shaped) {
        // Split into sentences on ., !, ? and hard newlines. Keep the
        // delimiter with its sentence so reassembly reads naturally.
        struct Sent { std::size_t begin, end; };
        std::vector<Sent> sents;
        std::size_t s = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            const bool boundary =
                (c == '.' || c == '!' || c == '?' || c == '\n')
                && (i + 1 >= text.size()
                    || text[i + 1] == ' ' || text[i + 1] == '\n'
                    || text[i + 1] == '\t');
            if (boundary) { sents.push_back({s, i + 1}); s = i + 1; }
        }
        if (s < text.size()) sents.push_back({s, text.size()});

        if (sents.size() > 2) {
            // Rarer query terms are more discriminative: weight a term by
            // 1/(1+corpus-frequency-in-this-passage). Cheap in-passage IDF
            // proxy — no global stats needed, and it stops a common term
            // ('the', 'code') from dominating the score.
            std::vector<std::size_t> tf(terms.size(), 0);
            std::string lowered{text};
            for (char& c : lowered)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (std::size_t ti = 0; ti < terms.size(); ++ti) {
                std::size_t at = 0;
                while ((at = lowered.find(terms[ti], at)) != std::string::npos) {
                    ++tf[ti]; at += terms[ti].size();
                }
            }
            auto sent_score = [&](const Sent& se) -> double {
                double sc = 0.0;
                std::string_view seg = text.substr(se.begin, se.end - se.begin);
                std::string low{seg};
                for (char& c : low)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (std::size_t ti = 0; ti < terms.size(); ++ti)
                    if (low.find(terms[ti]) != std::string::npos)
                        sc += 1.0 / (1.0 + static_cast<double>(tf[ti]));
                // Length-normalise lightly so a long sentence doesn't win on
                // surface area alone, but don't over-penalise (sqrt).
                return sc / std::sqrt(static_cast<double>(seg.size()) + 1.0);
            };
            std::vector<std::size_t> order(sents.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                [&](std::size_t a, std::size_t b) {
                    return sent_score(sents[a]) > sent_score(sents[b]);
                });
            // Greedily admit top sentences until the budget fills. If the very
            // top sentence scores 0 (no query overlap anywhere), bail to the
            // contiguous path — extraction has no signal to work with.
            if (sent_score(sents[order[0]]) > 0.0) {
                std::vector<char> keep(sents.size(), false);
                std::size_t used = 0;
                const std::size_t sep = 1;   // newline between kept sentences
                for (std::size_t k = 0; k < order.size(); ++k) {
                    const auto& se = sents[order[k]];
                    std::size_t len = se.end - se.begin;
                    if (used + len + sep > budget) {
                        if (used == 0) break;   // first is already over budget
                        continue;               // skip, try a smaller one
                    }
                    keep[order[k]] = true;
                    used += len + sep;
                }
                std::string out;
                out.reserve(used + 8);
                bool gap = false;
                for (std::size_t i = 0; i < sents.size(); ++i) {
                    if (!keep[i]) { gap = true; continue; }
                    if (!out.empty()) {
                        if (gap) { out += " … "; gap = false; }
                        else if (out.back() != '\n' && out.back() != ' ')
                            out += ' ';
                    }
                    std::string_view seg = text.substr(sents[i].begin,
                                                        sents[i].end - sents[i].begin);
                    // Trim a leading space the split may have left attached.
                    while (!seg.empty() && (seg.front() == ' ' || seg.front() == '\n'))
                        seg.remove_prefix(1);
                    out.append(seg);
                }
                if (!out.empty()) {
                    if (out.size() > budget) out.resize(utf8_boundary(out, budget));
                    return out;
                }
            }
        }
    }

    // ── Contiguous line-window fallback (code / config / no-signal prose) ──
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size(); else ++end;
        lines.emplace_back(start, end);
        start = end;
    }
    auto line_score = [&](std::size_t i) {
        std::string lower{text.substr(lines[i].first, lines[i].second - lines[i].first)};
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::size_t score = 0;
        for (const auto& term : terms) if (lower.find(term) != std::string::npos) ++score;
        return score;
    };
    std::size_t best = 0, best_score = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (auto score = line_score(i); score > best_score) { best = i; best_score = score; }

    std::size_t first = best, last = best + 1;
    while (true) {
        bool grew = false;
        if (first > 0 && lines[last - 1].second - lines[first - 1].first <= budget) {
            --first; grew = true;
        }
        if (last < lines.size() && lines[last].second - lines[first].first <= budget) {
            ++last; grew = true;
        }
        if (!grew) break;
    }
    std::size_t begin = lines[first].first;
    std::size_t end = lines[last - 1].second;
    if (end - begin > budget) {
        // A minified/generated single line: center the excerpt on the first
        // query term rather than blindly keeping an unrelated prefix.
        std::string lower{text};
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::size_t hit = std::string::npos;
        for (const auto& term : terms) {
            hit = lower.find(term);
            if (hit != std::string::npos) break;
        }
        if (hit == std::string::npos) hit = begin;
        begin = hit > budget / 3 ? hit - budget / 3 : 0;
        end = std::min(text.size(), begin + budget);
    }
    begin = utf8_boundary(text, begin);
    end = utf8_boundary(text, end);
    std::string out;
    if (begin > 0) out += "…\n";
    out.append(text.substr(begin, end - begin));
    if (end < text.size()) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "…";
    }
    if (out.size() > budget) out.resize(utf8_boundary(out, budget));
    return out;
}

// ── FeedbackStore ─────────────────────────────────────────────────────────
//   The learning loop, made real. A single process-wide store backs BOTH the
//   write side (feedback::note_surfaced / note_file_opened free functions) and
//   the read side (Retriever::Impl::feedback_boost). It persists an append-only
//   TSV of `use`/`win` events to <cwd>/.agentty/rag_feedback.tsv and folds the
//   Beta-smoothed per-path win-rate back into ranking as a BOUNDED nudge.
//
//   Signal semantics (matches docs/website/retrieval.md):
//     use — search_docs surfaced this path (denominator).
//     win — the agent then `read` a path that was RECENTLY surfaced
//           (numerator). A read of a never-surfaced file is NOT a win: it
//           carries no relevance judgment about retrieval.
//
//   The nudge is a multiplicative factor in [1-kMax, 1+kMax] centred on 1.0,
//   scaled by how far the smoothed win-rate sits from the neutral prior and by
//   a confidence term that grows with sample count. Paths with no history are
//   untouched, so the loop can only help once it has evidence.
class FeedbackStore {
public:
    static FeedbackStore& instance() {
        static FeedbackStore s;
        return s;
    }

    // Ranking nudge for `path`, in roughly [0.85, 1.15]. 1.0 (neutral) when
    // learning is off, the path is unseen, or it has too little evidence.
    float boost(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        ensure_loaded_();
        auto it = counts_.find(path);
        if (it == counts_.end()) return 1.0f;
        const double uses = it->second.uses;
        const double wins = it->second.wins;
        if (uses <= 0.0) return 1.0f;
        // Beta(1,1) smoothing: (wins+1)/(uses+2) — a bounded win-rate that
        // starts at the 0.5 prior and moves only as evidence accrues.
        const double rate = (wins + 1.0) / (uses + 2.0);
        // Confidence in the estimate grows with sample count (saturating).
        const double conf = uses / (uses + kPriorN);
        const double delta = (rate - 0.5) * 2.0;         // → [-1, 1]
        double factor = 1.0 + kMax * delta * conf;
        if (factor < 1.0 - kMax) factor = 1.0 - kMax;
        if (factor > 1.0 + kMax) factor = 1.0 + kMax;
        return static_cast<float>(factor);
    }

    // Record that these paths were surfaced by a retrieval (each a "use"), and
    // remember them as recently-surfaced so a following read can be a win.
    void note_surfaced(const std::vector<std::string>& paths) {
        std::lock_guard<std::mutex> lk(mu_);
        ensure_loaded_();
        std::vector<std::string> to_write;
        for (const auto& p : paths) {
            if (p.empty()) continue;
            counts_[p].uses += 1.0;
            recent_.insert(p);
            to_write.push_back(p);
        }
        append_("use", to_write);
    }

    // Record that `path` was opened. It is a WIN only if it (or its basename)
    // was recently surfaced — otherwise the read tells us nothing about
    // retrieval quality and is ignored (no denominator inflation either).
    void note_opened(const std::string& path) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        ensure_loaded_();
        std::string matched = match_recent_(path);
        if (matched.empty()) return;               // not surfaced ⇒ not a signal
        counts_[matched].wins += 1.0;
        append_("win", {matched});
    }

private:
    struct Tally { double uses = 0.0; double wins = 0.0; };

    static constexpr double kMax    = 0.15;   // max ±15% ranking nudge
    static constexpr double kPriorN = 4.0;    // uses needed for ~half confidence

    std::mutex mu_;
    bool loaded_ = false;
    std::string loaded_for_;                   // cwd the TSV was loaded for
    std::unordered_map<std::string, Tally> counts_;
    std::unordered_set<std::string> recent_;   // surfaced this session

    fs::path tsv_path_() const {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) return {};
        return cwd / ".agentty" / "rag_feedback.tsv";
    }

    // Load (once per cwd) the persisted counts so nudges survive restarts.
    void ensure_loaded_() {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        std::string cwds = ec ? std::string{} : cwd.string();
        if (loaded_ && loaded_for_ == cwds) return;
        counts_.clear();
        loaded_ = true;
        loaded_for_ = cwds;
        auto p = tsv_path_();
        if (p.empty()) return;
        std::ifstream f(p);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            // <epoch>\t<use|win>\t<path>
            auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            auto t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::string kind = line.substr(t1 + 1, t2 - t1 - 1);
            std::string path = line.substr(t2 + 1);
            if (path.empty()) continue;
            if (kind == "use") counts_[path].uses += 1.0;
            else if (kind == "win") counts_[path].wins += 1.0;
        }
    }

    void append_(const char* kind, const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        auto p = tsv_path_();
        if (p.empty()) return;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::app);
        if (!f) return;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        for (const auto& path : paths)
            f << now << '\t' << kind << '\t' << path << '\n';
    }

    // A read matches a surfaced passage if the exact path is recent, or if a
    // recent path ends with the same basename (docs://foo/bar.md surfaced,
    // agent reads foo/bar.md or an absolute .../foo/bar.md).
    std::string match_recent_(const std::string& opened) {
        if (auto it = recent_.find(opened); it != recent_.end()) return opened;
        auto slash = opened.find_last_of("/\\");
        std::string base = slash == std::string::npos ? opened
                                                       : opened.substr(slash + 1);
        if (base.empty()) return {};
        for (const auto& r : recent_) {
            if (r.size() >= base.size() &&
                r.compare(r.size() - base.size(), base.size(), base) == 0) {
                // Guard against a bare-basename false match on a longer name.
                if (r.size() == base.size() ||
                    r[r.size() - base.size() - 1] == '/' ||
                    r[r.size() - base.size() - 1] == '\\')
                    return r;
            }
        }
        return {};
    }
};

} // namespace

Config Config::from_env() {
    Config c;
    if (const char* d = std::getenv("AGENTTY_DOCS_DIR"); d && d[0]) c.docs_root = d;
    // Embedder selection + endpoint, including the legacy AGENTTY_OLLAMA_HOST /
    // AGENTTY_EMBED_MODEL names. One owner for the parsing.
    ::agentty::rag::embed::apply_env(c.embed);
    c.skills        = truthy_default_on("AGENTTY_RAG_SKILLS");
    c.memory        = truthy_default_on("AGENTTY_RAG_MEMORY");
    c.mcp_resources = truthy_default_off("AGENTTY_RAG_MCP");

    c.contextual  = truthy_default_on("AGENTTY_RAG_CONTEXTUAL");
    c.mmr         = truthy_default_on("AGENTTY_RAG_MMR");
    c.mmr_lambda  = env_float("AGENTTY_RAG_MMR_LAMBDA", 0.65f);
    c.stitch      = truthy_default_on("AGENTTY_RAG_STITCH");
    c.dedup       = truthy_default_on("AGENTTY_RAG_DEDUP");
    c.dedup_threshold = env_float("AGENTTY_RAG_DEDUP_THRESHOLD", 0.92f);
    c.autocut     = truthy_default_on("AGENTTY_RAG_AUTOCUT");
    c.autocut_sensitivity = env_float("AGENTTY_RAG_AUTOCUT_SENSITIVITY", 2.0f);
    if (const char* f = std::getenv("AGENTTY_RAG_FUSION"); f && f[0]) {
        std::string fv{f};
        for (char& ch : fv) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        // Accept only the two modes we implement; anything else keeps the
        // convex default rather than silently disabling fusion.
        if (fv == "rrf" || fv == "convex") c.fusion = fv;
    }
    c.adaptive_fusion = truthy_default_on("AGENTTY_RAG_ADAPTIVE");
    c.prf         = truthy_default_off("AGENTTY_RAG_PRF");
    c.corrective  = truthy_default_off("AGENTTY_RAG_CORRECT");
    c.graph       = truthy_default_off("AGENTTY_RAG_GRAPH");
    // Generative query expansion has a measurable latency cost even when its
    // model is local. It is an explicit power-mode, not a tax on every turn.
    c.expand      = truthy_default_off("AGENTTY_RAG_EXPAND");
    c.hyde        = truthy_default_off("AGENTTY_RAG_HYDE");
    if (const char* g = std::getenv("AGENTTY_RAG_GEN_MODEL"); g && g[0]) c.gen_model = g;
    c.persist     = truthy_default_on("AGENTTY_RAG_PERSIST");
    c.learn       = truthy_default_off("AGENTTY_RAG_LEARN");
    // The per-stage retrieval trace is the engine's "show your work": it powers
    // the funnel the tool card renders, so it is ON by default. AGENTTY_RAG_TRACE=0
    // suppresses it (compact one-line mode header only).
    c.trace       = truthy_default_on("AGENTTY_RAG_TRACE");
    c.dense_weight = env_float("AGENTTY_RAG_DENSE_WEIGHT", 1.0f);
    c.bm25_weight  = env_float("AGENTTY_RAG_BM25_WEIGHT", 1.0f);
    // Proactive / "fork" behaviour (consumed by the tools backend).
    // Pre-turn injection is an EXPLICIT opt-in: it spends model-context
    // tokens before every knowledge-shaped turn without the user asking,
    // so it defaults OFF (matching the docs + proactive_enabled()'s
    // "explicit opt-in" contract + the mcp_tools_backends header). The
    // user turns it on via the Ctrl+K → RAG picker (persisted, which sets
    // configured=true so this env default no longer governs) or
    // AGENTTY_RAG_PROACTIVE=1 for a one-off run.
    c.proactive          = truthy_default_off("AGENTTY_RAG_PROACTIVE");
    c.proactive_min_conf = static_cast<double>(env_float("AGENTTY_RAG_PROACTIVE_MIN", 0.35f));
    c.proactive_bytes    = static_cast<int>(env_float("AGENTTY_RAG_PROACTIVE_BYTES", 6144.0f));
    return c;
}

// ─────────────────────────────────────────────────────────────────────────
// Dense-embedder availability. Three states, spelled as three types:
//
//   Unprobed    — no probe has run yet for the current config.
//   Ready       — an embedder answered; carries the MEASURED dimension, the
//                 vector-space identity, and the observed latency.
//   Unavailable — carries WHY. This is the whole point: retrieval silently
//                 degrading to BM25 with no user-visible signal was this
//                 subsystem's worst failure mode.
namespace dense {
struct Unprobed {};
struct Ready {
    std::uint64_t identity   = 0;
    std::uint32_t dim        = 0;
    int           latency_ms = 0;
};
struct Unavailable { std::string reason; };
}  // namespace dense
using DenseState = std::variant<dense::Unprobed, dense::Ready, dense::Unavailable>;

// ─────────────────────────────────────────────────────────────────────────
struct Retriever::Impl {
    Config cfg = Config::from_env();

    std::mutex mu;
    ::rag::Engine engine;
    bool   embedder_ready = false;
    // The dense-embedder state machine. Was `ollama_probed` + `ollama_ready`:
    // two bools encoding three states, with the invariants living only in
    // comments. Now a sum type whose failure arm carries the reason string the
    // picker and `agentty diagnostics` render.
    DenseState dense{dense::Unprobed{}};
    // Freshness of the docs index: (root, fingerprint) it was built for.
    std::string indexed_root;
    std::uint64_t indexed_fp = 0;
    bool docs_initialized = false;
    std::unordered_map<std::string, std::uint64_t> docs_files;
    std::string persist_checked_for;
    // Content fingerprints, not item counts: editing a skill or replacing a
    // memory record without changing cardinality must invalidate retrieval.
    std::uint64_t skills_gen = 0;
    std::uint64_t memory_gen = 0;
    std::uint64_t mcp_gen = 0;

    // Skills/memory-only engine for latency-budgeted proactive retrieval. It is
    // deliberately separate so a cold warm-only query can never walk, rebuild,
    // or discard the docs corpus.
    ::rag::Engine warm_engine;
    bool warm_initialized = false;
    std::uint64_t warm_skills_gen = 0;
    std::uint64_t warm_memory_gen = 0;

    std::atomic<bool> warming{false};

    // Cooperative-cancellation flag for the warm worker. Set by the worker's
    // std::stop_token (tripped when the jthread is re-seated or destroyed) AND
    // by Retriever::shutdown() at process teardown. refresh_docs()/reindex()
    // poll warm_cancelled() at every loop boundary so a multi-second embed
    // pass unwinds PROMPTLY instead of blocking warmer.join() — which used to
    // run at STATIC destruction (after main returns) and stall ^C for 4–10 s.
    std::atomic<bool> warm_stop{false};
    [[nodiscard]] bool warm_cancelled() const noexcept {
        return warm_stop.load(std::memory_order_relaxed);
    }

    // Optional LLM seam for HyDE / multi-query (agentty's provider).
    Retriever::Generator generator;

    // Separate engine for search_code (cwd source tree), with its own
    // edit-drift fingerprint. Kept apart from the docs engine so a docs
    // reindex never disturbs code search and vice-versa.
    ::rag::Engine code_engine{::rag::index::CorpusConfig{}};
    bool          code_embedder_ready = false;
    bool          code_initialized = false;
    std::string code_root;
    std::unordered_map<std::string, std::uint64_t> code_files;

    // LAST data member ON PURPOSE: members destroy in reverse declaration
    // order, so the jthread's destructor (which JOINS the warm worker) runs
    // FIRST in ~Impl — the worker can never observe a partially-destroyed
    // Impl. Declaring it any earlier means everything below it dies while
    // the worker may still be running. Keep it last.
    std::jthread warmer;

    Impl() : engine(make_engine_config()) {
        probe_embedder();
        attach_embedder();
        apply_pipeline(engine);
        install_default_generator();
    }

    ~Impl() {
        if (warmer.joinable()) warmer.join();
    }

    // Install a ZERO-COST local generator for HyDE / multi-query: a tiny model
    // on a local Ollama. No cloud tokens, no auth, no provider plumbing. If
    // Ollama isn't up the call fails fast and HyDE/expand no-op (plain hybrid
    // still runs) — free when unavailable, a recall win when present. An
    // explicit set_generator() overrides it.
    //
    // Note this is deliberately INDEPENDENT of the embedding backend: you can
    // embed through a corporate endpoint while a local Ollama (if any) does
    // query expansion. It dials the embed endpoint only when that endpoint is
    // itself an Ollama, otherwise the historical localhost default.
    void install_default_generator() {
        namespace eb = ::agentty::rag::embed;
        const bool embed_is_ollama =
            cfg.embed.backend == eb::Backend::Ollama
            || cfg.embed.backend == eb::Backend::Auto;
        std::string   host  = embed_is_ollama ? cfg.embed.host : std::string{"127.0.0.1"};
        std::uint16_t port  = embed_is_ollama ? cfg.embed.port : std::uint16_t{11434};
        std::string   model = cfg.gen_model;
        generator = [host, port, model](const std::string& prompt, int n)
                        -> std::vector<std::string> {
            std::vector<std::string> outs;
            const int want = n > 0 ? n : 1;
            try {
                for (int i = 0; i < want; ++i) {
                    ::rag::plugin::Json body = {
                        {"model", model},
                        {"prompt", prompt},
                        {"stream", false},
                        {"options", {{"temperature", i == 0 ? 0.0 : 0.7},
                                      {"num_predict", 160}}},
                    };
                    ::agentty::http::Request req;
                    req.method    = ::agentty::http::HttpMethod::Post;
                    req.host      = host;
                    req.port      = port;
                    req.path      = "/api/generate";
                    req.plaintext = true;   // local Ollama speaks plain HTTP/1.1
                    req.headers.push_back({"content-type", "application/json"});
                    req.body      = body.dump();
                    req.max_body_bytes = 512 * 1024;

                    ::agentty::http::Timeouts to;
                    to.connect = std::chrono::milliseconds(600);   // Ollama absent → fail fast
                    to.total   = std::chrono::milliseconds(4000);  // hard cap per hypothetical
                    auto res = ::agentty::http::default_client().send(req, to);
                    if (!res || res->status != 200) break;   // Ollama down → give up
                    auto j = ::rag::plugin::Json::parse(res->body, nullptr, false);
                    if (j.is_discarded() || !j.contains("response")) continue;
                    std::string text = j["response"].get<std::string>();
                    if (!text.empty()) outs.push_back(std::move(text));
                }
            } catch (...) { /* best-effort; empty → plain hybrid */ }
            return outs;
        };
    }

    ::rag::index::CorpusConfig make_engine_config(bool source_code = false) {
        ::rag::index::CorpusConfig cc;
        cc.contextual = cfg.contextual;
        if (source_code)
            cc.chunking = ::rag::index::CorpusConfig::Chunking::source;
        return cc;
    }

    // Compose the FULL-POWER retrieval pipeline agentty drives, per Config.
    // This mirrors rag-cpp's own Pipeline::best() funnel, adapted to agentty's
    // per-stage toggles:
    //   [PRF expand] → hybrid(adaptive-convex fusion) → filter
    //   → feature-rerank → [dedup] → [MMR diversity] → [parent stitch]
    //   → [autocut] → top-k
    // Convex (TM2C2) fusion is rag-cpp's measured-better default over RRF; the
    // adaptive variant additionally re-weights per query toward whichever
    // retriever is more confident. dedup folds near-duplicate passages and
    // autocut trims the low-relevance tail — both straight from best().
    void apply_pipeline(::rag::Engine& eng) {
        namespace pl = ::rag::pipeline;
        pl::HybridRetrieveConfig hy;
        hy.candidate_k  = 60;
        hy.bm25_weight  = std::max(0.0f, cfg.bm25_weight);
        hy.dense_weight = std::max(0.0f, cfg.dense_weight);
        if (cfg.fusion == "rrf") {
            // Weighted reciprocal-rank fusion — the ONLY mode that honours the
            // public bm25/dense weights (convex ignores them by design).
            hy.fusion = pl::HybridRetrieveConfig::Fusion::rrf;
        } else {
            // Convex (TM2C2) is the default: preserves score distributions RRF
            // discards. Adaptive shifts alpha per query toward the retriever
            // with the sharper (more confident) score curve.
            hy.fusion            = pl::HybridRetrieveConfig::Fusion::convex;
            hy.convex.adaptive   = cfg.adaptive_fusion;
        }

        pl::Pipeline p;
        if (cfg.prf)
            p.add(std::make_shared<pl::PrfExpandStage>(pl::ExpandConfig{}));
        p.add(std::make_shared<pl::HybridRetrieveStage>(hy));
        p.add(std::make_shared<pl::FilterStage>());
        // The exact accuracy-preserving feature reranker the built-in
        // standard()/quality()/context() pipelines use (deterministic, no model).
        p.add(pl::make_feature_rerank_stage());
        // Fold near-duplicate passages BEFORE diversity/stitch so an LLM context
        // window isn't spent re-reading paraphrased copies of the same content.
        if (cfg.dedup) {
            ::rag::rerank::DedupConfig dc;
            dc.threshold = cfg.dedup_threshold;
            p.add(::rag::rerank::make_dedup_stage(dc));
        }
        if (cfg.mmr)
            p.add(::rag::rerank::make_mmr_stage(cfg.mmr_lambda));
        if (cfg.stitch)
            p.add(std::make_shared<pl::ParentStitchStage>(1));
        // Autocut runs on the FINAL relevance order, right before top-k: it
        // trims the low-relevance tail at the score knee so a query with three
        // strong answers returns three, not k padded with weak matches.
        if (cfg.autocut) {
            ::rag::rerank::AutocutConfig ac;
            ac.sensitivity = cfg.autocut_sensitivity;
            p.add(::rag::rerank::make_autocut_stage(ac));
        }
        p.add(std::make_shared<pl::TopKStage>());
        eng.with_pipeline(std::move(p));
    }

    ::rag::plugin::Json embedder_spec() const {
        const auto s = ::agentty::rag::embed::spec_json(effective_embed());
        return s ? ::rag::plugin::Json::parse(*s) : ::rag::plugin::Json{};
    }

    // The embed config as it should be DIALLED right now. Auto resolves to
    // Ollama (the historical default); everything else is taken literally.
    [[nodiscard]] ::agentty::rag::embed::EmbedConfig effective_embed() const {
        return cfg.embed;
    }

    // Probe the configured embedder once per config. Replaces the old
    // `ollama_probed`/`ollama_ready` bool pair: the outcome is a sum type that
    // CARRIES ITS REASON, so a failure can be shown in the UI instead of
    // silently degrading retrieval to BM25 forever (the single worst failure
    // mode this subsystem had).
    void probe_embedder() {
        if (!std::holds_alternative<dense::Unprobed>(dense)) return;
        probe_embedder_unlocked();
    }

    // The probe itself, with no "already probed?" guard and no expectation
    // that `mu` is held. apply_config() calls this with the lock RELEASED,
    // because a probe dials a network endpoint (or memory-maps a model file)
    // and holding the retriever's mutex across that stalls every reader,
    // including the UI thread's status row.
    //
    // `cfg.embed` is read into a local before the call and the result written
    // back after, so the unlocked window touches no shared state.
    void probe_embedder_unlocked() {
        namespace eb = ::agentty::rag::embed;
        const auto ec = effective_embed();
        if (ec.backend == eb::Backend::Disabled) {
            dense = dense::Unavailable{"embeddings disabled"};
            ::agentty::util::dbglog("rag.embed", "disabled; using BM25");
            return;
        }
        auto r = eb::probe(ec);
        if (const auto* ok = std::get_if<eb::ProbeOk>(&r)) {
            // The MEASURED dimension is authoritative — pin it so every
            // subsequent spec (and the identity hash) carries it.
            cfg.embed.dim = ok->dim;
            dense = dense::Ready{eb::identity(cfg.embed), ok->dim, ok->latency_ms};
            ::agentty::util::dbglog("rag.embed",
                "ready: " + eb::describe(cfg.embed) + " ("
                + std::to_string(ok->dim) + "d)");
        } else {
            dense = dense::Unavailable{std::get<eb::ProbeErr>(r).why};
            ::agentty::util::dbglog("rag.embed",
                "unavailable: " + std::get<dense::Unavailable>(dense).reason
                + "; using BM25");
        }
    }

    [[nodiscard]] bool dense_ready() const noexcept {
        return std::holds_alternative<dense::Ready>(dense);
    }

    void attach_embedder() {
        embedder_ready = false;
        if (!dense_ready()) return;
        embedder_ready = engine.with_embedder_spec(embedder_spec()).has_value();
    }

    static void hash_text(std::uint64_t& h, std::string_view value) {
        for (unsigned char c : value) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xff; h *= 1099511628211ull;
    }

    std::uint64_t skills_fingerprint() const {
        std::uint64_t h = 1469598103934665603ull;
        if (!cfg.skills) return h;
        for (const auto& s : tools::skills::all()) {
            hash_text(h, s.name);
            hash_text(h, s.body);
        }
        return h;
    }

    std::uint64_t memory_fingerprint() const {
        std::uint64_t h = 1469598103934665603ull;
        if (!cfg.memory) return h;
        for (auto scope : {tools::memory::Scope::User, tools::memory::Scope::Project})
            for (const auto& r : tools::memory::load_all(scope)) {
                hash_text(h, r.id);
                hash_text(h, r.text);
            }
        return h;
    }

    std::uint64_t mcp_fingerprint() const {
        std::uint64_t h = 1469598103934665603ull;
        if (!cfg.mcp_resources) return h;
        for (const auto& r : ::agentty::mcp::mcp_resources()) {
            hash_text(h, r.uri);
            hash_text(h, r.title);
            hash_text(h, r.description);
            hash_text(h, r.mime_type);
        }
        return h;
    }

    void ensure_warm_index() {
        const auto sfp = skills_fingerprint();
        const auto mfp = memory_fingerprint();
        if (warm_initialized
            && sfp == warm_skills_gen && mfp == warm_memory_gen) return;
        warm_engine = ::rag::Engine(make_engine_config());
        if (dense_ready()) (void)warm_engine.with_embedder_spec(embedder_spec());
        if (cfg.skills)
            for (const auto& s : tools::skills::all())
                if (!s.body.empty())
                    (void)warm_engine.add("skill://" + s.name, s.body,
                                          {{"kind", "skill"}}, s.name);
        if (cfg.memory)
            for (auto scope : {tools::memory::Scope::User, tools::memory::Scope::Project})
                for (const auto& r : tools::memory::load_all(scope))
                    if (!r.text.empty())
                        (void)warm_engine.add("memory://" + r.id, r.text,
                            {{"kind", "memory"},
                             {"scope", std::string(tools::memory::to_string(scope))}});
        (void)warm_engine.build();
        apply_pipeline(warm_engine);
        warm_skills_gen = sfp;
        warm_memory_gen = mfp;
        warm_initialized = true;
    }

    // Rebuild the whole engine from scratch for the current source set. Called
    // under `mu`. `skip_docs` means "don't WALK the docs folder" — but any docs
    // already indexed for `indexed_root` are preserved (re-loaded from that
    // folder without a freshness walk), so the warm/proactive path never loses
    // the docs corpus. Cheap paths (unchanged fp/gen) return early via
    // needs_reindex().
    void reindex(const fs::path& root, bool skip_docs) {
        engine = ::rag::Engine(make_engine_config());
        embedder_ready = false;
        attach_embedder();

        // Docs / knowledge folder. On the warm path (skip_docs) we still index
        // the LAST KNOWN docs root if there was one — we just skip the
        // fingerprint walk that would detect drift. This keeps docs available
        // to proactive retrieval without paying the freshness scan.
        fs::path docs_root = root;
        if (skip_docs && docs_root.empty() && !indexed_root.empty())
            docs_root = fs::path{indexed_root};
        if (!docs_root.empty()) {
            ::rag::loaders::DirOptions opts = docs_dir_options();
            auto docs = ::rag::loaders::load_directory(docs_root, opts);
            if (docs) {
                for (auto& d : *docs) {
                    std::string rel = d.meta.count("rel") ? d.meta["rel"] : d.uri;
                    rel = fs::path{rel}.generic_string();
                    d.meta["rel"] = rel;
                    (void)engine.add("docs://" + rel, std::move(d.text), d.meta, d.title);
                }
            }
        }

        // Skills.
        if (cfg.skills) {
            for (const auto& s : tools::skills::all()) {
                if (s.body.empty()) continue;
                (void)engine.add("skill://" + s.name, s.body, {{"kind", "skill"}}, s.name);
            }
        }

        // Learned memory (both scopes).
        if (cfg.memory) {
            for (auto scope : {tools::memory::Scope::User, tools::memory::Scope::Project}) {
                for (const auto& r : tools::memory::load_all(scope)) {
                    if (r.text.empty()) continue;
                    (void)engine.add("memory://" + r.id, r.text,
                               {{"kind", "memory"}, {"scope", std::string(tools::memory::to_string(scope))}});
                }
            }
        }

        // Connected MCP resources are explicitly opt-in because reading them
        // may involve server I/O. Failures are isolated per resource.
        if (cfg.mcp_resources) {
            for (const auto& r : ::agentty::mcp::mcp_resources()) {
                if (warm_cancelled()) return;
                std::string err;
                auto text = ::agentty::mcp::mcp_read_resource(r.uri, err);
                if (!text || text->empty()) continue;
                (void)engine.add("mcp://" + r.uri, std::move(*text),
                    {{"kind", "mcp"}, {"server", r.server},
                     {"mime", r.mime_type}}, r.title);
            }
        }

        // The embed pass below can take seconds on a large corpus; bail here
        // so process teardown / a re-seated warm doesn't block on it.
        if (warm_cancelled()) return;
        auto built = engine.build();
        if (!built)
            ::agentty::util::dbglog("rag.build", std::string(::rag::to_string(built.error().code)));
        apply_pipeline(engine);

        if (!skip_docs) {
            indexed_root = root.string();
            ::rag::loaders::DirOptions opts = docs_dir_options();
            docs_files = file_manifest(root, opts);
            indexed_fp = manifest_fingerprint(docs_files);
            docs_initialized = true;
        } else if (indexed_root.empty() && !root.empty()) {
            indexed_root = root.string();
        }
        skills_gen = skills_fingerprint();
        memory_gen = memory_fingerprint();
        mcp_gen = mcp_fingerprint();

        // Persist the built corpus and a source/config manifest. The manifest
        // prevents a warm open from serving an index built for another root,
        // source generation, chunking profile, or embedding model.
        persist_index();
    }

    // Atomically publish text to `dest`: write a sibling temp file, flush, and
    // rename over the target. rename(2) is atomic on the same filesystem, so a
    // reader never observes a half-written .meta.json and a crash mid-write
    // leaves the previous good file (or none) intact — never a truncated one
    // that would desync the meta from its .ragdb.
    static bool atomic_write(const fs::path& dest, const std::string& body) {
        if (dest.empty()) return false;
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        fs::path tmp = dest;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << body;
            out.flush();
            if (!out) { fs::remove(tmp, ec); return false; }
        }
        fs::rename(tmp, dest, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    }

    // Where the persisted docs index lives (under the workspace .agentty/).
    // The persisted index path carries the EMBEDDER IDENTITY. Two reasons,
    // one of them a correctness bug this fixes:
    //
    //  1. A .ragdb is only reusable by an embedder producing the SAME vector
    //     geometry. The manifest used to record `embed_model` — a NAME, which
    //     stops identifying the space the moment the backend is pluggable
    //     ("nomic-embed-text" served by Ollama vs loaded in-process as GGUF
    //     differ in pooling/normalization). Reopening one with the other gave
    //     silently wrong cosines: no crash, no warning, just bad retrieval.
    //
    //  2. Because it is in the FILENAME rather than only the manifest,
    //     switching backends SWITCHES between warm indexes instead of
    //     invalidating one — free A/B between a local model and an endpoint.
    fs::path ragdb_path() {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) return {};
        return cwd / ".agentty"
             / ("rag_docs." + ::agentty::rag::embed::identity_tag(cfg.embed) + ".ragdb");
    }

    fs::path ragmeta_path() {
        auto p = ragdb_path();
        return p.empty() ? fs::path{} : fs::path{p.string() + ".meta.json"};
    }

    void persist_index() {
        if (!cfg.persist || cfg.mcp_resources) return;
        auto db = ragdb_path();
        auto meta = ragmeta_path();
        if (db.empty() || meta.empty()) return;
        std::error_code ec;
        fs::create_directories(db.parent_path(), ec);
        auto saved = engine.save(db.string());
        if (!saved) return;
        ::rag::plugin::Json j = {
            {"version", 3}, {"root", indexed_root}, {"docs_fp", indexed_fp},
            {"skills_fp", skills_gen}, {"memory_fp", memory_gen},
            {"contextual", cfg.contextual},
            // Identity of the VECTOR SPACE, not the model's name. See
            // ragdb_path() for why a name is not sufficient.
            {"embed_identity", ::agentty::rag::embed::identity(cfg.embed)},
            {"dense", dense_ready()},
        };
        atomic_write(meta, j.dump());
    }

    bool try_load_persisted(const fs::path& root) {
        if (!cfg.persist || cfg.mcp_resources) return false;
        const std::string root_s = root.string();
        if (persist_checked_for == root_s) return false;
        persist_checked_for = root_s;
        auto db = ragdb_path();
        auto meta = ragmeta_path();
        if (db.empty() || meta.empty()) return false;
        try {
            std::ifstream in(meta);
            if (!in) return false;
            ::rag::plugin::Json j;
            in >> j;
            ::rag::loaders::DirOptions opts = docs_dir_options();
            auto current_files = file_manifest(root, opts);
            const auto current_docs = manifest_fingerprint(current_files);
            const auto current_skills = skills_fingerprint();
            const auto current_memory = memory_fingerprint();
            if (j.value("version", 0) != 3
                || j.value("root", std::string{}) != root_s
                || j.value("docs_fp", std::uint64_t{}) != current_docs
                || j.value("skills_fp", std::uint64_t{}) != current_skills
                || j.value("memory_fp", std::uint64_t{}) != current_memory
                || j.value("contextual", false) != cfg.contextual
                || j.value("embed_identity", std::uint64_t{})
                       != ::agentty::rag::embed::identity(cfg.embed)
                || j.value("dense", false) != dense_ready())
                return false;
            auto opened = ::rag::Engine::open(db.string());
            if (!opened) return false;
            engine = std::move(*opened);
            embedder_ready = false;
            attach_embedder();
            apply_pipeline(engine);
            indexed_root = root_s;
            indexed_fp = current_docs;
            docs_files = std::move(current_files);
            docs_initialized = true;
            skills_gen = current_skills;
            memory_gen = current_memory;
            ::agentty::util::dbglog("rag.persist", "opened warm index " + db.string());
            return true;
        } catch (...) {
            return false;
        }
    }

    bool needs_reindex(const fs::path& root, bool skip_docs) {
        // A changed docs root ALWAYS forces a rebuild — even on the warm path —
        // so proactive retrieval can never serve a stale corpus after the
        // folder is repointed.
        if (!root.empty() && indexed_root != root.string()) return true;
        if (!skip_docs) {
            ::rag::loaders::DirOptions opts = docs_dir_options();
            if (manifest_fingerprint(file_manifest(root, opts)) != indexed_fp) return true;
        }
        if (skills_gen != skills_fingerprint()) return true;
        if (memory_gen != memory_fingerprint()) return true;
        if (mcp_gen != mcp_fingerprint()) return true;
        return false;
    }

    using FileManifest = std::unordered_map<std::string, std::uint64_t>;

    // Bounded, build-aware DirOptions for the docs / knowledge corpus.
    //
    // The rag-cpp DEFAULT DirOptions is UNBOUNDED (max_files = 0) with a 4 MB
    // per-file cap and an exclude list that only knows the common VCS/package
    // dirs. When AGENTTY_DOCS_DIR is pointed at a large repo root (a supported
    // config) the defaults walk and load the ENTIRE tree — vendored deps under
    // _deps/, generated CMakeFiles/, build output, the .agentty cache — into
    // the HNSW dense store, whose backing vector grows to n_chunks * dim. On a
    // very large codebase that allocation is what the OS OOM-kills, and an
    // OOM-kill can't be caught by the try/catch guarding every retrieval entry
    // point. Cap the walk (files + bytes) and skip build/vendor dirs so the
    // docs path is as bounded as the search_code path already is.
    static ::rag::loaders::DirOptions docs_dir_options() {
        ::rag::loaders::DirOptions opts;   // inherit rag-cpp include_ext
        opts.exclude_dirs = {
            ".git",".hg",".svn","node_modules","build","dist","out",
            "target","venv",".venv","__pycache__",".cache","_deps",
            "CMakeFiles",".agentty","vendor","third_party"};
        opts.max_file_bytes = 1 * 1024 * 1024;   // 1 MB: docs, not blobs
        opts.max_files = 6000;                    // hard ceiling on corpus size
        return opts;
    }

    FileManifest file_manifest(const fs::path& root,
                               const ::rag::loaders::DirOptions& opts) {
        FileManifest out;
        if (root.empty()) return out;
        std::unordered_set<std::string> wanted(opts.include_ext.begin(), opts.include_ext.end());
        std::unordered_set<std::string> skipped(opts.exclude_dirs.begin(), opts.exclude_dirs.end());
        std::error_code ec;
        auto flags = fs::directory_options::skip_permission_denied;
        if (opts.follow_symlinks) flags |= fs::directory_options::follow_directory_symlink;
        for (fs::recursive_directory_iterator it(root, flags, ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& e = *it;
            if (e.is_directory(ec)) {
                if (skipped.contains(e.path().filename().string())) it.disable_recursion_pending();
                continue;
            }
            if (!e.is_regular_file(ec)) continue;
            auto ext = e.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!wanted.contains(ext)) continue;
            auto size = e.file_size(ec);
            if (ec || size > opts.max_file_bytes) { ec.clear(); continue; }
            if (opts.max_files > 0 && out.size() >= opts.max_files) break;
            auto mtime = e.last_write_time(ec).time_since_epoch().count();
            if (ec) { ec.clear(); continue; }
            auto rel = fs::relative(e.path(), root, ec).generic_string();
            if (ec) { ec.clear(); continue; }
            std::uint64_t stamp = 1469598103934665603ull;
            hash_text(stamp, rel);
            stamp ^= static_cast<std::uint64_t>(size); stamp *= 1099511628211ull;
            stamp ^= static_cast<std::uint64_t>(mtime); stamp *= 1099511628211ull;
            out.emplace(std::move(rel), stamp);
        }
        return out;
    }

    static std::uint64_t manifest_fingerprint(const FileManifest& files) {
        std::vector<std::pair<std::string, std::uint64_t>> ordered(files.begin(), files.end());
        std::sort(ordered.begin(), ordered.end());
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& [path, stamp] : ordered) {
            hash_text(h, path);
            h ^= stamp; h *= 1099511628211ull;
        }
        return h;
    }

    // Same identity discipline as the docs index — see ragdb_path().
    fs::path code_ragdb_path() const {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        return ec ? fs::path{}
                  : cwd / ".agentty"
                        / ("rag_code." + ::agentty::rag::embed::identity_tag(cfg.embed)
                           + ".ragdb");
    }

    fs::path code_meta_path() const {
        auto db = code_ragdb_path();
        return db.empty() ? fs::path{} : fs::path{db.string() + ".meta.json"};
    }

    bool try_load_code_index(const fs::path& root, const FileManifest& manifest) {
        if (!cfg.persist) return false;
        auto db = code_ragdb_path();
        auto meta = code_meta_path();
        if (db.empty() || meta.empty()) return false;
        try {
            std::ifstream in(meta);
            if (!in) return false;
            ::rag::plugin::Json j;
            in >> j;
            const auto stored = j.value("files", FileManifest{});
            if (j.value("version", 0) != 2
                || j.value("root", std::string{}) != root.string()
                || stored != manifest
                || j.value("contextual", false) != cfg.contextual
                || j.value("embed_identity", std::uint64_t{})
                       != ::agentty::rag::embed::identity(cfg.embed)
                || j.value("dense", false) != dense_ready())
                return false;
            auto opened = ::rag::Engine::open(db.string());
            if (!opened) return false;
            code_engine = std::move(*opened);
            attach_code_embedder();
            apply_pipeline(code_engine);
            code_root = root.string();
            code_files = manifest;
            code_initialized = true;
            return true;
        } catch (...) { return false; }
    }

    void persist_code_index() {
        if (!cfg.persist || !code_initialized) return;
        auto db = code_ragdb_path();
        auto meta = code_meta_path();
        if (db.empty() || meta.empty()) return;
        std::error_code ec;
        fs::create_directories(db.parent_path(), ec);
        if (!code_engine.save(db.string())) return;
        ::rag::plugin::Json j = {
            {"version", 2}, {"root", code_root}, {"files", code_files},
            {"contextual", cfg.contextual},
            {"embed_identity", ::agentty::rag::embed::identity(cfg.embed)},
            {"dense", dense_ready()},
        };
        atomic_write(meta, j.dump());
    }

    void refresh_docs(const fs::path& root) {
        ::rag::loaders::DirOptions opts = docs_dir_options();
        auto manifest = file_manifest(root, opts);
        const auto fp = manifest_fingerprint(manifest);
        const bool non_doc_changed = skills_gen != skills_fingerprint()
                                  || memory_gen != memory_fingerprint()
                                  || mcp_gen != mcp_fingerprint();
        if (!docs_initialized || indexed_root != root.string()
            || non_doc_changed) {
            reindex(root, /*skip_docs=*/false);
            return;
        }
        if (fp == indexed_fp) return;

        std::size_t changed = 0;
        for (const auto& [path, stamp] : manifest) {
            auto old = docs_files.find(path);
            if (old == docs_files.end() || old->second != stamp) ++changed;
        }
        for (const auto& [path, _] : docs_files)
            if (!manifest.contains(path)) ++changed;
        if (changed > std::max<std::size_t>(32, docs_files.size() / 3)) {
            reindex(root, /*skip_docs=*/false);
            return;
        }

        auto remove_uri = [&](const std::string& rel) {
            // O(1) URI→id lookup instead of a linear scan over every document
            // in the corpus — which made incremental docs updates O(changed ×
            // corpus_size) and pathological on large corpora.
            auto& corpus = engine.corpus();
            if (auto id = corpus.find_by_uri("docs://" + rel))
                (void)corpus.remove_document(*id);
        };
        for (const auto& [path, old_stamp] : docs_files) {
            auto now = manifest.find(path);
            if (now == manifest.end() || now->second != old_stamp) remove_uri(path);
        }
        for (const auto& [path, stamp] : manifest) {
            if (warm_cancelled()) return;
            auto old = docs_files.find(path);
            if (old != docs_files.end() && old->second == stamp) continue;
            auto loaded = ::rag::loaders::load_file(root / fs::path{path});
            if (!loaded) continue;
            loaded->meta["rel"] = path;
            // upsert_document atomically replaces any live doc with this uri,
            // so a changed file can't leave a stale duplicate even if the
            // remove_uri above missed a tombstoned entry.
            (void)engine.corpus().upsert_document("docs://" + path,
                             std::move(loaded->text),
                             loaded->meta, loaded->title);
        }
        if (warm_cancelled()) return;
        auto built = engine.build();
        if (!built) {
            reindex(root, /*skip_docs=*/false);
            return;
        }
        apply_pipeline(engine);
        docs_files = std::move(manifest);
        indexed_fp = fp;
        persist_index();
    }

    void attach_code_embedder() {
        code_embedder_ready = false;
        if (!dense_ready()) return;
        code_embedder_ready = code_engine.with_embedder_spec(embedder_spec()).has_value();
    }
};

Retriever::Retriever() : impl_(std::make_unique<Impl>()) {}
Retriever::~Retriever() = default;

void Retriever::set_generator(Generator g) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->generator = std::move(g);
}

Retrieval Retriever::retrieve(const std::string& query, int k, bool skip_docs) {
    Retrieval out;
    if (query.empty()) { out.error = "empty query"; return out; }
    const int kk = k > 0 ? k : 6;
    try {
        std::lock_guard<std::mutex> lock(impl_->mu);
        auto root = skip_docs ? fs::path{}
                              : resolve_docs_root(impl_->cfg.docs_root);

        if (skip_docs) {
            impl_->ensure_warm_index();
        } else {
            if (impl_->engine.corpus().chunk_count() == 0)
                (void)impl_->try_load_persisted(root);
            impl_->refresh_docs(root);
        }

        auto& active_engine = skip_docs ? impl_->warm_engine : impl_->engine;
        const bool any = active_engine.corpus().chunk_count() > 0;
        if (!any) {
            out.error = "no knowledge configured. Set AGENTTY_DOCS_DIR to a "
                        "folder of docs, put files under ./docs, install skills, "
                        "or store memories to give search_docs something to find.";
            return out;
        }

        // ── RETRIEVE ──────────────────────────────────────────────────
        // 1. Base hybrid retrieval through the full pipeline (PRF → convex
        //    fusion → filter → feature-rerank → MMR → parent-stitch → top-k).
        //    Optionally trace each stage for the mode header.
        std::vector<std::string> trace;
        std::vector<std::string>* tracep = impl_->cfg.trace ? &trace : nullptr;
        const std::size_t want = static_cast<std::size_t>(kk);

        std::vector<::rag::SearchResult> hits;
        std::string retriever_mode = active_engine.corpus().has_embedder()
                                   ? "hybrid+ctx" : "bm25+ctx";

        // 2. LLM-assisted retrieval (HyDE / multi-query) when a Generator is
        //    wired AND enabled — closes the query↔document asymmetry gap and
        //    lifts recall. Degrades gracefully to plain search when absent.
        //    SKIPPED on the warm/proactive path (skip_docs): that path is
        //    latency-budgeted (pre-turn hedge) and must not wait on generation.
        bool used_llm = false;
        if (!skip_docs && impl_->generator && (impl_->cfg.hyde || impl_->cfg.expand)) {
            ::rag::query::Generator gen =
                [&](std::string_view prompt) -> ::rag::Result<std::vector<std::string>> {
                    try {
                        int n = impl_->cfg.expand ? 3 : 1;
                        auto outs = impl_->generator(std::string(prompt), n);
                        return outs;
                    } catch (...) {
                        return std::vector<std::string>{};
                    }
                };
            ::rag::Result<std::vector<::rag::Hit>> lh =
                std::unexpected(::rag::Error{});
            if (impl_->cfg.expand)
                lh = ::rag::query::multi_query_search(active_engine.corpus(), query, want, gen, 3);
            else
                lh = ::rag::query::hyde_search(active_engine.corpus(), query, want, gen);
            if (lh && !lh->empty()) {
                for (const auto& h : *lh) hits.push_back(active_engine.corpus().resolve(h));
                used_llm = true;
                retriever_mode += impl_->cfg.expand ? "+multiquery" : "+hyde";
            }
        }

        if (!used_llm) {
            auto res = active_engine.search(query, want, {}, tracep);
            if (!res) { out.error = "retrieval failed"; return out; }
            hits = std::move(*res);
        }

        // 3. Optional GraphRAG expansion. Fuse by reciprocal rank because graph
        // and base scores have different scales; appending after an already-k
        // base list made graph hits either dominate incorrectly or get trimmed
        // without ever surfacing.
        if (impl_->cfg.graph) {
            try {
                auto g = active_engine.graph_local(query, want);
                if (g && !g->empty()) {
                    std::unordered_map<std::string, std::size_t> positions;
                    auto key_of = [](const ::rag::SearchResult& r) {
                        return r.uri + "\n" + std::to_string(r.start_line);
                    };
                    for (std::size_t i = 0; i < hits.size(); ++i) {
                        hits[i].score.value = 1.0f / static_cast<float>(60 + i + 1);
                        positions.emplace(key_of(hits[i]), i);
                    }
                    for (std::size_t i = 0; i < g->size(); ++i) {
                        const float add = 1.0f / static_cast<float>(60 + i + 1);
                        auto key = key_of((*g)[i]);
                        if (auto it = positions.find(key); it != positions.end()) {
                            hits[it->second].score.value += add;
                        } else {
                            (*g)[i].score.value = add;
                            positions.emplace(std::move(key), hits.size());
                            hits.push_back(std::move((*g)[i]));
                        }
                    }
                    std::stable_sort(hits.begin(), hits.end(),
                        [](const auto& a, const auto& b) {
                            return a.score.value > b.score.value;
                        });
                    retriever_mode += "+graph";
                }
            } catch (...) { /* graph optional */ }
        }

        // 4. CRAG corrective grading: a model-free retrieval evaluator that
        //    drops passages graded irrelevant and yields a real confidence in
        //    [0,1]. This turns retrieval from "always inject whatever came
        //    back" into a self-checking step.
        double crag_conf = -1.0;
        if (impl_->cfg.corrective && !hits.empty()) {
            try {
                std::vector<::rag::Hit> raw;
                raw.reserve(hits.size());
                for (const auto& h : hits) raw.push_back(::rag::Hit{h.chunk, h.score});
                ::rag::crag::CragConfig cc;
                cc.strips = want;
                cc.drop_irrelevant = false;
                auto corr = ::rag::crag::correct(active_engine.corpus(), query, raw, cc);
                crag_conf = static_cast<double>(corr.confidence);
                if (!corr.kept.empty()) {
                    // Re-resolve only the kept chunks, preserving CRAG's order.
                    std::vector<::rag::SearchResult> kept;
                    for (const auto& h : corr.kept)
                        kept.push_back(active_engine.corpus().resolve(h));
                    if (!kept.empty()) { hits = std::move(kept); retriever_mode += "+crag"; }
                }
            } catch (...) { /* grading optional */ }
        }

        if (hits.empty()) {
            out.error = "no relevant passages (retrieval graded low-confidence)";
            return out;
        }
        if (hits.size() > want) hits.resize(want);

        // 5. Learning-loop read side: nudge each hit by its historical
        //    Beta-smoothed win-rate (bounded ±15%), then re-sort so a passage
        //    that has repeatedly proven useful in THIS workspace edges ahead of
        //    a near-tied one that hasn't. Neutral (×1.0) for unseen paths, so
        //    this can only refine — never invent — ranking. Off with
        //    AGENTTY_RAG_LEARN=0.
        bool learned = false;
        if (impl_->cfg.learn) {
            auto& fb = FeedbackStore::instance();
            for (auto& h : hits) {
                auto [src, path] = split_uri(h.uri);
                (void)src;
                float b = fb.boost(path);
                if (b != 1.0f) {
                    h.score.value *= b;
                    learned = true;
                }
            }
            if (learned)
                std::stable_sort(hits.begin(), hits.end(),
                                 [](const ::rag::SearchResult& a,
                                    const ::rag::SearchResult& b) {
                                     return a.score.value > b.score.value;
                                 });
        }

        double top = 0.0;
        std::vector<std::string> surfaced;
        surfaced.reserve(hits.size());

        // Relevance floor: drop the low-confidence tail before spending any
        // tokens on it. The top hit always survives; a later hit survives only
        // if it scores within floor_frac of the top. This is where the token
        // saving comes from — the model ignores 0.1-confidence passages, so we
        // don't pay to inject them.
        {
            double hi = 0.0;
            for (const auto& r : hits) hi = std::max(hi, static_cast<double>(r.score.value));
            const double floor = hi * relevance_floor_frac();
            if (hi > 0.0 && floor > 0.0) {
                std::size_t keep = 1; // always keep the best
                while (keep < hits.size()
                       && static_cast<double>(hits[keep].score.value) >= floor)
                    ++keep;
                if (keep < hits.size()) hits.resize(keep);
            }
        }

        // Score-proportional (water-filling) budget: confident passages get
        // room to be complete; kept tail passages get a tight excerpt — instead
        // of the old flat total/n split that spent equal tokens on signal and
        // noise. The TOTAL is first scaled down by CRAG confidence so a
        // barely-passing retrieval injects a cheap block, not the full ~3k tok.
        const std::size_t total_budget =
            confidence_scaled_budget(retrieval_output_budget(), crag_conf);
        std::vector<double> pre_scores;
        pre_scores.reserve(hits.size());
        for (const auto& r : hits) {
            double s = std::clamp(static_cast<double>(r.score.value), 0.0, 1.0);
            pre_scores.push_back(s);
        }
        const std::vector<std::size_t> allowances =
            allocate_budget(pre_scores, total_budget);
        std::size_t remaining_budget = total_budget;
        std::size_t idx = 0;
        for (const auto& r : hits) {
            auto [src, path] = split_uri(r.uri);
            Passage p;
            p.source     = src;
            p.path       = path;
            p.line_start = static_cast<int>(r.start_line);
            p.line_end   = static_cast<int>(r.end_line);
            double s = static_cast<double>(r.score.value);
            if (s < 0.0) s = 0.0;
            if (s > 1.0) s = 1.0;
            p.score      = s;
            std::string raw = r.context.empty() ? r.text : (r.context + "\n" + r.text);
            std::size_t want_bytes = idx < allowances.size() ? allowances[idx] : 768;
            const std::size_t allowance = std::min(want_bytes, remaining_budget);
            ++idx;
            if (allowance < 256) break;
            p.text = compress_passage(query, raw, allowance);
            remaining_budget -= std::min(remaining_budget, p.text.size());
            top = std::max(top, p.score);
            surfaced.push_back(path);
            out.passages.push_back(std::move(p));
        }
        // Learning-loop write side (denominator): record what we surfaced so a
        // later `read` of one of these can be scored as a win.
        if (impl_->cfg.learn && !surfaced.empty())
            FeedbackStore::instance().note_surfaced(surfaced);
        // Prefer CRAG's calibrated confidence when available; else top score.
        out.confidence = crag_conf >= 0.0 ? crag_conf : top;

        // ── The retrieval funnel: show the engine's actual working ──────
        // `mode` is what the tool card renders. We build a compact one-line
        // headline (retriever + fusion + confidence) followed by an OPTIONAL
        // multi-line "funnel" that walks the candidate set through every stage
        // it actually passed through, with the counts rag-cpp itself recorded.
        // The funnel is the UX centrepiece: a user (and the model) can SEE that
        // 47 candidates were retrieved, reranked to 30, deduped to 24, and
        // autocut to 8 — instead of trusting an opaque black box.
        std::string headline = retriever_mode;
        headline += impl_->cfg.fusion == "rrf"
                      ? ", rrf-fusion"
                      : (impl_->cfg.adaptive_fusion ? ", convex-fusion(adaptive)"
                                                    : ", convex-fusion");
        if (learned) headline += ", learned-boost";
        if (!root.empty() && !skip_docs)
            headline += ", docs=" + root.filename().string();
        char buf[48];
        std::snprintf(buf, sizeof buf, ", confidence %.2f", out.confidence);
        headline += buf;

        std::string m = headline;
        if (impl_->cfg.trace && !trace.empty()) {
            // rag-cpp emits two kinds of trace line: diagnostic lines that
            // carry counts ("hybrid: 47 candidates", "dedup 30 -> 24") and bare
            // stage markers ("→ dedup") that just echo the stage name. Keep the
            // former — they show the funnel narrowing — and drop the latter,
            // which are noise once the diagnostics are present.
            std::vector<std::string> steps;
            steps.reserve(trace.size());
            for (const auto& t : trace) {
                if (t.empty()) continue;
                // Bare "→ stagename" markers begin with the arrow; skip them.
                if (t.rfind("→ ", 0) == 0 || t.rfind("-> ", 0) == 0) continue;
                steps.push_back(t);
            }
            if (!steps.empty()) {
                // Render as an indented funnel the card shows verbatim. Each
                // rung is one stage; the arrow prefix reads top-to-bottom as
                // the query flowing down the pipeline.
                m += "\n  funnel:";
                for (const auto& s : steps)
                    m += "\n    ↳ " + s;
                m += "\n    ↳ top-" + std::to_string(out.passages.size());
            }
        }
        out.mode = std::move(m);
    } catch (const std::exception& e) {
        out.error = std::string("retrieval error: ") + e.what();
    } catch (...) {
        out.error = "retrieval error";
    }
    // Retrieval quietly returning nothing looks identical to "the docs have
    // no answer", so a broken index / missing embedder / unreachable Ollama
    // reads as a model failure. Record the outcome and the pipeline that
    // produced it.
    AGT_LOGL(Rag, out.error.empty() ? ::agentty::logx::Level::Debug
                                    : ::agentty::logx::Level::Warn,
             "rag.retrieve", "k={} passages={} mode={} err={}",
             k, out.passages.size(),
             out.mode.empty() ? std::string{"-"} : out.mode,
             out.error.empty() ? std::string{"-"} : out.error);
    return out;
}

Retrieval Retriever::retrieve_code(const std::string& query, int k) {
    Retrieval out;
    if (query.empty()) { out.error = "empty query"; return out; }
    const int kk = k > 0 ? k : 6;
    try {
        std::lock_guard<std::mutex> lock(impl_->mu);
        std::error_code ec;
        auto root = fs::current_path(ec);
        if (ec) { out.error = "search_code: cannot resolve cwd"; return out; }

        ::rag::loaders::DirOptions opts;
        opts.include_ext = {
            ".c",".cc",".cpp",".cxx",".h",".hh",".hpp",".hxx",".inl",
            ".py",".js",".jsx",".ts",".tsx",".mjs",".go",".rs",".java",
            ".kt",".swift",".rb",".php",".cs",".scala",".sh",".bash",
            ".zig",".lua",".sql",".proto",".cmake",".md"};
        opts.exclude_dirs = {
            ".git",".hg",".svn","node_modules","build","dist","out",
            "target","venv",".venv","__pycache__",".cache","_deps",
            "CMakeFiles",".agentty","vendor","third_party"};
        opts.max_file_bytes = 256 * 1024;
        opts.max_files = 4000;

        auto manifest = impl_->file_manifest(root, opts);
        if (!impl_->code_initialized)
            (void)impl_->try_load_code_index(root, manifest);
        const bool cold = !impl_->code_initialized
                       || impl_->code_root != root.string();

        std::size_t changed = 0;
        if (!cold) {
            for (const auto& [path, stamp] : manifest) {
                auto it = impl_->code_files.find(path);
                if (it == impl_->code_files.end() || it->second != stamp) ++changed;
            }
            for (const auto& [path, _] : impl_->code_files)
                if (!manifest.contains(path)) ++changed;
        }
        const bool rebuild = cold
            || changed > std::max<std::size_t>(64, impl_->code_files.size() / 3);

        if (rebuild) {
            impl_->code_engine = ::rag::Engine(
                impl_->make_engine_config(/*source_code=*/true));
            impl_->attach_code_embedder();
            auto files = ::rag::loaders::load_directory(root, opts);
            if (files) {
                for (auto& d : *files) {
                    std::string rel = d.meta.count("rel") ? d.meta["rel"] : d.uri;
                    rel = fs::path{rel}.generic_string();
                    d.meta["rel"] = rel;
                    (void)impl_->code_engine.add("code://" + rel,
                                                 std::move(d.text), d.meta, d.title);
                }
            }
            auto built = impl_->code_engine.build();
            if (!built) { out.error = "search_code: index build failed"; return out; }
            impl_->apply_pipeline(impl_->code_engine);
        } else if (changed > 0) {
            auto remove_uri = [&](const std::string& rel) {
                // O(1) URI→id lookup, not a full-corpus linear scan.
                auto& corpus = impl_->code_engine.corpus();
                if (auto id = corpus.find_by_uri("code://" + rel))
                    (void)corpus.remove_document(*id);
            };

            for (const auto& [path, old_stamp] : impl_->code_files) {
                auto it = manifest.find(path);
                if (it == manifest.end() || it->second != old_stamp)
                    remove_uri(path);
            }
            for (const auto& [path, stamp] : manifest) {
                auto old = impl_->code_files.find(path);
                if (old != impl_->code_files.end() && old->second == stamp) continue;
                auto loaded = ::rag::loaders::load_file(root / fs::path{path});
                if (!loaded) continue;
                loaded->meta["rel"] = path;
                // upsert = dedup-safe replace (see docs path).
                (void)impl_->code_engine.corpus().upsert_document("code://" + path,
                    std::move(loaded->text), loaded->meta, loaded->title);
            }
            auto built = impl_->code_engine.build();
            if (!built) { out.error = "search_code: incremental update failed"; return out; }
            impl_->apply_pipeline(impl_->code_engine);
        }
        impl_->code_root = root.string();
        impl_->code_files = std::move(manifest);
        impl_->code_initialized = true;
        if (rebuild || changed > 0) impl_->persist_code_index();

        if (impl_->code_engine.corpus().chunk_count() == 0) {
            out.error = "search_code: no source files found under " + root.string();
            return out;
        }

        std::vector<std::string> code_trace;
        std::vector<std::string>* code_tracep = impl_->cfg.trace ? &code_trace : nullptr;
        auto res = impl_->code_engine.search(query, static_cast<std::size_t>(kk),
                                             {}, code_tracep);
        if (!res) { out.error = "search_code failed"; return out; }
        double top = 0.0;
        // Relevance floor + score-proportional budget (same rationale as the
        // docs path): don't pay tokens for the low-confidence tail, and give
        // the confident head room to be complete.
        std::vector<::rag::SearchResult> ranked(res->begin(), res->end());
        {
            double hi = 0.0;
            for (const auto& r : ranked) hi = std::max(hi, static_cast<double>(r.score.value));
            const double floor = hi * relevance_floor_frac();
            if (hi > 0.0 && floor > 0.0 && !ranked.empty()) {
                std::size_t keep = 1;
                while (keep < ranked.size()
                       && static_cast<double>(ranked[keep].score.value) >= floor)
                    ++keep;
                if (keep < ranked.size()) ranked.resize(keep);
            }
        }
        const std::size_t total_budget = confidence_scaled_budget(
            retrieval_output_budget(),
            ranked.empty() ? -1.0
                           : std::clamp(static_cast<double>(ranked.front().score.value),
                                        0.0, 1.0));
        std::vector<double> pre_scores;
        pre_scores.reserve(ranked.size());
        for (const auto& r : ranked)
            pre_scores.push_back(std::clamp(static_cast<double>(r.score.value), 0.0, 1.0));
        const std::vector<std::size_t> allowances =
            allocate_budget(pre_scores, total_budget);
        std::size_t remaining_budget = total_budget;
        std::size_t idx = 0;
        for (const auto& r : ranked) {
            auto [src, path] = split_uri(r.uri);
            (void)src;
            Passage p;
            p.source     = "code";
            p.path       = path;
            p.line_start = static_cast<int>(r.start_line);
            p.line_end   = static_cast<int>(r.end_line);
            p.score      = static_cast<double>(r.score.value);
            std::size_t want_bytes = idx < allowances.size() ? allowances[idx] : 768;
            const std::size_t allowance = std::min(want_bytes, remaining_budget);
            ++idx;
            if (allowance < 256) break;
            // ── Disk verification: RAG proposes, live disk disposes. ────────
            // The index can lag an unindexed edit; re-read the cited line range
            // from the CURRENT file on disk and serve that (the truth). Only
            // fall back to the indexed chunk text when the disk read fails
            // (file moved/deleted since the walk). Keeps search_code results
            // correct-on-bytes, not just approximately-ranked.
            std::string verified = read_disk_lines(
                (root / fs::path{path}).string(), p.line_start, p.line_end);
            const std::string& source_text = !verified.empty() ? verified : r.text;
            p.text = compress_passage(query, source_text, allowance);
            remaining_budget -= std::min(remaining_budget, p.text.size());
            top = std::max(top, p.score);
            out.passages.push_back(std::move(p));
        }
        out.confidence = top;
        std::string cm = std::string(impl_->code_embedder_ready ? "code:hybrid" : "code:bm25")
                       + (impl_->cfg.fusion == "rrf" ? ", rrf-fusion"
                          : (impl_->cfg.adaptive_fusion ? ", convex-fusion(adaptive)"
                                                        : ", convex-fusion"))
                       + ", " + std::to_string(impl_->code_engine.corpus().chunk_count())
                       + " chunks from " + root.filename().string();
        {
            char cbuf[48];
            std::snprintf(cbuf, sizeof cbuf, ", confidence %.2f", out.confidence);
            cm += cbuf;
        }
        if (impl_->cfg.trace && !code_trace.empty()) {
            std::vector<std::string> steps;
            steps.reserve(code_trace.size());
            for (const auto& t : code_trace) {
                if (t.empty()) continue;
                if (t.rfind("→ ", 0) == 0 || t.rfind("-> ", 0) == 0) continue;
                steps.push_back(t);
            }
            if (!steps.empty()) {
                cm += "\n  funnel:";
                for (const auto& s : steps) cm += "\n    ↳ " + s;
                cm += "\n    ↳ top-" + std::to_string(out.passages.size());
            }
        }
        out.mode = std::move(cm);
    } catch (const std::exception& e) {
        out.error = std::string("search_code failed: ") + e.what();
    } catch (...) {
        out.error = "search_code failed";
    }
    return out;
}

bool Retriever::warm() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto root = resolve_docs_root(impl_->cfg.docs_root);
    if (root.empty()) return true;
    if (impl_->engine.corpus().chunk_count() == 0)
        (void)impl_->try_load_persisted(root);
    return !impl_->needs_reindex(root, /*skip_docs=*/false);
}

bool Retriever::code_warm() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->code_initialized) return true;
    // A persisted code index from a prior session counts as warm: loading it
    // is Engine::open() + a manifest stat-walk, not an embed-everything
    // build. Its absence means an opportunistic query would pay the full
    // cold build — refuse; only an explicit search_code call should do that.
    std::error_code ec;
    return fs::exists(impl_->code_ragdb_path(), ec) && !ec;
}

void Retriever::warm_async() {
    bool expected = false;
    if (!impl_->warming.compare_exchange_strong(expected, true)) return;
    // Reap a finished previous warmer + seat the new one under mu: even
    // though today's callers are all on the UI thread, the join/assign of
    // `warmer` must never race a concurrent warm_async — the atomic gate
    // above only guarantees one LOGICAL warm at a time, not that the
    // handle handoff itself is synchronized. mu makes it airtight from
    // any thread, at the cost of a lock the warm path pays anyway.
    Impl* state = impl_.get();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->warmer.joinable()) impl_->warmer.join();
        // Fresh warm pass: clear any stale cancellation from a prior worker.
        impl_->warm_stop.store(false, std::memory_order_relaxed);
        impl_->warmer = std::jthread([state](std::stop_token st) {
            // Bridge the jthread's stop_token to the atomic flag the deep
            // index functions poll. request_stop() (on re-seat or ~Impl) now
            // actually interrupts the embed pass instead of being ignored.
            std::stop_callback on_stop(st, [state] {
                state->warm_stop.store(true, std::memory_order_relaxed);
            });
            try {
                std::lock_guard<std::mutex> lock(state->mu);
                auto root = resolve_docs_root(state->cfg.docs_root);
                if (state->engine.corpus().chunk_count() == 0)
                    (void)state->try_load_persisted(root);
                state->refresh_docs(root);
            } catch (...) { /* best-effort */ }
            state->warming.store(false);
        });
    }
}

// Stop any in-flight warm promptly and reclaim the worker. Called early in
// process teardown so ^C is instant. Deliberately does NOT take state->mu: the
// warm worker holds mu for its whole run, so locking here would DEADLOCK — we
// only touch the atomics + the jthread handle, both safe to poke concurrently.
void Retriever::shutdown() {
    // 1. Trip the cooperative flag so refresh_docs()/reindex() bail at their
    //    next loop boundary (and before the expensive engine.build()).
    impl_->warm_stop.store(true, std::memory_order_relaxed);
    // 2. Ask the jthread to stop (fires the stop_callback too) and reclaim it.
    //    request_stop() + join is bounded now that the worker observes the
    //    flag; a wedged blocking embed can't stall teardown because the flag
    //    is checked between network batches, and the worst case is one
    //    in-flight batch — far short of the old full-corpus 4–10 s hang.
    if (impl_->warmer.joinable()) {
        impl_->warmer.request_stop();
        impl_->warmer.join();
    }
}

// Replace the live configuration. Anything that changes the CORPUS shape
// (sources, contextual chunking, embed model, persistence, fusion, pipeline
// stages) must invalidate the built indexes so the next retrieve() rebuilds
// from the new config; pure ranking/labelling toggles (learn, trace,
// proactive*) take effect on the very next call with no rebuild. We take the
// conservative route: if ANY corpus-affecting field differs, drop all three
// engines back to cold so refresh_docs/retrieve_code rebuild cleanly. The docs
// root is preserved (docs_root is not picker-exposed). The EMBEDDER is now
// picker-owned, so unlike docs_root it is taken from the incoming config and
// a change to it forces a re-probe + cold rebuild.
void Retriever::apply_config(const Config& cfg) {
    try {
        std::unique_lock<std::mutex> lock(impl_->mu);
        const Config& old = impl_->cfg;
        // A different embedder means a different VECTOR SPACE: every persisted
        // vector is incomparable, so this is always a cold rebuild (and the
        // .ragdb path changes with the identity, so the old index survives on
        // disk for when the user switches back).
        const bool embedder_changed =
            ::agentty::rag::embed::identity(old.embed)
                != ::agentty::rag::embed::identity(cfg.embed);
        const bool corpus_changed =
               old.skills        != cfg.skills
            || old.memory        != cfg.memory
            || old.mcp_resources != cfg.mcp_resources
            || old.contextual    != cfg.contextual
            || old.dedup         != cfg.dedup
            || old.mmr           != cfg.mmr
            || old.stitch        != cfg.stitch
            || old.autocut       != cfg.autocut
            || old.fusion        != cfg.fusion
            || old.adaptive_fusion != cfg.adaptive_fusion
            || old.dense_weight  != cfg.dense_weight
            || old.bm25_weight   != cfg.bm25_weight
            || old.persist       != cfg.persist
            || embedder_changed;
        // Preserve only what the picker genuinely never touches.
        Config next = cfg;
        next.docs_root  = old.docs_root;
        impl_->cfg = std::move(next);
        if (embedder_changed) {
            // Re-probe: the new backend may be unreachable, and its dimension
            // must be measured rather than inherited from the previous one.
            //
            // The probe DIALS A NETWORK ENDPOINT (or memory-maps a model), so
            // it runs with the lock RELEASED. Holding `mu` across a
            // multi-second timeout is what made every reader — including the
            // UI's status row — stall behind it. Readers see the old state
            // until the new one lands, which is the honest answer while a
            // probe is in flight.
            impl_->dense = dense::Unprobed{};
            lock.unlock();
            impl_->probe_embedder_unlocked();
            lock.lock();
            impl_->install_default_generator();
        }
        if (corpus_changed) {
            // Cold-start every engine; the pipeline is rebuilt from the new
            // toggles on the next reindex/refresh. Rebuilding the docs engine
            // in place (make_engine_config reads cfg) keeps ownership simple.
            impl_->engine = ::rag::Engine(impl_->make_engine_config());
            impl_->embedder_ready = false;
            impl_->docs_initialized = false;
            impl_->indexed_root.clear();
            impl_->indexed_fp = 0;
            impl_->docs_files.clear();
            impl_->persist_checked_for.clear();
            impl_->warm_initialized = false;
            impl_->warm_engine = ::rag::Engine(impl_->make_engine_config());
            impl_->code_initialized = false;
            impl_->code_root.clear();
            impl_->code_files.clear();
            impl_->attach_embedder();
        } else {
            // Ranking-only change: re-apply the pipeline in place so e.g. an
            // MMR/autocut toggle that DID land in corpus_changed is covered,
            // and cheap re-labels (fusion label) refresh without a rebuild.
            impl_->apply_pipeline(impl_->engine);
        }
    } catch (...) { /* best-effort; keep the old config live on failure */ }
}

Config Retriever::snapshot_config() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->cfg;
}

Retriever::EmbedStatus Retriever::embed_status() const {
    EmbedStatus out;

    // NEVER blocks. This is called from the UI thread to render a status row,
    // and `mu` is held for the whole of apply_config() — which re-probes the
    // embedder (a network dial) and cold-rebuilds three engines. A plain
    // lock_guard here meant the UI froze, animations included, for as long as
    // a rebuild took.
    //
    // A status readout is advisory by nature: if the retriever is busy being
    // reconfigured, "reconfiguring" IS the true answer, and it costs nothing
    // to say so. The next frame picks up the real state.
    std::unique_lock<std::mutex> lock(impl_->mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        out.state    = EmbedStatus::State::Unprobed;
        out.describe = "reconfiguring\xe2\x80\xa6";
        return out;
    }

    out.describe = ::agentty::rag::embed::describe(impl_->cfg.embed);
    std::visit([&](const auto& st) {
        using T = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<T, dense::Ready>) {
            out.state      = EmbedStatus::State::Ready;
            out.dim        = st.dim;
            out.latency_ms = st.latency_ms;
        } else if constexpr (std::is_same_v<T, dense::Unavailable>) {
            out.state  = EmbedStatus::State::Unavailable;
            out.reason = st.reason;
        } else {
            out.state = EmbedStatus::State::Unprobed;
        }
    }, impl_->dense);
    return out;
}

Retriever::EmbedStatus Retriever::reprobe_embedder() {
    // Same discipline as apply_config: the probe itself runs UNLOCKED, because
    // it dials a network endpoint. Only the state write and the re-attach take
    // the lock, and both are cheap.
    {
        std::unique_lock<std::mutex> lock(impl_->mu);
        impl_->dense = dense::Unprobed{};
        lock.unlock();
        impl_->probe_embedder_unlocked();
        lock.lock();
        impl_->attach_embedder();
    }
    return embed_status();
}

// ── Learning loop (write side) ─────────────
// Both halves delegate to the process-wide FeedbackStore, which owns the TSV
// and the read-side nudge. Best-effort; never throws.
namespace feedback {
void note_surfaced(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    try {
        if (truthy_default_off("AGENTTY_RAG_LEARN"))
            FeedbackStore::instance().note_surfaced(paths);
    } catch (...) { /* best-effort */ }
}
void note_file_opened(const std::string& path) {
    // A `read` of a file a recent passage pointed at is an IMPLICIT relevance
    // judgment. FeedbackStore credits a "win" ONLY if the path was recently
    // surfaced by retrieval — a read of a never-surfaced file is not a signal
    // and is dropped, so the win-rate stays attributable.
    if (path.empty()) return;
    try {
        if (truthy_default_off("AGENTTY_RAG_LEARN"))
            FeedbackStore::instance().note_opened(path);
    } catch (...) { /* best-effort */ }
}
} // namespace feedback

// ── CLI: `agentty rag-bench <root>` ────────────────────────────────────
// A pipeline you can't measure is a pile of vibes. rag-bench evaluates the
// retrieval funnel ON YOUR OWN CORPUS, offline, in milliseconds — no LLM, no
// network, fully deterministic.
//
// Method (known-item retrieval):
//   1. Index the corpus (BM25 + local hash-dense fallback if Ollama is down).
//   2. Sample chunks; for each, synthesize a query from its most
//      DISCRIMINATIVE terms (top BM25 term contributions, via Corpus::explain).
//      The chunk the query was minted from is that query's known GOLD answer.
//   3. Run every query through the retrieval LADDER — one stage added per rung:
//        bm25-only → hybrid+prf → +feature-rerank → +mmr
//      and report recall@k, MRR, and nDCG@10 per rung. Because each rung adds
//      exactly one stage, a metric that DROPS at a rung points at the stage
//      worth tuning — and every AGENTTY_RAG_* knob can be set against numbers.
namespace bench {
namespace {

struct Query {
    std::string text;
    std::uint32_t gold;   // ChunkId value of the source chunk
};

struct Metrics {
    double recall = 0.0, mrr = 0.0, ndcg = 0.0;
    std::size_t n = 0;
    void add(int rank /* 1-based, 0 = miss */, std::size_t k) {
        ++n;
        if (rank >= 1 && static_cast<std::size_t>(rank) <= k) {
            recall += 1.0;
            mrr    += 1.0 / rank;
        }
        // Single relevant doc ⇒ IDCG = 1; DCG = 1/log2(rank+1) when in top-10.
        if (rank >= 1 && rank <= 10)
            ndcg += 1.0 / std::log2(static_cast<double>(rank) + 1.0);
    }
    void finalize() { if (n) { recall /= n; mrr /= n; ndcg /= n; } }
};

// 1-based rank of the gold chunk in a hit list, or 0 if absent.
int rank_of(const std::vector<::rag::Hit>& hits, std::uint32_t gold, std::size_t k) {
    for (std::size_t i = 0; i < hits.size() && i < k; ++i)
        if (hits[i].chunk.value == gold) return static_cast<int>(i) + 1;
    return 0;
}

// Split a chunk's text into distinct alphanumeric terms (deterministic order).
std::vector<std::string> distinct_terms(const std::string& text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 3 && seen.insert(cur).second) out.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else flush();
    }
    flush();
    return out;
}

} // namespace

int run(const std::string& root) {
    fs::path r = resolve_docs_root(root);
    if (r.empty()) {
        std::fprintf(stderr, "rag-bench: no docs root (pass a folder or set AGENTTY_DOCS_DIR)\n");
        return 2;
    }
    try {
        auto cfg = Config::from_env();
        ::rag::Engine engine;
        bool have_dense = false;
        // Same SSOT the adapter uses — this site used to mint its own
        // {"type":"ollama"} literal and had already drifted from it.
        if (const auto spec = ::agentty::rag::embed::spec_json(cfg.embed);
            spec && engine.with_embedder_spec(::rag::plugin::Json::parse(*spec))) {
            auto probe = engine.corpus().embed_text("rag benchmark availability probe");
            have_dense = probe.has_value() && !probe->empty();
        }
        if (!have_dense) engine = ::rag::Engine{};

        auto t0 = std::chrono::steady_clock::now();
        ::rag::loaders::DirOptions opts;
        // Match the interactive docs path: skip build/vendor dirs and cap the
        // corpus so `rag-bench` on a huge tree can't OOM the way an unbounded
        // walk would.
        opts.exclude_dirs = {
            ".git",".hg",".svn","node_modules","build","dist","out",
            "target","venv",".venv","__pycache__",".cache","_deps",
            "CMakeFiles",".agentty","vendor","third_party"};
        opts.max_file_bytes = 1 * 1024 * 1024;
        opts.max_files = 6000;
        auto docs = ::rag::loaders::load_directory(r, opts);
        std::size_t n = 0;
        if (docs) for (auto& d : *docs) { (void)engine.add(d.uri, std::move(d.text), d.meta, d.title); ++n; }
        (void)engine.build();
        auto t1 = std::chrono::steady_clock::now();
        auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        const auto& corpus = engine.corpus();
        std::size_t nchunks = corpus.chunk_count();
        std::printf("rag-bench: indexed %zu docs, %zu chunks from %s in %lld ms "
                    "(dense: %s)\n",
                    n, nchunks, r.string().c_str(),
                    static_cast<long long>(build_ms),
                    have_dense ? "ollama" : "bm25-only");
        if (nchunks == 0) { std::fprintf(stderr, "rag-bench: empty corpus\n"); return 1; }

        // 2. Synthesize known-item queries. Sample up to ~200 chunks evenly so a
        //    large corpus still benches in milliseconds and reproducibly.
        const std::size_t kQueryBudget = 200;
        const std::size_t stride = nchunks > kQueryBudget ? nchunks / kQueryBudget : 1;
        std::vector<Query> queries;
        {
            auto lease = corpus.chunks();
            for (std::size_t i = 0; i < lease.size(); i += stride) {
                const auto& ch = lease[i];
                auto terms = distinct_terms(ch.text);
                if (terms.size() < 3) continue;
                // Rank terms by their BM25 contribution to THIS chunk, keep the
                // top few most discriminative — the known-item query.
                std::vector<std::pair<float, std::string>> scored;
                scored.reserve(terms.size());
                for (const auto& t : terms) {
                    auto ex = corpus.explain(t, ch.id);
                    scored.emplace_back(ex.lexical_score, t);
                }
                std::sort(scored.begin(), scored.end(),
                          [](auto& a, auto& b) { return a.first > b.first; });
                std::string q;
                for (std::size_t j = 0; j < scored.size() && j < 5; ++j) {
                    if (scored[j].first <= 0.0f) break;
                    if (!q.empty()) q += ' ';
                    q += scored[j].second;
                }
                if (q.empty()) continue;
                queries.push_back({std::move(q), ch.id.value});
            }
        }
        if (queries.empty()) { std::fprintf(stderr, "rag-bench: no queries synthesized\n"); return 1; }

        // 3. Assemble the ladder — one stage added per rung.
        namespace pl = ::rag::pipeline;
        auto hybrid_cfg = [&](bool dense) {
            pl::HybridRetrieveConfig hy;
            hy.candidate_k  = 60;
            hy.bm25_weight  = 1.0f;
            hy.dense_weight = dense ? 1.0f : 0.0f;
            // convex fusion IGNORES the per-retriever weights (it uses
            // convex.alpha), so to actually isolate BM25 for the first rung we
            // fall back to weighted RRF, which honours dense_weight=0.
            hy.fusion = dense ? pl::HybridRetrieveConfig::Fusion::convex
                              : pl::HybridRetrieveConfig::Fusion::rrf;
            return hy;
        };
        struct Rung { const char* name; pl::Pipeline pipe; };
        std::vector<Rung> ladder;
        {   // bm25-only
            pl::Pipeline p;
            p.add(std::make_shared<pl::HybridRetrieveStage>(hybrid_cfg(false)));
            p.add(std::make_shared<pl::TopKStage>());
            ladder.push_back({"bm25-only", std::move(p)});
        }
        {   // hybrid + prf
            pl::Pipeline p;
            p.add(std::make_shared<pl::PrfExpandStage>(pl::ExpandConfig{}));
            p.add(std::make_shared<pl::HybridRetrieveStage>(hybrid_cfg(true)));
            p.add(std::make_shared<pl::TopKStage>());
            ladder.push_back({"hybrid+prf", std::move(p)});
        }
        {   // + feature-rerank
            pl::Pipeline p;
            p.add(std::make_shared<pl::PrfExpandStage>(pl::ExpandConfig{}));
            p.add(std::make_shared<pl::HybridRetrieveStage>(hybrid_cfg(true)));
            p.add(pl::make_feature_rerank_stage());
            p.add(std::make_shared<pl::TopKStage>());
            ladder.push_back({"+feature-rerank", std::move(p)});
        }
        {   // + mmr (the full quality funnel, PRF-prepended)
            pl::Pipeline p = pl::Pipeline::quality_with(hybrid_cfg(true), cfg.mmr_lambda);
            ladder.push_back({"+mmr", std::move(p)});
        }

        const std::size_t k = 10;
        std::printf("\n  %-16s  recall@%zu   MRR    nDCG@10   ms/query\n",
                    "stage", k);
        std::printf("  %-16s  --------  ------  -------  --------\n", "----------------");
        for (auto& rung : ladder) {
            Metrics m;
            auto s0 = std::chrono::steady_clock::now();
            for (const auto& q : queries) {
                auto hits = rung.pipe.run(corpus, q.text, k);
                if (!hits) { m.add(0, k); continue; }
                m.add(rank_of(*hits, q.gold, k), k);
            }
            auto s1 = std::chrono::steady_clock::now();
            double per = std::chrono::duration_cast<std::chrono::microseconds>(s1 - s0).count()
                         / 1000.0 / static_cast<double>(queries.size());
            m.finalize();
            std::printf("  %-16s   %.3f   %.3f   %.3f     %6.2f\n",
                        rung.name, m.recall, m.mrr, m.ndcg, per);
        }
        std::printf("\n  %zu known-item queries over %zu chunks. "
                    "A metric that DROPS at a rung is the stage to tune.\n",
                    queries.size(), nchunks);

        // 4. TOKEN-COST measurement (opt-in via AGENTTY_RAG_MEASURE=1). Runs the
        //    synthesized queries through the REAL Retriever::retrieve() output
        //    path — the one that applies the relevance floor, water-fill,
        //    confidence-scaled budget and extractive compression — and tallies
        //    the bytes/tokens actually injected into a model turn. Run the
        //    binary twice (levers on vs off via env) and diff to see the real
        //    saving. Token estimate: bytes/4 (Claude/GPT English heuristic).
        if (const char* mv = std::getenv("AGENTTY_RAG_MEASURE"); mv && mv[0] == '1') {
            Retriever ret;
            std::size_t total_bytes = 0, total_passages = 0, empty = 0;
            std::size_t sampled = 0;
            const std::size_t kMax = std::min<std::size_t>(queries.size(), 80);
            const std::size_t qstride = queries.size() > kMax ? queries.size() / kMax : 1;
            auto q0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < queries.size(); i += qstride) {
                auto rr = ret.retrieve(queries[i].text, /*k=*/8, /*skip_docs=*/false);
                ++sampled;
                if (rr.passages.empty()) { ++empty; continue; }
                total_passages += rr.passages.size();
                for (const auto& p : rr.passages) total_bytes += p.text.size();
            }
            auto q1 = std::chrono::steady_clock::now();
            double per_ms = std::chrono::duration_cast<std::chrono::microseconds>(q1 - q0).count()
                            / 1000.0 / static_cast<double>(std::max<std::size_t>(sampled, 1));
            const double est_tokens = static_cast<double>(total_bytes) / 4.0;
            std::printf("\n  ── TOKEN COST (real retrieve() output path) ──\n");
            std::printf("  queries measured : %zu (%zu returned nothing)\n", sampled, empty);
            std::printf("  passages injected: %zu total, %.2f avg/query\n",
                        total_passages,
                        static_cast<double>(total_passages)
                            / static_cast<double>(std::max<std::size_t>(sampled, 1)));
            std::printf("  output bytes     : %zu total, %.0f avg/query\n",
                        total_bytes,
                        static_cast<double>(total_bytes)
                            / static_cast<double>(std::max<std::size_t>(sampled, 1)));
            std::printf("  est. tokens      : %.0f total, %.0f avg/query (bytes/4)\n",
                        est_tokens,
                        est_tokens / static_cast<double>(std::max<std::size_t>(sampled, 1)));
            std::printf("  retrieve latency : %.2f ms/query\n", per_ms);
            // Emit a machine-readable line for scripted before/after diffing.
            std::printf("  MEASURE\ttokens=%.0f\tbytes=%zu\tpassages=%zu\tqueries=%zu\n",
                        est_tokens, total_bytes, total_passages, sampled);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rag-bench: %s\n", e.what());
        return 1;
    }
}
} // namespace bench

} // namespace agentty::rag

#else  // !AGENTTY_HAS_RAGCPP  ─────────────────────────────────────────────

// Retrieval unavailable on this platform (e.g. MSVC, until rag-cpp is
// Windows-portable). Every public symbol from rag_adapter.hpp is defined here
// as a no-op so agentty links and runs; search_docs/search_code surface a
// clear "unavailable" message instead of crashing or returning stale data.

#include <cstdio>
#include <cstdlib>

namespace agentty::rag {

Config Config::from_env() {
    // A default-constructed Config is fine; nothing reads it in the no-op build,
    // but tests and callers may still construct one.
    return Config{};
}

// Impl is empty — the header stores an opaque pointer we simply leave null.
struct Retriever::Impl {};

Retriever::Retriever() : impl_(nullptr) {}
Retriever::~Retriever() = default;

void Retriever::set_generator(Generator /*g*/) {}

static Retrieval unavailable_() {
    Retrieval r;
    r.mode  = "retrieval unavailable on this platform";
    r.error = "retrieval engine (rag-cpp) is not built on this platform";
    r.confidence = 0.0;
    return r;
}

Retrieval Retriever::retrieve(const std::string& /*query*/, int /*k*/,
                              bool /*skip_docs*/) {
    return unavailable_();
}

Retrieval Retriever::retrieve_code(const std::string& /*query*/, int /*k*/) {
    return unavailable_();
}

bool Retriever::warm() const { return true; }  // nothing to build → always warm
void Retriever::warm_async() {}
void Retriever::shutdown() {}
void Retriever::apply_config(const Config& /*cfg*/) {}
Config Retriever::snapshot_config() const { return Config{}; }

Retriever::EmbedStatus Retriever::embed_status() const {
    EmbedStatus s;
    s.state  = EmbedStatus::State::Unavailable;
    s.reason = "retrieval engine (rag-cpp) is not built on this platform";
    return s;
}
Retriever::EmbedStatus Retriever::reprobe_embedder() { return embed_status(); }

namespace feedback {
void note_surfaced(const std::vector<std::string>& /*paths*/) {}
void note_file_opened(const std::string& /*path*/) {}
}  // namespace feedback

namespace bench {
int run(const std::string& /*root*/) {
    std::fprintf(stderr,
                 "rag-bench: the retrieval engine is not built on this "
                 "platform (rag-cpp is not yet Windows-portable).\n");
    return 2;
}
}  // namespace bench

}  // namespace agentty::rag

#endif  // AGENTTY_HAS_RAGCPP
