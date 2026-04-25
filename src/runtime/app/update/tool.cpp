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
                       std::expected<tools::ToolOutput, tools::ToolError>&& result) {
    for (auto& msg : m.d.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                // Idempotent: a tool already in a terminal state
                // (Done / Failed / Rejected) keeps that state. Realistic
                // ways a late ToolExecOutput can land here:
                //   (a) Wall-clock watchdog force-failed the tool at
                //       60 s; the worker thread eventually unwound
                //       seconds/minutes later. The original failure
                //       reason ("hung") is more useful to the user
                //       than the late output would be — and overwriting
                //       could re-arm a turn that's already advanced
                //       past this tool.
                //   (b) A duplicate dispatch on the same id (shouldn't
                //       happen but cheap to defend against).
                // Either way, dropping the late result keeps history
                // stable.
                if (tc.is_terminal()) return;
                auto now = std::chrono::steady_clock::now();
                auto started = tc.started_at();
                if (result) {
                    tc.status = ToolUse::Done{
                        started, now,
                        std::move(result->text),
                        std::move(result->change),
                    };
                } else {
                    // Render typed error as "[kind] detail" so the category
                    // is visible in tool-card / history without losing the
                    // human-readable detail. The model needs only the
                    // string back; the kind is preserved structurally for
                    // the future, when the view branches on category.
                    tc.status = ToolUse::Failed{started, now,
                        result.error().render()};
                }
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
