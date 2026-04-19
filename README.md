# moha

A native terminal client for Claude — written in modern C++26, rendered on a custom TUI engine, no Electron, no Node, no browser.

<p align="center">
  <img src="maya.png" alt="moha" />
</p>

```
moha = the model + maya (the renderer) + you
```

## What it is

A single ~5 MB binary that gives you Claude in your terminal with the tools you'd expect from an agentic coding assistant — `read`, `write`, `edit`, `bash`, `grep`, `glob`, `git_*`, `web_fetch`, and friends — wired through an Elm-style architecture so the state machine is small enough to reason about.

Auth is yours: an API key, an OAuth token from a Claude Pro / Max subscription, or `moha login` to set it up interactively.

## Install

```bash
git clone --recursive git@github.com:1ay1/moha.git
cd moha
cmake -B build && cmake --build build
./build/moha login
./build/moha
```

Requires GCC 14+ or Clang 18+ (C++26 modules), CMake 3.28+, and a terminal that speaks SGR. That's it — no runtime dependencies.

## Why

- **Native.** One binary. Cold-starts in milliseconds. No JIT, no V8, no garbage collector pause mid-stream.
- **Fast.** 30 fps inline rendering with row-diff serialization and SIMD canvas comparisons. Typing never lags behind the model.
- **Honest about state.** Pure functional update loop (`Model -> Msg -> (Model, Cmd)`). Strong ID types prevent the kinds of bugs where you pass a `ToolCallId` where a `ThreadId` belongs.
- **Calm UI.** Phase-aware status pills, transparent overlays, terminal-default colors — it respects your color scheme instead of fighting it.
- **Yours.** Your credentials, your filesystem, your git history. Nothing leaves the machine except the API call.

## Features

### Conversation

- **Live streaming** with token-by-token delta application — partial responses are persisted on error, never lost.
- **Type while it's typing.** New input is queued during a stream and submitted automatically when the model finishes; no interrupt needed.
- **Threads on disk.** Every conversation auto-saves to `~/.moha/`. Reopen, fork, or delete from the thread list (`^J`).
- **Checkpoints.** Each user message marks a restorable point — rewind without losing surrounding context.
- **Markdown rendering** in the assistant pane, with syntax-aware code blocks.

### Tools, with widgets

Every tool has a purpose-built card so output stays scannable instead of scrolling away into a wall of text.

| Tool | Widget |
|---|---|
| `read` `list_dir` | bordered file viewer with line numbers and truncation hint |
| `write` `edit` | unified diff with `+`/`-` gutters |
| `bash` `diagnostics` | command + exit code + truncated output, expandable |
| `grep` `glob` `find_definition` | grouped match list per file with line numbers |
| `git_status` | branch, ahead/behind, staged/modified/untracked counts |
| `git_log` | inline graph with hash, author, time, message |
| `git_diff` | full diff view with hunk navigation |
| `git_commit` | message preview before staging |
| `web_fetch` `web_search` | status code + content type + body preview |
| `todo` | checklist modal with completion state |

Cards collapse once a tool is done. Hit the tool to expand; status icons (`◐ running`, `✓ done`, `✗ failed`) tell you what happened at a glance.

### Safety

- **Three profiles** — `Write` (autonomous), `Ask` (prompt before edits/shell), `Minimal` (prompt for everything). Cycle with `Shift+Tab`.
- **Inline permission cards** show the exact arguments — file path, bash command, write contents — before you approve. Approve once, approve always, or reject. Esc rejects.
- **Diff review pane** (`^K → review changes`) lets you accept/reject hunks file by file before they touch disk.

### Switching context

- **Model picker** (`^/`) fetches the available model list live from the API. Star favorites; switch mid-thread without losing it.
- **Thread list** (`^J`) — browse, restore, or delete past sessions.
- **Command palette** (`^K`) — fuzzy access to every action without remembering the keymap.
- **Composer expand** (`Ctrl+E`) — flips the input from 3 lines to 8; pasted multiline content auto-expands.

### Status bar that earns its row

A single `maya::ActivityBar` shows what matters and nothing else: current model, token in/out, context window %, phase pill (`Idle / Streaming / AwaitingPermission / ExecutingTool`), profile dot, and the most recent error or status hint inline.

### Auth, your way

- **`moha login`** — interactive OAuth via your Claude Pro / Max subscription.
- **`ANTHROPIC_API_KEY`** or **`CLAUDE_CODE_OAUTH_TOKEN`** env vars — picked up automatically.
- **`-k`** / **`--key`** flag — one-shot override for this session.
- Credentials are written to `~/.config/moha/` with restrictive file modes; nothing is sent anywhere except `api.anthropic.com`.

### Polish details

- Inline render mode preserves your scrollback — moha doesn't take over the terminal, it lives at the bottom of it.
- Background respects terminal default — overlays don't paint over your colorscheme.
- UTF-8-correct cursor + backspace in the composer.
- Spinners and phase glyphs use the same color as the active state — visual continuity instead of decoration.

## Keys

```
Enter      send                   ^K     command palette
Alt+Enter  newline                ^J     thread list
Ctrl+E     expand composer        ^/     model picker
Esc        reject permission      ^N     new thread
S-Tab      cycle profile          ^C     quit
```

## Architecture

```
                 ┌──────────────┐
  keystrokes ──> │   maya       │── ANSI ──▶ terminal
                 │   (TUI)      │
                 └──────┬───────┘
                        │ Element tree
                        ▼  
                 ┌──────────────┐
                 │   moha       │── view(Model)
                 │   view/      │
                 ├──────────────┤
                 │   update/    │── (Msg, Model) -> (Model, Cmd)
                 ├──────────────┤
                 │   io/        │── HTTP, SSE, filesystem
                 └──────┬───────┘
                        │ JSON over HTTPS
                        ▼  
                 ┌──────────────┐
                 │  Anthropic   │
                 └──────────────┘
```

The renderer ([maya](https://github.com/1ay1/maya)) is a sister project — a yoga-flavored flexbox layout engine with a typestate-checked DSL, shipped as a header-mostly C++26 library. moha is what happens when you point it at a streaming chat model and give it tools.

## Status

Pre-1.0 and moving fast. The core loop, tools, streaming, permissions, OAuth, persistence, and the picker overlays all work. Expect rough edges around obscure terminals; file an issue with `$TERM` and a screenshot.

## License

MIT.
