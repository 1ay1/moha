---
title: Logging & diagnostics
description: One log, one switch. Leveled channels, raw wire bytes, a crash-time flight recorder — and everything captured by default in non-release builds.
nav_section: Advanced
nav_order: 55
slug: logging
---

When something misbehaves — a custom host that won't answer, a provider that
errors mid-stream, a model that "ignores tools" — agentty can tell you exactly
what happened.

There is **one log**, **one switch** (`AGENTTY_LOG`), and **one file**. Events
and raw wire bytes live in the same place, in order, so a byte-level question
and a behavioural one are answered from the same file.

:::tip You don't have to set anything up front
**Release builds keep warnings and errors by default** (a healthy session
writes about two lines), so if something goes wrong you can just run
`agentty diagnostics` — the failure was already captured. Raw wire bytes and
per-event detail still need `AGENTTY_LOG`.

**Non-release builds capture everything** at `trace`, no variable needed.

Either way the file is `~/.agentty/logs/agentty.log`.
:::

## Start here: what are you trying to find out?

| Symptom | Do this |
|---------|---------|
| **Anything at all just broke** | `agentty diagnostics` — no setup needed, the failure is already captured |
| Model "ignores tools", empty tool args | `AGENTTY_LOG=wire=trace agentty` — see the raw bytes |
| Custom host / local server won't answer | `grep 'http\.\|provider.select' ~/.agentty/logs/agentty.log` |
| Turn failed and you don't know why | `grep 'stream\.' ~/.agentty/logs/agentty.log` |
| A tool keeps failing | `grep 'tool.exec' ~/.agentty/logs/agentty.log` |
| Plugin tools missing | `grep 'mcp.connect' ~/.agentty/logs/agentty.log` |
| Signed out unexpectedly | `grep 'auth.refresh' ~/.agentty/logs/agentty.log` |
| Settings / history not persisting | `grep 'settings.save\|thread.save' ~/.agentty/logs/agentty.log` |
| Everything, maximum detail | `AGENTTY_LOG=trace agentty` |

## Quick start

```bash
# Nothing to configure — warnings and errors are already being kept:
agentty diagnostics

# Everything, to the default log file:
AGENTTY_LOG=trace agentty

# One subsystem wide open, quiet elsewhere:
AGENTTY_LOG=warn,wire=trace agentty

# The raw bytes a provider actually sent (see "Debugging a provider" below):
AGENTTY_LOG=wire=trace agentty
```

## `AGENTTY_LOG` — the filter

RUST_LOG-style: a default level plus optional `channel=level` overrides,
comma-separated.

```bash
AGENTTY_LOG=debug                    # every channel at debug and above
AGENTTY_LOG=wire=trace               # just the wire channel, at trace
AGENTTY_LOG=warn,wire=trace,auth=debug
                                     # default warn; wire and auth louder
AGENTTY_LOG=off                      # silence (including the default warnings)
```

**Levels** (low → high): `trace` · `debug` · `info` · `warn` · `error`. A
filter of `warn` passes `warn` and `error`; `off` silences a channel entirely.

**Channels** — one per subsystem:

| Channel | Covers |
|---------|--------|
| `wire` | HTTP/SSE transports, raw request/response **bytes** at `trace`, and how every turn ended (`stream.end`) |
| `auth` | OAuth flows, token refresh outcomes, key resolution, account switching |
| `persist` | settings / threads / memory disk I/O — including **failed saves**, which otherwise lose work silently |
| `tool` | every tool call: name, duration, outcome, and the **arguments that caused a failure** |
| `ui` | reducer decisions: stream errors as the user sees them, retry attempts and their backoff |
| `rag` | the retrieval engine |
| `mcp` | MCP bridge + plugins, including server **connect failures** (previously stderr-only, so invisible under the TUI) |
| `acp` | ACP server / adapter — every JSON-RPC frame at `trace` |
| `smart` | Smart Mode routing decisions |
| `net` | sockets, TLS, proxy, prewarm, and per-attempt **connect failures** with the endpoint tried |
| `model` | **provider/model heterogeneity**: which dialect adapter a turn routed through, what effort survived the capability clamp, capability facts learned from provider rejections, weak-model fallbacks, tool-call salvage |
| `general` | uncategorised (swallowed exceptions land here) |

### Where it writes

- `AGENTTY_LOG_FILE=<path>` sets the file explicitly.
- Otherwise: `~/.agentty/logs/agentty.log` (or `$AGENTTY_HOME/logs/`). So
  `AGENTTY_LOG=debug agentty` just works — no path needed.
