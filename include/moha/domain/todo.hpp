#pragma once
// moha todo domain — the value types for the `todo_write` tool's tracked
// task list.  The modal in the UI and the tool implementation share the
// same vocabulary.

#include <cstdint>
#include <string>

namespace moha {

enum class TodoStatus : std::uint8_t { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    TodoStatus  status = TodoStatus::Pending;
};

} // namespace moha
