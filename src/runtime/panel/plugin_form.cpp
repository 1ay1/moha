// plugin_form.cpp — the plugin editor's row model. See header for the
// kind-first add contract. Pure projection: inputs → rows, no IO.

#include "agentty/runtime/panel/plugin_form.hpp"

namespace agentty::plugin_form {

namespace {

// One place for the kind vocabulary: ids, labels, and the one-line help the
// choice dropdown shows. Order = dropdown order (stdio first — the common
// case; passthrough last — the expert case).
struct KindInfo {
    const char* id;
    const char* label;
    const char* hint;
};
constexpr KindInfo kKinds[] = {
    {kKindStdio,       "stdio (spawn a command)",
     "launch a local MCP server process"},
    {kKindHttp,        "http (remote MCP)",
     "connect to a Streamable-HTTP MCP server"},
    {kKindSse,         "sse (remote MCP, legacy)",
     "connect to an SSE MCP server"},
    {kKindPassthrough, "passthrough (proxy tools)",
     "execute tools a proxy/gateway advertises, via POST to a URL"},
};

[[nodiscard]] const KindInfo& kind_info(const std::string& id) {
    for (const auto& k : kKinds)
        if (id == k.id) return k;
    return kKinds[0];
}

} // namespace

form::Form build_form(const PluginFormInputs& in) {
    const bool add = in.add_mode;
    form::Builder b{add ? " Add plugin " : (" " + in.name + " ")};

    // ── Subtitle: the pane's one-line situation report ─────────────
    if (add) {
        b.subtitle("pick a kind — the fields below follow");
    } else if (in.untrusted) {
        b.subtitle("untrusted project config — approve below to enable");
    } else if (!in.error.empty()) {
        b.subtitle(in.error);
    } else if (!in.enabled) {
        b.subtitle("disabled — nothing from this server is on the wire");
    } else if (in.kind == kKindPassthrough) {
        b.subtitle("forwards proxy-advertised tool calls to your URL");
    } else {
        b.subtitle(in.connected ? "connected" : "connecting…");
    }

    // ── Kind ───────────────────────────────────────────────────────
    {
        std::vector<std::string> labels, ids, hints;
        for (const auto& k : kKinds) {
            labels.emplace_back(k.label);
            ids.emplace_back(k.id);
            hints.emplace_back(k.hint);
        }
        b.choice(kKind, "Kind", std::move(labels), std::move(ids),
                 kind_info(in.kind).id, {}, std::move(hints));
        if (!add)
            b.lock("transport is identity — remove and re-add to change it");
    }

    // ── Identity ───────────────────────────────────────────────────
    if (add) {
        b.text(kName, "Name", in.name,
               "letters, digits, _ - : (this is the mcp.json key)");
    }

    // ── Kind-specific transport rows ───────────────────────────────
    if (in.kind == kKindStdio) {
        b.text(kCommand, "Command", in.command,
               "executable to spawn (searched on PATH)");
        b.text(kArgs, "Arguments", in.args,
               "space-separated; quoting is not shell — one token per arg");
    } else if (in.kind == kKindPassthrough) {
        b.text(kUrl, "URL", in.url,
               "calls POST their args JSON here; the body is the result");
        if (add) {
            b.text(kTools, "Tools", in.tools,
                   "comma-separated tool names the proxy advertises");
        }
        b.toggle(kAdvertise, "Advertise on the wire", in.advertise,
                 "off = proxy owns the schema (usual); on = agentty "
                 "advertises it too");
    } else {   // http / sse
        b.text(kUrl, "URL", in.url,
               "http(s)://host[:port]/path of the MCP endpoint");
    }

    // ── Scope + provenance ─────────────────────────────────────────
    if (add) {
        b.toggle(kScopeProject, "Project-scoped", in.project,
                 "write to ./.agentty/mcp.json instead of your user config");
    } else {
        b.header("Where it lives");
        b.text("scope_ro", "Scope", in.scope_label, {});
        b.lock("provenance");
        b.path("file_ro", "Config file", in.config_file, {});
        b.lock("edit via Save below — this row is the address");
    }

    // ── Detail-mode: enabled + tools ───────────────────────────────
    if (!add) {
        b.toggle(kEnabled, "Enabled", in.enabled,
                 in.kind == kKindPassthrough
                     ? "off = its declared tools stop dispatching"
                     : "off = disconnect; nothing from it is advertised");

        if (!in.tool_rows.empty()) {
            b.header(in.kind == kKindPassthrough ? "Declared tools"
                                                 : "Advertised tools");
            for (const auto& t : in.tool_rows) {
                b.toggle(kToolPrefix + t.name, t.name, t.enabled,
                         t.description);
            }
        }
    }

    // ── Actions ────────────────────────────────────────────────────
    b.header("");
    if (!add && in.untrusted) {
        b.action(kApprove, "Approve this server",
                 "records a hash of this exact spec; edits re-ask");
    }
    b.action(kSave, add ? "Add plugin" : "Save changes",
             add ? "validates, writes mcp.json, connects on next reload"
                 : "writes this entry back to its mcp.json");
    if (!add) {
        b.action(kRemove, "Remove…",
                 "deletes the entry from its mcp.json (asks once)");
    }

    b.note(add ? kNoteAdd : kNoteDetail);
    return b.build();
}

} // namespace agentty::plugin_form
