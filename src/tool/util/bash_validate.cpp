#include "moha/tool/util/bash_validate.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moha::tools::util {

namespace {

std::string_view first_token(std::string_view cmd) noexcept {
    size_t i = 0;
    while (i < cmd.size() && (cmd[i] == ' ' || cmd[i] == '\t')) i++;
    size_t start = i;
    while (i < cmd.size() && cmd[i] != ' ' && cmd[i] != '\t'
           && cmd[i] != '|' && cmd[i] != '&' && cmd[i] != ';') i++;
    auto tok = cmd.substr(start, i - start);
    size_t slash = tok.find_last_of("/\\");
    if (slash != std::string_view::npos) tok.remove_prefix(slash + 1);
    if (tok.size() > 4
        && (tok.ends_with(".exe") || tok.ends_with(".EXE")
            || tok.ends_with(".cmd") || tok.ends_with(".bat")))
        tok.remove_suffix(4);
    return tok;
}

} // namespace

std::string validate_bash_command(const std::string& cmd) {
    auto tok = first_token(cmd);
    // REPLs & editors that block waiting on stdin or take over the terminal.
    // Some (python/node) are only interactive when invoked with no args —
    // accept `python foo.py` but reject `python` alone.
    static const std::vector<std::string_view> always_interactive = {
        "vim", "vi", "nvim", "nano", "emacs", "pico", "ed", "joe", "mcedit",
        "less", "more", "man", "top", "htop", "btop", "tmux", "screen",
        "mysql", "psql", "sqlite3", "redis-cli", "mongo",
        "ghci", "ocaml", "irb", "pry", "lua", "tclsh", "gdb", "lldb",
        "fzf", "dialog", "whiptail",
    };
    for (auto name : always_interactive) {
        if (tok == name)
            return "refusing to run interactive command '" + std::string{name}
                 + "' — it would block waiting for stdin. Use a non-interactive "
                   "alternative (e.g. for editors: use the write/edit tools).";
    }
    static const std::vector<std::string_view> interactive_if_bare = {
        "python", "python3", "node", "deno", "ruby", "php", "iex", "bash",
        "sh", "zsh", "fish", "pwsh", "powershell", "cmd",
    };
    for (auto name : interactive_if_bare) {
        if (tok != name) continue;
        auto rest = cmd.substr(cmd.find(tok) + tok.size());
        bool has_more = false;
        for (char c : rest) if (c != ' ' && c != '\t' && c != '\n') { has_more = true; break; }
        if (!has_more)
            return "refusing to start interactive " + std::string{name}
                 + " REPL — it would block waiting for stdin. Provide a script "
                   "path or use `-c \"…\"` to run a snippet.";
    }
    auto contains_word = [&](std::string_view needle) {
        size_t p = 0;
        while ((p = cmd.find(needle, p)) != std::string::npos) {
            bool left_ok  = p == 0 || cmd[p - 1] == ' ' || cmd[p - 1] == '\t';
            bool right_ok = p + needle.size() == cmd.size()
                         || cmd[p + needle.size()] == ' '
                         || cmd[p + needle.size()] == '\t';
            if (left_ok && right_ok) return true;
            p += needle.size();
        }
        return false;
    };
    if (tok == "git") {
        if (contains_word("rebase") && contains_word("-i"))
            return "refusing to run interactive rebase (`git rebase -i`) — the editor "
                   "would block the agent. Use non-interactive rebase options instead.";
        if (contains_word("add") && (contains_word("-i") || contains_word("-p")
                                     || contains_word("--interactive")
                                     || contains_word("--patch")))
            return "refusing to run interactive git add — use explicit file paths.";
        if (contains_word("commit")
            && !contains_word("-m") && !contains_word("-F")
            && !contains_word("--message") && !contains_word("--file")
            && !contains_word("--amend") && !contains_word("-C")
            && !contains_word("--no-edit"))
            return "refusing `git commit` without -m/-F — it would open an editor. "
                   "Pass -m \"<message>\" to commit non-interactively, or use the "
                   "git_commit tool.";
    }
    static const std::vector<std::pair<std::string_view, std::string_view>> danger = {
        {"rm -rf /",               "refusing wide rm that could wipe the filesystem root"},
        {"rm -rf /*",              "refusing wide rm that could wipe the filesystem root"},
        {"rm -rf ~",               "refusing to recursively delete the home directory"},
        {":(){ :|:& };:",          "fork-bomb pattern refused"},
        {"mkfs",                   "refusing mkfs — would reformat a filesystem"},
        {"dd if=",                 "refusing raw `dd` write — can corrupt disks if misdirected"},
        {"shutdown",               "refusing shutdown"},
        {"reboot",                 "refusing reboot"},
        {"git push --force",       "refusing `git push --force`; use --force-with-lease and ask the user first"},
        {"git push -f",            "refusing `git push -f`; use --force-with-lease and ask the user first"},
    };
    for (const auto& [needle, msg] : danger) {
        if (cmd.find(needle) != std::string::npos) return std::string{msg};
    }
    auto piped_to_shell = [&](std::string_view prog) {
        size_t p = cmd.find(prog);
        while (p != std::string::npos) {
            size_t pipe = cmd.find('|', p);
            if (pipe == std::string::npos) break;
            auto rest = cmd.substr(pipe + 1);
            size_t i = 0;
            while (i < rest.size() && (rest[i] == ' ' || rest[i] == '|')) i++;
            auto next = first_token(rest.substr(i));
            if (next == "sh" || next == "bash" || next == "zsh"
                || next == "dash" || next == "ksh")
                return true;
            p = cmd.find(prog, pipe);
        }
        return false;
    };
    if (piped_to_shell("curl") || piped_to_shell("wget"))
        return "refusing `curl|sh` / `wget|sh` — download the script, inspect it, then run explicitly.";
    return {};
}

} // namespace moha::tools::util
