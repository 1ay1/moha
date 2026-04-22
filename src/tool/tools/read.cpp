#include "moha/tool/tools.hpp"
#include "moha/tool/util/fs_helpers.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

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
    t.execute = [](const json& args) -> ExecResult {
        std::string raw = args.value("path", "");
        int offset = args.value("offset", 1);
        int limit  = args.value("limit", 2000);
        if (raw.empty())
            return std::unexpected(ToolError{"path required"});
        auto p = util::normalize_path(raw);
        std::error_code ec;
        if (!fs::exists(p, ec))
            return std::unexpected(ToolError{"file not found: " + p.string()
                + ". Run `list_dir` on the parent directory or `glob` by name to verify."});
        if (!fs::is_regular_file(p, ec))
            return std::unexpected(ToolError{"not a regular file: " + p.string()});
        if (util::is_binary_file(p)) {
            uintmax_t sz = fs::file_size(p, ec);
            std::ostringstream msg;
            msg << "cannot read binary file: " << p.string()
                << " (" << (ec ? 0 : sz) << " bytes). "
                << "Use the bash tool with `file`, `hexdump`, or similar "
                << "if you need to inspect it.";
            return std::unexpected(ToolError{msg.str()});
        }
        // Size cap: reading a huge file blows the model's context and rarely
        // helps. Ask the model to page via offset/limit instead.
        constexpr uintmax_t kMaxBytes = 1024u * 1024u;   // 1 MiB
        uintmax_t sz = fs::file_size(p, ec);
        if (!ec && sz > kMaxBytes && offset == 1) {
            std::ostringstream msg;
            msg << "file is " << (sz / 1024) << " KiB (> 1 MiB cap). "
                << "Pass offset/limit to page through it — e.g. "
                << "{\"path\":\"" << p.string() << "\",\"offset\":1,\"limit\":500}, "
                << "then offset=501, etc. For a structural overview, run "
                << "`grep` for the symbols you need.";
            return std::unexpected(ToolError{msg.str()});
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
            if (n >= offset && shown < limit) {
                out << line << "\n";
                shown++;
            }
            n++;
        }
        if (offset > 1 || shown < total_lines) {
            std::ostringstream hint;
            hint << "\n[showing lines " << offset << "-" << (offset + shown - 1)
                 << " of " << total_lines;
            int remaining = total_lines - (offset + shown - 1);
            if (remaining > 0)
                hint << "; " << remaining << " more — pass offset="
                     << (offset + shown) << " for the next chunk";
            hint << "]";
            return ToolOutput{out.str() + hint.str(), std::nullopt};
        }
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

} // namespace moha::tools
