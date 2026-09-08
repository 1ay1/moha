#pragma once
// agentty::tools::plugin — the MCP plugin manager (`agentty plugin …`).
//
// A "plugin" in agentty IS an MCP server entry in mcp.json — there is no
// separate plugin runtime, registry, or manifest format. This module is
// the thin ergonomic layer over that fact: it edits mcp.json cleanly so
// users never hand-write JSON, with recipes for the common runtimes:
//
//   agentty plugin add weather --uvx mcp-weather          # PyPI, via uv
//   agentty plugin add mytool --python tools/my_mcp.py    # local script
//   agentty plugin add fs --npx @modelcontextprotocol/server-filesystem
//   agentty plugin add custom -- ./bin/server --flag x    # raw argv
//   agentty plugin list
//   agentty plugin remove weather
//
// Scope: --user (default) edits ~/.agentty/mcp.json; --project edits
// ./.agentty/mcp.json. Project configs additionally require the existing
// AGENTTY_MCP_ALLOW_PROJECT=1 trust gate before agentty will CONNECT to
// them (writing the entry is always allowed; the gate protects against
// cloned-repo configs running code, not against the user's own edits).
//
// Editing contract (pinned by plugin_config_test):
//   • Round-trip safe: every key this module does not own (other servers,
//     "servers" spelling, unknown top-level keys, per-server env/url/…)
//     is preserved byte-for-byte in JSON value terms.
//   • add on an existing name fails unless --force (no silent clobber).
//   • remove of an absent name is a distinct "not found" outcome.
//   • A missing file is created (with parent dirs) on first add.

#include <filesystem>
#include <string>
#include <vector>

namespace agentty::tools::plugin {

struct ServerSpec {
    std::string              name;
    std::string              command;
    std::vector<std::string> args;
    std::string              url;   // HTTP/SSE transport (no command)
    std::string              type;  // "http" | "sse" | "passthrough" | "" (default http for url)
    // type == "passthrough": foreign tool names to register for dispatch
    // (a proxy in front of the model endpoint advertises their schemas;
    // agentty executes by POSTing args to `url`).
    std::vector<std::string> passthrough;
};

enum class EditResult : std::uint8_t {
    Ok,
    AlreadyExists,   // add without --force onto an existing name
    NotFound,        // remove of an absent name
    ParseError,      // existing file is not valid JSON — refuse to touch it
    IoError,         // cannot read/write the path
};

// Add `spec` to the mcpServers object of the JSON file at `path`,
// creating the file if absent. Preserves every other key. `force`
// overwrites an existing entry of the same name.
[[nodiscard]] EditResult add_server(const std::filesystem::path& path,
                                    const ServerSpec& spec, bool force);

// Remove the named server. Preserves everything else.
[[nodiscard]] EditResult remove_server(const std::filesystem::path& path,
                                       const std::string& name);

// Enable/disable a WHOLE server without removing it — persisted as the
// server's top-level `disabled` flag in mcp.json (the bridge already skips a
// disabled server on connect). This is the primary Enter action on a plugin
// row: a reversible on/off, distinct from the destructive remove. No-op-Ok if
// already in the desired state.
[[nodiscard]] EditResult set_server_disabled(const std::filesystem::path& path,
                                             const std::string& name,
                                             bool disabled);

// True when the named server carries `"disabled": true` in mcp.json.
[[nodiscard]] bool is_server_disabled(const std::filesystem::path& path,
                                      const std::string& name);

// List the servers in the file (empty on missing/invalid file).
[[nodiscard]] std::vector<ServerSpec>
list_servers(const std::filesystem::path& path);

// Enable/disable ONE tool of a server, persisted as the server's
// `tools.exclude` list in mcp.json (the bridge already honours it). A
// disabled tool is dropped from the wire catalog on the next reload.
// `bare` is the tool's short name (e.g. "current_date", NOT the
// mcp__server__tool form). No-op-Ok if already in the desired state.
[[nodiscard]] EditResult set_tool_enabled(const std::filesystem::path& path,
                                          const std::string& server,
                                          const std::string& bare,
                                          bool enabled);

// True when `bare` is in server's tools.exclude (i.e. disabled).
[[nodiscard]] bool is_tool_disabled(const std::filesystem::path& path,
                                    const std::string& server,
                                    const std::string& bare);

// The bare names in a server's tools.exclude list (its disabled tools).
// Empty if the server/list is absent. Lets the UI show disabled tools
// (which are dropped from the live pool, so invisible otherwise).
[[nodiscard]] std::vector<std::string>
disabled_tools(const std::filesystem::path& path, const std::string& server);

// The config path for a scope. user → ~/.agentty/mcp.json,
// project → ./.agentty/mcp.json.
[[nodiscard]] std::filesystem::path config_path(bool project);

// ── Project-config trust (the RCE gate) ─────────────────────────────────
// A workspace-local ./.agentty/mcp.json can ride in on a clone and spawn
// arbitrary commands, so its servers don't connect until the human vouches
// for it: either AGENTTY_MCP_ALLOW_PROJECT=1, or an approval of the file's
// CONTENT HASH recorded here. Editing the file changes the hash and re-gates
// it (the MCPoison fix). Approvals persist under ~/.agentty, so a clone can
// neither approve itself nor keep an approval valid after its bytes change.
//
// is_project_config_trusted() — would the project config connect right now?
// approve_project_config()    — record trust for the CURRENT project config's
//                               content; returns false if there's no project
//                               config or the store can't be written.
[[nodiscard]] bool is_project_config_trusted();
[[nodiscard]] bool approve_project_config();

// Per-server trust — finer than the whole-file gate. Trust is bound to ONE
// server's spec (its command + args), so approving `date` doesn't bless a
// later-added `db`, and editing `date`'s command re-gates only `date`. A
// server counts as trusted if the blanket env opt-in is set, OR the whole
// project file is approved, OR this server's own spec hash is approved.
//   is_server_trusted(name) — would THIS project server connect right now?
//   approve_server(name)    — record trust for this server's current spec;
//                             false if the server/config is absent or unwritable.
[[nodiscard]] bool is_server_trusted(const std::filesystem::path& path,
                                     const std::string& name);
[[nodiscard]] bool approve_server(const std::filesystem::path& path,
                                  const std::string& name);

// The spawn-identity hash for one server's spec (command + url + args) — the
// bytes that would actually run. The bridge's connect loop builds these from
// the LIVE spec and checks the result against the approvals store, so
// file-side and live-side agree on identity. Empty when command AND url are
// both empty (nothing spawnable to trust).
[[nodiscard]] std::string server_spec_hash(const std::string& command,
                                           const std::string& url,
                                           const std::vector<std::string>& args);

// The `agentty plugin` CLI: verb ∈ {add, remove, list} with the argv tail
// after the verb. Returns a process exit code. Prints results/errors and,
// after a successful add, a short "restart to connect / trust gate" note.
int cli(const std::vector<std::string>& argv);

} // namespace agentty::tools::plugin
