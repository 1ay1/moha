#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/bash_validate.hpp"
#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <chrono>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

struct BashArgs {
    std::string command;
    int timeout;
};

std::expected<BashArgs, ToolError> parse_bash_args(const json& j) {
    util::ArgReader ar(j);
    auto cmd_opt = ar.require_str("command");
    if (!cmd_opt)
        return std::unexpected(ToolError::invalid_args("command required"));
    std::string cmd = *std::move(cmd_opt);
    if (auto why = util::validate_bash_command(cmd); !why.empty())
        return std::unexpected(ToolError::invalid_args(std::move(why)));
    int timeout = ar.integer("timeout", 120);
    if (timeout <= 0 || timeout > 600) timeout = 120;
    return BashArgs{std::move(cmd), timeout};
}

ExecResult run_bash(const BashArgs& a) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = util::run_command_s(a.command, 30000, a.timeout);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Zed-style per-state output: success+empty is affirmative,
    // failure names its exit code, timeout surfaces partial output.
    if (!r.started)
        return std::unexpected(ToolError::spawn(
            "failed to spawn command: " + r.start_error));

    auto fence = [](const std::string& body) {
        return std::string{"```\n"} + body + (body.empty() || body.back() == '\n'
                                              ? "" : "\n") + "```";
    };

    std::ostringstream out;
    if (r.timed_out) {
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. No output was captured.";
        } else {
            out << "Command \"" << a.command << "\" timed out after "
                << a.timeout << "s. Output captured before timeout:\n\n"
                << fence(r.output);
        }
    } else if (r.exit_code != 0) {
        if (r.output.empty()) {
            out << "Command \"" << a.command << "\" failed with exit code "
                << r.exit_code << ".";
        } else {
            out << "Command \"" << a.command << "\" failed with exit code "
                << r.exit_code << ".\n\n" << fence(r.output);
        }
    } else if (r.output.empty()) {
        out << "Command executed successfully.";
    } else {
        out << fence(r.output);
    }
    if (r.truncated)
        out << "\n\n[output truncated at 30000 bytes]";
    // Elapsed is useful for planning follow-ups; omit for anything
    // under 500 ms to keep output tidy.
    if (elapsed_ms >= 500)
        out << "\n\n[elapsed: "
            << (elapsed_ms < 10000
                ? (std::to_string(elapsed_ms) + " ms")
                : (std::to_string(elapsed_ms / 1000) + "."
                   + std::to_string((elapsed_ms % 1000) / 100) + " s"))
            << "]";

    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_bash() {
    ToolDef t;
    t.name = ToolName{std::string{"bash"}};
    t.description =
#ifdef _WIN32
        "Run a shell command via Windows cmd.exe and return its output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "This runs under cmd.exe on Windows — use native equivalents like "
        "`dir`, `where`, `systeminfo`, `type`, `findstr`, or `powershell -c`. "
        "Do NOT use POSIX-only commands (`uname`, `cat /etc/os-release`, "
        "`sw_vers`, `ls`, `grep`, `sed`, `awk`, heredocs) — they will fail. "
        "Do NOT use for file IO — use the write/edit/read tools instead."
#else
        "Run a shell command and return its output. "
        "Output is truncated at 30k chars. Use for builds, tests, git, etc. "
        "Do NOT use for file IO — use the write/edit/read tools instead "
        "(no cat/echo/sed/heredoc to create or modify files)."
#endif
    ;
    t.input_schema = json{
        {"type","object"},
        {"required", {"command"}},
        {"properties", {
            {"command", {{"type","string"}, {"description","The shell command to execute"}}},
            {"timeout", {{"type","integer"}, {"description","Timeout in seconds (default 120)"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<BashArgs>(parse_bash_args, run_bash);
    return t;
}

} // namespace moha::tools
