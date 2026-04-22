#include "moha/tool/tools.hpp"
#include "moha/tool/util/subprocess.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

ToolDef tool_git_status() {
    ToolDef t;
    t.name = ToolName{std::string{"git_status"}};
    t.description = "Show the current git status: branch, staged/unstaged changes, "
                    "untracked files, ahead/behind counts.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"path", {{"type","string"}, {"description","Repository path (default: cwd)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string root = args.value("path", ".");
        auto output = util::run_argv({"git", "-C", root, "status",
                                      "--porcelain=v2", "--branch"});
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

ToolDef tool_git_diff() {
    ToolDef t;
    t.name = ToolName{std::string{"git_diff"}};
    t.description = "Show git diff. By default shows unstaged changes. Use staged=true "
                    "for staged changes, or specify a ref/range.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"path",    {{"type","string"}, {"description","File or directory to diff"}}},
            {"staged",  {{"type","boolean"}, {"description","Show staged changes (default: false)"}}},
            {"ref",     {{"type","string"}, {"description","Git ref or range (e.g. HEAD~3, main..HEAD)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string path = args.value("path", "");
        bool staged = args.value("staged", false);
        std::string ref = args.value("ref", "");
        std::vector<std::string> argv = {"git", "diff", "--stat", "-p"};
        if (staged) argv.push_back("--cached");
        if (!ref.empty()) argv.push_back(ref);
        if (!path.empty()) { argv.push_back("--"); argv.push_back(path); }
        auto output = util::run_argv(argv, 50000);
        if (output.empty()) return ToolOutput{"no changes", std::nullopt};
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

ToolDef tool_git_log() {
    ToolDef t;
    t.name = ToolName{std::string{"git_log"}};
    t.description = "Show git commit history. Returns commit hash, author, date, and message.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"count",   {{"type","integer"}, {"description","Number of commits (default: 20)"}}},
            {"path",    {{"type","string"}, {"description","Filter by file path"}}},
            {"ref",     {{"type","string"}, {"description","Branch or ref (default: HEAD)"}}},
            {"oneline", {{"type","boolean"}, {"description","One-line format (default: false)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        int count = args.value("count", 20);
        std::string path = args.value("path", "");
        std::string ref = args.value("ref", "HEAD");
        bool oneline = args.value("oneline", false);
        std::vector<std::string> argv = {"git", "log"};
        if (oneline) {
            argv.push_back("--oneline");
        } else {
            argv.push_back("--format=%h %ad %an%n  %s");
            argv.push_back("--date=short");
        }
        argv.push_back("-" + std::to_string(count));
        argv.push_back(ref);
        if (!path.empty()) { argv.push_back("--"); argv.push_back(path); }
        auto output = util::run_argv(argv);
        if (output.empty()) return ToolOutput{"no commits", std::nullopt};
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

ToolDef tool_git_commit() {
    ToolDef t;
    t.name = ToolName{std::string{"git_commit"}};
    t.description = "Stage files and create a git commit. Specify files to stage, "
                    "or use stage_all to stage everything.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"message"}},
        {"properties", {
            {"message",   {{"type","string"}, {"description","Commit message"}}},
            {"files",     {{"type","array"}, {"items",{{"type","string"}}},
                           {"description","Files to stage before committing"}}},
            {"stage_all", {{"type","boolean"}, {"description","Stage all changes (default: false)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return true; };
    t.execute = [](const json& args) -> ExecResult {
        std::string message = args.value("message", "");
        bool stage_all = args.value("stage_all", false);
        if (message.empty())
            return std::unexpected(ToolError{"commit message required"});

        if (stage_all) {
            auto out = util::run_argv({"git", "add", "-A"});
            if (out.find("[exit code") != std::string::npos)
                return std::unexpected(ToolError{"git add failed: " + out});
        } else if (args.contains("files") && args["files"].is_array()) {
            for (const auto& f : args["files"]) {
                auto out = util::run_argv({"git", "add", f.get<std::string>()});
                if (out.find("[exit code") != std::string::npos)
                    return std::unexpected(ToolError{"git add failed: " + out});
            }
        }

        auto output = util::run_argv({"git", "commit", "-m", message});
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

} // namespace moha::tools
