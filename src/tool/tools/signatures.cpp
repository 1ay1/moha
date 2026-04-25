#include "moha/index/repo_index.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct SignaturesArgs {
    std::string pattern;
    int         limit;
    std::string display_description;
};

std::expected<SignaturesArgs, ToolError> parse_signatures_args(const json& j) {
    util::ArgReader ar(j);
    auto p = ar.require_str("pattern");
    if (!p)
        return std::unexpected(ToolError::invalid_args("pattern required"));
    int limit = ar.integer("limit", 50);
    if (limit < 1)   limit = 1;
    if (limit > 200) limit = 200;
    return SignaturesArgs{
        *std::move(p),
        limit,
        ar.str("display_description", ""),
    };
}

ExecResult run_signatures(const SignaturesArgs& a) {
    // Make sure the index is at least minimally populated. Cheap if
    // already-fresh files dominate.
    if (!index::shared().ready())
        index::shared().refresh(fs::current_path());

    auto hits = index::shared().find_symbols(a.pattern,
                                              static_cast<std::size_t>(a.limit));
    if (hits.empty())
        return ToolOutput{
            "No symbols match '" + a.pattern + "'. Try a shorter "
            "pattern, or call `repo_map` to refresh the index if "
            "you've just added files.",
            std::nullopt};

    std::ostringstream out;
    out << "Symbols matching '" << a.pattern << "' ("
        << hits.size() << (hits.size() >= static_cast<std::size_t>(a.limit) ? "+" : "")
        << " hits, signatures only — read the file for the body):\n\n";
    fs::path cur;
    for (const auto& [path, s] : hits) {
        if (path != cur) {
            cur = path;
            out << "\n## " << path.string() << "\n";
        }
        out << "  L" << s.line << "  ["
            << index::to_string(s.kind) << "] " << s.name;
        if (!s.signature.empty() && s.signature.size() > s.name.size() + 2)
            out << "    " << s.signature;
        out << "\n";
    }

    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_signatures() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"signatures">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Search the workspace symbol index for any declaration whose "
        "name contains `pattern` (case-insensitive substring). Returns "
        "the kind, file, line, and signature line — no bodies. Cheaper "
        "and more targeted than `grep` for symbol-shaped queries: "
        "find me every `parse_*`, every `*Handler`, every class ending "
        "in `Manager`. Use `find_definition` when you have an exact name.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"pattern", {{"type","string"},
                {"description","Substring to look for in symbol names"}}},
            {"limit",   {{"type","integer"},
                {"description","Max hits returned (default 50, max 200)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<SignaturesArgs>(parse_signatures_args, run_signatures);
    return t;
}

} // namespace moha::tools
