// settings_items.cpp — build each category's rows live from the loaders.

#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/util/home_dir.hpp"
#include "agentty/util/user_root.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/tool/hooks.hpp"

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "agentty/mcp/client.hpp"   // PluginModel, plugin_model()
#include <sstream>

namespace fs = std::filesystem;

namespace agentty::settings {

namespace {

std::vector<Item> general(const Model& m) {
    std::vector<Item> out;

    // Permission profile.
    {
        Item i;
        i.primary   = "Permission profile";
        i.secondary = std::string("current: ") +
                      std::string(to_string(m.d.profile));
        i.hint      = "Enter: cycle";
        i.action    = Action::CycleProfile;
        out.push_back(std::move(i));
    }
    // Smart Mode.
    {
        Item i;
        i.primary   = "Smart Mode";
        i.secondary = m.d.smart.enabled ? "on — role-based routing active"
                                        : "off";
        i.hint      = "Enter: configure";
        i.action    = Action::OpenSmart;
        out.push_back(std::move(i));
    }
    // RAG proactive retrieval.
    {
        Item i;
        i.primary   = "Proactive retrieval (RAG)";
        i.secondary = "pre-turn context injection";
        i.hint      = "Enter: configure";
        i.action    = Action::OpenRag;
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<Item> plugins(const agentty::mcp::PluginModel& model, bool loading) {
    // Pure projection of the PluginModel snapshot OWNED BY THE MODEL
    // (m.ui.plugins) — NOT a live plugin_model() call. This keeps items_for
    // (and therefore the view) a pure function of the Model, so the render
    // gate + update loop can see every change. No reconciliation here: the
    // snapshot already unified config vs live catalog.
    std::vector<Item> out;

    if (model.servers.empty()) {
        Item i;
        if (loading) {
            i.primary   = "connecting to plugins…";
            i.secondary = "reading mcp.json and handshaking servers";
        } else {
            i.primary   = "(no plugins configured)";
            i.secondary = "press `a` to add one, or agentty plugin add <name> …";
        }
        i.hint      = "docs: plugins";
        out.push_back(std::move(i));
        return out;
    }

    // Header: total tools on the wire + budget warning.
    {
        Item w;
        if (model.over_budget()) {
            w.primary   = std::to_string(model.wire_tool_count) +
                          " tools (budget " + std::to_string(model.tool_budget) + ")";
            w.secondary = std::to_string(model.trimmed_count) +
                          " over budget were dropped from this session — "
                          "disable some below to make room";
            w.status    = Item::Status::Bad;
        } else {
            w.primary   = std::to_string(model.wire_tool_count) +
                          " tools on the wire";
            w.secondary = "◉ on / ○ off · Enter toggles · d removes · a adds";
            w.status    = Item::Status::Neutral;
        }
        out.push_back(std::move(w));
    }

    for (const auto& s : model.servers) {
        Item i;
        i.primary   = s.name;
        i.on        = !s.disabled;
        i.arg       = s.name;
        // Provenance: badge the scope and carry the config dir so a
        // remove/toggle edits THIS server's actual mcp.json, not always the
        // user file. A user-scope server needs no badge (the common case).
        i.scope_label = std::string{agentty::mcp::to_string(s.origin)};
        i.config_dir  = s.config_dir;
        i.untrusted   = s.untrusted;
        const std::string scope_tag =
            (s.origin == agentty::mcp::Origin::User) ? "" : (i.scope_label + " · ");
        // Passthrough servers execute nothing locally — they forward a
        // proxy-advertised call's args to a URL. Badge the row so the
        // difference from a spawned/MCP server is visible at a glance.
        const std::string kind_tag = s.passthrough ? "⇄ passthrough · " : "";
        // A tool is EFFECTIVELY inactive unless its plugin is enabled AND
        // connected — a disabled or still-connecting plugin can run nothing,
        // so its whole subtree renders dimmed (but keeps individual state).
        const bool subtree_inactive = s.disabled || !s.connected;
        if (s.disabled) {
            i.secondary = scope_tag + kind_tag + "disabled · "
                        + std::to_string(s.tools.size())
                        + (s.tools.size() == 1 ? " tool" : " tools")
                        + " — Enter to enable";
            i.status    = Item::Status::Neutral;   // off on purpose — not an error
        } else if (!s.error.empty()) {
            i.secondary = scope_tag + kind_tag + s.error;
            i.status    = Item::Status::Bad;
        } else if (!s.connected) {
            i.secondary = scope_tag + kind_tag + "connecting…";
            i.status    = Item::Status::Pending;
        } else if (s.passthrough) {
            // "active", not "connected" — nothing handshakes; the tools
            // dispatch to the URL on demand. Show WHERE calls go: that URL
            // is the entire trust story of this entry.
            i.secondary = scope_tag + kind_tag
                        + std::to_string(s.enabled_count()) + " of "
                        + std::to_string(s.tools.size())
                        + (s.tools.size() == 1 ? " tool" : " tools")
                        + " → " + s.url;
            i.status    = Item::Status::Ok;
        } else {
            i.secondary = scope_tag + std::to_string(s.enabled_count()) + " of " +
                          std::to_string(s.tools.size()) + " tools active";
            i.status    = Item::Status::Ok;
        }
        // Enter toggles the WHOLE plugin on/off (reversible). Remove is the
        // deliberate `d` key — destructive actions aren't the default Enter.
        // But an UNTRUSTED project server can't be toggled into life at all
        // until its config is vouched for, so there Enter APPROVES instead.
        if (s.untrusted) {
            i.action = Action::ApprovePlugin;
            i.hint   = "trust & enable";
        } else {
            i.action = Action::TogglePlugin;
            i.hint   = s.disabled ? "enable" : "disable";
        }
        out.push_back(std::move(i));

        for (const auto& t : s.tools) {
            Item ti;
            ti.primary   = t.name;
            if (t.over_budget && !subtree_inactive) {
                ti.secondary = "over budget — not on the wire";
                ti.status    = Item::Status::Bad;
            }
            ti.action    = Action::ToggleTool;
            ti.arg       = s.name;
            ti.arg2      = t.name;
            ti.config_dir = s.config_dir;   // route the exclude edit to THIS server's file
            ti.indented  = true;
            ti.on        = t.enabled;
            ti.inactive  = subtree_inactive;
            // Under a disabled plugin, toggling one tool is meaningless —
            // enable the plugin first. Say so instead of "disable/enable".
            ti.hint      = subtree_inactive ? ""
                                            : (t.enabled ? "disable" : "enable");
            out.push_back(std::move(ti));
        }
    }
    return out;
}

std::vector<Item> commands() {
    std::vector<Item> out;
    for (const auto& c : tools::commands::all()) {
        Item i;
        i.primary   = "/" + c.name;
        i.secondary = c.description;
        i.hint      = c.source;   // project | user
        out.push_back(std::move(i));
    }
    if (out.empty()) {
        Item i;
        i.primary   = "(no slash commands)";
        i.secondary = "author one: .agentty/commands/<name>.md";
        i.hint      = "docs: slash-commands";
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<Item> agents() {
    std::vector<Item> out;
    // Built-ins first (always available), then user agents.
    for (const char* b : {"explorer", "reviewer", "tester", "coder", "general"}) {
        Item i;
        i.primary   = b;
        i.secondary = "built-in";
        out.push_back(std::move(i));
    }
    // User agents are discovered by the task backend; surface the authoring
    // path so the pane is self-documenting even before any exist.
    Item hint;
    hint.primary   = "+ user subagents";
    hint.secondary = "author one: .agentty/agents/<name>.md "
                     "(frontmatter: tools, read-only)";
    hint.hint      = "docs: subagents";
    out.push_back(std::move(hint));
    return out;
}

std::vector<Item> hooks() {
    std::vector<Item> out;
    const std::string file = tools::hooks::active_file();
    if (file.empty()) {
        Item i;
        i.primary   = "(no hooks file)";
        i.secondary = "author one: .agentty/hooks.json "
                      "(pre_tool / post_tool)";
        i.hint      = "docs: hooks";
        out.push_back(std::move(i));
        return out;
    }
    Item i;
    i.primary = file;
    if (tools::hooks::pending_approval()) {
        i.secondary = "NOT APPROVED — hooks will not run";
        i.hint      = "Enter: review & approve";
        i.action    = Action::ApproveHooks;
        i.status    = Item::Status::Bad;
    } else {
        i.secondary = "approved — active";
        i.hint      = "";
        i.status    = Item::Status::Ok;
    }
    out.push_back(std::move(i));
    return out;
}

} // namespace

std::vector<Item> items_for(const Model& m, Category cat) {
    switch (cat) {
        case Category::General:  return general(m);
        case Category::Plugins:  return plugins(m.ui.plugins, m.ui.plugins_loading);
        case Category::Commands: return commands();
        case Category::Agents:   return agents();
        case Category::Hooks:    return hooks();
    }
    return {};
}

namespace {

// Whitespace-split a line into tokens.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(std::move(tok));
    return out;
}

// A plugin/command NAME token: [A-Za-z0-9_:-], non-empty, length-capped.
// `:` is allowed here because commands namespace with it (git:fixup) and
// plugin names may too; create_starter() maps it to a subdirectory rather
// than writing it into a single filename (which would be illegal on Windows).
constexpr std::size_t kMaxNameLen = 96;   // whole spec
constexpr std::size_t kMaxSegLen  = 64;   // one path segment between colons

bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > kMaxNameLen) return false;
    // A leading '-' is a flag the user misplaced (e.g. `--http url` with no
    // name), not a real plugin name — reject rather than mint a plugin called
    // "--http".
    if (n.front() == '-') return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'
              || c == '-' || c == ':'))
            return false;
    return true;
}

// Validate a create_starter name AND split it into path segments on ':'.
// Each segment must be a safe filename fragment: non-empty, length-capped,
// no leading dot / dash (avoids hidden files and option-looking names), and
// never "."/".." (path escape). Returns {} on any violation.
std::vector<std::string> starter_segments(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameLen) return {};
    std::vector<std::string> segs;
    std::string cur;
    auto flush = [&]() -> bool {
        if (cur.empty() || cur.size() > kMaxSegLen) return false;
        if (cur == "." || cur == "..") return false;
        if (cur.front() == '.' || cur.front() == '-') return false;
        for (char c : cur)
            if (!(std::isalnum(static_cast<unsigned char>(c))
                  || c == '_' || c == '-'))
                return false;
        segs.push_back(std::move(cur));
        cur.clear();
        return true;
    };
    for (char c : name) {
        if (c == ':') { if (!flush()) return {}; }
        else          cur.push_back(c);
    }
    if (!flush()) return {};
    if (segs.size() > 3) return {};   // matches the loader's kMaxDepth nesting
    return segs;
}

} // namespace

