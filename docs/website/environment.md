---
title: Environment variables
description: Every environment variable agentty reads — what it does, what it defaults to, and when you'd actually reach for it.
nav_section: Advanced
nav_order: 56
slug: environment
---

Every environment variable agentty reads, grouped by what it affects. Nothing
here is required: agentty runs with none of them set. Reach for one when you
need to override a default, debug a problem, or script a headless run.

Persistent preferences belong in
[settings](/docs/configuration) (the config overlay, [[Ctrl+S]]) —
environment variables are for **per-invocation** overrides and diagnostics.

:::tip Debugging something right now?
Start with [`AGENTTY_LOG`](#diagnostics). In a non-release build everything is
already logged to `~/.agentty/logs/agentty.log` with no variable set at all.
:::

## Diagnostics

One log, one switch. See **[Logging & diagnostics](/docs/logging)** for the
channel list and line format.

| Variable | Effect |
|----------|--------|
| `AGENTTY_LOG` | The diagnostic filter. `trace` · `debug` · `info` · `warn` · `error` · `off`, plus `channel=level` overrides (`warn,wire=trace`). **Release builds default to `warn`** (~2 lines per healthy session) so `agentty diagnostics` works with no setup; non-release builds default to everything. |
| `AGENTTY_LOG_FILE` | Log destination. Default `~/.agentty/logs/agentty.log`. |
| `AGENTTY_DEBUG_LOG` | Legacy: sets the file *and* implies `AGENTTY_LOG=debug`. Prefer the two above. |
| `AGENTTY_TRACE_TOOLS` | `=1` emits one `TOOL <name> <ok\|error>` line to **stderr** per tool a headless `agentty run` executes. stdout stays clean, so it's safe to pipe. |
| `AGENTTY_RAG_TRACE` | Fold rag-cpp's per-stage trace into the retrieval `mode` string shown on the tool card. On by default; `=0` disables. |
| `AGENTTY_NO_STDERR_REDIRECT` | Keep subsystem stderr (MCP servers, sandbox) on the terminal instead of capturing it. Debug-only — it will scribble over the TUI. |

## Providers & models

| Variable | Effect |
|----------|--------|
| `ANTHROPIC_API_KEY` · `OPENAI_API_KEY` · `GROQ_API_KEY` · `OPENROUTER_API_KEY` · `TOGETHER_API_KEY` · `CEREBRAS_API_KEY` · `DEEPSEEK_API_KEY` · `XAI_API_KEY` · `MISTRAL_API_KEY` · `GEMINI_API_KEY` / `GOOGLE_API_KEY` · `FIREWORKS_API_KEY` | Per-provider API keys. Each provider also falls back to `OPENAI_API_KEY` where its wire is OpenAI-compatible. Keys saved via `agentty login` take precedence. |
| `CLAUDE_CODE_OAUTH_TOKEN` | Use an existing Claude Code OAuth token instead of signing in. |
| `AGENTTY_API_HOST` / `AGENTTY_OAUTH_HOST` | Point the Anthropic wire at a different API / OAuth host (gateways, enterprise proxies). |
| `AGENTTY_OLLAMA_HOST` | Ollama base URL. Default `http://localhost:11434`. |
| `AGENTTY_OLLAMA_NUM_CTX` · `AGENTTY_OLLAMA_NUM_PREDICT` · `AGENTTY_OLLAMA_TEMPERATURE` | Override Ollama's context window, output cap, and sampling temperature. Otherwise agentty picks per model. |
| `AGENTTY_MAX_OUTPUT_TOKENS` | Cap `max_tokens` on every request (mirrors Claude Code's knob). Useful against a provider that rejects large output budgets. |
| `AGENTTY_MAX_SKILLS` | Override the tier-1 skill catalog cap (default 64, clamped to 8–4096). Lower it for small-context models, raise it for large skill libraries. |
| `AGENTTY_FORCE_EFFORT` | Force a reasoning-effort tier for every turn, overriding what the model's capabilities imply. For A/B-ing effort, not everyday use. |
| `AGENTTY_CHATGPT_DEVICE_AUTH` | Use the device-code flow for ChatGPT sign-in instead of the loopback browser flow — for headless or remote machines. |
| `AGENTTY_NO_PREWARM` | Skip the TLS/connection prewarm at startup. |
| `AGENTTY_POOL_IDLE_TTL` | Seconds an idle HTTP/2 connection may be reused (default 90, or 450 under a SOCKS proxy; `0` disables pooling). **Lower this if you see `h2 socket hangup (server accepted request; not replayed automatically)` mid-answer** — that means a proxy/VPN/CDN killed the connection before agentty did. Set it below your network's idle timeout and the pool evicts first, turning a visible failure into an invisible re-dial. `grep http.stream_hangup` in the log shows each hangup's idle age, so the right value is measurable. |
| `AGENTTY_FORCE_REMOTE` | Force the remote streaming cadence even on a local endpoint. Takes precedence over autodetection. |

## Smart Mode

Off by default and a byte-for-byte no-op when off — see
**[Smart Mode](/docs/smart-mode)**.

| Variable | Effect |
|----------|--------|
| `AGENTTY_SMART_MODE` | Session pin for the master switch: `1` on, `0` off, for this process only. Never persisted (safe for CI). |
| `AGENTTY_SMART_NO_INTERNAL` | Keep engine-internal calls (compaction summary, thread title) on the main model instead of the Utility slot. |
| `AGENTTY_SMART_NO_SUBAGENTS` · `AGENTTY_SMART_NO_ORCHESTRATE` | Disable subagent delegation / orchestration independently. For bisecting a routing problem. |
| `AGENTTY_SMART_COMPLEX_THRESHOLD` · `AGENTTY_SMART_DEEP_MARGIN` · `AGENTTY_SMART_BIAS_CLAMP` | Router tuning: complexity cut-off, deep-work margin, and the clamp on learned bias. Defaults are measured; change them only with a benchmark. |

## Retrieval (RAG)

Full detail in **[Retrieval](/docs/retrieval)**. Most default sensibly; the
common ones are `AGENTTY_DOCS_DIR` and `AGENTTY_EMBED_MODEL`.

| Variable | Effect |
|----------|--------|
| `AGENTTY_DOCS_DIR` | Directory to index as your knowledge base. |
| `AGENTTY_EMBED_MODEL` | Embedding model (via Ollama). |
| `AGENTTY_RAG_PERSIST` | Cache the built index to `.agentty/rag_docs.ragdb` so later sessions open warm. On by default; `=0` disables. |
| `AGENTTY_RAG_LEARN` | Fold each passage's win-rate back into ranking. On by default; `=0` disables. |
| `AGENTTY_RAG_PROACTIVE` · `AGENTTY_RAG_PROACTIVE_MIN` · `AGENTTY_RAG_PROACTIVE_BYTES` | Pre-turn auto-retrieval and its thresholds. Off by default. |
| `AGENTTY_RAG_EXPAND` · `AGENTTY_RAG_HYDE` · `AGENTTY_RAG_GEN_MODEL` | LLM-assisted recall (multi-query, HyDE) via a small local model. Opt-in. |
| `AGENTTY_RAG_BM25_WEIGHT` · `AGENTTY_RAG_DENSE_WEIGHT` · `AGENTTY_RAG_FUSION` · `AGENTTY_RAG_ADAPTIVE` | Hybrid-search weighting and the fusion strategy. `_ADAPTIVE` (on) picks weights per query. |
| `AGENTTY_RAG_MMR` · `AGENTTY_RAG_MMR_LAMBDA` · `AGENTTY_RAG_DEDUP` · `AGENTTY_RAG_DEDUP_THRESHOLD` | Diversity and near-duplicate suppression in results. Both on by default. |
| `AGENTTY_RAG_AUTOCUT` · `AGENTTY_RAG_AUTOCUT_SENSITIVITY` · `AGENTTY_RAG_RELEVANCE_FLOOR` | Drop weak tail results automatically. On by default. |
| `AGENTTY_RAG_CONTEXTUAL` · `AGENTTY_RAG_EXTRACTIVE` · `AGENTTY_RAG_STITCH` · `AGENTTY_RAG_CORRECT` · `AGENTTY_RAG_PRF` · `AGENTTY_RAG_GRAPH` | Individual retrieval stages (contextual headers, extractive compression, chunk stitching, correction, pseudo-relevance feedback, graph expansion). |
| `AGENTTY_RAG_MEMORY` · `AGENTTY_RAG_SKILLS` · `AGENTTY_RAG_MCP` | Include learned memory / skills / MCP resources in the index. |
| `AGENTTY_RAG_OUTPUT_BYTES` · `AGENTTY_RAG_BUDGET_GAMMA` · `AGENTTY_RAG_CONF_BUDGET_FLOOR` | Output size budget and how it scales with confidence. |
| `AGENTTY_RAG_MEASURE` | Emit retrieval quality metrics — for benchmarking. |

## Tools, MCP & ACP

| Variable | Effect |
|----------|--------|
| `AGENTTY_MCP_CONFIG` | Path to the MCP server config. Otherwise `~/.agentty` then `./.agentty`. |
| `AGENTTY_MCP_ALLOW_PROJECT` | Allow a project-local `.agentty/mcp.json` to add servers. Off by default — a repo you clone should not silently gain tool servers. |
| `AGENTTY_MCP_TIMEOUT_MS` · `AGENTTY_MCP_CONNECT_TIMEOUT_MS` | Per-call and initial-connect timeouts for MCP servers. |
| `AGENTTY_MCP_CLIENT_ID` | OAuth client id for MCP servers that require one. |
| `AGENTTY_ACP_AGENTS` | Path to the ACP agent config (same precedence chain as MCP). |
| `AGENTTY_ACP_ALLOW_PROJECT` | Allow a project-local ACP agent config. Off by default, same reasoning as MCP. |
| `AGENTTY_NO_HOOKS` | Disable all lifecycle hooks for this run. |
| `AGENTTY_HOOK_EVENT` · `AGENTTY_HOOK_TOOL` · `AGENTTY_HOOK_PAYLOAD_FILE` | **Set by agentty for your hook**, not by you — the event name, tool name, and a file holding the JSON payload. See **[Hooks](/docs/hooks)**. |

## Security & storage

| Variable | Effect |
|----------|--------|
| `AGENTTY_HOME` | Root for all agentty state (credentials, threads, settings, logs). Default `~/.agentty`. Point it at a scratch dir to sandbox a session. |
| `AGENTTY_ENCRYPT_PASSPHRASE` / `AGENTTY_PASSPHRASE` | Passphrase for the encrypted credential store — for headless machines where no prompt is possible. |
| `AGENTTY_USE_KEYSTORE` | Use the OS keychain for credentials instead of the file store. |
| `AGENTTY_KDF` | Key-derivation parameters for the credential store. Change only if you know why. |
| `AGENTTY_TLS_PINS` | Pin TLS certificates for provider hosts. |
| `AGENTTY_INSECURE` | **Disable TLS verification.** For debugging a proxy with a self-signed cert. Never leave it set. |
| `AGENTTY_SOCKS_PROXY` | Route provider traffic through a SOCKS proxy. See **[Proxies](/docs/proxies)**. |
| `SSL_CERT_FILE` · `SSL_CERT_DIR` · `CURL_CA_BUNDLE` | Standard OpenSSL root-store overrides. |
| `AGENTTY_AIRGAP_SSH` | SSH target for airgap mode. See **[Airgap](/docs/airgap)**. |
| `AGENTTY_NO_SSH_THROTTLE` | Skip render throttling over SSH. A fast LAN hop doesn't need it. |
| `AGENTTY_NO_UPDATE_CHECK` | Disable the background update check (also implied by airgap mode). |

## Interface

| Variable | Effect |
|----------|--------|
| `AGENTTY_GRID` | Grid-protocol rendering for cooperating hosts. `=0` opts out. (`AGENTTY_EMACS_GRID` is the legacy name.) |
| `AGENTTY_HOST` | Declare the cooperating host explicitly rather than autodetecting. |
| `AGENTTY_CLIPBOARD_CMD` | Command to pipe clipboard writes through (e.g. `pbcopy`, `wl-copy`) when autodetection fails. |
| `AGENTTY_PAINTED_CARET` | `=1` draws the caret manually — for terminals with broken cursor-visibility handling. |
| `AGENTTY_REVEAL` | Master switch for the streaming reveal effect. **On by default everywhere, tmux included**; `0`/`false`/`off`/`no` turns it off, anything else forces it on. Set it to `0` if your terminal/multiplexer combination ghosts the live tail. |
| `AGENTTY_REVEAL_TYPEWRITER` | The typewriter clip alone — text arriving character-by-character. ANDed with `AGENTTY_REVEAL`; same default and same truthiness rule. |
| `AGENTTY_REVEAL_DECORATE` | The decorative overlay on the revealing tail alone. ANDed with `AGENTTY_REVEAL`; same default and same truthiness rule. |
| `AGENTTY_NO_REVEAL_GLIDE` | `=1` disables the bounded **end-of-turn glide** (the visible catch-up when a turn finishes) and restores the immediate finish. Not the reveal animation itself — that's `AGENTTY_REVEAL`. The glide is otherwise on only where frames are dense: not over SSH, and only with synchronized-output support. |
| `AGENTTY_NO_TAPE` | `=1` replaces the animated activity tape with a quiet static row carrying the same elapsed / tok-s detail. |
| `AGENTTY_FROZEN_COLLAPSE` | `=1` opts **in** to collapsing frozen turns. Off by default. |
| `AGENTTY_NO_TRANSFORMS` | Disable output transforms. |

## Development & testing

Not for normal use. Listed so a reader of the source isn't left guessing.

| Variable | Effect |
|----------|--------|
| `AGENTTY_CACHE_PROF` · `AGENTTY_LOAD_PROF` · `AGENTTY_STREAM_PROF` · `AGENTTY_VIEW_PROF` | Profiling counters for the render cache, thread load, streaming, and view paths. |
| `AGENTTY_STRICT_TEST_ROOT` · `AGENTTY_TEST_FAKE_PASSWD_HOME` | Test-harness isolation: enforce a sandboxed user root and fake the passwd home lookup. |
| `AGENTTY_UNDER_TEST` | Set by the test mains before anything renders: arms the user-root tripwire so a test can never touch the real `~/.agentty`. |

## Variables agentty *reads* from your environment

Standard variables consulted for platform behaviour, listed for completeness:
`HOME` / `USERPROFILE` / `HOMEDRIVE` + `HOMEPATH` · `XDG_CONFIG_HOME` ·
`APPDATA` · `PATH` · `USER` / `USERNAME` · `SUDO_USER` · `HOSTNAME` ·
`COLUMNS` / `LINES` · `DISPLAY` / `WAYLAND_DISPLAY` / `XDG_SESSION_TYPE` ·
`SSH_CONNECTION` / `SSH_CLIENT` / `SSH_TTY` / `SSH_AUTH_SOCK` ·
`MOSH_CONNECTION` / `MOSH_KEY` / `MOSH_SERVER_PID` · `INSIDE_EMACS`.
