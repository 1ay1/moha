#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

enum class TodoStatus { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    TodoStatus status;
};

struct TodoArgs {
    std::vector<TodoItem> todos;
    std::string display_description;
};

TodoStatus parse_status(std::string_view s) {
    if (s == "completed")    return TodoStatus::Completed;
    if (s == "in_progress")  return TodoStatus::InProgress;
    return TodoStatus::Pending;
}

std::expected<TodoArgs, ToolError> parse_todo_args(const json& j) {
    util::ArgReader ar(j);
    TodoArgs out;
    out.display_description = ar.str("display_description", "");
    const json* raw = ar.raw("todos");
    if (!raw || !raw->is_array()) return out;  // tolerate missing/invalid
    out.todos.reserve(raw->size());
    for (const auto& td : *raw) {
        if (!td.is_object()) continue;
        util::ArgReader inner(td);
        out.todos.push_back(TodoItem{
            inner.str("content", ""),
            parse_status(inner.str("status", "pending")),
        });
    }
    return out;
}

ExecResult run_todo(const TodoArgs& a) {
    std::ostringstream out;
    if (!a.display_description.empty())
        out << a.display_description << "\n\n";
    for (const auto& td : a.todos) {
        char mark = td.status == TodoStatus::Completed   ? 'x'
                  : td.status == TodoStatus::InProgress  ? '-'
                                                         : ' ';
        out << "[" << mark << "] " << td.content << "\n";
    }
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_todo() {
    ToolDef t;
    t.name = ToolName{std::string{"todo"}};
    t.description = "Maintain the session todo list. Overwrites with the provided list.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"todos"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
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
    // Without fine-grained tool streaming, Anthropic's edge buffers the
    // whole `todos: [...]` array before delivering. A 10-item plan with
    // multi-line `content` strings is multi-KB; the card looks frozen
    // ("stuck") for 10–30 s while the wire trickles. Same fix as write/edit
    // — see write.cpp for the full story.
    t.eager_input_streaming = true;
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<TodoArgs>(parse_todo_args, run_todo);
    return t;
}

} // namespace moha::tools
