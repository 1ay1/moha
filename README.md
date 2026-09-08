<h1 align="center">agentty</h1>

<p align="center">
  <b>A blazing-fast, open-source coding agent in your terminal.</b><br>
  A drop-in <a href="https://agentty.org/alternatives/claude-code-alternative">Claude Code alternative</a> in C++26 — one static binary, millisecond startup, any model.
</p>

<p align="center">
  <a href="https://github.com/1ay1/agentty/releases/latest"><img src="https://img.shields.io/github/v/release/1ay1/agentty?style=flat-square&color=blue" alt="Release" /></a>
  <a href="https://github.com/1ay1/agentty/stargazers"><img src="https://img.shields.io/github/stars/1ay1/agentty?style=flat-square&color=f1c40f&labelColor=555555" alt="Stars" /></a>
  <a href="https://github.com/1ay1/agentty/releases"><img src="https://img.shields.io/github/downloads/1ay1/agentty/total?style=flat-square&color=brightgreen" alt="Downloads" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="License" /></a>
  <a href="https://discord.gg/qhb9AZ8f3c"><img src="https://img.shields.io/badge/Discord-join%20chat-5865F2?style=flat-square&logo=discord&logoColor=white" alt="Discord" /></a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/1ay1/agentty/master/agentty.gif" alt="agentty demo" width="800" />
</p>

## Why agentty?

Most terminal coding agents ship as a Node or Python app and send big chunks of your repository to the model on every turn. agentty takes the opposite approach:

- **It's one native binary.** 16.7 MB, ~3 ms cold start, zero runtime dependencies — no Node, no Python, no `npm install`, no `node_modules`. Download and run.
- **It sends only the relevant code.** Built-in retrieval (hybrid BM25 + dense embeddings, code-aware chunking, GraphRAG) fetches just the slices that matter — often cutting context by 80%+ vs. whole-repo dumping.
- **It's not locked to one vendor.** Sign in with your Claude Pro/Max, or point it at OpenAI, Groq, OpenRouter, Cerebras, DeepSeek, xAI (Grok), Mistral, Gemini, Fireworks, or a fully local Ollama model. Switch live with `^P`.
- **It's safe by default.** Shell and build commands run in a sandbox; air-gap an entire session over SSH with one command.
- **It's open source (MIT)** and runs inside Zed over ACP.