AddResult add_plugin_from_line(const std::string& line) {
    auto tok = split_ws(line);
    // Pull an optional --project flag from anywhere in the line; the rest is
    // the <name> <recipe> [args…] spec.
    bool project = false;
    std::vector<std::string> pos;
    for (auto& t : tok) {
        if (t == "--project") project = true;
        else pos.push_back(std::move(t));
    }
    if (pos.size() < 2)
        return {false, "usage: <name> <command> [args…]  ·  <name> --http <url>  ·  "
                       "<name> --passthrough <url> <tool>[,<tool>…]  ·  "
                       "<name> --python file.py / --uvx pkg / --npx pkg  "
                       "[--project]"};
    const std::string name = pos[0];
    if (!valid_name(name))
        return {false, "plugin name may use letters, digits, _ - : only"};

    tools::plugin::ServerSpec spec;
    spec.name = name;
    const std::string& recipe = pos[1];
    std::vector<std::string> rest(pos.begin() + 2, pos.end());

    if (recipe == "--python") {
        if (rest.empty()) return {false, "--python needs a script path"};
        spec.command = "python3";
        std::error_code ec;
        fs::path abs = fs::absolute(rest[0], ec);
        // Reject a non-existent script up front — otherwise the plugin is
        // written to mcp.json and only fails opaquely at spawn time on the
        // next reload, with no hint the path was a typo.
        if (!ec && !fs::is_regular_file(abs, ec))
            return {false, "no such script: " + abs.string()};
        if (!ec) rest[0] = abs.string();
        spec.args = std::move(rest);
    } else if (recipe == "--uvx") {
        if (rest.empty()) return {false, "--uvx needs a package name"};
        spec.command = "uvx";
        spec.args = std::move(rest);
    } else if (recipe == "--npx") {
        if (rest.empty()) return {false, "--npx needs a package name"};
        spec.command = "npx";
        spec.args.push_back("-y");
        for (auto& t : rest) spec.args.push_back(std::move(t));
    } else if (recipe == "--http" || recipe == "--sse") {
        // Remote transport — a url, no local command. Not trust-gated (it
        // spawns nothing) and connects on the next reload.
        if (rest.empty()) return {false, std::string{recipe} + " needs a url"};
        if (rest[0].rfind("http://", 0) != 0 && rest[0].rfind("https://", 0) != 0)
            return {false, "url must start with http:// or https://"};
        spec.url  = rest[0];
        spec.type = (recipe == "--sse") ? "sse" : "http";
    } else if (recipe == "--passthrough") {
        // Foreign tools a proxy/gateway advertises to the model (e.g.
        // LiteLLM + headroom injecting `headroom_retrieve`): register them
        // for DISPATCH — agentty POSTs the call's args to <url> and returns
        // the body — without advertising them (the proxy owns the schema).
        if (rest.size() < 2)
            return {false, "--passthrough needs <url> <tool>[,<tool>…]"};
        if (rest[0].rfind("http://", 0) != 0 && rest[0].rfind("https://", 0) != 0)
            return {false, "url must start with http:// or https://"};
        spec.url  = rest[0];
        spec.type = "passthrough";
        // Tools: remaining tokens, each possibly comma-separated.
        for (std::size_t r = 1; r < rest.size(); ++r) {
            std::string_view sv{rest[r]};
            while (!sv.empty()) {
                const auto comma = sv.find(',');
                std::string_view tn = sv.substr(0, comma);
                if (!tn.empty()) spec.passthrough.emplace_back(tn);
                if (comma == std::string_view::npos) break;
                sv.remove_prefix(comma + 1);
            }
        }
        if (spec.passthrough.empty())
            return {false, "--passthrough needs at least one tool name"};
    } else {
        spec.command = recipe;
        spec.args = std::move(rest);
    }

    const auto path = tools::plugin::config_path(project);
    switch (tools::plugin::add_server(path, spec, /*force=*/false)) {
        case tools::plugin::EditResult::Ok:
            return {true, "added " + std::string{project ? "project " : ""}
                          + "plugin '" + name + "'"};
        case tools::plugin::EditResult::AlreadyExists:
            return {false, "'" + name + "' already exists"};
        case tools::plugin::EditResult::ParseError:
            return {false, "mcp.json is not valid JSON — fix it by hand"};
        default:
            return {false, "could not write mcp.json"};
    }
}

