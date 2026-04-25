#include "moha/index/repo_index.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct RepoMapArgs {
    std::string root;
    int         max_kb;
    std::string display_description;
};

std::expected<RepoMapArgs, ToolError> parse_repo_map_args(const json& j) {
    util::ArgReader ar(j);
    // Default 24 KB now that the map is two-pane (top-by-importance +
    // tree) with signatures and descriptions: the extra payload is
    // dense signal the model can act on, not filler. Hard cap raised
    // to 96 KB for monorepo cases where the agent explicitly asks for
    // a wide window.
    int max_kb = ar.integer("max_kb", 24);
    if (max_kb < 1)  max_kb = 1;
    if (max_kb > 96) max_kb = 96;
    return RepoMapArgs{
        ar.str("path", ""),
        max_kb,
        ar.str("display_description", ""),
    };
}

ExecResult run_repo_map(const RepoMapArgs& a) {
    // Refresh against the workspace (or the requested subtree) — picks
    // up file-system changes since the last call without forcing the
    // user to restart moha. Cached entries with unchanged mtime aren't
    // re-parsed, so the steady-state cost is one stat() per file.
    fs::path root = a.root.empty() ? fs::current_path() : fs::path{a.root};
    auto wp = util::make_workspace_path(root.string(), "repo_map");
    if (!wp) return std::unexpected(std::move(wp.error()));
    index::shared().refresh(wp->path());

    auto map = index::shared().compact_map(static_cast<std::size_t>(a.max_kb) * 1024);
    if (map.empty())
        return ToolOutput{
            "Empty repo map — no recognised source files under " + wp->string()
            + ". Supported: C/C++, Python, JS/TS, Go, Rust.",
            std::nullopt};

    std::string body =
        "# Repo map — " + wp->string() + "\n"
        "# Each line: `<file>  [<symbol>, <symbol>, ...]`.\n"
        "# Use `outline(path)` for one file's full outline; "
        "`signatures(name)` to grep symbol names; `read(path, offset, limit)` "
        "for actual content.\n\n" + std::move(map);
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_repo_map() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"repo_map">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Return a compact tree of every code file in the workspace and "
        "the symbols (functions / classes / etc.) it declares. The "
        "agent's table-of-contents — call this once early in a thread "
        "to know the shape of the codebase before doing any `read` or "
        "`grep`. ~5-15 KB output for medium repos; cap with `max_kb`. "
        "Supports C/C++, Python, JS/TS, Go, Rust.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",   {{"type","string"},
                {"description","Subtree to map (default: workspace root)"}}},
            {"max_kb", {{"type","integer"},
                {"description","Soft cap on output size in KB (default 24, max 96)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<RepoMapArgs>(parse_repo_map_args, run_repo_map);
    return t;
}

} // namespace moha::tools
