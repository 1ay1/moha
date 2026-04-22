#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <filesystem>
#include <format>
#include <sstream>
#include <string>
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
};

std::expected<ReadArgs, ToolError> parse_read_args(const json& j) {
    util::ArgReader ar(j);
    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    return ReadArgs{
        util::NormalizedPath{std::move(*path_opt)},
        ar.integer("offset", 1),
        ar.integer("limit", 2000),
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
    if (util::is_binary_file(p)) {
        uintmax_t sz = fs::file_size(p, ec);
        return std::unexpected(ToolError::binary(std::format(
            "cannot read binary file: {} ({} bytes). "
            "Use the bash tool with `file`, `hexdump`, or similar "
            "if you need to inspect it.",
            a.path.string(), ec ? 0u : sz)));
    }
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
    auto content = util::read_file(p);
    int total_lines = 0;
    for (char c : content) if (c == '\n') ++total_lines;
    if (!content.empty() && content.back() != '\n') ++total_lines;
    std::istringstream iss(content);
    std::ostringstream out;
    std::string line;
    int n = 1;
    int shown = 0;
    while (std::getline(iss, line)) {
        if (n >= a.offset && shown < a.limit) {
            out << line << "\n";
            shown++;
        }
        n++;
    }
    if (a.offset > 1 || shown < total_lines) {
        std::ostringstream hint;
        hint << "\n[showing lines " << a.offset << "-" << (a.offset + shown - 1)
             << " of " << total_lines;
        int remaining = total_lines - (a.offset + shown - 1);
        if (remaining > 0)
            hint << "; " << remaining << " more — pass offset="
                 << (a.offset + shown) << " for the next chunk";
        hint << "]";
        return ToolOutput{out.str() + hint.str(), std::nullopt};
    }
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_read() {
    ToolDef t;
    t.name = ToolName{std::string{"read"}};
    t.description = "Read a file from the filesystem. Returns up to 2000 lines "
                    "starting at an optional offset.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"path"}},
        {"properties", {
            {"path",   {{"type","string"}, {"description","Absolute or relative path"}}},
            {"offset", {{"type","integer"}, {"description","Start line (1-based)"}}},
            {"limit",  {{"type","integer"}, {"description","Max lines"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p == Profile::Minimal; };
    t.execute = util::adapt<ReadArgs>(parse_read_args, run_read);
    return t;
}

} // namespace moha::tools