- Append-only, rotated once at startup past **32 MB** (the previous log
  becomes `.old`), so an always-on log can't grow unboundedly.

### The line format

```
2026-08-28T01:23:45.678 +0012345ms 1a2b W wire    openai.stream: connect refused host=localhost:8080
└── wall clock ────────┘ └ mono ─┘ tid  L channel  site: message
```

- **wall clock** — human-readable, millisecond precision.
- **`+…ms`** — monotonic time since start, for reading event *pacing*.
- **`tid`** — short thread tag, so interleaved worker output is separable.
- **`L`** — level char (`T`/`D`/`I`/`W`/`E`).
- **channel** and **site** (`openai.stream`, `persistence.save`) locate the
  emitter.

One line per event, logfmt-ish and grep-first:

```bash
grep ' E ' ~/.agentty/logs/agentty.log       # every error
grep ' W \| E ' ~/.agentty/logs/agentty.log  # errors + warnings
grep ' wire ' ~/.agentty/logs/agentty.log    # everything on the wire
grep 'stream.end' ~/.agentty/logs/agentty.log | tail -20   # how turns ended
```

### The site tags worth knowing

Every event carries a `site` tag. These are the ones that answer real
questions — grep for the tag, not for prose:

| Site | Answers |
|------|---------|
| `startup` | Which version / OS / build is this? *(always present)* |
| `provider.select` | Which provider and endpoint was active? *(always present)* |
| `stream.end` | How did this turn end? `clean_close` · `cancelled` · `http_error` · `transport_error` · `already_terminated` |
| `stream.http_error` / `stream.transport_error` | The exact failure text the user saw |
| `stream.error` | How the reducer classified a failure (drives retry-vs-surface) |
| `stream.retry` | Which attempt, and how long the backoff was |
| `models.loaded` | How many models the catalog returned, and any fetch error |
| `dispatch.turn` | The **turn fingerprint**: route (dialect adapter), provider, model, effort, tool count, protocol flags — every heterogeneity decision, before any bytes hit the wire |
| `caps.effort_learned` | A capability fact learned from the provider's own rejection (persisted — changes future sessions) |
| `salvage.tool_call` / `salvage.dropped_*` | A tool call recovered from (or lost in) leaked content JSON — the "model ignores tools" signature |
| `responses.tool_args_unroutable` | Tool arguments arrived but couldn't be attached to a call — the upstream shape of every `invalid args` report on the Responses dialect |
| `copilot.models.*` / `copilot.auto_session.*` | Why Copilot fell back to the bundled catalog / lost its Auto session |
| `tool.exec` | Every tool call: name, duration, outcome, and the **arguments** on failure |
| `mcp.connect` | Did each plugin server connect, and how many tools did it advertise? |
| `auth.refresh` | Did the OAuth token refresh succeed? |
| `http.connect_failed` / `http.local_failed` | Which host:port refused, and why |
| `thread.save` / `settings.save` | Did your conversation / settings actually persist? |
| `rag.retrieve` | How many passages came back, through which pipeline |
| `*.request.body` / `wire.chunk` | Raw bytes (needs `wire=trace`) |

Reading `stream.end`:

```
W wire  stream.end: end=transport_error http=0 stop=unspecified replayable=true
```

- **`end`** — the classification. `transport_error` means the connection
  failed; `http_error` means the server answered with ≥ 400.
- **`http`** — the status code, or `0` if headers never arrived.
- **`stop`** — why the model stopped: `end_turn` · `tool_use` · `max_tokens` ·
  `stop_sequence` · `unspecified`.
- **`replayable`** — whether agentty may safely retry the request.

### Performance

Free when off. Each call site is gated by a single atomic load *before* its
message is formatted — a disabled statement costs about a nanosecond and
allocates nothing. When enabled, each event formats into a stack buffer and
lands as one atomic `write(2)`, no lock on the write path.

## Debugging a provider

This is the section to read when a model **"ignores tools"**, sends **empty
tool arguments**, or a turn fails in a way that looks like model quality.

Those symptoms are usually not the model. They are a wire event the parser
didn't decode. A real example: GitHub Copilot's endpoint delivers a tool call's
arguments in a `response.function_call_arguments.done` event, agentty only read
the incremental `.delta` events, and every tool call arrived empty — surfacing
as `Grep: invalid args, pattern required`. It reads like a weak model. It was
one missing event, and one look at the bytes would have shown it immediately.

```bash
AGENTTY_LOG=wire=trace agentty
```

At `trace` the `wire` channel carries the **full request body** and **every
response chunk, verbatim and untruncated**, each tagged with the dialect that
produced it:

