#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

// ── git_status ───────────────────────────────────────────────────────────

namespace {

struct GitStatusArgs {
    std::string root;
    std::string display_description;
};

std::expected<GitStatusArgs, ToolError> parse_git_status_args(const json& j) {
    util::ArgReader ar(j);
    return GitStatusArgs{
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_status(const GitStatusArgs& a) {
    auto output = util::run_argv({"git", "-C", a.root, "status",
                                  "--porcelain=v2", "--branch"});
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_status() {
    ToolDef t;
    t.name = ToolName{std::string{"git_status"}};
    t.description = "Show the current git status: branch, staged/unstaged changes, "
                    "untracked files, ahead/behind counts.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path", {{"type","string"}, {"description","Repository path (default: cwd)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<GitStatusArgs>(parse_git_status_args, run_git_status);
    return t;
}

// ── git_diff ─────────────────────────────────────────────────────────────

namespace {

struct GitDiffArgs {
    std::string path;
    bool staged;
    std::string ref;
    std::string display_description;
};

std::expected<GitDiffArgs, ToolError> parse_git_diff_args(const json& j) {
    util::ArgReader ar(j);
    return GitDiffArgs{
        ar.str("path", ""),
        ar.boolean("staged", false),
        ar.str("ref", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_diff(const GitDiffArgs& a) {
    std::vector<std::string> argv = {"git", "diff", "--stat", "-p"};
    if (a.staged) argv.push_back("--cached");
    if (!a.ref.empty()) argv.push_back(a.ref);
    if (!a.path.empty()) { argv.push_back("--"); argv.push_back(a.path); }
    auto output = util::run_argv(argv, 50000);
    if (output.empty()) return ToolOutput{"no changes", std::nullopt};
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_diff() {
    ToolDef t;
    t.name = ToolName{std::string{"git_diff"}};
    t.description = "Show git diff. By default shows unstaged changes. Use staged=true "
                    "for staged changes, or specify a ref/range.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",    {{"type","string"}, {"description","File or directory to diff"}}},
            {"staged",  {{"type","boolean"}, {"description","Show staged changes (default: false)"}}},
            {"ref",     {{"type","string"}, {"description","Git ref or range (e.g. HEAD~3, main..HEAD)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<GitDiffArgs>(parse_git_diff_args, run_git_diff);
    return t;
}

// ── git_log ──────────────────────────────────────────────────────────────

namespace {

struct GitLogArgs {
    int count;
    std::string path;
    std::string ref;
    bool oneline;
    std::string display_description;
};

std::expected<GitLogArgs, ToolError> parse_git_log_args(const json& j) {
    util::ArgReader ar(j);
    return GitLogArgs{
        ar.integer("count", 20),
        ar.str("path", ""),
        ar.str("ref", "HEAD"),
        ar.boolean("oneline", false),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_log(const GitLogArgs& a) {
    std::vector<std::string> argv = {"git", "log"};
    if (a.oneline) {
        argv.push_back("--oneline");
    } else {
        argv.push_back("--format=%h %ad %an%n  %s");
        argv.push_back("--date=short");
    }
    argv.push_back("-" + std::to_string(a.count));
    argv.push_back(a.ref);
    if (!a.path.empty()) { argv.push_back("--"); argv.push_back(a.path); }
    auto output = util::run_argv(argv);
    if (output.empty()) return ToolOutput{"no commits", std::nullopt};
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_log() {
    ToolDef t;
    t.name = ToolName{std::string{"git_log"}};
    t.description = "Show git commit history. Returns commit hash, author, date, and message.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"count",   {{"type","integer"}, {"description","Number of commits (default: 20)"}}},
            {"path",    {{"type","string"}, {"description","Filter by file path"}}},
            {"ref",     {{"type","string"}, {"description","Branch or ref (default: HEAD)"}}},
            {"oneline", {{"type","boolean"}, {"description","One-line format (default: false)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<GitLogArgs>(parse_git_log_args, run_git_log);
    return t;
}

// ── git_commit ───────────────────────────────────────────────────────────

namespace {

struct GitCommitArgs {
    std::string message;
    std::vector<std::string> files;
    bool stage_all;
    std::string display_description;
};

std::expected<GitCommitArgs, ToolError> parse_git_commit_args(const json& j) {
    util::ArgReader ar(j);
    auto msg_opt = ar.require_str("message");
    if (!msg_opt)
        return std::unexpected(ToolError::invalid_args("commit message required"));
    std::vector<std::string> files;
    if (const json* f = ar.raw("files"); f && f->is_array()) {
        files.reserve(f->size());
        for (const auto& el : *f) {
            if (el.is_string()) files.push_back(el.get<std::string>());
            else                files.push_back(el.dump());
        }
    }
    return GitCommitArgs{
        *std::move(msg_opt),
        std::move(files),
        ar.boolean("stage_all", false),
        ar.str("display_description", ""),
    };
}

ExecResult run_git_commit(const GitCommitArgs& a) {
    if (a.stage_all) {
        auto out = util::run_argv({"git", "add", "-A"});
        if (out.find("[exit code") != std::string::npos)
            return std::unexpected(ToolError::subprocess("git add failed: " + out));
    } else {
        for (const auto& f : a.files) {
            auto out = util::run_argv({"git", "add", f});
            if (out.find("[exit code") != std::string::npos)
                return std::unexpected(ToolError::subprocess("git add failed: " + out));
        }
    }
    auto output = util::run_argv({"git", "commit", "-m", a.message});
    if (!a.display_description.empty())
        output = a.display_description + "\n\n" + output;
    return ToolOutput{std::move(output), std::nullopt};
}

} // namespace

ToolDef tool_git_commit() {
    ToolDef t;
    t.name = ToolName{std::string{"git_commit"}};
    t.description = "Stage files and create a git commit. Specify files to stage, "
                    "or use stage_all to stage everything.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"message"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"message",   {{"type","string"}, {"description","Commit message"}}},
            {"files",     {{"type","array"}, {"items",{{"type","string"}}},
                           {"description","Files to stage before committing"}}},
            {"stage_all", {{"type","boolean"}, {"description","Stage all changes (default: false)"}}},
        }},
    };
    // Multi-paragraph commit messages can be multi-KB (especially the
    // PR-description style). Same eager-streaming rationale as todo/bash.
    t.eager_input_streaming = true;
    t.needs_permission = [](Profile){ return true; };
    t.execute = util::adapt<GitCommitArgs>(parse_git_commit_args, run_git_commit);
    return t;
}

} // namespace moha::tools
