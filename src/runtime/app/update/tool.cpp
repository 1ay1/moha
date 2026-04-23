// Tool-execution-result helpers: apply_tool_output translates a
// ToolExecOutput into a Done/Failed status on the matching ToolUse;
// mark_tool_rejected is the symmetric one-liner for permission denial.
// Both walk m.d.current.messages because a ToolCallId is only locally
// unique within a turn — we don't index them.

#include "moha/runtime/app/update/internal.hpp"

#include <chrono>
#include <utility>

namespace moha::app::detail {

void apply_tool_output(Model& m, const ToolCallId& id,
                       std::string&& output, bool error) {
    for (auto& msg : m.d.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                auto now = std::chrono::steady_clock::now();
                auto started = tc.started_at();
                if (error) tc.status = ToolUse::Failed{started, now, std::move(output)};
                else       tc.status = ToolUse::Done  {started, now, std::move(output)};
            }
}

void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason) {
    for (auto& msg : m.d.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                auto now = std::chrono::steady_clock::now();
                if (reason.empty()) {
                    tc.status = ToolUse::Rejected{now};
                } else {
                    tc.status = ToolUse::Failed{tc.started_at(), now, std::string{reason}};
                }
            }
}

} // namespace moha::app::detail