| Dialect tag | Endpoint |
|-------------|----------|
| `openai-chat` | `/chat/completions` (OpenAI, Groq, Mistral, custom hosts, Copilot) |
| `openai-responses` | `/responses` (ChatGPT/Codex, some Copilot models) |
| `anthropic-messages` | `/v1/messages` |
| `ollama-native` | Ollama's `/api/chat` |

The dialect tag matters: **one account can serve some models on
`/chat/completions` and others on `/responses`**, so without it you're guessing
which decoder to suspect.

Bytes are never pretty-printed, re-encoded, or clipped — when the parser is the
thing under suspicion, a reformatted frame hides the evidence.

:::warn A trace contains your conversation
Prompts, file contents, and tool results all appear in the log — read a trace
before sharing it.

**Secrets are stripped automatically.** Every line passes one redaction step on
its way to disk, which replaces API keys, bearer tokens, OAuth codes and JWTs
with `<redacted>` while leaving the surrounding structure intact:

```
{"model":"gpt-5.6-luna","api_key":"<redacted>","stream":true}
```

Because it happens at the single point every event passes through, no
subsystem can bypass it. `agentty --version` and `agentty diagnostics` both
report how many secrets have been removed so far.
:::

## Worked examples

Four investigations, each from a bug that actually shipped.

### "The model ignores my tools" / `invalid args, pattern required`

Almost never the model. Nearly always a wire event the decoder didn't read —
the arguments arrive empty, so tools that *need* arguments fail while
zero-argument tools (`repo_map`, `list`) look fine. That asymmetry is the tell.

```bash
AGENTTY_LOG=wire=trace agentty
# reproduce, then:
grep 'tool.exec' ~/.agentty/logs/agentty.log
```

```
W tool  tool.exec: name=grep ms=0 ok=0 err=invalid args: pattern required args={}
```

`args={}` is the answer: the model *did* send arguments; we failed to decode
them. Now look at what the server actually sent:

```bash
grep 'wire.chunk\|request.body' ~/.agentty/logs/agentty.log | grep -i 'arguments'
```

### "It just hangs for a second"

Look at the monotonic column — that's what it's for. A gap between two
adjacent events *is* the stall:

```bash
grep ' wire \| net ' ~/.agentty/logs/agentty.log | tail -40
```

```
W net  http.connect_failed: host=api.example.com:443 attempt=1/3 err=[connect] timeout
```

`attempt=1/3` shows the retry ladder; the `+…ms` deltas show what each attempt
cost.

### "My custom host returns no models"

```bash
grep 'provider.select\|models.loaded\|http\.' ~/.agentty/logs/agentty.log
```

```
W wire  provider.select: provider=localhost:1 kind=openai endpoint=localhost/v1/chat/completions
W net   http.local_failed: host=localhost:1 err=[connect] connect: poll hangup
```

`provider.select` shows the **endpoint agentty actually dialled** — usually
enough on its own, because the common causes are a missing `/v1` or a port
typo, both visible right there.

### "A plugin's tools are missing"

```bash
grep 'mcp.connect' ~/.agentty/logs/agentty.log
```

```
W mcp  mcp.connect: server=date result=spawn_failed
I mcp  mcp.connect: server=wordcount result=ok tools=2 resources=0 prompts=0
```

A failed server used to be indistinguishable from one you never configured —
its stderr is swallowed by the TUI.

## The flight recorder

Even with file logging **off**, agentty keeps the last ~256 significant events
(`warn` and above) in an in-process ring. On a crash (SIGSEGV / SIGABRT) the
handler dumps that ring to stderr right after the backtrace:

```
=== agentty: SIGSEGV (segmentation fault) ===
  <backtrace frames>
=== agentty flight recorder (last events, oldest first) ===
2026-08-28T01:23:44.101 +0012310ms 1a2b W wire  openai.stream: retry 3/6 …
2026-08-28T01:23:45.678 +0012345ms 1a2b E persist save_thread: disk full
==================
```

Every crash report ships with *what was happening right before it*, at
essentially zero steady-state cost. Capture it with `agentty 2> crash.log`.

## Legacy variable

`AGENTTY_DEBUG_LOG=<path>` (the older single-file debug var) still works: it
sets the log file *and* implies `AGENTTY_LOG=debug` when `AGENTTY_LOG` is
unset. Existing scripts keep working; new setups should prefer `AGENTTY_LOG`.

Retired in favour of the single log: `AGENTTY_DEBUG_API`, `AGENTTY_DEBUG_FILE`,
and `AGENTTY_ACP_TRACE`. Their output now lands on the `wire` and `acp`
channels above.

