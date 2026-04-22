#include "moha/tool/tools.hpp"
#include "moha/tool/util/subprocess.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

ToolDef tool_diagnostics() {
    ToolDef t;
    t.name = ToolName{std::string{"diagnostics"}};
    t.description = "Run the project's build or lint command and return errors/warnings. "
                    "Auto-detects build system (CMake, cargo, go, npm, make).";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"command", {{"type","string"}, {"description",
                "Custom build command. If omitted, auto-detects."}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string cmd = args.value("command", "");
        std::error_code ec;
        // Auto-detect commands use run_argv so we don't depend on shell
        // features like `| head -N` (cmd.exe has no head). The 30k-char
        // truncation caps runaway output.
        std::vector<std::string> auto_argv;
        if (cmd.empty()) {
            if (fs::exists("build/build.ninja", ec) || fs::exists("build/Makefile", ec))
                auto_argv = {"cmake", "--build", "build"};
            else if (fs::exists("Cargo.toml", ec))
                auto_argv = {"cargo", "check"};
            else if (fs::exists("go.mod", ec))
                auto_argv = {"go", "build", "./..."};
            else if (fs::exists("package.json", ec))
                auto_argv = {"npx", "tsc", "--noEmit"};
            else if (fs::exists("Makefile", ec))
                auto_argv = {"make", "-n"};
            else
                return std::unexpected(ToolError{"no build system detected; pass a command"});
        }
        auto output = auto_argv.empty() ? util::run_command(cmd) : util::run_argv(auto_argv);
        if (output.empty()) return ToolOutput{"no diagnostics (clean build)", std::nullopt};
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

} // namespace moha::tools
