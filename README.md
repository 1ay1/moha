# moha

A native terminal client for Claude. C++26, no Electron / Node / Python in the loop.

<p align="center">
  <img src="moha.png" alt="moha" />
</p>

- **One binary.** Statically linked except libc; spawns in milliseconds, no JIT warmup, no GC pauses mid-stream.
- **Read every line.** The reducer is one `std::visit` over a closed event sum. The permission trust matrix is a `constexpr` function with `static_assert`s — change a policy cell and the build breaks, not a test that nobody runs.
- **Sandboxed tools.** `bash` and `diagnostics` execute inside `bwrap` (Linux) / `sandbox-exec` (macOS). Workspace + system libs + network are reachable; `~/.ssh`, `/etc`, other projects are read-only. Even an approved bash call can't `cat ~/.ssh/id_rsa`.
- **Workspace boundary.** Filesystem tools refuse paths outside the directory you launched from. `--workspace /` opts out.
- **Inline render.** Lives at the bottom of your terminal, preserves scrollback, doesn't take over the screen.

## Install

```bash
git clone --recursive git@github.com:1ay1/moha.git
cd moha
cmake -B build && cmake --build build
./build/moha
```

GCC 14+ / Clang 18+, CMake 3.28+. Auth happens in-app on first launch.

## What ships

- **Streaming** with mid-stream input queuing — type while the model answers, your message lands when it's done.
- **Threads** persisted under `~/.moha/`. Browse / fork / delete from `^J`.
- **Markdown** with syntax-highlighted code blocks.
- **Tools** — `read`, `write`, `edit`, `bash`, `grep`, `glob`, `list_dir`, `find_definition`, `web_fetch`, `web_search`, `todo`, `diagnostics`, `git_*`. Each one gets a purpose-built widget: diffs render as diffs, search results group by file with line numbers, bash shows exit codes, todos become checklists.
- **Permission profiles** — `Write` (autonomous), `Ask` (prompt before any Exec/WriteFs/Net call), `Minimal` (prompt for everything except Pure). Cycle with `S-Tab`.
- **Auth, in-app.** Paste an API key (`sk-ant-…`) or OAuth against your Claude Pro/Max subscription. Credentials live at `~/.config/moha/` with `0600` perms (POSIX) / restrictive ACLs (Windows). `ANTHROPIC_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`, and `-k` still work.

## Keys

```
Enter      send                   ^K     command palette
Alt+Enter  newline                ^J     thread list
Ctrl+E     expand composer        ^T     todo / plan
Esc        cancel / reject        ^/     model picker
S-Tab      cycle profile          ^N     new thread
                                  ^C     quit
```

## How it works

Pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. Strong ID newtypes (`ToolCallId`, `ThreadId`, `OAuthCode`, `PkceVerifier`) — swapping arguments is a compile error, not a debugging session.

View is a single function `Model -> Element`. Rendering is delegated to [maya](https://github.com/1ay1/maya), a sister header-mostly TUI engine — moha builds widget Configs from `Model` state, maya owns every chrome glyph, layout decision, and breathing animation. The host constructs no Elements.

Subprocess uses `posix_spawn` + `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX, `CreateProcessW` + a reader thread on Windows — no GNU `timeout` dependency, no `popen` quoting hazards. File writes are atomic (`write` + `fsync`/`_commit` + `rename`/`MoveFileExW`).

Deep dive: [`docs/RENDERING.md`](docs/RENDERING.md) walks the view pipeline turn-by-turn; [`docs/UI.md`](docs/UI.md) is the per-widget Config reference.

## Standalone build

```bash
cmake -B build -DMOHA_STANDALONE=ON
```

Statically links OpenSSL + nghttp2 + libstdc++ + libgcc when their `.a` archives are installed. libc stays dynamic on Linux/macOS (fully-static glibc breaks `getaddrinfo` and the NSS resolver). Pass `-DMOHA_FULLY_STATIC=ON` with a musl toolchain for a 100% static binary. Windows: implies `/MT` and pulls third-party libs from the `x64-windows-static` vcpkg triplet.

So the accurate one-liner: **statically linked except libc and (usually) OpenSSL.**

## Status

Pre-1.0. Core loop, tools, streaming, permission profiles, in-app auth, persistence, and cross-platform subprocess all work and are built daily.

Stubbed honestly:
- **Checkpoint restore** — `CheckpointId` + per-message marker exist; `RestoreCheckpoint` currently surfaces "not implemented yet" and does nothing.
- **Diff review pane** — modal renders, but `pending_changes` isn't populated by any tool yet, so review/accept/reject toast "no pending changes".

Linux gets daily smoke testing. macOS + Windows code paths exist (`#ifdef` branches throughout, `posix_spawn` for POSIX, `CreateProcessW` for Windows, `fdatasync`/`fsync` switched per OS); CI for those platforms is next.

File terminal-rendering bugs with `$TERM`, your terminal emulator name, and a screenshot. Code-path bugs welcome too — paste the relevant block and `git rev-parse HEAD`.

## License

MIT.
