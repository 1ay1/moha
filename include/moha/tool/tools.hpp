#pragma once
// Tool factory declarations. Each tool_*() returns a freshly-constructed
// ToolDef; registry.cpp's aggregator calls every one of these exactly once
// at static-init time. Per-tool bodies live in src/tool/tools/<name>.cpp —
// splitting them out lets a single-tool edit rebuild just its TU rather
// than dragging the whole 1900-line registry through the compiler.

#include "moha/tool/registry.hpp"

namespace moha::tools {

[[nodiscard]] ToolDef tool_read();
[[nodiscard]] ToolDef tool_write();
[[nodiscard]] ToolDef tool_edit();
[[nodiscard]] ToolDef tool_bash();
[[nodiscard]] ToolDef tool_grep();
[[nodiscard]] ToolDef tool_glob();
[[nodiscard]] ToolDef tool_list_dir();
[[nodiscard]] ToolDef tool_todo();
[[nodiscard]] ToolDef tool_web_fetch();
[[nodiscard]] ToolDef tool_web_search();
[[nodiscard]] ToolDef tool_find_definition();
[[nodiscard]] ToolDef tool_diagnostics();
[[nodiscard]] ToolDef tool_git_status();
[[nodiscard]] ToolDef tool_git_diff();
[[nodiscard]] ToolDef tool_git_log();
[[nodiscard]] ToolDef tool_git_commit();
// ── Tier-2 token-saver tools (index-backed) ──────────────────────────────
[[nodiscard]] ToolDef tool_outline();
[[nodiscard]] ToolDef tool_repo_map();
[[nodiscard]] ToolDef tool_signatures();
// ── Tier-3 sub-agent ─────────────────────────────────────────────────────
[[nodiscard]] ToolDef tool_investigate();

// ── Persistent workspace memory ──────────────────────────────────────────
[[nodiscard]] ToolDef tool_remember();
[[nodiscard]] ToolDef tool_forget();
[[nodiscard]] ToolDef tool_memos();

} // namespace moha::tools
