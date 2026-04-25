# moha

A native terminal client for Claude — written in modern C++26, rendered on a custom TUI engine, no Electron / Node / Python runtime in the loop.

<p align="center">
  <img src="maya.png" alt="moha" />
</p>

```
moha = the model + maya (the renderer) + you
```

For people who'd rather have a single binary they can read end-to-end than a 200 MB Electron app — and who care about being able to audit every code path that handles their credentials, their filesystem, and their tool calls.

## Install

```bash
git clone --recursive git@github.com:1ay1/moha.git
cd moha
cmake -B build && cmake --build build
./build/moha
```

Build needs GCC 14+ or Clang 18+ (`std::expected`, `std::format`, designated init through templates) and CMake 3.28+. Auth happens in-app on first launch.

## What ships

- **Live streaming** with mid-stream input queuing — type while the model is answering, your message submits when it lands.
- **Threads on disk** under `~/.moha/`. Browse / fork / delete from `^J`.
- **Markdown** with syntax-highlighted code blocks in the assistant pane.
- **Tools**: `read`, `write`, `edit`, `bash`, `grep`, `glob`, `list_dir`, `find_definition`, `web_fetch`, `web_search`, `todo`, `diagnostics`, and the `git_*` family. Each tool gets a purpose-built widget — diffs render as diffs, search results group by file with line numbers, bash shows exit codes, todo lists become checklists.
- **Permission profiles** — `Write` (autonomous), `Ask` (prompt before any Exec/WriteFs/Net call), `Minimal` (prompt for everything except Pure). Cycle with `S-Tab`. The trust matrix is a `constexpr` function with `static_assert`s proving every (Effect × Profile) cell — refactoring the policy fails the build, not a test that nobody ran.
- **Workspace boundary** — every filesystem tool refuses paths outside the directory you launched from. `--workspace <dir>` widens it; `--workspace /` disables the gate for users who explicitly want unrestricted access.
- **Auth, in-app**. First launch opens a modal with two paths: paste an API key (`sk-ant-…`), or run OAuth against your Claude Pro/Max subscription. Credentials live in `~/.config/moha/` with `0600` perms (POSIX) / restrictive ACLs (Windows). Env-var overrides (`ANTHROPIC_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`) and `-k` flag still work.
- **Inline render mode** — moha lives at the bottom of your terminal, preserves scrollback, doesn't take over the screen.

## Keys

```
Enter      send                   ^K     command palette
Alt+Enter  newline                ^J     thread list
Ctrl+E     expand composer        ^/     model picker
Esc        cancel / reject        ^N     new thread
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
                 │   io/        │── HTTP/2, SSE, filesystem
                 └──────┬───────┘
                        │ JSON over HTTPS
                        ▼
                 ┌──────────────┐
                 │  Anthropic   │
                 └──────────────┘
```

Pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. The reducer is a single `std::visit` over a closed sum of every event the runtime can process. Strong ID newtypes (`ToolCallId`, `ThreadId`, `OAuthCode`, `PkceVerifier`) — swapping two arguments to `exchange_code(code, verifier, state)` is a compile error, not a debugging session.

Subprocess execution uses `posix_spawn` + `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX, and `CreateProcessW` with a separate reader thread on Windows — no dependency on GNU `timeout`, no `popen` quoting hazards. File writes are atomic (`write` + `fsync`/`_commit` + `rename`/`MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`).

The renderer ([maya](https://github.com/1ay1/maya)) is a sister project — yoga-flavored flexbox with a typestate DSL, shipped as a header-mostly C++26 library. moha is what happens when you point it at a streaming chat model and give it tools.

## Runtime dependencies

The honest version of "no runtime dependencies." Default build:

- `libc` — glibc / musl on Linux, libSystem on macOS, MSVCRT on Windows. Always dynamic.
- `libssl` + `libcrypto` (OpenSSL), `libnghttp2`, `libstdc++`, `libgcc` — dynamic by default; must be present on the target machine.

Standalone build (`cmake -B build -DMOHA_STANDALONE=ON`):

- OpenSSL + nghttp2 statically linked when their `.a` archives are installed.
- `libstdc++` + `libgcc` folded in via `-static-libstdc++ -static-libgcc`.
- `libc` stays dynamic on Linux — fully-static glibc breaks the NSS resolver and `getaddrinfo`. Use `MOHA_FULLY_STATIC=ON` with a musl toolchain if you need a 100% static binary.
- macOS: `libSystem` stays dynamic (Apple ABI requires it). Third-party libs static.
- Windows: `MOHA_STANDALONE` implies `/MT` (static MSVCRT). Third-party libs from `x64-windows-static` vcpkg triplet.

So the accurate one-liner is: **statically linked except libc and (usually) OpenSSL.** No JIT, no script runtime, no headless browser, no garbage collector pause mid-stream.

## Status

Pre-1.0. Core loop, tools, streaming, permission profiles, in-app auth, persistence, picker overlays, and cross-platform subprocess all work and are built daily.

Honest about what's stubbed:

- **Checkpoint restore** — the `CheckpointId` type and the per-message marker exist; `RestoreCheckpoint` currently surfaces "not implemented yet" and does nothing. Coming.
- **Diff review pane** — the modal renders, but `pending_changes` is never populated by any tool today, so `Review changes` / `Accept all` / `Reject all` toast "no pending changes" rather than diffing your edits. Will land alongside checkpoint restore.

Cross-platform paths exist (`#ifdef _WIN32` branches throughout, `posix_spawn` for POSIX, `CreateProcessW` for Windows, `fdatasync`/`fsync` switched per OS), but only Linux gets daily smoke-testing. CI for macOS + Windows is next on the list — bug reports from those platforms welcome.

File terminal-rendering bugs with `$TERM`, your terminal emulator name, and a screenshot. Code-path bugs welcome too — paste the relevant block and `git rev-parse HEAD`.

## License

MIT.