AddResult create_starter(Category cat, const std::string& name) {
    // Validate AND split on ':' into safe path segments. The command/agent
    // loaders map a `:`-namespaced name to a SUBDIRECTORY (git:fixup ->
    // git/fixup.md), so writing the colon verbatim into one filename both
    // fails on Windows (`:` is illegal there) and mismatches the loader.
    const std::vector<std::string> segs = starter_segments(name);
    if (segs.empty())
        return {false, "name: letters/digits/_/- per segment, ':' to nest, "
                       "≤3 levels, no leading dot or dash"};
    const fs::path uroot = util::user_root();
    if (uroot.empty()) return {false, "no HOME to write under"};

    const char* sub = nullptr;
    std::string tmpl;
    if (cat == Category::Commands) {
        sub = "commands";
        tmpl = "---\ndescription: " + name + " command\n"
               "argument-hint: <args>\n---\n"
               "Do the thing for $ARGUMENTS.\n";
    } else if (cat == Category::Agents) {
        sub = "agents";
        tmpl = "---\ndescription: " + name + " agent\n"
               "read-only: false\n"
               "# tools: read grep glob list_dir   # optional allowlist\n"
               "---\nYour role: " + name +
               ". Complete the delegated task end-to-end, then report.\n";
    } else {
        return {false, "create_starter only supports commands/agents"};
    }

    // Build <user-root>/<sub>/<seg1>/<seg2>/<leaf>.md from the validated
    // segments (all-but-last are directories). Segments are known-safe
    // (no '.'/'..'/separators), so no path escape is possible. user_root
    // honors $AGENTTY_HOME and is the same base the skills/commands
    // scan ladder resolves (scope::Env::user_native_base).
    fs::path dir = uroot / sub;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) dir /= segs[i];
    const fs::path file = dir / (segs.back() + ".md");

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {false, "could not create dir " + dir.string()};
    if (fs::exists(file, ec))
        return {false, "already exists: " + file.string()};
    std::ofstream f(file, std::ios::binary);
    if (!f) return {false, "could not create " + file.string()};
    f << tmpl;
    f.flush();
    if (!f) return {false, "write failed: " + file.string()};

    // Force a rescan so the new entry shows on the next open.
    if (cat == Category::Commands) tools::commands::invalidate_cache();
    return {true, "created " + file.string() + " — edit it, then reopen"};
}

} // namespace agentty::settings
