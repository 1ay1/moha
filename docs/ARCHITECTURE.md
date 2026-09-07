# agentty — Architecture

A field guide to how the binary is put together. This is a map for someone
about to change the code, not marketing. The single source of truth is always
the code itself; where this doc and the code disagree, the code wins.

---

## 1. The shape of the program

agentty is an Elm-style application. The entire runtime is one pure function
applied in a loop:

```
(Model, Msg) -> (Model, Cmd<Msg>)
```

- **Model** is the whole application state — one aggregate struct.
- **Msg** is a closed sum type of every event that can happen.
- **Cmd** is a description of side effects to run (network, disk, timers); the
  runtime executes them and feeds their results back as new `Msg`s.

Rendering is a second pure function, `view : Model -> Element`, delegated to
**maya** — a sister TUI engine pulled in as a git submodule. The host never
constructs chrome glyphs or makes layout decisions; it builds widget *Config*
values from `Model` state and maya owns every pixel, border, and animation.

The four maya `Program` hooks are bound in
`include/agentty/runtime/app/program.hpp`:

| Hook           | Meaning                                         |
|----------------|-------------------------------------------------|
| `init`         | Load settings + recent threads via Store seam.  |
| `update`       | The reducer — `src/runtime/app/update.cpp`.     |
| `view`         | `Model -> Element`.                             |
| `subscribe`    | Timers and the live stream subscription.        |
| `visual_hash`  | Render-skip gate; identical hash → skip frame.  |
| `needs_warmup` | One-shot fast scrollback rehydration on resume. |

`main.cpp` is wiring only: parse argv, resolve credentials, construct the
concrete `AnthropicProvider` + `FsStore`, install them behind the `Deps` seam,
then hand `AgenttyApp` to `maya::run`.

---

## 2. Directory layout

`include/agentty/` and `src/` mirror each other by domain. Headers carry the
types and inline logic; `src/` carries the heavier implementations.

- **`domain/`** — pure data, no I/O. `session`, `conversation`, `catalog`,
  `todo`, `profile`, and the strong-id newtypes in `id.hpp` (`ToolCallId`,
  `ThreadId`, `OAuthCode`, `PkceVerifier`). Swapping two ids of different
  newtype is a compile error, not a debugging session.
- **`runtime/`** — the application proper.
  - `model.hpp` — the composed `Model` plus UI-only sub-states (composer,
    the panel slot, modals) that belong to no domain.
  - `panel/` — the panel subsystem: the exclusive slot, priority routing,
    per-panel state, forms. See [`PANELS.md`](PANELS.md).
  - `msg.hpp` — the `Msg` sum, split into domain sub-variants (see §4).
  - `app/update/<domain>.cpp` — per-domain reducers.
  - `view/` — the `Model -> Element` pipeline, one file per widget family;
    every panel view under `view/panels/`.
- **`provider/`** — the `Provider` concept and its implementations:
  `anthropic/transport.cpp` (HTTP/2 + SSE, the OAuth/Pro/Max default) and
  `openai/transport.cpp` (any OpenAI-compatible endpoint — openai, groq,
  openrouter, together, cerebras, ollama, or a raw host). `selection.cpp`
  resolves which one a `--provider` flag / persisted setting picks.
- **`tool/`** — the `Tool` concept, the registry, the permission policy, and
  one file per tool under `tool/tools/`. `memory_store.cpp` backs
  `remember`/`forget`.
- **`scope/`** — the config-resolution algebra (`Locus × Dialect` precedence,
  provenance-carrying `Source`, `resolve_first`/`resolve_union` folds,
  content-bound `Trust`). One pure primitive that memory, skills, agents, and
  slash-command discovery all fold through instead of hand-rolling roots. See
  [`design/scope-model.md`](design/scope-model.md).
- **`io/`** — `http`, `inflate` (gzip/deflate response decoding — self-contained,
  no zlib dependency), `tls` (certificate pinning), `auth` (OAuth + PKCE),
  `persistence` (atomic writes), `clipboard`.
