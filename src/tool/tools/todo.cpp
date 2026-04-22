#include "moha/tool/tools.hpp"

#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

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

} // namespace moha::tools
