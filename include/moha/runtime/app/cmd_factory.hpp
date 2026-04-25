#pragma once
// moha::app::cmd — factories for the side-effecting commands the runtime issues.
//
// These wrap maya's Cmd<Msg> with moha-specific glue: kicking off a streaming
// turn, executing a tool, advancing pending tool execution after a turn ends.

#include <maya/maya.hpp>

#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::app::cmd {

// Mutates `m` to install a fresh cancel token in m.stream.cancel, then
// dispatches the streaming task on a worker. Esc (CancelStream) flips the
// token to abort the in-flight stream.
[[nodiscard]] maya::Cmd<Msg> launch_stream(Model& m);

[[nodiscard]] maya::Cmd<Msg> run_tool(ToolCallId id,
                                      ToolName tool_name,
                                      nlohmann::json args);

// Inspect the latest assistant turn and either fire off pending tool calls,
// request permission, or kick the follow-up stream once tool results are in.
// Mutates `m` (sets phase, may push a placeholder assistant message).
[[nodiscard]] maya::Cmd<Msg> kick_pending_tools(Model& m);

[[nodiscard]] maya::Cmd<Msg> fetch_models();

// ── In-app login modal ──────────────────────────────────────────────────
// Fire-and-forget: shells out to the platform browser opener. Wrapped in
// Cmd::task so a wedged xdg-open / open / ShellExecute can never block
// the reducer tick.
[[nodiscard]] maya::Cmd<Msg> open_browser_async(std::string url);

// Run the OAuth code-exchange HTTP POST off the UI thread. Dispatches
// LoginExchanged{result} on completion regardless of success/failure —
// the reducer matches on `expected<OAuthToken, OAuthError>` to decide
// whether to install creds or transition to Failed.
[[nodiscard]] maya::Cmd<Msg> oauth_exchange(auth::OAuthCode    code,
                                            auth::PkceVerifier verifier,
                                            auth::OAuthState   state);

// ── Auto-compact ────────────────────────────────────────────────────────
// Off-thread Haiku call that summarises `messages[first..last)` into a
// single dense paragraph. Dispatches `CompactCompleted` on the worker's
// completion. Caller (the reducer) is responsible for flipping
// `m.s.compaction = compact::Running{...}` *before* issuing the cmd
// so a duplicate request can't slip in.
[[nodiscard]] maya::Cmd<Msg> compact_thread(std::vector<Message> messages,
                                            std::size_t first,
                                            std::size_t last);

} // namespace moha::app::cmd
