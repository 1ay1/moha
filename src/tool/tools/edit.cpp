#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/fuzzy_match.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/diff/diff.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct OneEdit {
    std::string old_text;
    std::string new_text;
    bool        replace_all = false;
};

struct EditArgs {
    util::NormalizedPath  path;
    std::vector<OneEdit>  edits;
    std::string           display_description;  // one-line UI summary; optional
};

// Accepts three shapes, in preference order:
//   1. `edits: [{old_text, new_text, replace_all?}, ...]`   (new canonical)
//   2. `old_text` / `new_text` at top level    (Zed's legacy single-edit)
//   3. `old_string` / `new_string` at top level (moha's original schema)
// Missing / wrong-typed fields are tolerated where recoverable — we only
// return an error when there is genuinely nothing to do.
std::expected<EditArgs, ToolError> parse_edit_args(const json& j) {
    util::ArgReader ar(j);
    if (!ar.is_object())
        return std::unexpected(ToolError::invalid_args("args must be an object"));

    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));

    std::string desc = ar.str("display_description", "");
    std::vector<OneEdit> edits;

    // Shape 1: edits array.
    if (auto raw = ar.raw("edits"); raw && raw->is_array()) {
        edits.reserve(raw->size());
        int idx = 0;
        for (const auto& e : *raw) {
            ++idx;
            if (!e.is_object())
                return std::unexpected(ToolError::invalid_args(
                    std::format("edits[{}]: expected object", idx - 1)));
            util::ArgReader sub(e);
            auto old_opt = sub.require_str("old_text");
            // Accept old_string too.
            if (!old_opt) old_opt = sub.require_str("old_string");
            if (!old_opt)
                return std::unexpected(ToolError::invalid_args(
                    std::format("edits[{}]: old_text required", idx - 1)));
            std::string new_text = sub.str("new_text", "");
            if (new_text.empty() && sub.has("new_string"))
                new_text = sub.str("new_string", "");
            edits.push_back(OneEdit{
                std::move(*old_opt),
                std::move(new_text),
                sub.boolean("replace_all", false),
            });
        }
    } else {
        // Shape 2/3: single edit at top level (old_text or old_string).
        auto old_opt = ar.require_str("old_string");
        if (!old_opt) old_opt = ar.require_str("old_text");
        if (!old_opt)
            return std::unexpected(ToolError::invalid_args(
                "no edits provided — pass either `edits: [...]` or "
                "`old_string`/`new_string` at top level"));
        std::string new_s = ar.str("new_string", "");
        if (new_s.empty() && ar.has("new_text"))
            new_s = ar.str("new_text", "");
        edits.push_back(OneEdit{
            std::move(*old_opt), std::move(new_s),
            ar.boolean("replace_all", false),
        });
    }

    if (edits.empty())
        return std::unexpected(ToolError::invalid_args(
            "edits array is empty — nothing to change"));

    // Per-edit sanity: empty old_text is never legal; identical old/new is
    // a no-op we'd rather not silently accept.
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto& e = edits[i];
        if (e.old_text.empty())
            return std::unexpected(ToolError::invalid_args(
                std::format("edits[{}]: old_text cannot be empty", i)));
        if (e.old_text == e.new_text)
            return std::unexpected(ToolError::invalid_args(std::format(
                "edits[{}]: old_text and new_text are identical — nothing to change",
                i)));
    }

    return EditArgs{
        util::NormalizedPath{std::move(*path_opt)},
        std::move(edits),
        std::move(desc),
    };
}

// Apply a single edit to `buf`. Returns number of replacements; sets `err`
// on terminal failure (ambiguous match, not found).
int apply_one(std::string& buf, const OneEdit& e,
              const std::string& path_str, std::string& err) {
    if (e.replace_all) {
        // Exact match, build fresh buffer.
        std::string out;
        out.reserve(buf.size());
        std::size_t cursor = 0;
        int n = 0;
        for (;;) {
            auto pos = buf.find(e.old_text, cursor);
            if (pos == std::string::npos) break;
            out.append(buf, cursor, pos - cursor);
            out.append(e.new_text);
            cursor = pos + e.old_text.size();
            ++n;
        }
        if (n == 0) {
            err = "old_text not found in " + path_str;
            return 0;
        }
        out.append(buf, cursor, std::string::npos);
        buf = std::move(out);
        return n;
    }

    auto m = util::fuzzy_find(buf, e.old_text);
    if (!m.ok) {
        if (m.count >= 2) {
            err = std::format(
                "old_text appears {} times in {}. Add surrounding context to "
                "make it unique, or pass replace_all=true.", m.count, path_str);
        } else {
            // Hint: indentation mismatch is the common cause.
            std::string hint;
            std::string squashed_old, squashed_buf;
            for (char c : e.old_text)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    squashed_old += c;
            for (char c : buf)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    squashed_buf += c;
            if (!squashed_old.empty()
                && squashed_buf.find(squashed_old) != std::string::npos)
                hint = " (the text exists but whitespace differs — match the "
                       "exact indentation)";
            err = "old_text not found in " + path_str + hint;
        }
        return 0;
    }
    // Splice.
    std::string out;
    out.reserve(buf.size() - m.len + e.new_text.size());
    out.append(buf, 0, m.pos);
    out.append(e.new_text);
    out.append(buf, m.pos + m.len, std::string::npos);
    buf = std::move(out);
    return 1;
}

