#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/glob.hpp"
#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/tool/util/utf8.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ── Tunables ─────────────────────────────────────────────────────────────
// `kMaxFileBytes` is the per-file size cap. Source files are almost never
// over a few hundred KB; anything larger is generated/vendored/log noise we
// don't want to stream into the model. 8 MiB rides out edge cases (giant
// JSON fixtures, schema dumps) without dragging the scan.
constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;
constexpr int         kPerPage      = 20;
constexpr int         kContext      = 2;
constexpr int         kMaxScanned   = 500;
// Past 8 worker threads disk bandwidth saturates on consumer SSDs and we
// start losing to lock contention on the result-merge step. NVMe systems
// could push higher, but the diminishing-returns curve flattens hard.
constexpr unsigned    kMaxWorkers   = 8;

// ── Args ─────────────────────────────────────────────────────────────────

struct GrepArgs {
    std::string pattern;
    std::string root;
    std::string file_glob;
    bool        case_sensitive;
    int         offset;
    std::string display_description;
};

std::expected<GrepArgs, ToolError> parse_grep_args(const json& j) {
    util::ArgReader ar(j);
    auto pat_opt = ar.require_str("pattern");
    if (!pat_opt)
        return std::unexpected(ToolError::invalid_args("pattern required"));
    int offset = ar.integer("offset", 0);
    if (offset < 0) offset = 0;
    return GrepArgs{
        *std::move(pat_opt),
        ar.str("path", "."),
        ar.str("glob", ""),
        ar.boolean("case_sensitive", false),
        offset,
        ar.str("display_description", ""),
    };
}

// ── Fast paths ───────────────────────────────────────────────────────────

// True when the pattern contains no ECMAScript regex metacharacters and can
// be matched via `std::string_view::find` (Boyer-Moore-Horspool). Hits the
// 50-100x speedup over libstdc++'s `std::regex` for the dominant case
// (model searches for an identifier or substring).
[[nodiscard]] bool is_literal_pattern(std::string_view p) noexcept {
    constexpr std::string_view meta{".^$*+?()[]{}|\\"};
    return p.find_first_of(meta) == std::string_view::npos;
}

// Extension-based binary skip — saves an open()+read(512) per file vs.
// `is_binary_file`. Lower-cased compare. Anything that lands here is
// guaranteed not to contain text we'd want to grep.
[[nodiscard]] bool likely_binary_ext(const fs::path& p) {
    static const std::unordered_set<std::string> bins = {
        ".exe", ".dll", ".lib", ".a", ".o", ".obj", ".pdb", ".so", ".dylib",
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico", ".tiff",
        ".pdf", ".zip", ".tar", ".gz", ".bz2", ".xz", ".7z", ".rar",
        ".mp3", ".mp4", ".wav", ".avi", ".mov", ".webm", ".flac", ".ogg",
        ".ttf", ".otf", ".woff", ".woff2", ".eot",
        ".class", ".jar", ".pyc", ".pyo", ".wasm",
        ".bin", ".iso", ".dat", ".db", ".sqlite", ".sqlite3",
        ".dmg", ".deb", ".rpm", ".msi",
        ".lock",  // package-lock.json is text but we don't want it
    };
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return bins.contains(e);
}

// ── Scanners ─────────────────────────────────────────────────────────────

