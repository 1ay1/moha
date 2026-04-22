#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/io/diff.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct EditArgs {
    util::NormalizedPath path;
    std::string old_string;
    std::string new_string;
    bool replace_all;
};

std::expected<EditArgs, ToolError> parse_edit_args(const json& j) {
    util::ArgReader ar(j);
    if (!ar.is_object())
        return std::unexpected(ToolError::invalid_args("args must be an object"));
    auto raw = ar.require_str("path");
    if (!raw)
        return std::unexpected(ToolError::invalid_args("path required"));
    // old_string is the needle — genuinely required. new_string is
    // optional: a missing field or null means "delete the match", which
    // is the same shape as new_string:"". We'd rather silently accept
    // that than flash a red error card for a recoverable model slip.
    auto old_opt = ar.require_str("old_string");
    if (!old_opt)
        return std::unexpected(ToolError::invalid_args("old_string required (must be a string)"));
    std::string old_s = *std::move(old_opt);
    if (old_s.empty())
        return std::unexpected(ToolError::invalid_args("old_string cannot be empty"));
    std::string new_s = ar.str("new_string", "");
    if (old_s == new_s)
        return std::unexpected(ToolError::invalid_args("old_string and new_string are identical — nothing to change"));
    return EditArgs{
        util::NormalizedPath{std::move(*raw)},
        std::move(old_s),
        std::move(new_s),
        ar.boolean("replace_all", false),
    };
}

ExecResult run_edit(const EditArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: " + a.path.string()
            + ". Run `list_dir` on the parent directory or `glob` by name to verify."));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));
    std::string original = util::read_file(p);
    std::string updated = original;
    if (a.replace_all) {
        size_t pos = 0; int n = 0;
        while ((pos = updated.find(a.old_string, pos)) != std::string::npos) {
            updated.replace(pos, a.old_string.size(), a.new_string);
            pos += a.new_string.size();
            n++;
        }
        if (n == 0)
            return std::unexpected(ToolError::no_match("old_string not found in " + a.path.string()));
    } else {
        auto pos = updated.find(a.old_string);
        if (pos == std::string::npos) {
            // Help the model localize its error: if the string isn't
            // unique, whitespace is the most common cause.
            std::string hint;
            std::string squashed_old, squashed_new;
            for (char c : a.old_string) if (c != ' ' && c != '\t' && c != '\n') squashed_old += c;
            for (char c : updated) if (c != ' ' && c != '\t' && c != '\n') squashed_new += c;
            if (!squashed_old.empty()
                && squashed_new.find(squashed_old) != std::string::npos)
                hint = " (the text exists but whitespace differs — match the exact indentation)";
            return std::unexpected(ToolError::no_match("old_string not found in " + a.path.string() + hint));
        }
        if (auto pos2 = updated.find(a.old_string, pos + 1); pos2 != std::string::npos) {
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
            msg << "old_string appears multiple times in " << a.path.string()
                << ". Matches at lines: " << line_of(pos);
            size_t cursor = pos2;
            int count = 2;
            while (cursor != std::string::npos && count <= 5) {
                msg << ", " << line_of(cursor);
                cursor = updated.find(a.old_string, cursor + 1);
                count++;
            }
            if (cursor != std::string::npos) msg << ", ...";
            msg << ". Add surrounding context to make old_string unique, "
                   "or pass replace_all=true to replace every occurrence.";
            return std::unexpected(ToolError::ambiguous(msg.str()));
        }
        updated.replace(pos, a.old_string.size(), a.new_string);
    }
    if (original == updated)
        return ToolOutput{"No edits were made — old_string and new_string "
                          "produced identical content.", std::nullopt};
    auto change = diff::compute(a.path.string(), original, updated);
    if (auto err = util::write_file(p, updated); !err.empty())
        return std::unexpected(ToolError::io(err));
    // ```diff fenced block so the model sees exactly what changed —
    // makes follow-up edits more accurate and mirrors Zed's
    // EditFileTool output shape.
    std::string unified = diff::render_unified(change);
    std::ostringstream msg;
    msg << "Edited " << a.path.string() << " (" << change.added << "+ "
        << change.removed << "-):\n\n```diff\n" << unified;
    if (unified.empty() || unified.back() != '\n') msg << "\n";
    msg << "```";
    return ToolOutput{msg.str(), std::move(change)};
}

} // namespace

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
    t.execute = util::adapt<EditArgs>(parse_edit_args, run_edit);
    return t;
}

} // namespace moha::tools
