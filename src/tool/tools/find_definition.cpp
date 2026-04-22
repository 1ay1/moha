#include "moha/tool/tools.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

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

        // Regex-escape the symbol. Operator names (`operator*`, `operator<<`)
        // and templated forms otherwise explode the regex parser.
        std::string esc;
        esc.reserve(sym.size() * 2);
        for (char c : sym) {
            switch (c) {
                case '.': case '*': case '+': case '?': case '(': case ')':
                case '[': case ']': case '{': case '}': case '|': case '^':
                case '$': case '\\':
                    esc.push_back('\\'); [[fallthrough]];
                default:
                    esc.push_back(c);
            }
        }

        std::vector<std::regex> patterns;
        try {
            // C/C++
            patterns.emplace_back("\\b(class|struct|enum|union|namespace|typedef|using)\\s+" + esc + "\\b");
            patterns.emplace_back("\\b\\w[\\w:*&<> ]*\\s+" + esc + "\\s*\\(");
            patterns.emplace_back("#define\\s+" + esc + "\\b");
            // Python
            patterns.emplace_back("\\b(def|class)\\s+" + esc + "\\s*[\\(:]");
            // JS/TS
            patterns.emplace_back("\\b(function|const|let|var|type|interface|export)\\s+" + esc + "\\b");
            // Go
            patterns.emplace_back("\\b(func|type)\\s+" + esc + "\\b");
            // Rust
            patterns.emplace_back("\\b(fn|struct|enum|trait|type|mod|const|static)\\s+" + esc + "\\b");
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

} // namespace moha::tools