void scan_literal(std::string_view content, std::string_view needle,
                  bool case_insensitive, std::vector<std::size_t>& out,
                  std::atomic<int>& total) {
    if (needle.empty()) return;
    auto record = [&](std::size_t pos) -> bool {
        out.push_back(pos);
        return total.fetch_add(1, std::memory_order_relaxed) + 1 < kMaxScanned;
    };
    if (!case_insensitive) {
        std::size_t pos = 0;
        while ((pos = content.find(needle, pos)) != std::string_view::npos) {
            if (!record(pos)) return;
            pos += needle.size();
        }
        return;
    }
    // Lowercase pass — one allocation per file, amortised over many matches.
    std::string lower(content.size(), '\0');
    std::ranges::transform(content, lower.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::string nl(needle.size(), '\0');
    std::ranges::transform(needle, nl.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while ((pos = lower.find(nl, pos)) != std::string::npos) {
        if (!record(pos)) return;
        pos += nl.size();
    }
}

void scan_regex(std::string_view content, const std::regex& re,
                std::vector<std::size_t>& out, std::atomic<int>& total) {
    auto begin = std::cregex_iterator(content.data(),
                                       content.data() + content.size(), re);
    auto end = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
        out.push_back(static_cast<std::size_t>(it->position(0)));
        if (total.fetch_add(1, std::memory_order_relaxed) + 1 >= kMaxScanned)
            return;
    }
}

// Map sorted byte-offsets in `content` to (line_no, line_start, line_end) in
// a single linear pass. O(file_size + matches) instead of O(file_size *
// matches) per the naive per-offset scan.
struct LineInfo {
    int          line_no;     // 1-based
    std::size_t  line_start;  // byte offset of line start
    std::size_t  line_end;    // byte offset of '\n' (or content end)
};

[[nodiscard]] std::vector<LineInfo>
offsets_to_lines(std::string_view content,
                 const std::vector<std::size_t>& offsets) {
    std::vector<LineInfo> out;
    out.reserve(offsets.size());
    int line_no = 1;
    std::size_t line_start = 0;
    std::size_t cursor = 0;
    for (std::size_t off : offsets) {
        while (cursor < off) {
            if (content[cursor] == '\n') {
                ++line_no;
                line_start = cursor + 1;
            }
            ++cursor;
        }
        auto nl = content.find('\n', off);
        std::size_t line_end = (nl == std::string_view::npos)
                             ? content.size() : nl;
        out.push_back({line_no, line_start, line_end});
    }
    return out;
}

// ── File hit (per-file scratch handed from worker → formatter) ──────────

struct FileHit {
    fs::path                  path;
    std::string               content;
    std::vector<std::size_t>  match_offsets;
};

// ── Backend selection ────────────────────────────────────────────────────
// Probe for ripgrep (`rg`) once at first call; cache the result for the
// process lifetime. Ripgrep is dramatically faster than anything we can
// write in C++ (vectorised SIMD literal scan, regex via Rust's
// regex-automata, parallel by default), AND it transparently honours
// `.gitignore` / `.ignore` — no need to maintain our own skip list. When
// `rg` is missing we fall back to the in-process parallel scanner so the
// tool still works on machines where the user hasn't installed it.

enum class Backend { Ripgrep, BuiltIn };

[[nodiscard]] Backend detect_backend() {
    static const Backend cached = []{
        auto r = util::Subprocess::run(util::SubprocessOptions{
            .argv     = std::vector<std::string>{"rg", "--version"},
            .timeout  = std::chrono::seconds(3),
            .max_bytes = 1024,
        });
        return (r.started && r.exit_code == 0)
                ? Backend::Ripgrep : Backend::BuiltIn;
    }();
    return cached;
}

// ── Ripgrep path ────────────────────────────────────────────────────────

ExecResult run_ripgrep(const GrepArgs& a) {
    std::vector<std::string> argv = {"rg", "--json", "--no-config"};
    if (!a.case_sensitive) argv.push_back("-i");
    // Always pass -F for literal patterns — rg's literal scanner is faster
    // than its regex engine even on simple patterns, and side-steps the
    // ECMAScript-vs-PCRE escape difference for patterns the user might be
    // assuming are literal (e.g. searching for `foo()` works as-is).
    if (is_literal_pattern(a.pattern)) argv.push_back("-F");
    argv.push_back("-C");
    argv.push_back(std::to_string(kContext));
    if (!a.file_glob.empty()) {
        argv.push_back("-g");
        argv.push_back(a.file_glob);
    }
    argv.push_back("--");
    argv.push_back(a.pattern);
    argv.push_back(a.root.empty() ? std::string{"."} : a.root);

    auto r = util::Subprocess::run(util::SubprocessOptions{
        .argv      = std::move(argv),
        .timeout   = std::chrono::seconds(60),
        .max_bytes = 8 * 1024 * 1024,
    });
    if (!r.started)
        return std::unexpected(ToolError::spawn(
            "rg failed to start: " + r.start_error));
    // rg exit codes: 0 = matches, 1 = no matches, 2 = error.
    if (r.exit_code == 1)
        return ToolOutput{"No matches found.", std::nullopt};
    if (r.exit_code != 0)
        return std::unexpected(ToolError::subprocess(
            "rg exited " + std::to_string(r.exit_code) + ":\n"
            + r.output.substr(0, 1024)));

    // Parse newline-delimited JSON. Per file we get a `begin` event, a mix
    // of `context` / `match` events, then `end`. Other event types
    // (`summary`, internal status) are ignored.
    struct LineRow { int line_no; std::string text; bool is_match; };
    struct FileRows { std::string path; std::vector<LineRow> rows; int matches = 0; };
    std::vector<FileRows> files;
    int total_matches = 0;
    {
        std::size_t pos = 0;
        while (pos < r.output.size() && total_matches < kMaxScanned) {
            auto nl = r.output.find('\n', pos);
            if (nl == std::string::npos) nl = r.output.size();
            std::string_view line{r.output.data() + pos, nl - pos};
            pos = nl + 1;
            if (line.empty()) continue;
            json j = json::parse(line, /*cb*/nullptr, /*throw*/false);
            if (j.is_discarded() || !j.is_object()) continue;
            auto type = j.value("type", "");
            if (type == "begin") {
                auto path = j["data"]["path"].value("text", "");
                files.push_back({std::move(path), {}, 0});
            } else if ((type == "context" || type == "match") && !files.empty()) {
                auto& d = j["data"];
                int ln = d.value("line_number", 0);
                std::string text = d.contains("lines")
                    ? d["lines"].value("text", std::string{}) : std::string{};
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                bool is_match = (type == "match");
                files.back().rows.push_back({ln, std::move(text), is_match});
                if (is_match) {
                    ++files.back().matches;
                    ++total_matches;
                }
            }
        }
    }

    // Drop files whose only events were context (can happen if we cut off
    // mid-stream at the kMaxScanned cap).
    std::erase_if(files, [](const FileRows& f){ return f.matches == 0; });

    if (total_matches == 0)
        return ToolOutput{"No matches found.", std::nullopt};

    std::ostringstream out;
    out << "Found " << total_matches << " match"
        << (total_matches == 1 ? "" : "es")
        << (total_matches >= kMaxScanned ? "+" : "")
        << " across " << files.size()
        << " file" << (files.size() == 1 ? "" : "s") << ".\n\n";

    int shown = 0, skipped = 0;
    for (auto& f : files) {
        if (shown >= kPerPage) break;
        // Group consecutive line numbers into blocks (rg pre-merges via -C).
        struct Block { int s, e; std::vector<const LineRow*> rows; int matches; };
        std::vector<Block> blocks;
        for (const auto& row : f.rows) {
            if (blocks.empty() || row.line_no > blocks.back().e + 1) {
                blocks.push_back({row.line_no, row.line_no, {&row}, row.is_match ? 1 : 0});
            } else {
                blocks.back().e = row.line_no;
                blocks.back().rows.push_back(&row);
                if (row.is_match) ++blocks.back().matches;
            }
        }

        bool emitted_header = false;
        for (auto& b : blocks) {
            if (b.matches == 0) continue;
            if (skipped + b.matches <= a.offset) {
                skipped += b.matches;
                continue;
            }
            if (shown >= kPerPage) break;
            if (!emitted_header) {
                out << "## Matches in " << f.path << "\n\n";
                emitted_header = true;
            }
            out << "### L" << b.s << "-" << b.e << "\n```\n";
            for (const auto* row : b.rows) out << row->text << "\n";
            out << "```\n\n";
            shown += b.matches;
        }
    }

    int remaining = total_matches - (a.offset + shown);
    if (remaining > 0) {
        out << "Showing matches " << (a.offset + 1) << "-"
            << (a.offset + shown) << " of " << total_matches
            << (total_matches >= kMaxScanned ? "+ (scan limit reached)" : "")
            << ". Use offset: " << (a.offset + kPerPage)
            << " to see the next page.";
    } else if (shown == 0) {
        return ToolOutput{
            "No matches on this page. Total matches: "
            + std::to_string(total_matches) + ". Try a smaller offset.",
            std::nullopt};
    } else {
        out << "Showing all " << total_matches << " matches.";
    }
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ── Built-in fallback (parallel C++ scanner) ────────────────────────────

ExecResult run_builtin(const GrepArgs& a) {
    const bool literal = is_literal_pattern(a.pattern);
    std::regex re;
    if (!literal) {
        // `optimize` trades construction time for match time — worth it when
        // we're about to apply the same regex to hundreds of files.
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!a.case_sensitive) flags = flags | std::regex::icase;
        try { re = std::regex(a.pattern, flags); }
        catch (const std::regex_error& e) {
            return std::unexpected(ToolError::invalid_regex(
                std::string{"invalid regex '"} + a.pattern + "': " + e.what()));
        }
    }

    // ── Phase 1: collect candidate files (cheap, single-threaded) ────────
    std::vector<fs::path> candidates;
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(a.root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            auto fn = entry.path().filename().string();
            if (entry.is_directory(ec)) {
                if (util::should_skip_dir(fn)) it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(ec)) continue;
            if (fn.starts_with(".")) continue;
            if (!a.file_glob.empty()
                && !util::glob_match(a.file_glob, fn)) continue;
            if (likely_binary_ext(entry.path())) continue;
            // Size cap: skip empties (no matches) and overly-large files
            // (vendored / generated). One stat per candidate; cheap vs. the
            // open+read we'd do otherwise.
            std::error_code sec;
            auto sz = entry.file_size(sec);
            if (sec || sz == 0 || sz > kMaxFileBytes) { sec.clear(); continue; }
            candidates.push_back(entry.path());
        }
    }

    if (candidates.empty()) {
        return ToolOutput{
            "No matches found. The directory may be empty or every file was "
            "filtered (binary extension, size cap, or hidden).", std::nullopt};
    }

    // ── Phase 2: parallel scan ───────────────────────────────────────────
    std::vector<FileHit>  hits(candidates.size());
    std::atomic<std::size_t> next{0};
    std::atomic<int>      total_matches{0};

    auto worker = [&] {
        while (true) {
            if (total_matches.load(std::memory_order_relaxed) >= kMaxScanned)
                return;
            std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= candidates.size()) return;
            const auto& path = candidates[i];

            std::string content = util::read_file(path);
            if (content.empty()) continue;
            // In-buffer NUL check — catches misnamed binaries (e.g. .txt
            // that's actually a ZIP) without a second open().
            auto head = std::min<std::size_t>(content.size(), 4096);
            if (std::memchr(content.data(), '\0', head)) continue;

            std::vector<std::size_t> offsets;
            if (literal) {
                scan_literal(content, a.pattern, !a.case_sensitive,
                             offsets, total_matches);
            } else {
                scan_regex(content, re, offsets, total_matches);
            }
            if (offsets.empty()) continue;
            hits[i].path = path;
            hits[i].content = std::move(content);
            hits[i].match_offsets = std::move(offsets);
        }
    };

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 2;
    nthreads = std::min(nthreads, kMaxWorkers);
    nthreads = std::min(nthreads, static_cast<unsigned>(candidates.size()));
    {
        std::vector<std::jthread> pool;
        pool.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);
        // jthread destructors join here.
    }

    int total = total_matches.load();
    if (total == 0)
        return ToolOutput{
            "No matches found. Check the pattern syntax (this is ECMAScript "
            "regex, not PCRE — no look-behind, no named groups), try a "
            "broader pattern, or use `glob` first to narrow the file set.",
            std::nullopt};

    // ── Phase 3: format output (single-threaded, in discovery order) ────
    std::size_t files_with_hits = 0;
    for (const auto& h : hits) if (!h.path.empty()) ++files_with_hits;

    std::ostringstream out;
    out << "Found " << total << " match" << (total == 1 ? "" : "es")
        << (total >= kMaxScanned ? "+" : "")
        << " across " << files_with_hits
        << " file" << (files_with_hits == 1 ? "" : "s") << ".\n\n";

    int shown = 0, skipped = 0;
    for (const auto& h : hits) {
        if (h.path.empty()) continue;
        if (shown >= kPerPage) break;

        auto lines = offsets_to_lines(h.content, h.match_offsets);

        // Merge near-adjacent context windows so two matches within
        // 2*kContext lines collapse into one fenced block.
        std::vector<std::pair<int,int>> page_ranges;  // 0-based [start,end]
        for (const auto& li : lines) {
            if (skipped < a.offset) { ++skipped; continue; }
            if (shown >= kPerPage) break;
            int row = li.line_no - 1;
            // Total line count without splitting again: count newlines once
            // per file, lazily — but for context bounds we only need
            // line_no±kContext, and we never index past the actual content
            // when reading the line bytes (handled below).
            int start = std::max(0, row - kContext);
            int end   = row + kContext;
            if (!page_ranges.empty()
                && start <= page_ranges.back().second + 1) {
                page_ranges.back().second =
                    std::max(page_ranges.back().second, end);
            } else {
                page_ranges.emplace_back(start, end);
            }
            ++shown;
        }
        if (page_ranges.empty()) continue;

        // Build a small index of line starts for the lines we'll actually
        // emit — bounded by the highest end-row in page_ranges.
        int max_row = 0;
        for (auto [s, e] : page_ranges) max_row = std::max(max_row, e);
        std::vector<std::size_t> line_starts;
        line_starts.reserve(static_cast<std::size_t>(max_row + 2));
        line_starts.push_back(0);
        for (std::size_t i = 0; i < h.content.size()
                              && static_cast<int>(line_starts.size()) <= max_row + 1; ++i) {
            if (h.content[i] == '\n') line_starts.push_back(i + 1);
        }
        line_starts.push_back(h.content.size() + 1);  // sentinel
        const int last_line = static_cast<int>(line_starts.size()) - 2;

        out << "## Matches in " << h.path.string() << "\n\n";
        for (auto [s, e] : page_ranges) {
            int es = std::min(e, last_line);
            out << "### L" << (s + 1) << "-" << (es + 1) << "\n```\n";
            for (int i = s; i <= es; ++i) {
                std::size_t ls = line_starts[static_cast<std::size_t>(i)];
                std::size_t le = line_starts[static_cast<std::size_t>(i) + 1] - 1;
                if (le > h.content.size()) le = h.content.size();
                if (ls <= le) {
                    out.write(h.content.data() + ls,
                              static_cast<std::streamsize>(le - ls));
                }
                out << "\n";
            }
            out << "```\n\n";
        }
    }

    int remaining = total - (a.offset + shown);
    if (remaining > 0) {
        out << "Showing matches " << (a.offset + 1) << "-"
            << (a.offset + shown) << " of " << total
            << (total >= kMaxScanned ? "+ (scan limit reached)" : "")
            << ". Use offset: " << (a.offset + kPerPage)
            << " to see the next page.";
    } else if (shown == 0) {
        return ToolOutput{
            "No matches on this page. Total matches: "
            + std::to_string(total) + ". Try a smaller offset.",
            std::nullopt};
    } else {
        out << "Showing all " << total << " matches.";
    }
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

