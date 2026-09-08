#pragma once
// plugin_form — the plugin editor's row model (detail + add).
//
// Pure: reads a value snapshot (PluginFormInputs), emits a form::Form. The
// reducer resolves the snapshot from mcp.json + the live PluginModel and
// hands it in; this file only decides what the rows ARE. Same division of
// labour as smart_form.hpp — a config surface is a projection, not a pane
// with opinions.
//
// KIND-FIRST ADD: in add mode the first row is a `kind` choice
// (stdio/http/sse/passthrough + the python/uvx/npx shorthands). Changing it
// REBUILDS the form with exactly the field set that kind needs, preserving
// whatever name/url/command text the user already typed — a wizard without
// wizard chrome. In detail mode the kind is locked (changing a server's
// transport in place is a remove+add, not an edit — the spawn identity and
// trust hash change with it).

#include <string>
#include <vector>

#include "agentty/runtime/panel/form.hpp"

namespace agentty::plugin_form {

// Field ids (stable; the reducer switches on these).
inline constexpr const char* kKind        = "kind";
inline constexpr const char* kName        = "name";
inline constexpr const char* kCommand     = "command";
inline constexpr const char* kArgs        = "args";
inline constexpr const char* kUrl         = "url";
inline constexpr const char* kTools       = "tools";      // passthrough names
inline constexpr const char* kAdvertise   = "advertise";  // passthrough
inline constexpr const char* kEnabled     = "enabled";
inline constexpr const char* kScopeProject= "scope_project";  // add mode only
inline constexpr const char* kSave        = "save";
inline constexpr const char* kApprove     = "approve";
inline constexpr const char* kRemove      = "remove";
// Per-tool toggle rows use the id "tool:<bare-name>".
inline constexpr const char* kToolPrefix  = "tool:";

// Kind ids for the choice row.
inline constexpr const char* kKindStdio       = "stdio";
inline constexpr const char* kKindHttp        = "http";
inline constexpr const char* kKindSse         = "sse";
inline constexpr const char* kKindPassthrough = "passthrough";

struct ToolRow {
    std::string name;
    std::string description;
    bool enabled = true;
};

struct PluginFormInputs {
    bool add_mode = false;

    // Identity + transport (detail: from config; add: user-typed so far).
    std::string kind = kKindStdio;   // one of the kKind* ids
    std::string name;
    std::string command;
    std::string args;                // space-joined for the text row
    std::string url;
    std::string tools;               // passthrough: comma-joined names
    bool        advertise = false;   // passthrough: advertise on the wire too

    // Detail-mode state.
    bool        enabled   = true;    // !disabled in config
    bool        connected = false;
    std::string error;               // connect error / gate reason
    std::string scope_label;         // "user" / "project" / "explicit"
    std::string config_file;         // the mcp.json this entry lives in
    bool        untrusted = false;   // project entry awaiting approval
    std::vector<ToolRow> tool_rows;  // live/declared tools (detail mode)

    bool        project = false;     // add mode: write to project config
};

[[nodiscard]] form::Form build_form(const PluginFormInputs& in);

} // namespace agentty::plugin_form
