#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct DiagnosticsArgs {
    std::string command;  // empty means auto-detect
};

// Typed detection result — stringly-typed "cmake" / "cargo" would let a
// typo silently fall through to the default branch. An enum class + a
// single `detect_build_system` that names exactly the supported systems
// keeps the mapping from marker file → argv localised and total.
enum class BuildSystem { None, CMake, Cargo, Go, Node, Make };

[[nodiscard]] BuildSystem detect_build_system() noexcept {
    std::error_code ec;
    if (fs::exists("build/build.ninja", ec) || fs::exists("build/Makefile", ec)) return BuildSystem::CMake;
    if (fs::exists("Cargo.toml", ec))    return BuildSystem::Cargo;
    if (fs::exists("go.mod", ec))        return BuildSystem::Go;
    if (fs::exists("package.json", ec))  return BuildSystem::Node;
    if (fs::exists("Makefile", ec))      return BuildSystem::Make;
    return BuildSystem::None;
}

[[nodiscard]] std::vector<std::string> build_argv_for(BuildSystem bs) {
    switch (bs) {
        case BuildSystem::CMake: return {"cmake", "--build", "build"};
        case BuildSystem::Cargo: return {"cargo", "check"};
        case BuildSystem::Go:    return {"go", "build", "./..."};
        case BuildSystem::Node:  return {"npx", "tsc", "--noEmit"};
        case BuildSystem::Make:  return {"make", "-n"};
        case BuildSystem::None:  return {};
    }
    return {};
}

std::expected<DiagnosticsArgs, ToolError> parse_diagnostics_args(const json& j) {
    util::ArgReader ar(j);
    return DiagnosticsArgs{ar.str("command", "")};
}

ExecResult run_diagnostics(const DiagnosticsArgs& a) {
    std::vector<std::string> auto_argv;
    if (a.command.empty()) {
        auto_argv = build_argv_for(detect_build_system());
        if (auto_argv.empty())
            return std::unexpected(ToolError::not_found("no build system detected; pass a command"));
    }
    auto output = auto_argv.empty() ? util::run_command(a.command)
                                    : util::run_argv(auto_argv);
    if (output.empty()) return ToolOutput{"no diagnostics (clean build)", std::nullopt};
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

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
    t.execute = util::adapt<DiagnosticsArgs>(parse_diagnostics_args, run_diagnostics);
    return t;
}

} // namespace moha::tools