ExecResult run_edit(const EditArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: "
            + a.path.string()
            + ". Run `list_dir` on the parent directory or `glob` by name "
              "to verify."));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file(
            "not a regular file: " + a.path.string()));

    std::string original = util::read_file(p);
    std::string updated  = original;
    int total_replacements = 0;

    // Apply in order. If any edit fails, report which one and abort — we'd
    // rather surface a precise error than leave the file half-transformed.
    for (std::size_t i = 0; i < a.edits.size(); ++i) {
        std::string err;
        int n = apply_one(updated, a.edits[i], a.path.string(), err);
        if (n == 0) {
            std::string ctx = a.edits.size() == 1
                ? err
                : std::format("edits[{}]: {}", i, err);
            // Preserve the error category based on the message shape.
            if (err.find("appears") != std::string::npos)
                return std::unexpected(ToolError::ambiguous(std::move(ctx)));
            return std::unexpected(ToolError::no_match(std::move(ctx)));
        }
        total_replacements += n;
    }

    if (original == updated)
        return ToolOutput{"No edits were made — all old_text / new_text pairs "
                          "produced identical content.", std::nullopt};

    auto change = diff::compute(a.path.string(), original, updated);
    if (auto werr = util::write_file(p, updated); !werr.empty())
        return std::unexpected(ToolError::io(werr));

    std::string unified = diff::render_unified(change);
    std::ostringstream msg;
    if (!a.display_description.empty())
        msg << a.display_description << "\n\n";
    msg << "Edited " << a.path.string() << " (" << change.added << "+ "
        << change.removed << "-";
    if (a.edits.size() > 1)
        msg << ", " << a.edits.size() << " edits";
    msg << "):\n\n```diff\n" << unified;
    if (unified.empty() || unified.back() != '\n') msg << "\n";
    msg << "```";
    return ToolOutput{msg.str(), std::move(change)};
}

} // namespace

ToolDef tool_edit() {
    ToolDef t;
    t.name = ToolName{std::string{"edit"}};
    t.description =
        "Modify an EXISTING file by applying one or more targeted text "
        "substitutions. PREFER this tool over `write` whenever you are "
        "changing only part of a file — it streams less data and produces "
        "a reviewable diff. Pass `edits: [{old_text, new_text}, ...]`; "
        "every edit is applied in order, each `old_text` must be uniquely "
        "present in the file (or pass `replace_all: true`). Trailing-"
        "whitespace differences are tolerated when matching, but indentation "
        "is not — copy `old_text` verbatim from a recent `read`. Include a "
        "brief `display_description` (e.g. 'Fix null-deref in auth.cpp') — "
        "it shows in the card while edits stream.";
    // Property order matters for streaming UX (see write.cpp for context).
    // path → display_description → edits puts the small fields first so the
    // tool card paints meaningful content within ~1s of the model starting
    // to emit, rather than after the entire edits[] array streams.
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","edits"}},
        {"properties", {
            {"path", {{"type","string"},
                {"description","Absolute or relative path of the existing "
                               "file. Stream this FIRST."}}},
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the card while "
                               "edits stream — e.g. 'Fix null-deref in "
                               "auth.cpp'. Stream second."}}},
            {"edits", {
                {"type","array"},
                {"description","One or more edits, applied in order."},
                {"items", {
                    {"type","object"},
                    {"required", {"old_text","new_text"}},
                    {"properties", {
                        {"old_text",  {{"type","string"},
                            {"description","Exact text to find. Must be "
                                "unique unless replace_all is true. "
                                "Trailing-whitespace differences are "
                                "tolerated, indentation is not."}}},
                        {"new_text",  {{"type","string"},
                            {"description","Replacement text."}}},
                        {"replace_all",{{"type","boolean"},
                            {"default", false},
                            {"description","Replace every occurrence "
                                "instead of exactly one."}}},
                    }},
                }},
            }},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<EditArgs>(parse_edit_args, run_edit);
    return t;
}

} // namespace moha::tools
