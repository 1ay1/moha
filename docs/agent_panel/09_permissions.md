# 09 — Permissions (allow / deny / always)

When the model wants to invoke a tool that has potential side effects
(write a file, run a command, fetch a URL), the agent should pause
and ask the user. This doc spells out:

1. The decision model — when to ask, and at what granularity
2. The inline UI surface (always inside the tool card, never modal)
3. The "Always allow" patterns (per-tool, per-file, per-command)
4. The persistent permissions store
5. Profile defaults (Write / Ask / Minimal)
6. Failure modes and edge cases

References:

- Zed: `crates/agent_ui/src/thread_view.rs:6500-7000` (the permission
  card render path) and `crates/agent_settings/...` for the
  always-allow store.
- maya: `maya::widget::Permission` in
  `maya/include/maya/widget/permission.hpp` — the bordered
  amber-card primitive. Already implemented; this doc tells you how
  to embed it correctly.

## 1. The decision model

For every tool call the model wants to invoke, the agent:

1. Computes a **gating key** — a stable string that encodes the
   sensitive part of the action.
2. Looks the key up in the **permissions store** (per-thread,
   per-workspace, per-user).
3. If allowed already → execute; mark the tool card as
   `Running → Completed/Failed` directly.
4. If denied already → fail the tool with `error: denied by
   permission rule`; render `Failed` card.
5. If no rule → put the tool into `Confirmation` state, render the
   permission footer, wait for the user.

Gating keys per tool kind:

| Tool kind | Gating key | Example |
|---|---|---|
| `read` | `read:<glob>` | `read:src/**` |
| `write` / `edit` | `write:<glob>` | `write:docs/**` |
| `bash` | `bash:<command_prefix>` | `bash:rm -rf` |
| `fetch` | `fetch:<host>` | `fetch:api.openai.com` |
| `search` | always allow (read-only) | n/a |
| `agent` (subagent) | `agent:<dispatch_name>` | `agent:research-bug-cause` |
| `delete` / `move` | `mutate:<glob>` | `mutate:**` |

The "command prefix" for bash is the first ~3 tokens (`rm -rf`,
`npm install`, `cargo build`). Long commands shouldn't reuse the
same key — running `rm -rf node_modules` doesn't grant `rm -rf /`.
Prefer to gate on the *intent* (the verb + objects) and re-prompt on
unrelated commands. When in doubt, lean stricter.

## 2. The inline UI surface

The permission UI **always lives inside the tool card** that needs
permission. Never a modal, never a separate banner. This is the
single most important UX decision in the agent panel — modals break
flow, banners are easy to ignore.

```
╭─ ⚠ Bash ────────────────────────────────────────╮
│ $ rm -rf node_modules                            │
│                                                  │
│ This will delete files. Allow?                   │
│                                                  │
│ [Y] Allow   [N] Deny   [A] Always allow `rm -rf` │
╰──────────────────────────────────────────────────╯
```

Properties (recap from `07_tool_cards.md § 6`):

