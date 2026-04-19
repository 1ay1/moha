#include "moha/tool/registry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "moha/io/diff.hpp"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}

void write_file(const fs::path& p, const std::string& content) {
    // parent_path() is empty for a bare filename ("foo.py"); calling
    // create_directories("") throws filesystem_error on POSIX, which the
    // tool dispatcher then surfaces as a Failed write to the model.
    auto parent = p.parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(content.data(), (std::streamsize)content.size());
}

#ifdef _WIN32
// CreateProcess-based runner: redirects the child's stdin to NUL so it cannot
// steal keystrokes from the TUI or disturb the console mode. stdout+stderr
// merge into a pipe read by the caller. Saves + restores the stdin console
// mode as a belt-and-suspenders guard against a child resetting ENABLE_LINE_INPUT
// / ENABLE_ECHO_INPUT, which would cause subsequent typing to echo at the
// cursor (the bug where keystrokes appear at the footer instead of the composer).
std::string run_command_win32(const std::string& cmd, int max_chars,
                              int timeout_secs) {
    HANDLE h_stdin = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD saved_in_mode = 0;
    bool  have_saved_mode =
        h_stdin != INVALID_HANDLE_VALUE && ::GetConsoleMode(h_stdin, &saved_in_mode);

    struct Restore {
        HANDLE h; DWORD mode; bool active;
        ~Restore() { if (active) ::SetConsoleMode(h, mode); }
    } restore{h_stdin, saved_in_mode, have_saved_mode};

    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return "[CreatePipe failed]";
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = ::CreateFileA("NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        ::CloseHandle(rd); ::CloseHandle(wr);
        return "[CreateFile(NUL) failed]";
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = wr;
    si.hStdError  = wr;

    // cmd.exe /S /C "…" — /S preserves quoting inside the command string.
    std::string line = "cmd.exe /S /C \"" + cmd + "\"";
    std::vector<char> cmdline(line.begin(), line.end());
    cmdline.push_back('\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                               TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &si, &pi);
    ::CloseHandle(wr);
    ::CloseHandle(nul);
    if (!ok) {
        ::CloseHandle(rd);
        DWORD e = ::GetLastError();
        return "[CreateProcess failed: " + std::to_string(e) + "]";
    }

    std::ostringstream out;
    size_t total = 0;
    bool truncated = false;
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(rd, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        if (!truncated) {
            size_t room = (total < (size_t)max_chars)
                        ? (size_t)max_chars - total : 0;
            size_t write = n < room ? n : room;
            out.write(buf, (std::streamsize)write);
            total += write;
            if (write < (size_t)n) { truncated = true; out << "\n[output truncated]"; }
        }
    }
    ::CloseHandle(rd);

    DWORD wait = ::WaitForSingleObject(pi.hProcess,
                                       timeout_secs > 0
                                       ? (DWORD)timeout_secs * 1000u
                                       : INFINITE);
    DWORD exit_code = 0;
    std::string output = out.str();
    if (wait == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        output += "\n[timed out after " + std::to_string(timeout_secs) + "s]";
    } else {
        ::GetExitCodeProcess(pi.hProcess, &exit_code);
        if (exit_code != 0)
            output += "\n[exit code " + std::to_string((int)exit_code) + "]";
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return output;
}
#endif

std::string run_command(const std::string& cmd, int max_chars = 30000,
                        int timeout_secs = 120) {
#ifdef _WIN32
    return run_command_win32(cmd, max_chars, timeout_secs);
#else
    // Enforce a wall-clock timeout via GNU coreutils `timeout`. Without this,
    // a hung command (network wait, infinite loop, REPL with no stdin close)
    // blocks the worker thread forever and the UI hangs on the spinner.
    std::string wrapped = "timeout --kill-after=2s " + std::to_string(timeout_secs)
                        + "s sh -c " + [&]{
        std::string q = "'";
        for (char c : cmd) { if (c == '\'') q += "'\\''"; else q += c; }
        q += "'";
        return q;
    }() + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
    if (!pipe) return "[popen failed]";
    std::ostringstream out;
    std::array<char, 4096> buf{};
    size_t total = 0;
    while (fgets(buf.data(), (int)buf.size(), pipe)) {
        out << buf.data();
        total += std::strlen(buf.data());
        if (total > (size_t)max_chars) { out << "\n[output truncated]"; break; }
    }
    int rc = pclose(pipe);
    std::string output = out.str();
    // GNU `timeout` exits 124 on timeout, 137 on KILL after grace.
    if (rc == 124 * 256 || rc == 137 * 256)
        output += "\n[timed out after " + std::to_string(timeout_secs) + "s]";
    else if (rc != 0)
        output += "\n[exit code " + std::to_string(rc) + "]";
    return output;
#endif
}

// ---- Read ------------------------------------------------------------------
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
        std::string path = args.value("path", "");
        int offset = args.value("offset", 1);
        int limit  = args.value("limit", 2000);
        if (path.empty())
            return std::unexpected(ToolError{"path required"});
        std::error_code ec;
        if (!fs::exists(path, ec))
            return std::unexpected(ToolError{"file not found: " + path});
        auto content = read_file(path);
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
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Write -----------------------------------------------------------------
ToolDef tool_write() {
    ToolDef t;
    t.name = ToolName{std::string{"write"}};
    t.description = "Write (or overwrite) a file with the given contents. "
                    "Creates parent directories as needed.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","content"}},
        {"properties", {
            {"path",    {{"type","string"}}},
            {"content", {{"type","string"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string path = args.value("path", "");
        std::string content = args.value("content", "");
        if (path.empty())
            return std::unexpected(ToolError{"path required"});
        std::string original;
        std::error_code ec;
        if (fs::exists(path, ec)) original = read_file(path);
        auto change = diff::compute(path, original, content);
        write_file(path, content);
        std::ostringstream msg;
        msg << "wrote " << path << " (" << change.added << "+ "
            << change.removed << "-)";
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

// ---- Edit ------------------------------------------------------------------
ToolDef tool_edit() {
    ToolDef t;
    t.name = ToolName{std::string{"edit"}};
    t.description = "Edit a file by replacing an exact old_string with new_string. "
                    "The old_string must be uniquely present unless replace_all is set.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","old_string","new_string"}},
        {"properties", {
            {"path",       {{"type","string"}}},
            {"old_string", {{"type","string"}}},
            {"new_string", {{"type","string"}}},
            {"replace_all",{{"type","boolean"}, {"default", false}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string path = args.value("path", "");
        std::string old_s = args.value("old_string", "");
        std::string new_s = args.value("new_string", "");
        bool all = args.value("replace_all", false);
        if (path.empty())
            return std::unexpected(ToolError{"path required"});
        std::error_code ec;
        if (!fs::exists(path, ec))
            return std::unexpected(ToolError{"file not found: " + path});
        std::string original = read_file(path);
        std::string updated = original;
        if (old_s.empty())
            return std::unexpected(ToolError{"old_string empty"});
        if (all) {
            size_t pos = 0; int n = 0;
            while ((pos = updated.find(old_s, pos)) != std::string::npos) {
                updated.replace(pos, old_s.size(), new_s);
                pos += new_s.size();
                n++;
            }
            if (n == 0) return std::unexpected(ToolError{"old_string not found"});
        } else {
            auto pos = updated.find(old_s);
            if (pos == std::string::npos)
                return std::unexpected(ToolError{"old_string not found"});
            if (updated.find(old_s, pos + 1) != std::string::npos)
                return std::unexpected(ToolError{"old_string is not unique; pass replace_all=true"});
            updated.replace(pos, old_s.size(), new_s);
        }
        auto change = diff::compute(path, original, updated);
        write_file(path, updated);
        std::ostringstream msg;
        msg << "edited " << path << " (" << change.added << "+ "
            << change.removed << "-)";
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

// ---- Bash ------------------------------------------------------------------
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
    t.execute = [](const json& args) -> ExecResult {
        std::string cmd = args.value("command", "");
        if (cmd.empty())
            return std::unexpected(ToolError{"command required"});
        int timeout = args.value("timeout", 120);
        if (timeout <= 0 || timeout > 600) timeout = 120;
        auto output = run_command(cmd, 30000, timeout);
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

bool should_skip_dir(const std::string& name) {
    static const std::vector<std::string> skip = {
        ".git", "node_modules", "build", "target", "__pycache__",
        ".cache", "vendor", "dist", "out", ".next", ".venv",
        "cmake-build-debug", "cmake-build-release", ".idea", ".vscode",
        "_deps", "third_party", "thirdparty", "3rdparty", "external",
    };
    for (const auto& s : skip) if (name == s) return true;
    return false;
}

bool is_binary_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return true;
    char buf[512];
    ifs.read(buf, sizeof(buf));
    auto n = ifs.gcount();
    for (int i = 0; i < n; ++i)
        if (buf[i] == '\0') return true;
    return false;
}

// ---- Grep ------------------------------------------------------------------
ToolDef tool_grep() {
    ToolDef t;
    t.name = ToolName{std::string{"grep"}};
    t.description = "Search for a regex pattern across files. Returns matching lines "
                    "with file paths and line numbers. Truncated at 500 matches.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"pattern", {{"type","string"}, {"description","Regex pattern to search for"}}},
            {"path",    {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
            {"glob",    {{"type","string"}, {"description","File extension filter (e.g. *.cpp)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string pat = args.value("pattern", "");
        std::string root = args.value("path", ".");
        std::string file_glob = args.value("glob", "");
        if (pat.empty()) return std::unexpected(ToolError{"pattern required"});
        std::regex re;
        try { re = std::regex(pat); } catch (...) {
            return std::unexpected(ToolError{"bad regex"});
        }
        std::ostringstream out;
        int matches = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            auto fn = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (fn.starts_with(".")) continue;
            auto p = it->path();
            if (!file_glob.empty()) {
                auto dot = file_glob.find_last_of('.');
                if (dot != std::string::npos) {
                    if (p.extension() != file_glob.substr(dot)) continue;
                }
            }
            if (is_binary_file(p)) continue;
            std::ifstream ifs(p);
            if (!ifs) continue;
            std::string line;
            int n = 1;
            while (std::getline(ifs, line)) {
                if (std::regex_search(line, re)) {
                    out << p.string() << ":" << n << ":" << line << "\n";
                    if (++matches > 500) { out << "[>500 matches, truncated]\n"; goto done; }
                }
                n++;
            }
        }
        done:
        if (matches == 0) return ToolOutput{"no matches", std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Glob ------------------------------------------------------------------
ToolDef tool_glob() {
    ToolDef t;
    t.name = ToolName{std::string{"glob"}};
    t.description = "Find files matching a pattern (substring or glob). "
                    "Returns matching file paths.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"pattern", {{"type","string"}, {"description","File name pattern to match"}}},
            {"path",    {{"type","string"}, {"description","Root directory (default: cwd)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string pat = args.value("pattern", "");
        std::string root = args.value("path", ".");
        std::ostringstream out;
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            auto fn = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            auto s = it->path().string();
            if (s.find(pat) != std::string::npos) {
                out << s << "\n";
                if (++n > 500) { out << "[>500, truncated]\n"; break; }
            }
        }
        if (n == 0) return ToolOutput{"no matches", std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- ListDir ---------------------------------------------------------------
ToolDef tool_list_dir() {
    ToolDef t;
    t.name = ToolName{std::string{"list_dir"}};
    t.description = "List the contents of a directory. Shows file type, size, and name. "
                    "Use this to explore project structure before reading files.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"path",      {{"type","string"}, {"description","Directory to list (default: cwd)"}}},
            {"recursive", {{"type","boolean"}, {"description","List recursively (default: false)"}}},
            {"max_depth", {{"type","integer"}, {"description","Max depth for recursive listing (default: 3)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string root = args.value("path", ".");
        bool recursive = args.value("recursive", false);
        int max_depth = args.value("max_depth", 3);
        std::error_code ec;
        if (!fs::exists(root, ec))
            return std::unexpected(ToolError{"directory not found: " + root});
        if (!fs::is_directory(root, ec))
            return std::unexpected(ToolError{"not a directory: " + root});

        std::ostringstream out;
        int count = 0;

        auto format_size = [](uintmax_t bytes) -> std::string {
            char buf[32];
            if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
            if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0); return buf; }
            if (bytes < 1024*1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024.0)); return buf; }
            std::snprintf(buf, sizeof(buf), "%.1fG", bytes/(1024.0*1024.0*1024.0)); return buf;
        };

        auto list_entry = [&](const fs::directory_entry& entry, int depth) {
            if (count > 1000) return;
            std::string indent(depth * 2, ' ');
            auto fn = entry.path().filename().string();
            if ((fn.starts_with(".") || should_skip_dir(fn)) && depth > 0) return;
            if (entry.is_directory(ec)) {
                out << indent << fn << "/\n";
            } else if (entry.is_regular_file(ec)) {
                auto sz = entry.file_size(ec);
                out << indent << fn << "  " << format_size(ec ? 0 : sz) << "\n";
            } else if (entry.is_symlink(ec)) {
                out << indent << fn << " -> " << fs::read_symlink(entry.path(), ec).string() << "\n";
            }
            count++;
        };

        if (recursive) {
            for (auto it = fs::recursive_directory_iterator(root,
                        fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it.depth() > max_depth) { it.disable_recursion_pending(); continue; }
                list_entry(*it, it.depth());
                if (count > 1000) { out << "[>1000 entries, truncated]\n"; break; }
            }
        } else {
            std::vector<fs::directory_entry> entries;
            for (auto& e : fs::directory_iterator(root, ec))
                entries.push_back(e);
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                bool da = a.is_directory(), db = b.is_directory();
                if (da != db) return da > db;
                return a.path().filename() < b.path().filename();
            });
            for (auto& e : entries) list_entry(e, 0);
        }
        if (count == 0) return ToolOutput{"empty directory", std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Todo ------------------------------------------------------------------
ToolDef tool_todo() {
    ToolDef t;
    t.name = ToolName{std::string{"todo"}};
    t.description = "Maintain the session todo list. Overwrites with the provided list.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"todos"}},
        {"properties", {
            {"todos", {{"type","array"},
                {"items", {{"type","object"},
                    {"properties", {
                        {"content", {{"type","string"}}},
                        {"status",  {{"type","string"},
                            {"enum", {"pending","in_progress","completed"}}}},
                    }},
                    {"required", {"content","status"}},
                }},
            }},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        auto todos = args.value("todos", json::array());
        std::ostringstream out;
        for (const auto& td : todos) {
            std::string st = td.value("status", "pending");
            char mark = st == "completed" ? 'x' : st == "in_progress" ? '-' : ' ';
            out << "[" << mark << "] " << td.value("content", "") << "\n";
        }
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- WebFetch --------------------------------------------------------------
namespace {
size_t fetch_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    if (buf->size() + total > 200000) {
        buf->append(ptr, 200000 - buf->size());
        return 0;
    }
    buf->append(ptr, total);
    return total;
}
} // namespace

ToolDef tool_web_fetch() {
    ToolDef t;
    t.name = ToolName{std::string{"web_fetch"}};
    t.description = "Fetch the contents of a URL. Supports HTTP/HTTPS. Returns the response "
                    "body, status code, and content type. Use for documentation, APIs, etc.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"url"}},
        {"properties", {
            {"url",     {{"type","string"}, {"description","The URL to fetch"}}},
            {"method",  {{"type","string"}, {"description","HTTP method (default: GET)"}}},
            {"headers", {{"type","object"}, {"description","Additional headers as key-value pairs"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string url = args.value("url", "");
        std::string method = args.value("method", "GET");
        if (url.empty())
            return std::unexpected(ToolError{"url required"});
        if (!url.starts_with("http://") && !url.starts_with("https://"))
            return std::unexpected(ToolError{"url must start with http:// or https://"});

        CURL* curl = curl_easy_init();
        if (!curl) return std::unexpected(ToolError{"failed to initialize HTTP client"});

        std::string body;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "User-Agent: moha/0.1");

        if (args.contains("headers") && args["headers"].is_object()) {
            for (auto& [k, v] : args["headers"].items()) {
                std::string hdr = k + ": " + v.get<std::string>();
                hdrs = curl_slist_append(hdrs, hdr.c_str());
            }
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        if (method == "HEAD")
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        else if (method == "POST")
            curl_easy_setopt(curl, CURLOPT_POST, 1L);

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        char* ct = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
        std::string content_type = ct ? ct : "";

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK)
            return std::unexpected(ToolError{std::string{"fetch failed: "} + curl_easy_strerror(rc)});

        std::ostringstream out;
        out << "HTTP " << http_code;
        if (!content_type.empty()) out << " (" << content_type << ")";
        out << "\n\n" << body;
        if (body.size() >= 200000) out << "\n[body truncated at 200KB]";
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- FindDefinition --------------------------------------------------------
ToolDef tool_find_definition() {
    ToolDef t;
    t.name = ToolName{std::string{"find_definition"}};
    t.description = "Find the definition of a symbol (function, class, struct, enum, type) "
                    "across the codebase. Searches for common definition patterns in C/C++, "
                    "Python, JavaScript/TypeScript, Go, and Rust.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"symbol"}},
        {"properties", {
            {"symbol", {{"type","string"}, {"description","The symbol name to find"}}},
            {"path",   {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string sym = args.value("symbol", "");
        std::string root = args.value("path", ".");
        if (sym.empty())
            return std::unexpected(ToolError{"symbol required"});

        // Patterns that typically indicate a definition
        std::vector<std::regex> patterns;
        try {
            // C/C++
            patterns.emplace_back("\\b(class|struct|enum|union|namespace|typedef|using)\\s+" + sym + "\\b");
            patterns.emplace_back("\\b\\w[\\w:*&<> ]*\\s+" + sym + "\\s*\\(");
            patterns.emplace_back("#define\\s+" + sym + "\\b");
            // Python
            patterns.emplace_back("\\b(def|class)\\s+" + sym + "\\s*[\\(:]");
            // JS/TS
            patterns.emplace_back("\\b(function|const|let|var|type|interface|export)\\s+" + sym + "\\b");
            // Go
            patterns.emplace_back("\\b(func|type)\\s+" + sym + "\\b");
            // Rust
            patterns.emplace_back("\\b(fn|struct|enum|trait|type|mod|const|static)\\s+" + sym + "\\b");
        } catch (...) {
            return std::unexpected(ToolError{"invalid symbol name for regex"});
        }

        static const std::vector<std::string> skip_dirs = {
            ".git", "node_modules", "build", "target", "__pycache__",
            ".cache", "vendor", "dist", "out", ".next"
        };

        std::ostringstream out;
        int matches = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root,
                    fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            auto fn = it->path().filename().string();
            if (fn.starts_with(".")) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec)) {
                for (const auto& skip : skip_dirs) {
                    if (fn == skip) { it.disable_recursion_pending(); break; }
                }
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            auto ext = it->path().extension().string();
            static const std::vector<std::string> code_exts = {
                ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx",
                ".py", ".js", ".ts", ".jsx", ".tsx", ".go", ".rs",
                ".java", ".kt", ".rb", ".swift", ".zig", ".lua",
            };
            bool is_code = false;
            for (const auto& e : code_exts) { if (ext == e) { is_code = true; break; } }
            if (!is_code) continue;

            std::ifstream ifs(it->path());
            if (!ifs) continue;
            std::string line;
            int n = 1;
            while (std::getline(ifs, line)) {
                for (const auto& re : patterns) {
                    if (std::regex_search(line, re)) {
                        out << it->path().string() << ":" << n << ": " << line << "\n";
                        if (++matches > 50) goto done;
                        break;
                    }
                }
                n++;
            }
        }
        done:
        if (matches == 0) return ToolOutput{"no definitions found for '" + sym + "'", std::nullopt};
        if (matches > 50) out << "[>50 definitions, truncated]\n";
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Diagnostics -----------------------------------------------------------
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
        if (cmd.empty()) {
            if (fs::exists("build/build.ninja", ec) || fs::exists("build/Makefile", ec))
                cmd = "cmake --build build 2>&1 | head -100";
            else if (fs::exists("Cargo.toml", ec))
                cmd = "cargo check 2>&1 | head -100";
            else if (fs::exists("go.mod", ec))
                cmd = "go build ./... 2>&1 | head -100";
            else if (fs::exists("package.json", ec))
                cmd = "npx tsc --noEmit 2>&1 | head -50";
            else if (fs::exists("Makefile", ec))
                cmd = "make -n 2>&1 | head -100";
            else
                return std::unexpected(ToolError{"no build system detected; pass a command"});
        }
        auto output = run_command(cmd);
        if (output.empty()) return ToolOutput{"no diagnostics (clean build)", std::nullopt};
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

// ---- GitStatus -------------------------------------------------------------
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
        auto output = run_command("git -C " + root + " status --porcelain=v2 --branch");
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

// ---- GitDiff ---------------------------------------------------------------
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
        std::string cmd = "git diff --stat -p";
        if (staged) cmd += " --cached";
        if (!ref.empty()) cmd += " " + ref;
        if (!path.empty()) cmd += " -- " + path;
        auto output = run_command(cmd, 50000);
        if (output.empty()) return ToolOutput{"no changes", std::nullopt};
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

// ---- GitLog ----------------------------------------------------------------
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
        std::string cmd = "git log";
        if (oneline)
            cmd += " --oneline";
        else
            cmd += " --format='%h %ad %an%n  %s' --date=short";
        cmd += " -" + std::to_string(count);
        cmd += " " + ref;
        if (!path.empty()) cmd += " -- " + path;
        auto output = run_command(cmd);
        if (output.empty()) return ToolOutput{"no commits", std::nullopt};
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

// ---- GitCommit -------------------------------------------------------------
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
            auto out = run_command("git add -A");
            if (out.find("[exit code") != std::string::npos)
                return std::unexpected(ToolError{"git add failed: " + out});
        } else if (args.contains("files") && args["files"].is_array()) {
            for (const auto& f : args["files"]) {
                auto out = run_command("git add " + f.get<std::string>());
                if (out.find("[exit code") != std::string::npos)
                    return std::unexpected(ToolError{"git add failed: " + out});
            }
        }

        // Escape single quotes in message
        std::string escaped;
        for (char c : message) {
            if (c == '\'') escaped += "'\\''";
            else escaped += c;
        }
        auto output = run_command("git commit -m '" + escaped + "'");
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

// ---- WebSearch (via DuckDuckGo HTML Lite) -----------------------------------
ToolDef tool_web_search() {
    ToolDef t;
    t.name = ToolName{std::string{"web_search"}};
    t.description = "Search the web using DuckDuckGo. Returns search result snippets. "
                    "Use for looking up documentation, error messages, API references.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"query", {{"type","string"}, {"description","Search query"}}},
            {"count", {{"type","integer"}, {"description","Max results (default: 10)"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string query = args.value("query", "");
        if (query.empty())
            return std::unexpected(ToolError{"query required"});

        CURL* curl = curl_easy_init();
        if (!curl) return std::unexpected(ToolError{"failed to initialize HTTP client"});

        char* encoded = curl_easy_escape(curl, query.c_str(), (int)query.size());
        std::string url = std::string{"https://html.duckduckgo.com/html/?q="} + encoded;
        curl_free(encoded);

        std::string body;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "User-Agent: moha/0.1 (terminal agent)");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK)
            return std::unexpected(ToolError{std::string{"search failed: "} + curl_easy_strerror(rc)});

        // Parse results from HTML - extract titles and snippets
        int max_results = args.value("count", 10);
        std::ostringstream out;
        int found = 0;

        // Extract result blocks: class="result__title" ... class="result__snippet"
        size_t pos = 0;
        while (pos < body.size() && found < max_results) {
            auto title_start = body.find("class=\"result__a\"", pos);
            if (title_start == std::string::npos) break;

            // Extract href
            auto href_start = body.rfind("href=\"", title_start);
            std::string link;
            if (href_start != std::string::npos && href_start > pos) {
                href_start += 6;
                auto href_end = body.find('"', href_start);
                if (href_end != std::string::npos)
                    link = body.substr(href_start, href_end - href_start);
            }

            // Extract title text
            auto tag_end = body.find('>', title_start);
            if (tag_end == std::string::npos) break;
            auto text_end = body.find('<', tag_end + 1);
            std::string title;
            if (text_end != std::string::npos)
                title = body.substr(tag_end + 1, text_end - tag_end - 1);

            // Extract snippet
            auto snippet_start = body.find("class=\"result__snippet\"", text_end);
            std::string snippet;
            if (snippet_start != std::string::npos) {
                auto stag = body.find('>', snippet_start);
                if (stag != std::string::npos) {
                    auto send = body.find("</", stag);
                    if (send != std::string::npos) {
                        snippet = body.substr(stag + 1, send - stag - 1);
                        // Strip HTML tags from snippet
                        std::string clean;
                        bool in_tag = false;
                        for (char c : snippet) {
                            if (c == '<') in_tag = true;
                            else if (c == '>') in_tag = false;
                            else if (!in_tag) clean += c;
                        }
                        snippet = clean;
                    }
                }
                pos = snippet_start + 10;
            } else {
                pos = text_end ? text_end + 1 : body.size();
            }

            // Strip HTML entities
            auto strip_entities = [](std::string& s) {
                size_t p = 0;
                while ((p = s.find("&amp;", p)) != std::string::npos) s.replace(p, 5, "&");
                p = 0;
                while ((p = s.find("&lt;", p)) != std::string::npos) s.replace(p, 4, "<");
                p = 0;
                while ((p = s.find("&gt;", p)) != std::string::npos) s.replace(p, 4, ">");
                p = 0;
                while ((p = s.find("&quot;", p)) != std::string::npos) s.replace(p, 6, "\"");
                p = 0;
                while ((p = s.find("&#x27;", p)) != std::string::npos) s.replace(p, 6, "'");
                p = 0;
                while ((p = s.find("&nbsp;", p)) != std::string::npos) s.replace(p, 6, " ");
            };

            strip_entities(title);
            strip_entities(snippet);

            if (!title.empty()) {
                out << found + 1 << ". " << title << "\n";
                if (!link.empty()) out << "   " << link << "\n";
                if (!snippet.empty()) out << "   " << snippet << "\n";
                out << "\n";
                found++;
            }
        }

        if (found == 0) return ToolOutput{"no results found for: " + query, std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ============================================================================

std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> r;
    r.push_back(tool_read());
    r.push_back(tool_write());
    r.push_back(tool_edit());
    r.push_back(tool_bash());
    r.push_back(tool_grep());
    r.push_back(tool_glob());
    r.push_back(tool_list_dir());
    r.push_back(tool_todo());
    r.push_back(tool_web_fetch());
    r.push_back(tool_web_search());
    r.push_back(tool_find_definition());
    r.push_back(tool_diagnostics());
    r.push_back(tool_git_status());
    r.push_back(tool_git_diff());
    r.push_back(tool_git_log());
    r.push_back(tool_git_commit());
    return r;
}

} // namespace

const std::vector<ToolDef>& registry() {
    static const std::vector<ToolDef> r = build_registry();
    return r;
}

const ToolDef* find(std::string_view name) {
    for (const auto& t : registry()) if (t.name == name) return &t;
    return nullptr;
}

} // namespace moha::tools
