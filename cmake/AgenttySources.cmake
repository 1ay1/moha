# AgenttySources.cmake — the per-domain source group lists. Pure variable
# declarations (set(AGENTTY_*_SOURCES ...)) consumed by the object libraries
# and the main exe. No targets, no flags — safe to include anywhere before the
# objlib/exe definitions.

# ── Source groups, by domain ──────────────────────────────────────────────
# The header tree under include/agentty/ mirrors this grouping one-for-one:
#   domain/    — pure value types (no I/O, no UI). Headers only.
#   io/        — sockets, TLS, HTTP/2, OAuth, on-disk persistence, OS
#                clipboard. Anything that talks to the kernel or the
#                wire and isn't tied to a specific provider.
#   provider/  — wire-format adapters for upstream LLM APIs.
#   diff/      — unified-diff parser used by the edit tool & review modal.
#   tool/      — the agent's capability surface: registry + per-tool impls.
#   workspace/ — pure-I/O scanners over the active workspace root (file
#                enumeration for @mention, symbol enumeration for #).
#                Consumed by the runtime's pickers but separate from the
#                UI state they feed.
#   airgap/    — the `agentty airgap` CLI subcommand (exec's into ssh,
#                never returns).  Not part of the maya runtime.
#   runtime/   — the Elm-style app: model, update, subscriptions, view tree.

set(AGENTTY_IO_SOURCES
    src/io/http.cpp
    src/io/inflate.cpp
    src/io/tls.cpp
    src/io/auth.cpp
    src/io/accounts.cpp
    src/io/account_switch.cpp
    src/io/vault.cpp
    src/io/cred_crypt.cpp
    src/io/keystore.cpp
    src/io/persistence.cpp
    src/io/clipboard.cpp
    src/util/base64.cpp
    src/util/dbglog.cpp
    src/util/logx.cpp
    src/util/home_dir.cpp
    src/util/user_root.cpp
    src/util/update.cpp
    src/util/modelsdev.cpp
    src/domain/complexity.cpp
    src/domain/model_name.cpp
)

set(AGENTTY_WORKSPACE_SOURCES
    src/workspace/files.cpp
    src/workspace/symbols.cpp
    src/workspace/checkpoint.cpp
)

set(AGENTTY_AIRGAP_SOURCES
    src/airgap/airgap.cpp
)

set(AGENTTY_PROVIDER_SOURCES
    src/provider/anthropic/transport.cpp
    src/provider/anthropic/sse.cpp
    src/provider/anthropic/wire_body.cpp
    src/provider/prompt.cpp
    src/provider/chatgpt/provider.cpp
    src/provider/chatgpt/codex_oauth.cpp
    src/provider/chatgpt/responses.cpp
    # The OpenAI Responses-API dialect, shared by every host that speaks it
    # (ChatGPT/Codex today, GitHub Copilot next). Extracted from
    # chatgpt/responses.cpp — see responses/responses.hpp for the contract.
    src/provider/responses/codec.cpp
    src/provider/copilot/provider.cpp
    src/provider/copilot/copilot_oauth.cpp
    src/provider/kimi/provider.cpp
    src/provider/kimi/kimi_oauth.cpp
    src/provider/openai/transport.cpp
    src/provider/openai/responses_site.cpp
    src/provider/ollama/transport.cpp
    src/provider/selection.cpp
    # Registry predicates that need a provider's own knowledge (the
    # model-aware reasoning-text answer consults Copilot's dialect table).
    src/provider/registry.cpp
    src/provider/dialect.cpp
    src/provider/credentials.cpp
    src/provider/auth_state.cpp
    src/provider/prompt_policy.cpp
    # The ONE provider-routing seam — dispatch_stream(). Provider-agnostic
    # (type-erased Routes, incl. the ACP arm), so it has no acp-TU dependency
    # and lives in the provider objlib every test links. main.cpp binds the
    # long-lived + external routes; dispatch just routes on the Selection.
    src/provider/dispatch.cpp
    # ACP agent launch registry (config loader). Pure nlohmann/json, no acp-cpp
    # dep, and referenced by selection.cpp (is_acp_agent_id) — so it lives in
    # the provider objlib every test links, NOT in agentty_acp_obj.
    src/provider/acp_agents.cpp
)

# ACP (Agent Client Protocol) — lets agentty run as a headless agent
# subprocess that Zed (or any ACP client) drives over JSON-RPC on stdio.
# The wire protocol/engine/transport live in the acp-cpp submodule (linked
# as acp::acp); server.cpp is the agentty-specific glue (turn loop + tools).
set(AGENTTY_ACP_SOURCES
    src/acp/server.cpp
    src/provider/external_acp_backend.cpp
    src/provider/acp_provider_adapter.cpp
)

# MCP (Model Context Protocol) client glue — spawns external MCP servers and
# exposes their tools as agentty ToolDefs. The heavy mcp-cpp templates are
# confined to these TUs (linked as mcp::mcp). Only compiled when AGENTTY_MCP.
set(AGENTTY_MCP_SOURCES
    src/mcp/bridge.cpp
    src/mcp/http_server.cpp
    src/mcp/serve.cpp
    src/mcp/oauth.cpp
    src/tool/mcp_tools_bridge.cpp
    src/tool/mcp_tools_backends.cpp
)

set(AGENTTY_DIFF_SOURCES
    src/diff/diff.cpp
)

# The RAG engine is now the external rag-cpp library (submodule rag-cpp/,
# target ragcpp::ragcpp). Only the thin agentty adapter that maps the app's
# retrieval boundary onto rag::Engine remains in-tree.
set(AGENTTY_RAG_SOURCES
    src/rag/adapter.cpp
    src/rag/embed_backend.cpp
    src/rag/embed_secret.cpp
)

