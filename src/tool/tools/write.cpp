#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/io/diff.hpp"

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct WriteArgs {
    util::NormalizedPath path;
    std::string content;
    std::string coercion_note;  // non-empty when `content` was coerced/missing
};

std::expected<WriteArgs, ToolError> parse_write_args(const json& j) {
    util::ArgReader ar(j);
    auto raw = ar.require_str("path");
    if (!raw)
        return std::unexpected(ToolError::invalid_args("path required"));
    // Tolerant coercion: missing/null/array/number content all produce a
    // writable string rather than a red error — the note tells the model
    // what we inferred so it can retry with a proper string if needed.
    std::string note;
    std::string content;
    if (!ar.has("content"))
        note = " (no `content` field provided — wrote empty file; re-run with content if that was not intended)";
    else
        content = ar.str("content", "", &note);
    return WriteArgs{
        util::NormalizedPath{std::move(*raw)},
        std::move(content),
        std::move(note),
    };
}

ExecResult run_write(const WriteArgs& a) {
    const auto& p = a.path.path();
    std::string original;
    std::error_code ec;
    bool exists = fs::exists(p, ec);
    if (exists) {
        if (!fs::is_regular_file(p, ec))
            return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));
        original = util::read_file(p);
    }
    // No-op short-circuit: an identical rewrite is often the model
    // "confirming" a file state it already reached. Skipping the fs
    // touch avoids spurious mtime bumps that break incremental builds.
    if (exists && original == a.content)
        return ToolOutput{"File already matches content — no changes written.",
                          std::nullopt};
    auto change = diff::compute(a.path.string(), original, a.content);
    if (auto err = util::write_file(p, a.content); !err.empty())
        return std::unexpected(ToolError::io(err));
    auto msg = std::format("{} {} ({}+ {}-){}",
                           exists ? "Overwrote" : "Created",
                           a.path.string(), change.added, change.removed,
                           a.coercion_note);
    return ToolOutput{std::move(msg), std::move(change)};
}

} // namespace

ToolDef tool_write() {
    ToolDef t;
    t.name = ToolName{std::string{"write"}};
    t.description = "Write (or overwrite) a file with the given contents. "
                    "Creates parent directories as needed.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","content"}},
        {"properties", {
            {"path",    {{"type","string"}}},
            {"content", {{"type","string"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<WriteArgs>(parse_write_args, run_write);
    return t;
}

} // namespace moha::tools