## Debugging model heterogeneity

The hardest bug class agentty deals with is *the same model behaving
differently on different providers* — tool grammars disabled server-side,
effort enums that differ per host, reasoning summaries that one account tier
emits and another doesn't. There is no clean abstraction over this (every
provider gets a thin adapter), so the log is designed to make the adapter's
decisions **inspectable** instead.

Every turn opens with a fingerprint on the `model` channel:

```
D model dispatch.turn: route=copilot provider=copilot model=sol-5.6 effort=low
        tools=24 json_protocol=0 show_reasoning=1 ctx_window=128000
        max_tokens=16384 retry=0
```

and the wire closes with a result line (`responses.result`,
`openai.result`, `anthropic.response`) carrying the HTTP status, transport
verdict, and `thinking_deltas`. Between those two brackets, every
heterogeneity event names itself:

| You observe | The log says | Meaning |
|---|---|---|
| "model ignores tools" | `salvage.tool_call` | model leaked tool JSON into content; agentty recovered it — the model's native tool grammar is broken/disabled on this host |
| "tool failed: invalid args" | `responses.tool_args_unroutable` | the args arrived on the wire but couldn't be attached — an adapter bug, report it |
| "tool failed: invalid args" | *(no unroutable line, `tool.exec` shows empty args)* | the model never sent arguments — a model bug |
| "reasoning not showing" | `…result: … thinking_deltas=0` | the server never sent reasoning text (model/tier) |
| "reasoning not showing" | `…result: … thinking_deltas=214` | reasoning arrived; the loss is client-side — report it |
| "effort setting seems ignored" | `caps.effort_learned: key=… effort high -> medium` | the provider rejected that effort once; agentty learned and clamped it (persisted) |
| "model went mute mid-turn" | `*.frame_unparseable` / `*.unknown_event` | the provider sent a frame we couldn't parse or don't know — report it with the log |
| "no models in the picker" | `copilot.models.fallback: reason=chat_disabled` | org policy / entitlement, not an agentty bug |

To capture all of it: `AGENTTY_LOG=model=debug,wire=debug`. The salvage,
learned-caps, and fallback events are `warn`+ — **captured by default in
every release build**, so a plain `agentty diagnostics` usually already
contains the answer.

## Cost: why this is free in release builds

Every `AGT_LOG` statement compiles to a single relaxed atomic load and an
integer compare *before any argument is evaluated*. When the (channel, level)
pair is below threshold — the release default is `warn` — the statement costs
~1 ns and **formats nothing**: no string construction, no allocation, no
syscall. The format string is `std::format` compile-time checked, so a
malformed site is a build error, not a runtime surprise.

What release users pay for by default is only the `warn`+ events — rare by
construction (failures, learned facts, dropped frames) — written with one
`write(2)` to an `O_APPEND` fd and mirrored into the fixed in-process crash
ring. Per-token hot paths (`wire.chunk`, `anthropic.event`) are `trace` and
completely dormant unless explicitly enabled. Debug builds default to `trace`
everything — which is also the statement's correctness test: if a log site is
too hot for trace-in-dev, it's too hot, period.

## For contributors: the log-site discipline

When adding or touching provider/model code, the invariant is:
**no silently-dropped wire data, no silently-degraded capability.** Concretely:

1. **Dropping a frame/event you can't parse?** `Warn` on `Wire` with the
   error and the head bytes (`*.frame_unparseable`).
2. **Ignoring an event type you don't know?** `Debug` on `Wire` naming it
   (`*.unknown_event`) — unknown events are usually fine, invisible ones are
   how the Copilot `.done` bug cost weeks.
3. **Making a capability decision** (clamping effort, disabling tools,
   switching dialect, engaging salvage)? `Debug` on `Model` — or `Warn` if
   the decision is *learned/persisted* (`caps.effort_learned`).
4. **Falling back** (bundled catalog, degraded session, negative cache)?
   `Warn` naming the reason — a fallback with no reason line is a future
   support ticket.
5. **Catching an exception you intend to swallow?** `util::dbglog(where, e.what())`
   — keeps recovery behaviour, stops throwing the *what happened* away. Site
   prefixes route to channels automatically (`caps.`/`salvage.`/`dispatch.` →
   `model`, `oauth.`/`token.` → `auth`, … see `src/util/dbglog.cpp`).

Level rubric: `Error` = broken invariant · `Warn` = failure or persisted
behaviour change (on by default in release — the bug-report tier) · `Info` =
notable-but-healthy · `Debug` = per-decision detail · `Trace` = per-event/
per-byte. Never log secrets — redaction is a safety net, not a licence.