set(AGENTTY_TOOL_SOURCES
    src/tool/registry.cpp
    src/scope/scope.cpp
    src/scope/trust.cpp
    src/tool/progress.cpp
    src/tool/util/utf8.cpp
    src/tool/util/fs_helpers.cpp
    src/tool/util/subprocess.cpp
    src/tool/util/sandbox.cpp
    src/tool/util/arg_reader.cpp
    src/tool/util/partial_json.cpp
    src/tool/subagent.cpp
    src/tool/skills.cpp
    src/tool/commands.cpp
    src/tool/hooks.cpp
    src/tool/plugin.cpp
    src/tool/memory_store.cpp
)

# Everything except the entry point — so tests can link the same runtime
# without fighting main().
set(AGENTTY_RUNTIME_NOMAIN_SOURCES
    src/runtime/composer_attachment.cpp
    src/runtime/app/deps.cpp
    src/runtime/app/settings_cache.cpp
    src/runtime/app/init.cpp
    src/runtime/app/cmd_factory.cpp
    src/runtime/app/update.cpp
    src/runtime/app/update/composer.cpp
    src/runtime/app/update/stream.cpp
    src/runtime/app/update/stream_preview.cpp
    src/runtime/app/update/smart_mode.cpp
    src/runtime/app/update/submit.cpp
    src/runtime/app/update/frozen.cpp
    src/runtime/app/update/tool.cpp
    src/runtime/app/update/tool_output.cpp
    src/runtime/app/update/login.cpp
    src/runtime/app/update/models.cpp
    src/runtime/app/update/providers.cpp
    src/runtime/app/update/thread_list.cpp
    src/runtime/app/update/palette.cpp
    src/runtime/app/update/mention.cpp
    src/runtime/app/update/symbol.cpp
    src/runtime/app/update/code_blocks.cpp
    src/runtime/app/update/checkpoints.cpp
    src/runtime/app/update/rag.cpp
    src/runtime/app/update/settings_list.cpp
    src/runtime/panel/settings_items.cpp
    src/runtime/panel/form.cpp
    src/runtime/panel/form_keys.cpp
    src/runtime/view/panels/form_common.cpp
    src/runtime/panel/rag_form.cpp
    src/runtime/settings_registry.cpp
    src/runtime/panel/smart_form.cpp
    src/runtime/app/update/fork.cpp
    src/runtime/app/update/diff_review.cpp
    src/runtime/app/update/meta.cpp
    src/runtime/app/subscribe.cpp

    src/runtime/view/cache.cpp
    src/runtime/view/helpers.cpp
    src/runtime/view/host_escape.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_args.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_helpers.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_body_common.cpp
    src/runtime/view/thread/turn/agent_timeline/edit_body.cpp
    src/runtime/view/thread/turn/agent_timeline/bash_body.cpp
    src/runtime/view/thread/turn/agent_timeline/write_body.cpp
    src/runtime/view/thread/turn/agent_timeline/git_diff_body.cpp
    src/runtime/view/thread/turn/agent_timeline/diff_preview_body.cpp
    src/runtime/view/thread/turn/agent_timeline/read_body.cpp
    src/runtime/view/thread/turn/agent_timeline/web_fetch_body.cpp
    src/runtime/view/thread/turn/agent_timeline/list_body.cpp
    src/runtime/view/thread/turn/agent_timeline/task_body.cpp
    src/runtime/view/thread/turn/agent_timeline/todo_body.cpp
    src/runtime/view/thread/turn/agent_timeline/tool_body_preview.cpp
    src/runtime/view/thread/turn/agent_timeline/agent_timeline.cpp
    src/runtime/view/thread/turn/permission.cpp
    src/runtime/view/thread/turn/turn.cpp
    src/runtime/view/thread/welcome_screen.cpp
    src/runtime/view/thread/conversation.cpp
    src/runtime/view/thread/thread.cpp
    src/runtime/view/composer.cpp
    src/runtime/view/status_bar/title_chip.cpp
    src/runtime/view/status_bar/phase_chip.cpp
    src/runtime/view/status_bar/token_stream_sparkline.cpp
    src/runtime/view/status_bar/context_gauge.cpp
    src/runtime/view/status_bar/status_banner.cpp
    src/runtime/view/status_bar/model_badge.cpp
    src/runtime/view/status_bar/status_bar.cpp
    src/runtime/view/changes_strip.cpp
    # One panel = one view file (view/panels/<name>.cpp); shared scaffolding
    # in panels_prologue.hpp / panels_common.hpp / form_common.cpp.
    src/runtime/view/panels/models.cpp
    src/runtime/view/panels/providers.cpp
    src/runtime/view/panels/thread_list.cpp
    src/runtime/view/panels/palette.cpp
    src/runtime/view/panels/mention.cpp
    src/runtime/view/panels/symbol.cpp
    src/runtime/view/panels/smart_mode.cpp
    src/runtime/view/panels/code_blocks.cpp
    src/runtime/view/panels/tool_output.cpp
    src/runtime/view/panels/checkpoints.cpp
    src/runtime/view/panels/todo.cpp
    src/runtime/view/panels/rag.cpp
    src/runtime/view/panels/settings_list.cpp
    src/runtime/view/panels/fork.cpp
    src/runtime/view/diff_review.cpp
    src/runtime/view/login.cpp
    src/runtime/view/view.cpp
)

set(AGENTTY_RUNTIME_SOURCES
    src/runtime/main.cpp
    ${AGENTTY_RUNTIME_NOMAIN_SOURCES}
)

