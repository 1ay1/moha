#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/diff/diff.hpp"

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
    std::string display_description;
    std::string coercion_note;  // non-empty when `content` was coerced from non-string
};

// Keys we *know* are metadata and will never be the file body.  Anything
// else in the args object is a candidate for "content" under a novel name
// (models love to key content as `html`, `code`, `source`, `payload`, …).
constexpr std::string_view kMetadataKeys[] = {
    "path", "file_path", "filepath", "filename",
    "display_description", "description",
    "append", "mode", "encoding", "overwrite",
};

bool is_metadata_key(std::string_view k) noexcept {
    for (auto m : kMetadataKeys) if (k == m) return true;
    return false;
}

// Last-resort salvage when neither `content` nor any known alias is present:
// pick the largest string value in the args object whose key isn't metadata.
// Rationale — a write tool call that carries a ~KB-scale string under *any*
// key is overwhelmingly the file body the model meant to send.  Beats
// rejecting the call and forcing a retry (which often loops).  Returns the
// picked value and (in `which`) the key we pulled it from so the tool can
// tell the model "I used your `html` field as content — rename to `content`
// next time".
std::optional<std::string> salvage_largest_string(const json& j,
                                                  std::string& which) {
    if (!j.is_object()) return std::nullopt;
    const std::string* best_key = nullptr;
    const std::string* best_val = nullptr;
    std::size_t best_len = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (is_metadata_key(it.key())) continue;
        if (!it->is_string()) continue;
        const auto& s = it->get_ref<const std::string&>();
        if (s.size() > best_len) {
            best_len = s.size();
            best_key = &it.key();
            best_val = &s;
        }
    }
    // Guard: require at least a token's worth so we don't pick up a stray
    // short metadata string the alias list missed.
    if (!best_val || best_len < 4) return std::nullopt;
    which = *best_key;
    return *best_val;
}

// Summarize top-level keys the model sent. Used in error messages so the
// model gets a concrete hint about what it actually produced vs. what was
// expected, making the loop-on-retry failure mode less sticky.
std::string describe_keys(const json& j) {
    if (!j.is_object() || j.empty()) return "(no object / empty)";
    std::string out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!out.empty()) out += ", ";
        out += it.key();
    }
    return out;
}

std::expected<WriteArgs, ToolError> parse_write_args(const json& j) {
    util::ArgReader ar(j);
    auto raw = ar.require_str("path");
    if (!raw)
        return std::unexpected(ToolError::invalid_args(
            std::format("path required (received keys: {})", describe_keys(j))));

    std::string note;
    std::string content;
    // Preferred path: canonical key or any of the built-in aliases
    // (file_text, text, body, …). `has` is alias-aware.
    if (ar.has("content")) {
        content = ar.str("content", "", &note);
    } else {
        // Fallback: scan for a large non-metadata string. This keeps the
        // tool usable when the model picks an off-spec key like `html` or
        // `code`, which previously caused an irrecoverable error loop.
        std::string picked_key;
        auto rescued = salvage_largest_string(j, picked_key);
        if (!rescued) {
            return std::unexpected(ToolError::invalid_args(std::format(
                "content required — no `content` field or known alias "
                "(file_text, text, body, data, contents, file_content) "
                "was present. Received keys: {}. "
                "Re-run with the full file body in the `content` field.",
                describe_keys(j))));
        }
        content = std::move(*rescued);
        note = std::format(" (content was pulled from non-standard key "
                           "`{}` — please use `content` next time)",
                           picked_key);
    }
    return WriteArgs{
        util::NormalizedPath{std::move(*raw)},
        std::move(content),
        ar.str("display_description", ""),
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
    std::string prefix;
    if (!a.display_description.empty())
        prefix = a.display_description + "\n\n";
    auto msg = std::format("{}{} {} ({}+ {}-){}",
                           prefix,
                           exists ? "Overwrote" : "Created",
                           a.path.string(), change.added, change.removed,
                           a.coercion_note);
    return ToolOutput{std::move(msg), std::move(change)};
}

} // namespace

ToolDef tool_write() {
    ToolDef t;
    t.name = ToolName{std::string{"write"}};
    // CC's exact Write tool description (verbatim from the v2.1.113 binary).
    // Matching byte-for-byte keeps the model on the same well-trained path
    // it uses for Claude Code — off-spec descriptions and extra fields
    // measurably slow input_json_delta streaming for big bodies (model emits
    // pre-content boilerplate; server appears to route differently). The
    // alias-tolerant arg reader still accepts `path` as a legacy synonym for
    // `file_path`, so existing on-disk thread state keeps loading.
    t.description =
        "Writes a file to the local filesystem.\n\n"
        "Usage:\n"
        "- This tool will overwrite the existing file if there is one at the "
        "provided path.\n"
        "- If this is an existing file, you MUST use the Read tool first to "
        "read the file's contents.\n"
        "- Prefer the Edit tool for modifying existing files — it only sends "
        "the diff. Only use this tool to create new files or for complete "
        "rewrites.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"file_path","content"}},
        {"properties", {
            {"file_path", {{"type","string"},
                {"description","The absolute path to the file to write "
                               "(must be absolute, not relative)."}}},
            {"content",   {{"type","string"},
                {"description","The content to write to the file."}}},
        }},
    };
    // Fine-grained tool streaming: tells Anthropic's edge to stream the
    // `content` field token-by-token via `input_json_delta` instead of
    // buffering+trickling. Without this, multi-KB write bodies arrive at
    // 0–1 tok/s on a stream that's otherwise hitting 60 tok/s for prose.
    t.eager_input_streaming = true;
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<WriteArgs>(parse_write_args, run_write);
    return t;
}

} // namespace moha::tools