Coming from another tool? See the honest comparisons: [vs Claude Code](https://agentty.org/compare/agentty-vs-claude-code) · [vs Aider](https://agentty.org/compare/agentty-vs-aider) · [vs Cursor](https://agentty.org/compare/agentty-vs-cursor) · [all alternatives](https://agentty.org/alternatives).

## Getting Started

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
cd your-project
agentty
```

First launch opens auth — **paste an API key** (Anthropic `sk-ant-…`, or any provider's key) or use a local Ollama model that needs no key at all. You can also sign in with your Claude Pro/Max OAuth if you prefer. Once you're in, a first-run welcome card suggests a few things to try; just type and hit Enter.

## Features

<table>
<tr>
<td width="50%">

### ⚡ Instant, all the way through
Cold start under 1 ms; keystroke-to-pixel about 1 ms. No Node, no Python, no npm install — just a static binary. Independent tool calls run in parallel and read-only tools start while the model is still typing, so tool-heavy turns finish sooner.

### 🔌 Any model
Claude, GPT, Groq, OpenRouter, Ollama, or any OpenAI-compatible endpoint. Switch live with `^P`.

### 🛡️ Sandboxed by default
Every shell call runs inside bwrap (Linux) / sandbox-exec (macOS). File tools refuse paths outside your workspace.

</td>
<td width="50%">

### 🌐 Air-gapped mode
Run on a box with no internet. Your laptop relays the bytes over SSH with TLS pinned end-to-end.

### 🔧 Full tool suite
read · write · edit · bash · grep · glob · git · web · search_docs · search_code · task — each with a purpose-built widget.

### 🧠 Learns your codebase
Agent Skills + remember/forget memory, plus a fully **local RAG** engine — hybrid BM25 + embeddings, RRF-fused, reranked, diversified, and expanded over a **GraphRAG** document graph — over your docs, skills, and memory. Teach it once, every session knows your conventions. [How it works ↓](#retrieval-rag)

### 🎯 Smart Mode
One flagship model plans; cheaper models do the legwork. Effort scales to each turn's complexity, a cascade retries harder only when a cheap attempt falls short, and the router **learns your repo** across sessions. Off is a strict no-op. [How it works ↓](#smart-mode)

</td>
</tr>
</table>

## Providers

```bash
agentty                                    # bring your own key/model
agentty --provider openai -m gpt-4o        # GPT
agentty --provider groq -m llama-3.3-70b   # Groq
agentty --provider ollama -m qwen2.5-coder # local model, no key
agentty --provider openrouter              # any model via OpenRouter
agentty --provider deepseek -m deepseek-v4-pro  # DeepSeek (DEEPSEEK_API_KEY)
agentty --provider xai -m grok-4.6         # xAI Grok (XAI_API_KEY)
agentty --provider gemini -m gemini-3.7-flash   # Google Gemini (GEMINI_API_KEY)
agentty -m claude-opus-4-5                 # Claude (API key or Pro/Max OAuth)
```

`--provider` persists. Switch live in-app with `^P`.

## Retrieval (RAG)

### Retrieval without context dumping

Rather than injecting a repository or every project document into each prompt,
agentty retrieves a **small, source-tagged set of relevant passages** only when
the task needs project knowledge. That keeps context focused, leaves more room
for the task itself, and avoids paying to repeatedly send irrelevant material.

agentty ships a **complete, fully-local retrieval engine** behind two tools — no
cloud, no dependencies, works offline. The only optional network hop is a
*localhost* [Ollama](https://ollama.com) server for embeddings; with none
reachable it falls back to keyword search and keeps working.

- **`search_docs`** — searches your *knowledge base*: a docs folder, your
  installed skills, your learned `remember` memory, and (opt-in) connected MCP
  resources. Useful from the first turn — skills and memory are always indexed,
  even with no docs folder.
- **`search_code`** — *semantic* search over your source by meaning, for
  "where is retry backoff handled" questions where you don't know the identifier.
  The hybrid complement to `grep`.

Every returned passage is **source-tagged** (`docs:` · `skill:` · `memory:` · MCP
URI) with its file + line range, so the model can cite, open, or follow it.

**Enable the semantic half** (BM25 works with zero setup):

```bash
ollama pull nomic-embed-text && ollama serve     # localhost embeddings
export AGENTTY_DOCS_DIR=~/my-project/docs         # optional; skills+memory always indexed
```

agentty auto-detects the running server and upgrades from BM25-only to full
hybrid retrieval — no restart needed.

<details>
<summary><b>The retrieval funnel</b></summary>

Every `search_docs` call runs this pipeline. The **default path makes no LLM
calls** — it's fast, deterministic, and safe to leave fully on.

1. **Hybrid retrieval** — BM25 (keyword, Porter-stemmed) and dense embeddings
   (HNSW-indexed at scale) each rank a wide candidate pool; the two lists are
   fused with **Reciprocal Rank Fusion** (or opt-in Relative Score Fusion,
   `AGENTTY_RAG_FUSION=rsf`). Every dense probe a search fans out — expansion,
   HyDE, multi-hop — embeds in **one batched round-trip**, not one per probe.
2. **Pseudo-relevance feedback (RM3)** — harvests discriminative terms from the
   top hits and fuses a second down-weighted probe, recovering the vocabulary
   you didn't type. Sub-millisecond, no model.
3. **Contextual retrieval** — each chunk is indexed with a breadcrumb of its
   doc title + heading path, so `guide.md › Install › Linux` is findable even
   when the body never says "linux".
4. **Re-ranking** — a deterministic feature-fusion reranker (term coverage,
   phrase proximity, title match, calibrated cosine), plus an optional batched
   embedding cross-encoder and an opt-in generative 0–10 judge.
5. **MMR diversification** — greedily keeps hits that are relevant *and*
   distinct, so duplicate windows don't crowd out real answers.
6. **Compression** — trims each survivor to its best query-relevant span:
   "20k noisy tokens" → "2k useful tokens."
7. **Parent-document expansion** — stitches the precise hit back into its
   adjacent sibling chunks so the model reads it in context.
8. **GraphRAG expansion** — builds the corpus's document graph (nodes = docs;
   edges = markdown links + tf·idf entity co-occurrence), runs PageRank and
   community detection over it, and pulls in supporting docs from four tiers
   around the top hits (outbound links, backlinks, entity neighbours, and the
   community hub). Deterministic, in-memory, no model.
9. **Corrective retry (CRAG)** — on a low-confidence result, de-noises the
   query, widens the pool, and keeps whichever attempt scored higher.

**Opt-in recall boosters** (cost a model call, off by default):
RAG-Fusion query expansion (`AGENTTY_RAG_EXPAND=1`),
HyDE hypothetical-document embeddings (`AGENTTY_RAG_HYDE=1`), and
GraphRAG community summaries (`AGENTTY_RAG_GRAPH_SUMMARY=1` — a cached
natural-language report per topic cluster, generated once per corpus shape).

Beyond the explicit tool, a **proactive path** runs the funnel *before you ask*
when your message looks knowledge-shaped, injecting a source-tagged
`<retrieved-context>` block into the very same turn — grounding without a tool
round-trip. The transcript renders it as one quiet 📚 card that shows a
confidence bar plus exactly *which* sources grounded the answer (`docs · path`,
`skill · name`, `memory`…) and a passage count, so you can always see — and
weigh — what the model was standing on.

BM25, RRF, HNSW, the reranker, MMR, compression, PRF, the chunker, and the
GraphRAG document graph (PageRank, entity extraction, community detection) are
all in-house C++/STL. Every stage degrades gracefully and is tunable via
`AGENTTY_RAG_*` env vars. For big corpora, two opt-in vector-cost levers keep
the ANN cheap while the full-precision rerank recovers quality: **Matryoshka**
dimension truncation (`AGENTTY_RAG_ANN_DIM=256`, ~2.3× faster walk) and
**binary quantization** (`AGENTTY_RAG_BINARY=1`, popcount-Hamming walk + float
rescore, ~2.5×). Full write-up:
[`docs/website/retrieval.md`](docs/website/retrieval.md).

</details>

## Smart Mode

One model rarely fits every turn. A one-line rename and a cross-file refactor
don't deserve the same effort, and burning flagship reasoning on trivia is
just slow and expensive. Smart Mode (`^S`) fixes that with an
**orchestrator-workers** design: a strong model owns the plan and delegates
well-scoped subtasks to cheaper workers.

It's built from **three roles** and **eight independent layers**, each a toggle:

- **Strategic** (flagship) plans and delegates · **Implementation** (mid) writes
  code · **Utility** (cheap) handles summaries and grunt work. The resolver maps
  a *role* to `(model, effort)` — it never checks a model name by string, so
  pinning any model to any slot is always safe.
- **Complexity-scaled effort** sizes each turn (trivial → complex), and a
  **cascade** retries at higher effort only when a cheap attempt actually falls
  short — the RouteLLM/cascade idea applied *inside* the agent loop.
- Because the loop sees outcomes a stateless router can't — the build fails, the
  test goes red, you correct the next turn — Smart Mode **learns per-workspace**:
  it persists the effort prior the cascade discovered and recalls decompositions
  that worked. The second session in a repo is smarter than the first. All
  learning is local, on-disk (`.agentty/`), and wipeable.

Open the overlay with `^S`; the footer shows what it's learned in this repo.
**Off is a strict byte-for-byte no-op** — zero extra tokens, zero latency.
Full write-up: [`docs/website/smart-mode.md`](docs/website/smart-mode.md).

## Keys

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `Enter` | Send / queue | `^K` | Command palette |
| `Esc` | Cancel / reject | `^J` | Thread list |
| `S-Tab` | Cycle profile | `^P` | Provider picker |
| `Alt+Enter` | Newline | `^/` | Model picker |
| `^B` | Loop: resend until off | `^N` | New thread |
| `^G` | Run code block | `^S` | Smart Mode |
| `^R` | Review changes | `^T` | Plan / todo |
| `^O` | Inspect tool outputs | `^E` | Expand composer |
| `^C` | Quit (from anywhere) | | |
| `^←/→` or `Alt+←/→` | Cycle threads | | |

The composer is a full readline-style editor:

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `^←/→` | Move by word | `^W` / `Alt+D` | Delete word back / forward |
| `^U` / `Alt+K` | Kill to line start / end | `^Z` / `^Y` | Undo / redo |
| `^V` / `Alt+V` | Paste image | `/` `@` `#` | Command / file / symbol picker |
| `↑` | Recall queue / history | `Alt+↑/↓` | Edit queued messages one at a time |

Full keymap: [docs/website/keybindings.md](docs/website/keybindings.md).
Pasting images **over SSH** needs one terminal setting — see [Clipboard & Images](docs/website/clipboard.md).

## More

<details>
<summary><b>Installation options</b></summary>

**Linux**

```bash
# Debian / Ubuntu
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty_amd64.deb
sudo dpkg -i agentty_amd64.deb

# Fedora / RHEL / CentOS
sudo dnf install https://github.com/1ay1/agentty/releases/latest/download/agentty-x86_64.rpm

# openSUSE
sudo zypper install https://github.com/1ay1/agentty/releases/latest/download/agentty-x86_64.rpm

# Arch (AUR)
yay -S agentty-bin   # prebuilt static release
yay -S agentty-git   # build HEAD from source, dynamically linked

# Alpine
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty-x86_64.apk
sudo apk add --allow-untrusted agentty-x86_64.apk
```

**macOS**

```bash
brew tap 1ay1/tap && brew install agentty
```

**Windows**

```powershell
scoop bucket add 1ay1 https://github.com/1ay1/scoop-bucket; scoop install agentty
# or
winget install agentty.agentty
```

**Termux / Android** (no root, no proot)

agentty builds natively against Termux's Bionic/libc++ toolchain. The install
script detects Termux and installs into `$PREFIX/bin` (on your PATH):

```bash
pkg install git cmake clang openssl libnghttp2
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --build
```

Shell/build tools run **unsandboxed** on unrooted Android (Bubblewrap needs
user namespaces Android doesn't grant) — everything else works. See
[`packaging/termux/`](packaging/termux/) for the `pkg install agentty` recipe.

**Anywhere (no package manager)**

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
```

If the prebuilt binary won't run on your system (e.g. a libc mismatch), pass
`--build` to compile from source instead — the installer also does this
automatically when the downloaded binary fails to execute:

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh -s -- --build
```

**From source** (needs a C++26 toolchain — GCC 14+ / recent Clang / MSVC)

```bash
git clone --recursive git@github.com:1ay1/agentty.git
cd agentty && cmake -B build && cmake --build build -j
```

**Named presets** (reproducible configs, no `-D` soup) — **use these if you are
going to edit the code**: `debug` rebuilds in ~1.7 s after a one-line change vs
~36 s for the plain `cmake -B build` above, because it skips LTO. See
[Building from source](https://agentty.org/docs/building#the-development-loop).

```bash
cmake --preset debug   && cmake --build --preset debug     # fast Debug loop (no LTO, ccache)
cmake --preset release && cmake --build --preset release   # optimized -O3 + LTO binary
ctest  --preset debug                                      # run the suite
```

Every preset shares one `build/` tree — switching preset reconfigures it in
place instead of leaving a second multi-gigabyte copy on disk.
`cmake --list-presets` shows them all (debug / debug-full / release / ci /
sanitizer / standalone / pch).

All binaries are a single fully-static executable (x86_64 + aarch64 on Linux, Intel + Apple Silicon on macOS; Termux/Android builds from source). Packaging details: [`packaging/README.md`](packaging/README.md).

</details>

<details>
<summary><b>Air-gapped hosts</b></summary>

```bash
agentty airgap --setup user@host   # first time: copies credentials
agentty airgap user@host           # every time after
```

Your laptop relays via SOCKS5-over-SSH. TLS pins on real upstreams — the network in between can't MITM you.

</details>

<details>
<summary><b>Inside Zed (ACP)</b></summary>

agentty speaks the [Agent Client Protocol](https://agentclientprotocol.com) — the same protocol Zed uses for Claude Code. Add to Zed's settings:

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp"]
    }
  }
}
```

</details>

<details>
<summary><b>Loop a prompt until it's done (Ctrl+B)</b></summary>

Some prompts are worth sending more than once — *"fix the next failing test"*,
*"keep refactoring until the build is clean"*, *"check if the deploy finished"*.
`^B` sends the composer's message and then **re-sends it automatically after
every completed turn**, until you press `^B` again.

While armed, the composer keeps showing the prompt on repeat and becomes
read-only (dimmed text, parked caret) — so the box can never say one thing
while agentty sends another. A `⟳ LOOP ×N` chip and a tinted border make an
auto-sending session impossible to mistake for an idle one, and the count
makes it distinguishable from a hang.

The model can't tell: an auto-sent turn goes through the same submit path as
one you typed, with no marker on the wire, in the transcript, or in the saved
thread.

If a turn **fails**, the loop doesn't hammer the endpoint — re-sending a
rate-limited turn instantly is how a 429 becomes a longer one. It waits: the
provider's own `Retry-After` is obeyed verbatim when it sends one, otherwise
the delay escalates per error class (rate-limit/auth from 30 s, transient
blips from 5 s) and resets after the next success. The chip counts down
(`⟳ RETRY 24s`) so a paused loop reads as waiting, not wedged. `Esc` cancels
the turn; `^B` stops the loop.

</details>

<details>
<summary><b>Run code blocks from replies (Ctrl+G)</b></summary>

The AI hands you a fenced block of commands — don't copy-paste it. `^G` lists
the blocks from the last reply; `Enter` (or a digit) runs one **interactively
on your real terminal**: the TUI suspends, sudo password prompts work, output
streams live, `Ctrl+C` kills the command (not agentty). When it exits, a
result card lets you attach the captured output to the composer as a
collapsed chip (`a`), copy it (`y`), or discard (`Esc`) — so "it failed with
X" reaches the model without you re-typing anything.

Runs the right shell per block on every OS: `sh`/`bash` blocks through
`/bin/sh` on Linux/macOS, `powershell`/`pwsh` and `cmd`/`bat` blocks through
PowerShell / `cmd.exe` on Windows. Prompt `$ ` markers are stripped, a block
your platform can't run offers edit/copy instead, and capture is capped at
2 MB. Details: [`docs/RUN_CODE_BLOCK.md`](docs/RUN_CODE_BLOCK.md)

</details>

<details>
<summary><b>Agent Skills</b></summary>

Drop a `SKILL.md` anywhere under `.agentty/skills/` or `~/.agentty/skills/` — it's live next turn. Compatible with Claude Code's `.claude/skills/` format.

The tier-1 catalog (name + description of every discovered skill) is capped at 64 entries. Set `AGENTTY_MAX_SKILLS` to override it (clamped to 8–4096) — lower it for small-context models, raise it for large skill libraries.

On codebases with internal DSLs or tribal conventions, agent accuracy jumps from ~20% to ~85% with curated skills ([research](https://arxiv.org/abs/2410.03981)).

</details>

<details>
<summary><b>Architecture</b></summary>

Pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. View is `Model -> Element`, rendered by [maya](https://github.com/1ay1/maya). Process management via `posix_spawn` + `poll(2)`. File writes are atomic (`write` + `fsync` + `rename`).

Deep dive: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) · [`docs/RENDERING.md`](docs/RENDERING.md)

</details>

<details>
<summary><b>Releasing (maintainers)</b></summary>

Cutting a release is one command:

```bash
scripts/cut-release.sh X.Y.Z      # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z     # Windows cmd.exe
```

It bumps `project(agentty VERSION …)` in `CMakeLists.txt` (the single source
of truth every manifest derives from), promotes `CHANGELOG.md`'s `[Unreleased]`
section to `[X.Y.Z]`, commits, tags `vX.Y.Z`, and pushes. The tag push fires
GitHub Actions, which builds every binary + OS package (Linux x86_64/aarch64
on native runners, macOS Intel/ARM, Windows `.exe`/`.msi`) and auto-submits to
winget, Homebrew, Scoop, and the AUR — nix/snap/gentoo manifests are attached
to the release. `--dry-run` previews without writing anything.

</details>

## Community

Join the [**Discord**](https://discord.gg/qhb9AZ8f3c) to ask questions, share sessions, and get help. The server has an AI helper bot that answers agentty questions using the real agent — `@mention` it, DM it, or use `/ask`.

## License

MIT — see [LICENSE](LICENSE).