- Border: Round, color `border::warning` (120, 100, 50) — amber
- Header icon: `⚠` (U+26A0)
- Status: `ToolStatus::Confirmation`
- The card is **always expanded** in this state (the user needs to
  see what they're approving)
- Body has three sections, in order:
  1. The action description (`$ rm -rf node_modules`)
  2. A short prose prompt (`This will delete files. Allow?`)
  3. The key-hint chip row (`[Y] Allow [N] Deny [A] Always`)

### Embedding via `maya::Permission`

The `Permission` widget in maya is a self-contained card. We don't
use it as the *outer* card here — we want the card to be the **same
shell** as the tool card, so the user sees one continuous element
(not a tool card + a separate permission card stacked).

Use `Permission`'s key-hint logic only as the inner body:

```cpp
Element views::tool_card_with_permission(const ToolUse& tu, const Model& m) {
    using namespace maya::dsl;
    using moha::tokens;

    auto& pp = *tu.pending_permission;

    // Header (same as normal tool card)
    auto header = render_tool_header(tu);

    // Action / description body
    auto desc_lines = render_tool_action_description(tu);

    // Prompt sentence (one short line)
    auto prompt = text(format_permission_prompt(tu))
                | Style{}.with_fg(tokens::fg::muted);

    // Key hints
    auto hints = h(
        text("[Y] Allow") | Bold | Style{}.with_fg(tokens::status::success),
        text("   "),
        text("[N] Deny")  | Bold | Style{}.with_fg(tokens::status::error),
        text("   "),
        when(pp.allow_always_available,
             [&]{ return text("[A] Always allow ")
                       + text(pp.always_allow_label, Dim); })
    ).build();

    auto body = v(
        desc_lines,
        text(""),
        prompt,
        text(""),
        hints
    ).build();

    return (v(header, body)
        | border(BorderStyle::Round)
        | bcolor(tokens::border::warning)
        | btext(" \xe2\x9a\xa0 " + tu.name + " ",
                BorderTextPos::Top, BorderTextAlign::Start)
        | padding(0, 1, 0, 1)
    ).build();
}
```

`render_tool_action_description(tu)` produces the per-tool action
preview (`$ <command>`, `path: <path>`, `→ <url>`, etc.). Reuse the
same render helper as the normal tool card so the visual matches.

`format_permission_prompt(tu)` produces a single-sentence English
prompt — examples:

| Tool kind | Prompt |
|---|---|
| `bash` | `"This will run a shell command. Allow?"` |
| `bash` (destructive — has `rm`/`mv`/`>`) | `"This may delete or overwrite files. Allow?"` |
| `write` (new file) | `"Create this file?"` |
| `write` (overwrites) | `"Overwrite the existing file?"` |
| `edit` | `"Apply this edit?"` |
| `fetch` (external host) | `"Fetch from `<host>`?"` |
| `agent` | `"Dispatch the `<name>` subagent?"` |

Keep prompts short. The header + action description already say
what's happening; the prompt asks for the decision.

### `pending_permission` data

```cpp
struct PendingPermission {
    std::string id;                       // matches the ToolUse.id
    std::string gating_key;               // e.g., "bash:rm -rf"
    std::string always_allow_label;       // shown as "[A] Always allow `rm -rf`"
    bool allow_always_available = true;   // false for one-off / dangerous
    std::chrono::steady_clock::time_point requested_at;
};
```

`always_allow_label` is the human-friendly version of the gating
key. We **hide** "Always allow" for actions where it doesn't make
sense (e.g., a fetch to a URL the user typed once) — set
`allow_always_available = false`.

## 3. "Always allow" patterns

### Pattern semantics

Three granularities, each with a different `gating_key` shape:

| Granularity | Example key | Example label | When to offer |
|---|---|---|---|
| Per-tool | `tool:fetch` | `"Always allow `fetch`"` | Trusted environments only |
| Per-file-glob | `write:docs/**` | `"Always allow writes to `docs/**`"` | When the file path matches a clear scope |
| Per-command-prefix | `bash:cargo build` | `"Always allow `cargo build`"` | When the command starts with a safe verb |

Choose the granularity automatically based on tool kind + content:

```cpp
PendingPermission build_pending(const ToolUse& tu) {
    PendingPermission pp;
    pp.id = tu.id;
    switch (tu.kind) {
        case ToolKind::Read:
            pp.gating_key = "read:" + glob_for(tu.input["path"]);
            pp.always_allow_label = "Always allow reads to `"
                                  + glob_for(tu.input["path"]) + "`";
            break;
        case ToolKind::Write:
        case ToolKind::Edit:
            pp.gating_key = "write:" + glob_for(tu.input["path"]);
            pp.always_allow_label = "Always allow writes to `"
                                  + glob_for(tu.input["path"]) + "`";
            break;
        case ToolKind::Bash:
            pp.gating_key = "bash:" + command_prefix(tu.input["command"]);
            pp.always_allow_label = "Always allow `"
                                  + command_prefix(tu.input["command"]) + "`";
            // Hide always-allow for destructive verbs — they should always re-ask
            if (is_destructive(tu.input["command"]))
                pp.allow_always_available = false;
            break;
        case ToolKind::Fetch:
            pp.gating_key = "fetch:" + host_of(tu.input["url"]);
            pp.always_allow_label = "Always allow fetches from `"
                                  + host_of(tu.input["url"]) + "`";
            break;
        // ...
    }
    return pp;
}
```

Helpers:

```cpp
// "src/main.cpp" -> "src/**"
// "src/foo/bar.rs" -> "src/foo/**"
// (one level above the file)
std::string glob_for(std::string_view path);

// "rm -rf node_modules && npm i" -> "rm -rf"
// "cargo build --release" -> "cargo build"
// (first 1-3 tokens up to first arg)
std::string command_prefix(std::string_view cmd);

// "https://api.openai.com/v1/foo" -> "api.openai.com"
std::string host_of(std::string_view url);

// returns true if the command contains rm/mv/cp -f/dd/mkfs/etc.
bool is_destructive(std::string_view cmd);
```

### Granularity dropdown (advanced)

Users may want to choose. Press `G` while the permission card is
focused to open a dropdown:

```
╭─ ⚠ Bash ────────────────────────────────────────╮
│ $ cargo build --release                          │
│                                                  │
│ [Y] Allow   [N] Deny                             │
│   ↳ [A] Always allow:                            │
│       ▶ `cargo build`        (3-token prefix)    │
│         `cargo`              (1-token prefix)    │
│         all `bash` commands   (entire tool kind) │
│         this command exact    (no future allow)  │
╰──────────────────────────────────────────────────╯
```

`↑`/`↓` move highlight; `Enter` accepts; `Esc` collapses back to the
default `[A] Always allow` chip. The default highlight is the
3-token prefix (most common choice).

For the initial rebuild, **defer the granularity dropdown**. Just
offer the auto-chosen gating key. Add the dropdown if user feedback
shows people want finer control.

## 4. Permissions store

A persistent dictionary of `gating_key → AllowState`:

```cpp
namespace moha {

enum class AllowState {
    Unknown,         // never decided — ask
    AllowOnce,       // allowed for this prompt only (no persistent rule)
    Allow,           // persistently allowed
    Deny,            // persistently denied
};

struct PermissionStore {
    std::unordered_map<std::string, AllowState> rules;

    AllowState lookup(std::string_view gating_key) const;
    void set(std::string_view gating_key, AllowState s);
    void erase(std::string_view gating_key);
    void load_from_disk();
    void save_to_disk();
};

} // namespace moha
```

### Scopes

Three layered stores, looked up in order:

1. **Per-thread** (`m.current.permissions`) — cleared when starting
   a new thread. Ephemeral allows like `[Y]` (one-shot) live here.
2. **Per-workspace** (`~/.config/moha/permissions/<workspace_hash>.json`)
   — persistent across threads in this directory, cleared on
   `Reset`.
3. **Global** (`~/.config/moha/permissions/global.json`) — for keys
   the user wants to whitelist universally (rare).

`[A] Always allow` writes to the per-workspace store by default. To
write to global, add a key combo (`Shift+A` → "Always allow,
globally").

For the initial rebuild, **only implement per-thread + per-workspace**.
Global is overkill; add it later if asked.

### File format

```json
{
  "version": 1,
  "rules": [
    {"key": "read:src/**",      "state": "Allow"},
    {"key": "write:docs/**",    "state": "Allow"},
    {"key": "bash:cargo build", "state": "Allow"}
  ]
}
```

Sort by key on save so the file diffs cleanly under git.

### Lookup ordering

```cpp
AllowState lookup_layered(const Model& m, std::string_view key) {
    if (auto s = m.current.permissions.lookup(key); s != AllowState::Unknown)
        return s;
    if (auto s = m.workspace_permissions.lookup(key); s != AllowState::Unknown)
        return s;
    if (auto s = m.global_permissions.lookup(key); s != AllowState::Unknown)
        return s;
    return AllowState::Unknown;
}
```

The first non-Unknown answer wins. Per-thread denies override
workspace allows (a user can revoke for the current thread without
deleting the rule).

## 5. Profile defaults (Write / Ask / Minimal)

The profile sets the **default permission disposition**:

| Profile | Default for read tools | Default for write/bash | Default for fetch |
|---|---|---|---|
| `Write` | Auto-allow | Ask | Ask |
| `Ask` | Ask | Ask | Ask |
| `Minimal` | Auto-allow | Auto-deny (model must convince user via text) | Ask |

Implementation:

```cpp
AllowState profile_default(Profile p, ToolKind k) {
    switch (p) {
        case Profile::Write:
            if (k == ToolKind::Read || k == ToolKind::Search)
                return AllowState::Allow;
            return AllowState::Unknown;       // ask
        case Profile::Ask:
            return AllowState::Unknown;       // always ask
        case Profile::Minimal:
            if (k == ToolKind::Read || k == ToolKind::Search)
                return AllowState::Allow;
            if (k == ToolKind::Write || k == ToolKind::Edit
                || k == ToolKind::Bash)
                return AllowState::Deny;
            return AllowState::Unknown;
    }
}
```

The full lookup wraps this:

```cpp
AllowState resolve(const Model& m, const ToolUse& tu) {
    auto key = build_pending(tu).gating_key;
    if (auto s = lookup_layered(m, key); s != AllowState::Unknown)
        return s;
    return profile_default(m.composer.profile, tu.kind);
}
```

User-set rules always win over profile defaults. This way switching
to `Minimal` doesn't suddenly invalidate everything the user
explicitly allowed.

## 6. Key handlers

When focus is on a `Confirmation`-state tool card:

| Key | Action |
|---|---|
| `Y` or `A` (capital A also works for "Allow") | Allow once (per-thread store) |
| `N` or `D` | Deny once (per-thread store) |
| `Shift+A` | Always allow (per-workspace store) |
| `Shift+D` | Always deny (per-workspace store) |
| `G` | Open granularity dropdown (deferred) |
| `Esc` | Same as `N` (deny once) |
| `Enter` | Toggle expand/collapse the action description (long commands) |

Enforce focus: while any tool is in `Confirmation`, focus is locked
to that card (Tab navigation skipped). The composer is also disabled
(see `08_composer § 8` placeholder text).

If multiple tool calls in the same assistant turn need permission,
queue them — show one card at a time, in the order they appear.
Don't show all of them simultaneously; the user can't reason about a
batch of consents.

## 7. Update arms

```cpp
[&](PermissionDecision ev) -> std::pair<Model, Cmd<Msg>> {
    auto* tu = find_tool(m, ev.tool_id);
    if (!tu || tu->status != ToolStatus::Confirmation)
        return {m, Cmd<Msg>::none()};

    auto& pp = *tu->pending_permission;

    // Persist according to chosen granularity
    switch (ev.kind) {
        case Decision::AllowOnce:
            m.current.permissions.set(pp.gating_key, AllowState::AllowOnce);
            break;
        case Decision::Allow:
            m.workspace_permissions.set(pp.gating_key, AllowState::Allow);
            break;
        case Decision::DenyOnce:
            m.current.permissions.set(pp.gating_key, AllowState::Deny);
            break;
        case Decision::Deny:
            m.workspace_permissions.set(pp.gating_key, AllowState::Deny);
            break;
    }

    // Update tool status and continue
    if (ev.kind == Decision::Allow || ev.kind == Decision::AllowOnce) {
        tu->status = ToolStatus::Running;
        tu->pending_permission.reset();
        m.phase = Phase::ExecutingTool;
        return {m, Cmd<Msg>::batch({
            execute_tool_cmd(*tu),
            save_workspace_permissions_cmd(m.workspace_permissions),
        })};
    } else {
        tu->status = ToolStatus::Failed;
        tu->error_message = "Denied by user";
        tu->pending_permission.reset();
        // Restream so the model sees the denial as a tool result
        return {m, Cmd<Msg>::batch({
            send_tool_result_cmd(tu->id, "Denied by user"),
            save_workspace_permissions_cmd(m.workspace_permissions),
        })};
    }
},
```

After persisting, **continue the kick loop** (`04_architecture.md
§ kick_pending_tools`). The next pending tool either auto-resolves
(if a rule now matches) or surfaces the next permission card.

## 8. Edge cases

### Race: model emits multiple tools that all need permission

Surface them sequentially. Don't auto-batch. Each gets its own card,
its own decision. If the user is impatient, they can `[A] Always
allow` to skip future asks for the same key in the same scope.

### Race: user denies, model re-emits the same tool

Treat as a separate decision opportunity (the model may have
adjusted parameters). Don't suppress.

If the user denies the same gating-key twice in a row, *suggest*
"Always deny" via a dim hint:

```
[Y] Allow   [N] Deny   [Shift+D] Always deny `rm -rf`
```

This is a UX detail; defer for the initial rebuild.

### Permission card is up but the user types in composer

The composer is disabled in `Phase::AwaitingPermission` (see § 6).
Typing should beep + flash a hint:

```
"Decide on the permission card above first."
```

Show this as a transient `maya::widget::toast` triggered by
`KeyPress` in the composer when phase is `AwaitingPermission`.

### Permission card stays for hours

If the user steps away mid-prompt, the card just waits. No timeout.
Don't auto-deny on idle — the user may still come back. If the
underlying `Cmd` (the SSE connection) drops, the tool card transitions
to `Failed{network dropped}` and the permission goes away.

### Restoring a checkpoint with active permissions

When restoring, drop all pending permissions for tools that no longer
exist in the restored thread. Per-thread allows/denies are scoped to
the thread, so they survive restores naturally.

### Rule conflicts on save

Two `Allow always` decisions for overlapping globs (e.g.,
`write:src/**` then later `write:src/foo/**`) — keep both rules. The
*deeper* glob wins on lookup (longest-prefix-match). Implement
lookup with substring match + longest-key-wins:

```cpp
AllowState PermissionStore::lookup(std::string_view key) const {
    // Exact match
    if (auto it = rules.find(std::string{key}); it != rules.end())
        return it->second;
    // Glob fallback — pick the longest matching glob
    std::string best_match;
    AllowState best_state = AllowState::Unknown;
    for (auto& [k, s] : rules) {
        if (glob_match(k, key) && k.size() > best_match.size()) {
            best_match = k;
            best_state = s;
        }
    }
    return best_state;
}
```

`glob_match("src/**", "src/main.cpp:contents")` returns true.

## 9. Visual checklist

After implementing permissions, verify:

- [ ] Permission renders inline inside the tool card (same border)
- [ ] Border is amber (`border::warning`), Round style
- [ ] Header icon is `⚠`
- [ ] Action description shown above the prompt
- [ ] Prompt sentence is short and tool-appropriate
- [ ] Key hints render `[Y] Allow [N] Deny [A] Always allow ...`
- [ ] `[A]` chip hides when always-allow is unsafe (destructive bash)
- [ ] Pressing `Y` allows + executes; card transitions Round green ✓
- [ ] Pressing `N` denies; card transitions Dashed red ✗
- [ ] `Shift+A` persists to workspace store; rule survives restart
- [ ] Multiple pending permissions surface one card at a time
- [ ] Composer disabled while permission is pending (placeholder
      reflects this)
- [ ] Profile `Minimal` auto-denies write/bash without prompting
- [ ] Profile `Write` auto-allows reads without prompting
- [ ] Per-workspace store persists across restarts
- [ ] Per-thread denies override workspace allows
- [ ] No modal popup; no top-of-panel banner