- **`airgap/`** — SOCKS5-over-SSH so the agent can run on a host with no direct
  internet while the laptop relays the bytes.

---

## 3. Seams: how concrete types stay hidden

`AgenttyApp` must not be templated on the Provider and Store types — that would
force every translation unit to know the concrete types and rebuild when they
change. Instead, `include/agentty/runtime/app/deps.hpp` defines a small
`Deps` struct of `std::function`s:

- **Provider seam** — `stream(Request, EventSink)`.
- **Store seam** — `save_thread`, `load_threads`, `load_thread`,
  `load_settings`, `save_settings`, `new_thread_id`, `title_from`.
- **Auth context** — the typed `AuthHeader` for the session.

`main.cpp` calls `app::install(provider, store, auth_header)` once at startup;
the reducer reaches the seams through `app::deps()`. `update_auth(...)`
live-swaps credentials after an in-app login without restarting the process —
in-flight streams cached the header at request-build time, so they are
unaffected.

The `Provider` concept is deliberately tiny:

```cpp
template <class P>
concept Provider = requires(P& p, Request req, EventSink sink) {
    { p.stream(std::move(req), std::move(sink)) } -> std::same_as<void>;
};
```

Anything that streams a chat completion satisfies it — the real Anthropic
and OpenAI-compatible transports in production, a deterministic in-memory
script in tests.

How the many concrete transports behind this seam stay coherent — routing
as a registry-row field with `static_assert` proofs, model quirks as
catalog facts, shared wire scaffolding — is its own document:
[PROVIDER_HETEROGENEITY.md](PROVIDER_HETEROGENEITY.md).

The layering question underneath that — why a model is keyed
`(provider, model)` while a subscription fact is keyed
`(provider, account, model)`, and how to tell which layer a new fact
belongs to — is
[IDENTITY_CAPABILITY_ENTITLEMENT.md](IDENTITY_CAPABILITY_ENTITLEMENT.md).

The four transports (Anthropic, OpenAI-compat, Ollama-native, ChatGPT/Codex
Responses) each own their provider-specific streaming/salvage logic, but every
ingress concern that is genuinely SHARED lives in exactly one place so a fix
can't drift into three copies of itself: `provider::finish_stream` +
`wire::SseFramer`/`LineFramer` (byte-level framing + terminal-event epilogue),
`provider::parse_retry_after` (Retry-After backoff), `wire::scrub_utf8`
(strict UTF-8 validation), `wire::could_be_tool_json` (the leaked-tool-call
prefix sniffer weak local models need), `provider::usage::{from_openai,
from_responses,from_ollama}` (the three token-usage wire shapes), and
`auth::bearer_token` (OpenAI-family auth header emission — Anthropic keeps its
own arm-typed visit since an API key must route to `x-api-key`, never
`Bearer`).

---

## 4. Msg: a closed sum, split for compile speed

A naive design inlines every leaf event in one giant variant. That pins
`sizeof(Msg)` to the heaviest leaf, instantiates an N-wide `std::visit`
dispatch table, and forces the whole reducer TU to rebuild on any leaf change.

agentty instead groups leaves into ~15 **domain sub-variants** in `msg.hpp`
(`ComposerMsg`, `StreamMsg`, `ToolMsg`, `ModelPickerMsg`, `ThreadListMsg`,
`CommandPaletteMsg`, `MentionPaletteMsg`, `SymbolPaletteMsg`, `TodoMsg`,
`LoginMsg`, `DiffReviewMsg`, `CheckpointMsg`, `MetaMsg`). The top-level reducer in
`update.cpp` is then a small `std::visit` that forwards each domain to its own
TU:

```cpp
auto step = std::visit(overload{
    [&](msg::ComposerMsg cm) { return detail::composer_update(std::move(m), std::move(cm)); },
    [&](msg::StreamMsg   sm) { return detail::stream_update  (std::move(m), std::move(sm)); },
    [&](msg::ToolMsg     tm) { return detail::tool_update    (std::move(m), std::move(tm)); },
    // … nine more domain arms …
}, msg);
```

