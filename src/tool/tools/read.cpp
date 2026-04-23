#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct ReadArgs {
    util::NormalizedPath path;
    int offset;
    int limit;
    std::string display_description;
};

std::expected<ReadArgs, ToolError> parse_read_args(const json& j) {
    util::ArgReader ar(j);
    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    int offset = ar.integer("offset", 1);
    if (offset < 1) offset = 1;
    // Zed-style `end_line` is inclusive (last line shown). Translate into our
    // limit = end_line - offset + 1. Only honored when the caller actually
    // passed end_line and didn't also pass an explicit limit.
    int limit = ar.integer("limit", 2000);
    if (ar.has("end_line") && !ar.has("limit")) {
        int end_line = ar.integer("end_line", 0);
        if (end_line >= offset) limit = end_line - offset + 1;
    }
    if (limit <= 0) limit = 2000;
    return ReadArgs{
        util::NormalizedPath{std::move(*path_opt)},
        offset,
        limit,
        ar.str("display_description", ""),
    };
}

ExecResult run_read(const ReadArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: " + a.path.string()
            + ". Run `list_dir` on the parent directory or `glob` by name to verify."));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));
    // Size cap: reading a huge file blows the model's context and rarely
    // helps. Ask the model to page via offset/limit instead.
    constexpr uintmax_t kMaxBytes = 1024u * 1024u;   // 1 MiB
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > kMaxBytes && a.offset == 1) {
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 1 MiB cap). "
            "Pass offset/limit to page through it — e.g. "
            "{{\"path\":\"{}\",\"offset\":1,\"limit\":500}}, "
            "then offset=501, etc. For a structural overview, run "
            "`grep` for the symbols you need.",
            sz / 1024, a.path.string())));
    }
    // Single open, single read. Then one linear scan builds the slice AND
    // detects binary content (NUL byte) AND counts lines in one pass.
    auto content = util::read_file(p);
    std::string out;
    // Reserve the full content size on small files (one big alloc, no
    // resize) and cap at 1 MiB on larger files where the slice is
    // almost certainly a page of the whole thing — the realloc cascade
    // for a million-byte append is what we're avoiding, not the peak RSS.
    out.reserve(content.size() < 1024 * 1024 ? content.size() : 1024 * 1024);
    int total_lines = 0;
    int shown = 0;
    size_t line_start = 0;
    const size_t N = content.size();
    for (size_t i = 0; i < N; ++i) {
        char c = content[i];
        if (c == '\0') {
            return std::unexpected(ToolError::binary(std::format(
                "cannot read binary file: {} ({} bytes). "
                "Use the bash tool with `file`, `hexdump`, or similar "
                "if you need to inspect it.",
                a.path.string(), N)));
        }
        if (c == '\n') {
            ++total_lines;
            int n = total_lines; // 1-based index of the line just ended
            if (n >= a.offset && shown < a.limit) {
                out.append(content.data() + line_start, i - line_start + 1);
                ++shown;
            }
            line_start = i + 1;
        }
    }
    // Trailing line without a final newline.
    if (line_start < N) {
        ++total_lines;
        int n = total_lines;
        if (n >= a.offset && shown < a.limit) {
            out.append(content.data() + line_start, N - line_start);
            out.push_back('\n');
            ++shown;
        }
    }
    if (a.offset > 1 || shown < total_lines) {
        std::string hint = std::format("\n[showing lines {}-{} of {}",
                                       a.offset, a.offset + shown - 1, total_lines);
        int remaining = total_lines - (a.offset + shown - 1);
        if (remaining > 0)
            hint += std::format("; {} more — pass offset={} (or start_line={}) "
                                "for the next chunk",
                                remaining, a.offset + shown, a.offset + shown);
        hint += "]";
        out += hint;
    }
    if (!a.display_description.empty())
        out = a.display_description + "\n\n" + out;
    return ToolOutput{std::move(out), std::nullopt};
}

} // namespace

ToolDef tool_read() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"read">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Read a file from the filesystem. Returns up to 2000 lines "
                    "starting at an optional offset. For large files, page via "
                    "offset/limit (or start_line/end_line) rather than reading "
                    "whole. Include a brief `display_description` so the user "
                    "sees why you're reading.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"path"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",       {{"type","string"}, {"description","Absolute or relative path"}}},
            {"offset",     {{"type","integer"}, {"description","Start line (1-based)"}}},
            {"limit",      {{"type","integer"}, {"description","Max lines"}}},
            {"start_line", {{"type","integer"}, {"description","Alias for offset (Zed-style)"}}},
            {"end_line",   {{"type","integer"}, {"description","Inclusive last line (Zed-style)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<ReadArgs>(parse_read_args, run_read);
    return t;
}

} // namespace moha::tools
