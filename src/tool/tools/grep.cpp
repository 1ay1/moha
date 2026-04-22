#include "moha/tool/tools.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/glob.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

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
            {"pattern",        {{"type","string"}, {"description","Regex pattern to search for"}}},
            {"path",           {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
            {"glob",           {{"type","string"}, {"description","File extension filter (e.g. *.cpp)"}}},
            {"case_sensitive", {{"type","boolean"}, {"description","Case-sensitive match (default: false)"}}},
            {"offset",         {{"type","integer"}, {"description","Skip this many matches (for pagination)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        constexpr int kPerPage = 20;
        constexpr int kContext = 2;
        constexpr int kMaxScanned = 500;

        std::string pat = args.value("pattern", "");
        std::string root = args.value("path", ".");
        std::string file_glob = args.value("glob", "");
        bool case_sensitive = args.value("case_sensitive", false);
        int offset = args.value("offset", 0);
        if (offset < 0) offset = 0;
        if (pat.empty()) return std::unexpected(ToolError{"pattern required"});

        auto flags = std::regex::ECMAScript;
        if (!case_sensitive) flags = flags | std::regex::icase;
        std::regex re;
        try { re = std::regex(pat, flags); } catch (const std::regex_error& e) {
            return std::unexpected(ToolError{std::string{"invalid regex '"} + pat + "': " + e.what()});
        }

        struct FileHits { std::vector<std::string> lines; std::vector<int> match_rows; };
        std::vector<std::pair<fs::path, FileHits>> files;
        int total_matches = 0;

        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            auto fn = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (util::should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (fn.starts_with(".")) continue;
            auto p = it->path();
            if (!file_glob.empty() && !util::glob_match(file_glob, fn)) continue;
            if (util::is_binary_file(p)) continue;
            std::ifstream ifs(p);
            if (!ifs) continue;

            FileHits fh;
            std::string line;
            while (std::getline(ifs, line)) fh.lines.push_back(line);
            for (int i = 0; i < (int)fh.lines.size(); ++i) {
                if (std::regex_search(fh.lines[i], re)) {
                    fh.match_rows.push_back(i);
                    if (++total_matches >= kMaxScanned) break;
                }
            }
            if (!fh.match_rows.empty())
                files.emplace_back(p, std::move(fh));
            if (total_matches >= kMaxScanned) break;
        }

        if (total_matches == 0)
            return ToolOutput{
                "No matches found. Check the pattern syntax (this is ECMAScript regex, "
                "not PCRE — no look-behind, no named groups), try a broader pattern, "
                "or use `glob` first to narrow the file set.", std::nullopt};

        int shown = 0;
        int skipped = 0;
        std::ostringstream out;
        out << "Found " << total_matches << " match"
            << (total_matches == 1 ? "" : "es")
            << (total_matches == kMaxScanned ? "+" : "")
            << " across " << files.size()
            << " file" << (files.size() == 1 ? "" : "s") << ".\n\n";
        for (const auto& [p, fh] : files) {
            // Merge near-adjacent matches so their context windows collapse
            // into a single fenced block (avoids duplicating lines when two
            // matches are within 2*kContext of each other).
            std::vector<std::pair<int,int>> page_ranges;
            for (int row : fh.match_rows) {
                if (skipped < offset) { skipped++; continue; }
                if (shown >= kPerPage) break;
                int start = std::max(0, row - kContext);
                int end = std::min((int)fh.lines.size() - 1, row + kContext);
                if (!page_ranges.empty() && start <= page_ranges.back().second + 1) {
                    page_ranges.back().second = std::max(page_ranges.back().second, end);
                } else {
                    page_ranges.emplace_back(start, end);
                }
                shown++;
            }
            if (page_ranges.empty()) continue;

            out << "## Matches in " << p.string() << "\n\n";
            for (auto [s, e] : page_ranges) {
                out << "### L" << (s + 1) << "-" << (e + 1) << "\n";
                out << "```\n";
                for (int i = s; i <= e; ++i) out << fh.lines[i] << "\n";
                out << "```\n\n";
            }
            if (shown >= kPerPage) break;
        }

        int remaining = total_matches - (offset + shown);
        if (remaining > 0) {
            out << "Showing matches " << (offset + 1) << "-" << (offset + shown)
                << " of " << total_matches
                << (total_matches == kMaxScanned ? "+ (scan limit reached)" : "")
                << ". Use offset: " << (offset + kPerPage)
                << " to see the next page.";
        } else if (shown == 0) {
            return ToolOutput{
                "No matches on this page. Total matches: "
                + std::to_string(total_matches)
                + ". Try a smaller offset.", std::nullopt};
        } else {
            out << "Showing all " << total_matches << " matches.";
        }
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

} // namespace moha::tools