// ── Public dispatch ────────────────────────────────────────────────────
// Prefer ripgrep when present: it brings .gitignore awareness, SIMD-
// accelerated literal scanning and a heavily-tuned regex engine, all of
// which we cannot match in-process. Falls back to the parallel C++
// scanner when rg is missing (vendored builds, sandboxes).
//
// The output is force-scrubbed to valid UTF-8 because grep streams raw
// bytes from arbitrary files into the result — Latin-1 .htm pages, CP1252
// CSVs, Shift-JIS source, etc. nlohmann::json::dump() throws hard on any
// non-UTF-8 byte (type_error.316), so leaving it for the request builder
// to scrub races with persistence and crashes the process.
ExecResult run_grep(const GrepArgs& a) {
    auto r = (detect_backend() == Backend::Ripgrep) ? run_ripgrep(a)
                                                    : run_builtin(a);
    if (r.has_value()) r->text = util::to_valid_utf8(std::move(r->text));
    return r;
}

} // namespace

ToolDef tool_grep() {
    ToolDef t;
    t.name = ToolName{std::string{"grep"}};
    t.description = "Search for a regex pattern across files. Returns matches grouped by "
                    "file with 2 lines of context, paginated 20 results per page. "
                    "Case-insensitive by default; pass case_sensitive=true for exact case. "
                    "Use offset for subsequent pages.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"pattern",        {{"type","string"}, {"description","Regex pattern to search for"}}},
            {"path",           {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
            {"glob",           {{"type","string"}, {"description","File extension filter (e.g. *.cpp)"}}},
            {"case_sensitive", {{"type","boolean"}, {"description","Case-sensitive match (default: false)"}}},
            {"offset",         {{"type","integer"}, {"description","Skip this many matches (for pagination)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<GrepArgs>(parse_grep_args, run_grep);
    return t;
}

} // namespace moha::tools
