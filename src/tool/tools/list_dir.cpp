#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct ListDirArgs {
    std::string root;
    bool recursive;
    int max_depth;
};

std::expected<ListDirArgs, ToolError> parse_list_dir_args(const json& j) {
    util::ArgReader ar(j);
    return ListDirArgs{
        ar.str("path", "."),
        ar.boolean("recursive", false),
        ar.integer("max_depth", 3),
    };
}

ExecResult run_list_dir(const ListDirArgs& a) {
    std::error_code ec;
    if (!fs::exists(a.root, ec))
        return std::unexpected(ToolError::not_found("directory not found: " + a.root));
    if (!fs::is_directory(a.root, ec))
        return std::unexpected(ToolError::not_a_directory("not a directory: " + a.root));

    std::ostringstream out;
    int count = 0;

    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[32];
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0); return buf; }
        if (bytes < 1024*1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024.0)); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fG", bytes/(1024.0*1024.0*1024.0)); return buf;
    };

    auto list_entry = [&](const fs::directory_entry& entry, int depth) {
        if (count > 1000) return;
        std::string indent(depth * 2, ' ');
        auto fn = entry.path().filename().string();
        if ((fn.starts_with(".") || util::should_skip_dir(fn)) && depth > 0) return;
        if (entry.is_directory(ec)) {
            out << indent << fn << "/\n";
        } else if (entry.is_regular_file(ec)) {
            auto sz = entry.file_size(ec);
            out << indent << fn << "  " << format_size(ec ? 0 : sz) << "\n";
        } else if (entry.is_symlink(ec)) {
            out << indent << fn << " -> " << fs::read_symlink(entry.path(), ec).string() << "\n";
        }
        count++;
    };

    if (a.recursive) {
        for (auto it = fs::recursive_directory_iterator(a.root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > a.max_depth) { it.disable_recursion_pending(); continue; }
            list_entry(*it, it.depth());
            if (count > 1000) { out << "[>1000 entries, truncated]\n"; break; }
        }
    } else {
        std::vector<fs::directory_entry> entries;
        for (auto& e : fs::directory_iterator(a.root, ec))
            entries.push_back(e);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            bool da = a.is_directory(), db = b.is_directory();
            if (da != db) return da > db;
            return a.path().filename() < b.path().filename();
        });
        for (auto& e : entries) list_entry(e, 0);
    }
    if (count == 0) return ToolOutput{"empty directory", std::nullopt};
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_list_dir() {
    ToolDef t;
    t.name = ToolName{std::string{"list_dir"}};
    t.description = "List the contents of a directory. Shows file type, size, and name. "
                    "Use this to explore project structure before reading files.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"path",      {{"type","string"}, {"description","Directory to list (default: cwd)"}}},
            {"recursive", {{"type","boolean"}, {"description","List recursively (default: false)"}}},
            {"max_depth", {{"type","integer"}, {"description","Max depth for recursive listing (default: 3)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<ListDirArgs>(parse_list_dir_args, run_list_dir);
    return t;
}

} // namespace moha::tools