Each `update/<domain>.cpp` recompiles only when its own leaves change.
Call sites still build a `Msg` directly via `std::variant`'s converting
constructor — only the owning domain accepts a given leaf, so the wrap is
unambiguous.

---

## 5. Tools: typed bundles behind a JSON edge

The `Tool` concept (`include/agentty/tool/tool.hpp`) requires a static bundle
of identity + schema + effects + behavior:

```cpp
template <class T>
concept Tool = requires {
    typename T::Args;
    typename T::Result;
    { T::name() }         -> std::convertible_to<std::string_view>;
    { T::description() }  -> std::convertible_to<std::string_view>;
    { T::input_schema() } -> std::convertible_to<nlohmann::json>;
    { T::effects() }      -> std::convertible_to<EffectSet>;
} && requires(const nlohmann::json& args) {
    { T::execute(args) }  -> std::convertible_to<ExecResult>;
};
```

Tools are fully typed internally; only the dispatcher boundary speaks JSON.
`DynamicDispatch` looks a tool up in the registry, executes it inside a
`try/catch` (a crashing tool becomes a typed `ToolError`, not a process abort),
and applies a **per-tool output budget** so a runaway `read`/`bash`/`grep`
can't blow the context window. Truncation is UTF-8-safe and comes in three
strategies:

- **Head** — keep the front; right for ordered chunks (read, edit, write).
- **Tail** — keep the end; right for log streams (bash, diagnostics).
- **HeadTail** — keep both ends with a middle elision marker; right for tools
  where both ends carry signal (grep, web_*, git diff/log/status).

The shipped tools: `read`, `write`, `edit`, `move`, `remove`, `bash`,
`process_start`/`process_poll`/`process_stop`, `grep`, `glob`, `list_dir`,
`repo_map`, `find_definition`, `search_structural`, `web_fetch`, `web_search`,
`todo`, `diagnostics`, `test`, `git_status`, `git_diff`, `git_log`,
`git_show`, `git_blame`, `git_commit`, `remember`, `forget`, `wipe_memory`,
`search_docs`, `search_code`, `task` (subagent dispatch), `skill` (load a
skill body on demand).

The tree-walking tools (`grep`, `glob`, `list_dir`, `repo_map`,
`find_definition`, the @-file picker, the symbol index) share one directory
skip-list (`should_skip_dir`: `.git`, `node_modules`, `build*`,
`cmake-build*`, `_deps`, `target`, `vendor`, `.venv`, …) so generated and
fetched trees never flood results. `grep`/`find_definition` prefer ripgrep
when present and pass that same skip-list as `-g '!…'` excludes, so both the
ripgrep and built-in backends prune identically — a cold-cache win (rg no
longer stat + gitignore-checks tens of thousands of build files) and a
quality win in repos with no `.gitignore` (build artifacts stop polluting
hits).

---

## 6. Permission policy: a constexpr matrix

Every tool declares an `EffectSet` over four bits: `ReadFs`, `WriteFs`, `Net`,
`Exec`. The active **Profile** plus that effect set feed the pure `constexpr`
function `policy::permission(effects, profile)` in `tool/policy.hpp`, which
returns `Allow` or `Prompt`. The rule:

| Profile     | Pure  | ReadFs | WriteFs | Net    | Exec   |
|-------------|-------|--------|---------|--------|--------|
| **Write**   | Allow | Allow  | Allow   | Allow  | Allow  |
| **Ask**     | Allow | Allow  | Prompt  | Prompt | Prompt |
| **Minimal** | Allow | Prompt | Prompt  | Prompt | Prompt |

`Write` is fully autonomous. `Ask` trusts read-only inspection so an agent
loop's read/grep/glob doesn't prompt on every step but gates anything that
mutates state, runs code, or hits the network. `Minimal` prompts for every tool
that touches the outside world and auto-allows only pure ones. `Exec` is the
maximal capability — a tool carrying it prompts regardless of what else it has,
on the type-theoretic claim that `bash` lets the model *author* the side
effect, so it dominates any individual filesystem mutation already gated.

