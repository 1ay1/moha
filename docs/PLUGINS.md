# Plugins

A plugin in agentty **is an MCP server**. That's the whole model — there
is no separate plugin API, no SDK to link, no manifest format, no ABI.
Anything that speaks the [Model Context Protocol](https://modelcontextprotocol.io)
over stdio is a plugin: a Python script, a Node package, a Go binary, a
shell wrapper — even another agentty (`agentty mcp-serve`). Whatever you
write also works unchanged in Claude Desktop, Zed, Cursor, and every
other MCP host, and every published MCP server on PyPI/npm works in
agentty.

## How it's integrated

At startup agentty reads its MCP config (first found wins):

1. `$AGENTTY_MCP_CONFIG` — explicit path
2. `./.agentty/mcp.json` — project-local *(gated, see below)*
3. `~/.agentty/mcp.json` — user-global

For each configured server it spawns the command as a child process,
performs the MCP `initialize` + `tools/list` handshake over the child's
stdin/stdout, and merges the advertised tools into the model's tool
catalog for the whole session:

- **Namespaced**: a plugin's tools appear as `mcp__<name>__<tool>`
  (`mcp__today__current_date`), so two plugins can both export `search`
  and you can always see where a capability came from.
- **First-class**: plugin tools go through the same permission prompts,
  the same tool cards in the TUI, the same cancellation (cancel kills the
  request; a dead server's tools error cleanly), and the same doom-loop
  accounting as built-in tools. The model cannot tell them apart.
- **Live**: a server that emits `tools/list_changed` gets its catalog
  re-pulled mid-session — plugins can grow tools at runtime.
- **Everywhere**: the TUI, `agentty run` (headless one-shot), the ACP
  agent mode, and subagents (`task`) all see the same merged catalog.
- **OAuth-ready**: servers that require authorization use the standard
  MCP OAuth flow — `agentty mcp-login <name>` / `mcp-logout` /
  `mcp-status`.

Config shape (Claude-Desktop-compatible; `env` vars are passed to the
child):

```json
{
  "mcpServers": {
    "today":  { "command": "python3", "args": ["/abs/path/today.py"] },
    "github": { "command": "mcp-server-github",
                "env": { "GITHUB_TOKEN": "…" } }
  }
}
```

### Passthrough tools (proxy-advertised)

Running agentty behind a compressing/augmenting proxy (LiteLLM + headroom,
an enterprise gateway) that **injects its own tool schemas** into your
requests? The model will call those tools, but a proxy cannot execute a
tool — only the client can. Declare them as a `passthrough` server and
agentty fulfils each call by POSTing its arguments to your URL and
returning the response body as the tool result:

```json
{
  "mcpServers": {
    "headroom": {
      "type": "passthrough",
      "url": "http://localhost:8787/v1/retrieve",
      "passthrough": [
        { "name": "headroom_retrieve",
          "description": "Fetch original bytes for a compressed marker" }
      ]
    }
  }
}
```

Inline add: press `a` in the Plugins pane (or `agentty plugin add`) with
`headroom --passthrough http://localhost:8787/v1/retrieve headroom_retrieve`.

Semantics worth knowing:

- **Dispatch-only by default.** The proxy already advertised the schema in
  your request; agentty registering it again would collide. `"advertise":
  true` on an entry flips it to a full first-class tool (agentty puts the
  schema on the wire too) — for plain HTTP tool services that aren't
  proxies.
- **"Allowing" one is a real decision**: passthrough tools carry the
  network effect, so under the Ask/Minimal profiles the standard permission
  prompt fires before a call's arguments leave the machine for the
  configured URL.
- **Project configs are trust-gated** exactly like stdio servers — a cloned
  repo must not silently route tool arguments to its own URL.
- Entries may be bare strings (`"passthrough": ["tool_a", "tool_b"]`) when
  no description is needed. Toggle/remove/disable behave like every other
  plugin row.

> **Trust gate**: the project-local config only connects when
> `AGENTTY_MCP_ALLOW_PROJECT=1` is set. A cloned repo must not be able to
> run code on your machine just because you opened it. Your user-global
> config has no gate — you wrote it.

**Why out-of-process, in one paragraph**: a plugin can't segfault the
TUI, can't read agentty's credentials or heap, is killed cleanly on
cancel, brings its own runtime (no Python-version or ABI fights), and on
Linux can be sandbox-wrapped like any child. Tool calls take seconds —
the model round-trip dominates — so stdio IPC (~1 ms) is noise. This is
a deliberate architecture decision, not a limitation.

## Managing plugins: `agentty plugin`

A thin, safe editor over the mcp.json — you never hand-write JSON unless
you want to:

```bash
agentty plugin add today --python tools/today.py     # local Python script
agentty plugin add fetch --uvx mcp-server-fetch      # PyPI package via uv
agentty plugin add fs    --npx @modelcontextprotocol/server-filesystem /tmp
agentty plugin add gomcp -- /usr/local/bin/my-go-server --flag x
agentty plugin list
agentty plugin remove fetch
```

- `--project` writes `./.agentty/mcp.json` instead of `~/.agentty/mcp.json`.
- `--force` overwrites an existing name (refused otherwise).
- The editor preserves everything it doesn't own (other servers, their
  `env` blocks, unknown keys), refuses to rewrite a file it can't parse,
  and writes atomically. Restart agentty to connect.

## The simplest possible plugin: today's date

Models don't reliably know today's date. Fix it with one file.

**Python** (`today.py` — needs `pip install mcp` or `uv add mcp`):

```python
from datetime import datetime, timezone
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("today")

@mcp.tool()
def current_date() -> str:
    """Today's date and time (UTC + local), for anything date-sensitive."""
    now_utc = datetime.now(timezone.utc)
    now_loc = datetime.now().astimezone()
    return (f"UTC:   {now_utc:%Y-%m-%d %H:%M:%S %Z}\n"
            f"Local: {now_loc:%Y-%m-%d %H:%M:%S %Z} ({now_loc:%A})")

mcp.run()
```

```bash
agentty plugin add today --python today.py
```

Restart agentty, ask *"what day is it?"* — the model calls
`mcp__today__current_date` instead of guessing. That's the entire
lifecycle: one file, one command, a new capability.

The same plugin in **Node**, to make the point that the protocol — not
the language — is the interface:

```js
// today.mjs  (npm i @modelcontextprotocol/sdk zod)
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

const server = new McpServer({ name: "today", version: "1.0.0" });
server.tool("current_date", "Today's date and time.", {}, async () => ({
  content: [{ type: "text", text: new Date().toString() }],
}));
await server.connect(new StdioServerTransport());
```

```bash
agentty plugin add today -- node /abs/path/today.mjs
```

Or no SDK at all — MCP is just JSON-RPC on stdio, so a static server fits
in a shell script; and `agentty mcp-serve` turns agentty itself into a
plugin for some *other* MCP host. The interface is symmetric.

## A compiled one: C++ with mcp-cpp

agentty's own MCP stack — [mcp-cpp](https://github.com/1ay1/mcp-cpp), the
header-only library its built-in tools are served with — works just as
well for writing plugins. A compiled plugin makes sense when the tool
wraps native code, needs real performance, or should ship as a single
static binary with zero runtime dependencies (no Python/Node on the
target machine).

The example: **sysmon**, live system stats — data a model cannot know
and `bash` answers only with flag-soup guessing.

`sysmon.cpp`:

```cpp
// sysmon — a tiny agentty plugin in C++ (mcp-cpp): live system stats.
#include <mcp/mcp.hpp>

#include <cstdio>
#include <iostream>
#include <string>

using namespace mcp;

// Run a shell command, capture stdout (bounded).
static std::string sh(const std::string& cmd) {
    std::string out;
    if (FILE* p = ::popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = ::fread(buf, 1, sizeof buf, p)) > 0 && out.size() < 30'000)
            out.append(buf, n);
        ::pclose(p);
    }
    return out.empty() ? "(no output)" : out;
}

int main() {
    StdioTransport transport(std::cin, std::cout);
    Server server(transport.sink(),
                  Implementation{"sysmon", "1.0.0",
                                 std::string("System Monitor"),
                                 Nothing, Nothing, Nothing});
    server.set_capabilities(ServerCapabilities{
        .tools = ToolsCapability{false},
    });

    {
        Tool t;
        t.name = "top_processes";
        t.description =
            "The busiest processes RIGHT NOW (CPU + memory), for questions "
            "like 'what is eating my CPU?'. Live data the model cannot know.";
        t.inputSchema.properties = Json{
            {"count", {{"type", "integer"},
                       {"description", "how many rows (default 10)"}}}};
        t.annotations = ToolAnnotations{};
        t.annotations->readOnlyHint = true;
        server.register_tool(std::move(t), [](const Json& a) -> CallToolResult {
            const int n = a.value("count", 10);
            CallToolResult r;
            r.content = {text(sh(
                "ps axo pid,pcpu,pmem,comm -r | head -" +
                std::to_string(n + 1)))};
            return r;
        });
    }
    {
        Tool t;
        t.name = "disk_usage";
        t.description = "Filesystem usage (df -h): free space per mount.";
        t.inputSchema.properties = Json::object();
        t.annotations = ToolAnnotations{};
        t.annotations->readOnlyHint = true;
        server.register_tool(std::move(t), [](const Json&) -> CallToolResult {
            CallToolResult r;
            r.content = {text(sh("df -h"))};
            return r;
        });
    }

    transport.start(server.engine());
    transport.join();   // serve until the host closes stdin
    return 0;
}
```

`CMakeLists.txt` — mcp-cpp is header-only, so the whole build setup is:

```cmake
cmake_minimum_required(VERSION 3.24)
project(sysmon CXX)
set(CMAKE_CXX_STANDARD 23)

include(FetchContent)
FetchContent_Declare(mcp
    GIT_REPOSITORY https://github.com/1ay1/mcp-cpp.git
    GIT_TAG        master)
FetchContent_MakeAvailable(mcp)

add_executable(sysmon sysmon.cpp)
target_link_libraries(sysmon PRIVATE mcp::mcp)
```

Build and install:

```bash
cmake -B build && cmake --build build -j
agentty plugin add sysmon -- $(pwd)/build/sysmon
```

Restart agentty and ask *"what's eating my CPU right now?"* — the model
calls `mcp__sysmon__top_processes` and reads live `ps` output.

The anatomy mirrors the Python version one-to-one: `Tool` spec =
docstring + type hints, `register_tool` lambda = the decorated function,
`transport.start(server.engine())` = `mcp.run()`. Same protocol, same
integration, different language — which is the whole point. (You can
sanity-check any stdio server by piping `initialize` / `tools/list`
JSON-RPC lines into it by hand; it's just newline-delimited JSON. See
`mcp-cpp/examples/server_example.cpp` for the full-featured reference —
resources, prompts, structured output.)

## A real one: the Git Time-Machine (Python example)

Something genuinely useful: **git archaeology**. Agents read code as it
is *now*; they're bad at "when did this break?", "what did this file look
like before the refactor?" — the pickaxe/blame workflows humans use
daily. Typed tools beat raw `bash` here because the model gets
discoverable, documented, argument-checked operations instead of
guessing flag soup.

```bash
mkdir git-time-machine && cd git-time-machine
uv init && uv add "mcp[cli]"
```

`server.py`:

```python
"""git-time-machine — git archaeology tools for coding agents."""

import subprocess
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("git-time-machine")

MAX_OUT = 30_000  # keep results context-friendly


def git(*args: str) -> str:
    """Run git, returning stdout (truncated) or a clear error string."""
    try:
        r = subprocess.run(["git", *args], capture_output=True,
                           text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return "error: git timed out after 30s"
    out = (r.stdout or r.stderr).strip()
    if len(out) > MAX_OUT:
        out = out[:MAX_OUT] + f"\n… [truncated at {MAX_OUT} chars]"
    return out if out else f"(no output, exit {r.returncode})"


@mcp.tool()
def when_did_this_appear(text: str, path: str = "") -> str:
    """Find the commits that ADDED or REMOVED an exact string (git pickaxe).

    The single best tool for "when did this bug/constant arrive". `text`
    is matched literally. Optionally scope to `path`. Newest first.
    """
    args = ["log", "-S", text, "--format=%h %ad %an  %s", "--date=short"]
    if path:
        args += ["--", path]
    return git(*args)


@mcp.tool()
def file_at_commit(path: str, ref: str) -> str:
    """Show a file EXACTLY as it existed at a ref ('HEAD~5', 'v1.2',
    'abc123'). Pair with when_did_this_appear: ref 'abc123~1' shows the
    world just before a change landed.
    """
    return git("show", f"{ref}:{path}")


@mcp.tool()
def blame_range(path: str, start_line: int, end_line: int) -> str:
    """Who last touched each line in [start_line, end_line], with commit
    and date. The precise "who wrote this and when" answer.
    """
    return git("blame", "-L", f"{start_line},{end_line}",
               "--date=short", "--", path)


@mcp.tool()
def what_changed_between(ref_a: str, ref_b: str, path: str = "") -> str:
    """Commits + per-file diffstat between two refs. Great for "what
    happened between v1.2 and v1.3".
    """
    scope = ["--", path] if path else []
    log = git("log", "--oneline", f"{ref_a}..{ref_b}", *scope)
    stat = git("diff", "--stat", f"{ref_a}..{ref_b}", *scope)
    return f"── commits ──\n{log}\n\n── diffstat ──\n{stat}"


if __name__ == "__main__":
    mcp.run()
```

```bash
agentty plugin add timemachine --python server.py
```

Now ask, in a git repo: *"when was the retry backoff constant changed,
and what did the file look like before that change?"* — and watch the
model compose `when_did_this_appear` → `file_at_commit(…, "<sha>~1")` on
its own, because the docstrings make the workflow discoverable.

### Lessons that generalize (any language)

- **Descriptions are for the model.** Say *when* to use the tool and
  what comes back, not just what it does — that's what makes workflows
  discoverable.
- **Typed parameters are the schema.** (`start_line: int` → a validated
  JSON-Schema field in Python; `zod` shapes in Node; struct tags in Go.)
- **Truncate outputs.** Everything you return lands in the model's
  context window; 30 KB with a marker beats a 2 MB dump.
- **Fail as text, not crashes**, so the model sees an actionable error
  and adapts.
- **Debug before involving an agent**: `uv run mcp dev server.py` opens
  the MCP Inspector for interactive tool calls; agentty logs spawn/
  connect diagnostics to stderr.

## Where plugins fit among agentty's extension surfaces

| Surface | What it is | When to use |
|---------|-----------|-------------|
| **plugin** (MCP) | new *tools* for the model | new capabilities: APIs, DBs, domain ops |
| skills | knowledge loaded on demand | teach workflows over existing tools |
| slash commands | user-typed prompt macros | reusable prompts (`/review …`) |
| agents | named `task` specialisations | reusable delegate roles |
| hooks | shell at tool lifecycle (consent-gated) | policy gates, auto-format, audit |

Rule of thumb: a **plugin** when the model should *call* something with
structured arguments; a **skill** when it should *learn* something; a
**hook** when it must happen *unconditionally*.
