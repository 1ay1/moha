#include "moha/tool/tools.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/io/diff.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

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
    t.execute = [](const json& args) -> ExecResult {
        std::string raw = args.value("path", "");
        if (raw.empty())
            return std::unexpected(ToolError{"path required"});
        // An explicit empty string is legitimate (creating an empty file),
        // but a missing key almost always means the model forgot to include
        // the content — fail loudly so it self-corrects.
        if (!args.is_object() || !args.contains("content"))
            return std::unexpected(ToolError{"content required (pass empty string for an empty file)"});
        if (!args["content"].is_string())
            return std::unexpected(ToolError{"content must be a string"});
        std::string content = args["content"].get<std::string>();
        auto p = util::normalize_path(raw);
        std::string original;
        std::error_code ec;
        bool exists = fs::exists(p, ec);
        if (exists) {
            if (!fs::is_regular_file(p, ec))
                return std::unexpected(ToolError{"not a regular file: " + p.string()});
            original = util::read_file(p);
        }
        // No-op short-circuit: an identical rewrite is often the model
        // "confirming" a file state it already reached. Skipping the fs
        // touch avoids spurious mtime bumps that break incremental builds.
        if (exists && original == content)
            return ToolOutput{"File already matches content — no changes written.",
                              std::nullopt};
        auto change = diff::compute(p.string(), original, content);
        if (auto err = util::write_file(p, content); !err.empty())
            return std::unexpected(ToolError{err});
        std::ostringstream msg;
        msg << (exists ? "Overwrote " : "Created ") << p.string()
            << " (" << change.added << "+ " << change.removed << "-)";
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

} // namespace moha::tools