## For developers: the always-on capture workflow

If you're hacking on agentty itself, the loop you want is *logs always
running, so when a bug happens the evidence already exists*. That's the
default: **a non-release build (the `dev` preset) captures `trace` on every
channel with no env var at all.** Just run your build:

```bash
./build/agentty
```

Everything — wire bytes, `dispatch.turn` fingerprints, tool exec, salvage
events — appends to `~/.agentty/logs/agentty.log`. The file is opened
`O_APPEND`, so concurrent sessions interleave line-atomically and a crash
never loses the tail.

Every run writes a **session banner** on startup:

```
=== agentty session: 0.5.0 debug pid=76883 cwd=/Users/you/projects/x ===
```

so a multi-day append-mode log splits into runs with
`grep -n "=== agentty"`, and any shared capture names the exact binary
(version + build type) that produced it.

**Mark the moment you see a bug.** Send `SIGUSR1` and agentty stamps the
live log — a `=== MARK ===` banner plus a flight-recorder snapshot (the
last ~256 events) — at the exact observation point, no TUI interaction:

```bash
kill -USR1 $(pgrep agentty)
```

Then find it with `grep -n "MARK (SIGUSR1)" ~/.agentty/logs/agentty.log`.
The handler is async-signal-safe (preformatted bytes, raw writes), so it's
safe to fire at any moment, including mid-stream.

**When you hit a bug**, snapshot the tail before it drowns in later trace
noise:

```bash
tail -c 512k ~/.agentty/logs/agentty.log > ~/bug-$(date +%H%M%S).log
```

Or watch live in a second pane while reproducing — warnings/errors plus the
per-turn fingerprint, without the token-level firehose:

```bash
tail -f ~/.agentty/logs/agentty.log | grep --line-buffered 'W \|E \|dispatch.turn'
```

Worth knowing:

- **Rotation is automatic.** The sink rotates at 32 MB — both at startup
  and mid-run — renaming the current file to `agentty.log.old` and starting
  fresh, so an always-on trace session never grows one file unbounded. You
  keep at most ~64 MB (`agentty.log` + `.old`). Manual truncation is still
  safe while agentty runs (`: > ~/.agentty/logs/agentty.log`) thanks to
  `O_APPEND`.

- **Multiple instances?** Give each its own file so streams don't
  interleave:

  ```bash
  AGENTTY_LOG_FILE=/tmp/agentty-$$.log ./build/agentty
  ```

Suggested muscle memory — the whole workflow is: see bug → mark → snapshot
→ keep working:

```bash
alias agl='tail -f ~/.agentty/logs/agentty.log'
alias agmark='kill -USR1 $(pgrep -n agentty)'   # stamp the log NOW
agbug() { tail -c 512k ~/.agentty/logs/agentty.log \
          > ~/agentty-bugs/bug-$(date +%m%d-%H%M%S).log; echo saved; }
```

Each capture is self-contained: the `dispatch.turn` line at the top of the
affected turn says route/model/effort, the `*.result` / `*.error.body` lines
say how it ended — the same triage flow as a user's `agentty diagnostics`,
just at trace granularity.


## Reporting a bug

### `agentty diagnostics`

One command, one file, safe to attach:

```bash
agentty diagnostics
```

It writes `~/.agentty/logs/agentty-diagnostics.txt` containing your version,
OS, architecture and build type; the session's `startup` and `provider.select`
lines (read back from the log, so they describe the run that actually failed,
not the collector process); and the tail of the log itself.

**This works with no prior setup.** Warnings and errors are recorded by
default, so the failure you just hit is already in the file — you do not have
to reproduce it under a special flag first. Add `AGENTTY_LOG=wire=trace` only
when the answer needs the raw provider bytes.

It is **plain text on purpose** — no archive to unpack, and you can read the
whole thing before you send it. Secrets are already stripped when each line is
written (see below), and the command prints how many were removed.

If logging was off when you hit the problem, the command says so and tells you
what to re-run:

```bash
AGENTTY_LOG=trace agentty      # reproduce the problem
agentty diagnostics            # then collect
```

### Manual collection

```bash
AGENTTY_LOG=trace AGENTTY_LOG_FILE=/tmp/agentty.log agentty
# reproduce, then attach /tmp/agentty.log
```

For a provider or custom-host problem, `AGENTTY_LOG=wire=trace` is the one that
matters — it captures what the server actually sent, which is almost always
where the answer is. If agentty crashed, include the stderr output (backtrace +
flight recorder).
