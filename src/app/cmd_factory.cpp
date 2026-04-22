#include "moha/app/cmd_factory.hpp"

#include <utility>

#include "moha/io/anthropic.hpp"
#include "moha/app/deps.hpp"
#include "moha/tool/tool.hpp"
#include "moha/tool/registry.hpp"
#include "moha/view/helpers.hpp"

namespace moha::app::cmd {

using maya::Cmd;

Cmd<Msg> launch_stream(const Model& m) {
    io::ProviderRequest req;
    req.model         = m.model_id.value;
    req.system_prompt = anthropic::default_system_prompt();
    req.messages      = m.current.messages;

    if (m.profile != Profile::Minimal) {
        for (const auto& t : tools::registry()) {
            if (m.profile == Profile::Ask
                && (t.name == "write" || t.name == "edit" || t.name == "bash"))
                continue;
            req.tools.push_back({t.name.value, t.description, t.input_schema});
        }
    }
    req.auth_header = deps().auth_header;
    req.auth_style  = deps().auth_style;

    return Cmd<Msg>::task([req = std::move(req)](std::function<void(Msg)> dispatch) mutable {
        try {
            deps().stream(std::move(req), [dispatch](Msg m) {
                dispatch(std::move(m));
            });
        } catch (const std::exception& e) {
            // The stream backend threw before producing a terminal event —
            // surface it as StreamError so the UI doesn't hang on the spinner.
            dispatch(StreamError{std::string{"stream backend: "} + e.what()});
        } catch (...) {
            dispatch(StreamError{"stream backend: unknown exception"});
        }
    });
}

Cmd<Msg> run_tool(ToolCallId id, ToolName tool_name, nlohmann::json args) {
    return Cmd<Msg>::task(
        [id = std::move(id),
         name = std::move(tool_name),
         args = std::move(args)]
        (std::function<void(Msg)> dispatch) {
            // Install a thread-local progress sink *before* dispatch so the
            // subprocess runner inside the tool can stream stdout+stderr to
            // the UI as bytes arrive. RAII scope guarantees the sink is
            // cleared even if the tool throws, so the next tool run can't
            // inherit a stale dispatch lambda.
            moha::tools::progress::Scope progress_scope{
                [dispatch, id](std::string_view snapshot) {
                    dispatch(ToolExecProgress{id, std::string{snapshot}});
                }};
            try {
                auto result = tool::DynamicDispatch::execute(name.value, args);
                if (result) {
                    dispatch(ToolExecOutput{id, std::move(result->text), false});
                } else {
                    dispatch(ToolExecOutput{id, std::move(result.error().message), true});
                }
            } catch (const std::exception& e) {
                // DynamicDispatch already catches tool exceptions, but guard
                // against anything in the dispatch infrastructure itself so
                // the tool never gets stuck in Running with no terminal Msg.
                dispatch(ToolExecOutput{id, std::string{"dispatch error: "} + e.what(), true});
            } catch (...) {
                dispatch(ToolExecOutput{id, "dispatch error: unknown exception", true});
            }
        });
}

Cmd<Msg> kick_pending_tools(Model& m) {
    if (m.current.messages.empty()) return Cmd<Msg>::none();
    auto& last = m.current.messages.back();
    if (last.role != Role::Assistant) return Cmd<Msg>::none();

    std::vector<Cmd<Msg>> cmds;
    bool any_pending = false;

    for (auto& tc : last.tool_calls) {
        if (tc.status == ToolUse::Status::Pending) {
            const bool needs_perm = tool::DynamicDispatch::needs_permission(tc.name.value, m.profile);
            if (needs_perm && !m.pending_permission) {
                m.pending_permission = PendingPermission{
                    tc.id, tc.name,
                    "Tool " + tc.name.value + " needs permission under "
                        + std::string{ui::profile_label(m.profile)} + " profile"};
                m.stream.phase = Phase::AwaitingPermission;
                return Cmd<Msg>::none();
            }
            if (!needs_perm) {
                tc.status = ToolUse::Status::Running;
                tc.started_at = std::chrono::steady_clock::now();
                cmds.push_back(run_tool(tc.id, tc.name, tc.args));
                m.stream.phase = Phase::ExecutingTool;
                // Keep the Tick subscription alive during tool execution so
                // the spinner advances and the view can show live elapsed
                // time — without this the UI looks frozen on long-running
                // bash commands, and users think moha has hung.
                m.stream.active = true;
                any_pending = true;
            }
        } else if (tc.status == ToolUse::Status::Running) {
            any_pending = true;
        }
    }

    if (!any_pending) {
        bool has_results = false;
        for (const auto& tc : last.tool_calls) {
            if (tc.status == ToolUse::Status::Done
                || tc.status == ToolUse::Status::Error
                || tc.status == ToolUse::Status::Rejected) {
                has_results = true;
                break;
            }
        }
        if (has_results) {
            m.stream.phase = Phase::Streaming;
            m.stream.active = true;
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.current.messages.push_back(std::move(placeholder));
            cmds.push_back(launch_stream(m));
        } else {
            m.stream.phase = Phase::Idle;
            m.stream.active = false;  // stop the Tick subscription at rest
        }
    }
    return Cmd<Msg>::batch(std::move(cmds));
}

Cmd<Msg> fetch_models() {
    return Cmd<Msg>::task([](std::function<void(Msg)> dispatch) {
        try {
            auto models = anthropic::list_models(deps().auth_header, deps().auth_style);
            dispatch(ModelsLoaded{std::move(models)});
        } catch (const std::exception& e) {
            dispatch(StreamError{std::string{"models fetch: "} + e.what()});
        } catch (...) {
            dispatch(StreamError{"models fetch: unknown exception"});
        }
    });
}

} // namespace moha::app::cmd
