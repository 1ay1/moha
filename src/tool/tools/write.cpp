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
        // Recover from common Claude tool-call shapes instead of failing:
        //   • content missing            → empty file
        //   • content: null              → empty file
        //   • content: number/bool       → coerce via dump()
        //   • content: array of strings  → join with "\n" (sometimes emitted
        //                                   when the model gets confused and
        //                                   treats content as "lines")
        // The output below names what we received so the model can re-issue
        // with proper content if it intended otherwise, without a red error.
        std::string content;
        std::string coercion_note;
        if (args.is_object() && args.contains("content")) {
            const auto& c = args["content"];
            if (c.is_string())       content = c.get<std::string>();
            else if (c.is_null())    coercion_note = " (content was null — wrote empty file)";
            else if (c.is_array()) {
                for (std::size_t i = 0; i < c.size(); ++i) {
                    if (i) content += '\n';
                    if (c[i].is_string()) content += c[i].get<std::string>();
                    else                  content += c[i].dump();
                }
                coercion_note = " (content was an array — joined with newlines)";
            } else {
                content = c.dump();
                coercion_note = " (content was not a string — coerced)";
            }
        } else {
            coercion_note = " (no `content` field provided — wrote empty file; re-run with content if that was not intended)";
        }
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
            << " (" << change.added << "+ " << change.removed << "-)"
            << coercion_note;
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

} // namespace moha::tools
