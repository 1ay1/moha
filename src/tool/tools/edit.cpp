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

ToolDef tool_edit() {
    ToolDef t;
    t.name = ToolName{std::string{"edit"}};
    t.description = "Edit a file by replacing an exact old_string with new_string. "
                    "The old_string must be uniquely present unless replace_all is set.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","old_string","new_string"}},
        {"properties", {
            {"path",       {{"type","string"}}},
            {"old_string", {{"type","string"}}},
            {"new_string", {{"type","string"}}},
            {"replace_all",{{"type","boolean"}, {"default", false}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        if (!args.is_object())
            return std::unexpected(ToolError{"args must be an object"});
        std::string raw = args.value("path", "");
        if (raw.empty())
            return std::unexpected(ToolError{"path required"});
        // old_string is the needle — genuinely required. new_string is
        // optional: a missing field or null means "delete the match", which
        // is the same shape as new_string:"". We'd rather silently accept
        // that than flash a red error card for a recoverable model slip.
        if (!args.contains("old_string") || !args["old_string"].is_string())
            return std::unexpected(ToolError{"old_string required (must be a string)"});
        std::string old_s = args["old_string"].get<std::string>();
        std::string new_s;
        if (args.contains("new_string")) {
            const auto& v = args["new_string"];
            if (v.is_string())      new_s = v.get<std::string>();
            else if (!v.is_null())  new_s = v.dump();
        }
        bool all = args.value("replace_all", false);
        if (old_s.empty())
            return std::unexpected(ToolError{"old_string cannot be empty"});
        if (old_s == new_s)
            return std::unexpected(ToolError{"old_string and new_string are identical — nothing to change"});
        auto p = util::normalize_path(raw);
        std::error_code ec;
        if (!fs::exists(p, ec))
            return std::unexpected(ToolError{"file not found: " + p.string()
                + ". Run `list_dir` on the parent directory or `glob` by name to verify."});
        if (!fs::is_regular_file(p, ec))
            return std::unexpected(ToolError{"not a regular file: " + p.string()});
        std::string original = util::read_file(p);
        std::string updated = original;
        if (all) {
            size_t pos = 0; int n = 0;
            while ((pos = updated.find(old_s, pos)) != std::string::npos) {
                updated.replace(pos, old_s.size(), new_s);
                pos += new_s.size();
                n++;
            }
            if (n == 0)
                return std::unexpected(ToolError{"old_string not found in " + p.string()});
        } else {
            auto pos = updated.find(old_s);
            if (pos == std::string::npos) {
                // Help the model localize its error: if the string isn't
                // unique, whitespace is the most common cause.
                std::string hint;
                std::string squashed_old, squashed_new;
                for (char c : old_s) if (c != ' ' && c != '\t' && c != '\n') squashed_old += c;
                for (char c : updated) if (c != ' ' && c != '\t' && c != '\n') squashed_new += c;
                if (!squashed_old.empty()
                    && squashed_new.find(squashed_old) != std::string::npos)
                    hint = " (the text exists but whitespace differs — match the exact indentation)";
                return std::unexpected(ToolError{"old_string not found in " + p.string() + hint});
            }
            if (auto pos2 = updated.find(old_s, pos + 1); pos2 != std::string::npos) {
                // Ambiguous match — name each occurrence with its line
                // number so the model can pick unique surrounding context
                // on retry.
                auto line_of = [&](size_t off) {
                    int n = 1;
                    for (size_t i = 0; i < off && i < updated.size(); ++i)
                        if (updated[i] == '\n') n++;
                    return n;
                };
                std::ostringstream msg;
                msg << "old_string appears multiple times in " << p.string()
                    << ". Matches at lines: " << line_of(pos);
                size_t cursor = pos2;
                int count = 2;
                while (cursor != std::string::npos && count <= 5) {
                    msg << ", " << line_of(cursor);
                    cursor = updated.find(old_s, cursor + 1);
                    count++;
                }
                if (cursor != std::string::npos) msg << ", ...";
                msg << ". Add surrounding context to make old_string unique, "
                       "or pass replace_all=true to replace every occurrence.";
                return std::unexpected(ToolError{msg.str()});
            }
            updated.replace(pos, old_s.size(), new_s);
        }
        if (original == updated)
            return ToolOutput{"No edits were made — old_string and new_string "
                              "produced identical content.", std::nullopt};
        auto change = diff::compute(p.string(), original, updated);
        if (auto err = util::write_file(p, updated); !err.empty())
            return std::unexpected(ToolError{err});
        // ```diff fenced block so the model sees exactly what changed —
        // makes follow-up edits more accurate and mirrors Zed's
        // EditFileTool output shape.
        std::string unified = diff::render_unified(change);
        std::ostringstream msg;
        msg << "Edited " << p.string() << " (" << change.added << "+ "
            << change.removed << "-):\n\n```diff\n" << unified;
        if (unified.empty() || unified.back() != '\n') msg << "\n";
        msg << "```";
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

} // namespace moha::tools