The whole table is proved at compile time. `EffectSet` is a 4-bit bitset (16
sets) × 3 profiles = exactly **48 cells**. A second function,
`expected_decision`, re-states the policy independently, and an exhaustive
`constexpr` loop `static_assert`s `permission(e, p) == expected_decision(e, p)`
over every cell — so a one-handed change to either side breaks the *build*, not
a test nobody runs. A further `static_assert` pins the bitset width, firing if
a fifth `Effect` is added without extending both sides.

`DynamicDispatch::needs_permission` is the single place the runtime asks "does
this gate on the user?", and unknown tools fail closed (default to requiring
permission). The companion `policy::reason` supplies the one-line explanation
rendered in the permission card ("wants to run an arbitrary subprocess", "will
modify files on disk", …).

---

## 7. Tool scheduling: parallel-safety from the effect set

The same `EffectSet` that drives permissions also decides whether two tools may
run concurrently. `effects::is_parallel_safe(active, want)` answers "may a tool
with `want` effects start while `active` effects are in flight?":

- **`WriteFs` and `Exec` demand exclusive access.** A write can mutate state a
  sibling is reading, writing, or shelling against — two edits to "different"
  files look independent until the model picks overlapping paths. `Exec` is
  worse still because the model chose the command, so the runtime serialises.
- **`Pure`, `ReadFs`, and `Net` compose freely.** Read-read never races, `Net`
  touches neither FS nor process state, and in-memory `Pure` tools (`todo`)
  operate on data the model can't observe concurrently.

The rule is, again, proved at compile time — `effects.hpp` carries a block of
`static_assert`s pinning the exclusive/compose decisions, and the tool spec
carries `parallel_rule_is_well_founded`. Effects are chosen by *what the tool
does to the world, not how it's implemented*: `git_status` is `ReadFs` even
though it shells out to `git`, because the runtime knows what that subprocess
does; `bash` is `Exec` because the model picks the command.

---

## 8. The streaming turn: a phase FSM with a retry watchdog

A turn is not a single request — it cycles `Streaming → AwaitingPermission →
ExecutingTool → Streaming → … → Idle`. `domain/session.hpp` models this as a
phase variant where the per-turn `Active` context (cancel token, start stamp,
retry counters) lives *inside* every non-`Idle` alternative — so reading those
fields from `Idle` is a type error, not a logic bug masked by zero defaults.
Legal transitions take the source by `&&` and re-wrap its context in the
destination, so the FSM itself carries the turn state across phases.

Reliability rides on two independent pieces:

- **A retry state machine** (`retry::Fresh / StallFired / Scheduled`) replaces
  what used to be two hand-synchronised bools. A 120-s stall watchdog trips the
  cancel token (`Fresh → StallFired`); the synthetic `StreamError` schedules a
  retry via `Cmd::after` (`→ Scheduled`); a second error during the wait can't
  schedule a duplicate; `RetryStream` firing returns to `Fresh`.
- **Two independent retry budgets.** `truncation_retries` covers a stream that
  EOFs mid-tool-args; `transient_retries` covers 5xx / network / overloaded /
  429. `transient_retries` is *not* monotonic per turn — it resets to 0
  whenever the wire proves healthy (first content delta, or an SSE ping /
  thinking delta), so a connect-ping-stall sequence gets a fresh budget each
  attempt instead of latching the session terminal.

---

## 8.5. Context management: smart compaction, not a fixed cliff

A long thread eventually has to be summarized so it fits the model's context
window. Two things make this cheap AND rarely-needed instead of a recurring
tax:

- **The trigger rides DEEP — 95 % of the window — and compaction is the
  expensive event, so we fire seldom.** `StreamState::compaction_threshold()`
  (`domain/session.hpp`) fires at `min(kSoftFillPct% of context_max,
  context_max - kMinOutputHeadroom)` = `min(95 %, window - 20K)`. A 200K
  window fires at 180K (the 20K reply floor binds); a 1M window rides all the
  way to **950K**. Counter-intuitively this is also the *cheapest* policy:
  the repeated conversation prefix is a prompt-cache HIT across turns (the
  1-hour Anthropic anchor breakpoint in `wire_body.cpp`, the pinned OpenAI
  `prompt_cache_key`), so a large live prefix costs ~10 % of fresh input per
  turn — while each **compaction** runs a summary request AND resets the
  cache (the next turn is a full miss). Firing at 95 % instead of early means
  ~4× fewer of those expensive events on a big window → lower total token
  burn. There is **no user knob** (the old *Compaction depth* command is
  gone); the policy is chosen to give maximum context and minimum burn
  automatically.
- **The summarised slice is bounded** so the summary request stays cheap even
  on a huge window. `wire_messages_for_compaction` (`cmd_factory.cpp`) hands
  the summariser the smaller of ~65 % of the window and `kCompactionSliceCap`
  (~150K) of the most recent transcript, keeping the first user turn so the
  original task framing survives. A cheap model compresses 150K of recent
  history far better — and cheaper — than 650K in one shot.
- **Idle cache-lapse pre-compaction — the one guard against a price spike.**
  Deep-ride is cheap *only while the prompt cache holds the prefix*. The one
  case it would cost you: ride to 950K, walk away past the cache TTL (~1h),
  and your next message re-prices all 950K at full input rate. So the `Tick`
  handler (`update/meta.cpp`) watches for it: `should_compact_on_idle()`
  (`domain/session.hpp`) fires a compaction PRE-EMPTIVELY once the session
  has been idle ~48 min (`kIdleCompactAfter`, 80 % of the TTL) AND the prefix
  is large (≥ `kIdleCompactMinTokens`, 200K). The summary runs while the
  prefix is still a warm cache hit (cheap), so you return to a small warm
  context instead of eating a cold re-price. It is bounded to large +
  near-TTL-idle states, so ordinary pauses never trip it — this is the *only*
  proactive early-compaction path.
- **The summarization request itself runs on the cheapest capable model on
  the active provider** (the same `cheapest_capable_model` router subagents
  use — see §8.5.1), not the flagship model you're chatting with.
  Compacting used to mean sending the ENTIRE conversation prefix plus a
  verbose summarize prompt to Opus/Sonnet at full input price on every trigger
  — the single biggest hidden cost in a long session. Now it costs a
  Haiku-class summary.
- **The 1M context window is an explicit, entitlement-gated model variant**,
  not an auto-detected tier probe — matching Claude Code's own model catalog
  (verified against its shipped binary). Anthropic's `/v1/models` list is
  augmented with a `<model>[1m]` companion row per 1M-capable model on the
  OAuth (Pro/Max) path; the `[1m]` marker is a **picker-only** signal
  (`ModelCapabilities::extended_context_1m`) that widens `context_window()`
  and requests the `context-1m-2025-08-07` beta — it is stripped
  (`wire_model_id()`) before the model field ever reaches the wire.

### 8.5.1 The subagent cost router

`task` subagents already avoid the same trap: read-only roles (`explorer`,
`reviewer`) route to the cheapest model on the active provider that still
passes a capability floor (tool support, non-embedding/image/audio asset,
non-Weak tier); `tester`/`coder`/`general` keep the parent model since they
mutate the workspace. `ModelCapabilities::tier()` derives strength
provider-RELATIVELY from the id (family lane + weak-tool-use signal) since no
vendor ships a comparable power number — it only orders models WITHIN one
provider, never across. The router never routes up and keeps the parent
model when nothing cheaper qualifies, so a single-model or Opus-only account
sees no behavior change.

---

## 9. Safety boundaries

- **Two roots: access boundary vs active project.** agentty keeps these
  distinct. The **access boundary** (`util::workspace_root()`) is the security
  gate: filesystem tools refuse any path outside it. It defaults to the launch
  directory and is *widenable* — `--workspace DIR` moves it, and `--workspace /`
  opts out entirely (whole-disk power). The **active project**
  (`util::project_root()`) is the process cwd (agentty never `chdir`s away from
  the launch dir) *clamped inside the boundary*. Relative tool paths and
  repo/project-scoped defaults resolve against the **project**, not the
  boundary, so under `--workspace /` a model's `read src/foo.cpp` still lands in
  the launched project rather than at `/src/foo.cpp`. By default the two are the
  same directory, so ordinary launches see no difference; the split only matters
  when the boundary is widened past the project. Everything that means "the
  project" — `normalize_path` (relative-path anchor), `grep`/`glob`/`list_dir`
  defaults, `repo_map`, `find_definition`, `diagnostics`/`test` (build dir +
  manifest), git tools (`default_git_start`), checkpoints, the @-file picker,
  the symbol index, and project-scoped `remember` — routes through
  `project_root()`; everything that means "the security gate" (containment
  checks, sandbox bind-mounts, refuse-delete-root) uses `workspace_root()`.
- **Sandbox.** `bash` and `diagnostics` run inside `bwrap` (Linux) or
  `sandbox-exec` (macOS) by default. Workspace + system libs + network are
  reachable; `~/.ssh`, `/etc`, and other projects are read-only. An approved
  `bash` call still can't `cat ~/.ssh/id_rsa`. `--sandbox auto|on|off`.
- **TLS pinning.** Certificates are pinned on the real upstreams, end-to-end,
  including through the airgap SOCKS tunnel.
- **Atomic writes.** Every persisted file is `write` + `fsync` + `rename` (or
  the Windows `MoveFileExW` equivalent), so a crash mid-write never corrupts a
  thread or the credential store.

---

## 10. Rendering performance

Idle agentty costs zero CPU: `fps = 0` means maya only renders on a `Msg`,
input, or timer tick. Two host-side optimizations keep it cheap under load:

- **`visual_hash`** mixes only the axes that change what's on screen. When the
  hash matches the previous frame, `view` + render are skipped entirely. The
  hash hashes only the *live* message tail (the frozen scrollback prefix is
  immutable archaeology), samples long strings instead of hashing every byte,
  and buckets time-driven animations so each visible step — and only each
  visible step — advances the hash. The animation bucket is phase-locked to
  whatever is actually on screen so the render gate and the animation never
  beat against each other.
- **`needs_warmup`** fires a one-shot off-wire render when a thread is resumed,
  converting the first visible frame of a tool-heavy thread from O(content) to
  O(blit).

---

## 11. Build notes

- Requires GCC 14+ / Clang 18+ / MSVC 14.40+ and CMake 3.28+ (C++26).
- `-DAGENTTY_STANDALONE=ON` statically links OpenSSL + nghttp2 + libstdc++ +
  libgcc when their `.a` archives are present; libc stays dynamic. A musl
  toolchain with `-DAGENTTY_FULLY_STATIC=ON` yields a 100% static binary.
- `-DAGENTTY_USE_MIMALLOC=ON` (default) routes `malloc`/`free` and global
  `new`/`delete` through **mimalloc**. CMake `FetchContent` follows upstream
  `main` and checks for updates on every configure; no mimalloc source or
  submodule is stored in this repository. mimalloc is compiled as a static C
  library with its C and C++ allocation overrides enabled.
  `release_to_kernel()` calls `mi_collect(true)` at coarse memory-release
  boundaries.
- **Gotcha:** `AGENTTY_AUTO_PULL_MAYA=ON` is the default and runs
  `git reset --hard origin/master` on the `maya/` submodule during build. Its
  only guard checks for *uncommitted* changes, so committed local maya work
  still gets wiped. Build with `-DAGENTTY_AUTO_PULL_MAYA=OFF` when iterating on
  maya.

---

## 12. One-paragraph mental model

`main.cpp` resolves credentials and installs a Provider + Store behind the
`Deps` seam, then hands control to maya. maya calls `view(model)` to paint and
`update(model, msg)` for every event. User input and SSE chunks become `Msg`s;
the reducer dispatches each to a per-domain handler that returns the next
`Model` plus a `Cmd` describing any side effects. Tools run behind a JSON
dispatch edge with a `constexpr` permission gate and OS-level sandboxing, and
their results loop back in as more `Msg`s. Nothing in the loop mutates global
state; the only escape hatches are the explicit `Cmd`s the runtime executes on
your behalf.
