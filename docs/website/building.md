---
title: Building from Source
description: Compile agentty with CMake, including the standalone static build.
nav_section: Advanced
nav_order: 60
slug: building
---

agentty builds with CMake and a C++26 toolchain. Cutting a release is a single command that tags and pushes; GitHub Actions builds every binary and OS package.

## Requirements

- GCC 14+ / Clang 18+ / MSVC 14.40+ (`/std:c++latest`)
- CMake 3.28+
- OpenSSL and nghttp2 (FetchContent pulls maya automatically)

:::warn
AppleClang tops out at C++23 — building the tests (`AGENTTY_BUILD_TESTS`) requires `g++` or stock LLVM `clang++` on macOS, not Xcode's bundled toolchain.
:::

## Basic build

```bash
git clone --recursive git@github.com:1ay1/agentty.git
cd agentty
cmake --preset release
cmake --build build -j
./build/agentty
```

:::tip Use the presets, not a hand-rolled `-B build`
`cmake -B build` **ignores `CMakePresets.json`** and gives you a Release +
LTO + Makefiles configuration — and because every preset now uses that same
`build/` directory, a hand-rolled configure *overwrites* your preset tree with
a Makefile one (Ninja and Make cannot share a directory; CMake will refuse
until you delete it). Always go through `--preset`. If you are about to change
code, jump to [The development loop](#the-development-loop) instead.
:::

## The development loop

If you are editing agentty, this is the section that matters. The `debug`
preset — the default — is Ninja + Debug + ccache + lld with no LTO, and it
turns the edit→build→test cycle from ~36 seconds into under two:

```bash
cmake --preset debug                           # once
cmake --build build --target agentty -j 12     # after each edit
./build/agentty
```

Pass `--target agentty`. Without it the build also makes every example,
benchmark and submodule tool — measured at 19.6 s versus 0.65 s for the one
target you actually care about.

**Measured on a 12-core Linux box**, changing one `.cpp` and rebuilding:

| Configuration | Incremental rebuild |
|---------------|--------------------:|
| Release + LTO | **36.2 s** |
| `cmake --preset debug` (Debug + Ninja + ccache + lld) | **1.7 s** |

That is a **21×** difference, and it is almost entirely the link step: LTO must
re-optimize the whole binary for a one-line change, while a Debug link is
nearly free. The first `debug` build is a normal cold compile (~5 min); every
one after it is sub-second.

Four things make it fast, all already configured:

- **Ninja** — a much tighter dependency graph than Make, and it parallelises
  the objlib fan-out properly.
- **No LTO** — the single biggest cost in an incremental Release build.
- **`-g1`** — line tables, no local-variable DWARF. Backtraces and breakpoints
  work; `print localvar` does not. Use `--preset debug-full` on the days you
  need it.
- **ccache** — auto-detected by `cmake/AgenttyToolchain.cmake`. Switching
  branches or rebasing mostly hits the cache instead of recompiling.

### Running tests fast

Tests live in one consolidated binary plus a handful of standalone ones. Build
and run only what your change touches:

```bash
cmake --build build --target agentty_tests -j 12   # ~1.3 s incremental
./build/agentty_tests -tc="*framing*"              # ~0.16 s
```

`-tc` takes a doctest pattern and matches on test-case *names*, so
`-tc="*conformance*"` or `-tc="*framing*"` narrows to one area. A pattern that
matches nothing reports `0 passed` and still exits 0 — check the case count.

A few suites are separate executables (they need their own process to set
environment before anything initialises). Run those through ctest by name:

```bash
cd build && ctest -R "logx"      # logx_test, logx_redaction_test, logx_format_test
```

For the pre-commit gate, run everything **except** the three long fuzz/replay
tests:

```bash
cd build
ctest -j 8 -E "reveal_scrollback_test|scrollback_wire_fuzz|frozen_invariant_fuzz"
```

| Scope | Tests | Time |
|-------|------:|-----:|
| One case (`-tc=…`) | 1–2 | **0.16 s** |
| Everything but the slow three | 432 | **26 s** |
| Full suite | 435 | **110 s** |

Those three account for nearly all of it — `reveal_scrollback_test` alone is
120 s of CPU, `scrollback_wire_fuzz` 86 s, `frozen_invariant_fuzz` 50 s. They
are deep property/fuzz runs worth having in CI and rarely worth waiting for
locally. Everything else is sub-second.

:::warn A green run proves nothing if nothing ran
Check the assertion count, not just the status line. doctest happily reports
`8 passed` for eight cases that asserted zero times — which is exactly what
happened when a set of log tests guarded on an environment variable the suite
did not set. `assertions: 0` is the tell.
:::

### The other presets

```bash
cmake --preset debug-full  # Debug + full DWARF, for `print localvar` in gdb
cmake --preset release     # -O3 + thin LTO — the optimized local binary
cmake --preset ci          # mirrors the Linux CI gate
cmake --preset sanitizer   # ASan + UBSan over agentty's own-logic set
cmake --preset standalone  # portable binary, no third-party shared libs
```

Every preset builds into the **same** `build/` tree. Switching preset
reconfigures that tree in place rather than leaving a second multi-gigabyte
copy on disk — `cmake --preset release` after `--preset debug` flips the build
type, and flipping back is a no-op because ccache still holds the objects.
If you genuinely need two configurations live at once, override the directory:
`cmake --preset debug -B build-other`.

## Standalone (static) build

```bash
cmake -B build -DAGENTTY_STANDALONE=ON
```

Statically links OpenSSL + nghttp2 + libstdc++ + libgcc when their `.a` archives are installed, while libc stays dynamic. For a 100% static binary that runs on any Linux userland, pass `-DAGENTTY_FULLY_STATIC=ON`.

The prebuilt Linux release binaries are **true standalone executables**: linked `-static -no-pie` into a classic `ET_EXEC` with no `NEEDED` entry and no `PT_INTERP`, so one file runs on glibc (Debian/Ubuntu/Fedora), musl (Alpine), and 64-bit Raspberry Pi OS alike. A build-time ELF-shape assertion (`cmake/assert_static_pie.cmake`) hard-fails the compile if the artifact ever regains a dynamic dependency. Termux/Android needs a PIE — build that with the opt-in `-DAGENTTY_STATIC_PIE=ON` on a musl toolchain.

## Optimized builds

Release builds already ship with link-time optimization and a stripped symbol table. Two opt-in levers squeeze out more, for a local build you run yourself:

```bash
# mimalloc allocator — measurable keystroke-latency win on the
# allocation-heavy render/parse paths. Not enabled on the fully-static
# release binaries (allocator override under static musl is a hazard).
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAGENTTY_USE_MIMALLOC=ON

# Profile-guided optimization (two phases). Phase 1 builds an instrumented
# binary and runs a scripted PTY workload over the hot paths; phase 2
# rebuilds using the collected counters.
cmake -B build-pgogen -DCMAKE_BUILD_TYPE=Release -DAGENTTY_PGO=generate
cmake --build build-pgogen -j$(nproc) --target agentty
scripts/pgo-train.sh build-pgogen/agentty
cmake -B build-pgouse -DCMAKE_BUILD_TYPE=Release -DAGENTTY_PGO=use
cmake --build build-pgouse -j$(nproc) --target agentty
```

See [Performance](/docs/performance) for what each buys you.

## Cutting a release (maintainers)

```bash
scripts/cut-release.sh X.Y.Z       # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z       # Windows cmd.exe

scripts/cut-release.sh X.Y.Z --dry-run   # preview the exact diff, write nothing
```

Single source of truth: `CMakeLists.txt`'s `project(agentty VERSION …)` line. `cut-release.sh` bumps it, promotes `CHANGELOG.md`'s `[Unreleased]` section to a dated `[X.Y.Z]`, commits `release: vX.Y.Z`, tags `vX.Y.Z`, and pushes. The tag push fires GitHub Actions, which builds every binary + OS package (Linux x86_64/aarch64 on native runners, macOS Intel/ARM, Windows exe/msi) and auto-submits to winget, Homebrew, Scoop, and the AUR — nix/snap/gentoo manifests are attached to the release. Guards refuse a downgrade, duplicate version, dirty tree, or existing tag.

The pipeline is **fully automatic and self-verifying** — after `cut-release.sh` there is nothing left to do by hand:

- A final `verify-release` job runs dead-last and checks each channel's LIVE state (release assets, Homebrew formula, Scoop manifest, AUR `pkgver`, a winget PR for the version). If any channel whose secret is configured did **not** reach the new version, the run goes **red** and names the channel — so a green release genuinely means every channel is up to date. Channels with no secret set are reported as skipped and never fail the gate.
- A separate `reconcile-manifests` workflow re-pins AUR/Homebrew/Scoop from the release's `SHA256SUMS` (no rebuild) **automatically when the release run completes**, and again **weekly** — so if a build leg was slow/flaky and a publisher was skipped, the package still catches up on its own.
- The winget submission gates on `checksums-final` and verifies the MSI hash against `SHA256SUMS` before opening its PR, so it can never submit a hash that drifted from the released asset.
