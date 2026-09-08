# Packaging

agentty ships as a single fully-static binary, so every package below simply
installs the published GitHub release artifact for the target arch — no source
build. **Versioning is centralized**: the single source of truth is
`project(agentty VERSION X.Y.Z)` in `CMakeLists.txt`. `scripts/release.sh`
reads that line and rewrites the version (and pins per-arch checksums) into
every manifest at release time. Never hardcode a version in a manifest.

## Cutting a release (the ONE manual step)

```sh
scripts/cut-release.sh X.Y.Z          # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z         # Windows cmd.exe
```

That's the whole ritual. The script bumps `project(agentty VERSION …)` in
`CMakeLists.txt`, promotes `CHANGELOG.md`'s `[Unreleased]` section to
`[X.Y.Z]`, commits `release: vX.Y.Z`, creates the annotated tag, and pushes
branch + tag. **The tag push is what fires everything downstream** —
`.github/workflows/release.yml` then builds every binary, every OS package,
and submits to winget/homebrew/scoop/AUR (plus attaches nix/snap/gentoo
manifests), all in the cloud with no further input.

Guards: refuses a downgrade or duplicate version, requires a clean tree, and
rejects a tag that already exists. Preview with `--dry-run` (writes nothing);
commit+tag without pushing with `--no-push`.

> `scripts/release.sh` is a different tool — it *builds* release artifacts
> locally on the host it runs on (used by CI and for local reproduction). You
> don't run it by hand to ship; `cut-release` + CI does that for you.

## Install matrix

Linux

| Distro / manager   | Command                     | Manifest                        |
|--------------------|-----------------------------|---------------------------------|
| Ubuntu / Debian    | `apt-get install agentty`   | `deb/` (`.deb` via `build.sh`)  |
| Arch / Manjaro     | `pacman -S agentty`         | `arch/PKGBUILD` (AUR)           |
| Arch (from source) | `yay -S agentty-git`        | `arch/agentty-git/PKGBUILD` (AUR) |
| Fedora             | `dnf install agentty`       | `rpm/agentty.spec.in`           |
| CentOS / RHEL      | `yum install agentty`       | `rpm/agentty.spec.in`           |
| openSUSE           | `zypper install agentty`    | `rpm/agentty.spec.in` (same rpm)|
| Alpine             | `apk add agentty`           | `alpine/APKBUILD`               |
| Snap               | `snap install agentty`      | `snap/snapcraft.yaml.in`        |
| Nix                | `nix-env -iA agentty`       | `nix/default.nix`               |
| Gentoo             | `emerge agentty`            | `gentoo/agentty-9999.ebuild`    |

macOS / Windows

| Platform | Command                | Manifest                    |
|----------|------------------------|-----------------------------|
| macOS    | `brew install agentty` | `homebrew/agentty.rb`       |
| Windows  | `scoop install agentty`| `scoop/agentty.json`        |
| Windows  | `winget install agentty`| `winget/*.yaml`            |
| Windows  | `.msi` installer       | `windows/agentty.wxs`       |

Universal (no package manager):

```sh
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
```

## AUR: agentty-bin vs agentty-git

Two AUR packages, two audiences (both `provides=agentty` / `conflicts=agentty`,
so they swap cleanly):

| | `agentty-bin` | `agentty-git` |
|---|---|---|
| What installs | prebuilt static release binary | built from **HEAD** on the user's machine |
| Linking | fully static (openssl et al. baked in) | **dynamic** against Arch's `openssl`, `libnghttp2`, `gcc-libs` |
| Version | last tagged release | whatever master is at build time |
| Security updates for deps | wait for our next release | arrive via normal `pacman -Syu` |
| AUR account | `0xBAAAAAAD` | `1ay1` (key: `~/.ssh/neowall_aur`, override `AUR_GIT_KEY`) |
| Updated by | CI `publish-aur` on tag push / `bump.sh` step 6 | `bump.sh` step 6b |

`arch/agentty-git/PKGBUILD` design notes (read before touching it):

- **Everything is fetched by makepkg, nothing by the build.** The four git
  submodules are in `source=()` and rewired to makepkg's local mirrors in
  `prepare()`; the three CMake FetchContent deps (nlohmann_json, simdjson,
  mimalloc) ship as sha256-checked tarballs fed through
  `FETCHCONTENT_SOURCE_DIR_*`, and `FETCHCONTENT_FULLY_DISCONNECTED=ON`
  makes a network touch in `build()` a hard error. This is both AUR
  etiquette and what lets reviewers audit the sources list.
- **LTO is off twice** — `options=('!lto')` strips makepkg's injected
  `-flto=auto`, and `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` stops the
  project's own Release IPO gate. GCC 16's LTO streamer ICEs on this C++26
  tree (`tree code 'decltype_type' is not supported in LTO streams`). The
  gate in `cmake/AgenttyStandalone.cmake` respects an explicitly user-set
  value for exactly this reason; re-test before ever re-enabling.
- **`pkgver=` in the file is cosmetic.** `pkgver()` re-derives
  `X.Y.Z.rN.g<hash>` from `git describe` at build time, so users always
  track HEAD even when the checked-in value (and the AUR page) lag.

### What a release means for agentty-git

Nothing breaks for users — their next build simply picks up the new HEAD.
Two maintenance concerns exist, and `scripts/bump.sh` step **6b** handles
both between the agentty-bin sync and the tap/bucket pushes:

1. **Pin drift (the real hazard).** The PKGBUILD's `_json_tag` /
   `_simdjson_tag` / `_mimalloc_tag` must equal the tags pinned in
   `cmake/AgenttySubmodules.cmake` (`GIT_TAG` lines + `AGENTTY_MIMALLOC_TAG`).
   If a release moves a pin without updating the PKGBUILD, **every**
   from-source AUR build fails on users' machines. bump.sh extracts both
   sides and hard-stops the release on mismatch — updating the tags *and*
   recomputing the tarball `sha256sums` is deliberately a human step
   (`curl -sL <tarball-url> | sha256sum`).
2. **Stale AUR page.** The version shown on aur.archlinux.org comes from the
   last-pushed `.SRCINFO`. bump.sh regenerates it at the fresh tag and
   pushes, and commits the same `pkgver=` back to
   `packaging/arch/agentty-git/PKGBUILD` so the two never diverge.

Manual resync (rarely needed — e.g. a PKGBUILD-only fix between releases):

```sh
cd dist/aur/agentty-git   # clone ssh://aur@aur.archlinux.org/agentty-git.git if absent
cp ../../../packaging/arch/agentty-git/PKGBUILD .
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO && git commit -m "agentty-git <ver>-1"
GIT_SSH_COMMAND="ssh -S none -i ~/.ssh/neowall_aur -o IdentitiesOnly=yes" git push origin master
```

(`-S none` matters: `~/.ssh/config` enables ControlMaster multiplexing, which
can silently reuse a session authenticated as the *other* AUR account.)
Always verify a PKGBUILD change with a clean local `makepkg -f` in a scratch
directory before pushing.

## How the version flows

```
CMakeLists.txt  project(agentty VERSION X.Y.Z)
        │
        ▼  scripts/release.sh reads VERSION, builds binaries, computes SHA256SUMS
        │
        ├─ deb/rpm/arch  → built directly with $VERSION
        ├─ alpine  APKBUILD      (@VERSION@ → $VERSION, sha512 pinned)
        ├─ nix     default.nix   (@VERSION@ + sha256 pinned)
        ├─ snap    snapcraft.yaml (@VERSION@ → $VERSION)
        ├─ gentoo  agentty-$VERSION.ebuild (version lives in filename / PV)
        ├─ homebrew agentty.rb   (version + all 4 sha256 pinned)
        └─ scoop   agentty.json  (version + win sha256 pinned)
```

Generated, version-pinned manifests land in `dist/packaging/` for publishing
to their respective repositories (AUR, alpine aports, nixpkgs, snapcraft,
a Gentoo overlay, the Homebrew tap, the scoop bucket).

## Automated publishing (CI)

`.github/workflows/release.yml` runs on every `vX.Y.Z` tag push. Besides
building + uploading all binaries and OS packages, it opens/pushes the
downstream package updates automatically — each step is **gated on a secret**
and skips silently when that secret is absent, so the release never fails just
because a channel isn't configured yet.

| Job / channel        | What it does                                   | Secret needed   |
|----------------------|------------------------------------------------|-----------------|
| `publish-winget`     | PR to microsoft/winget-pkgs                    | `WINGET_TOKEN`  |
| `publish-homebrew`   | push formula to 1ay1/homebrew-tap              | `TAP_TOKEN`     |
| `publish-scoop`      | push manifest to 1ay1/scoop-bucket             | `SCOOP_TOKEN`   |
| `publish-aur`        | push agentty-bin PKGBUILD + .SRCINFO to AUR    | `AUR_SSH_KEY`   |
| `package-alpine`     | build `.apk`, attach to the release            | *(none)*        |
| `publish-manifests`  | pin + attach nix/snap/gentoo manifests         | *(none)*        |

Secrets:

- **`WINGET_TOKEN` / `TAP_TOKEN` / `SCOOP_TOKEN`** — GitHub PAT (classic,
  `public_repo` / `repo` scope) that can push to the respective repo.
- **`AUR_SSH_KEY`** — an SSH private key whose public half is registered on
  the AUR account that owns `agentty-bin`.

Every job derives the version from the CMake `project(agentty VERSION …)` line
(via the `prepare` job's `version` output) and pins checksums from the release
`SHA256SUMS` — so tagging is the *only* manual step. `nixpkgs`, `snapcraft`,
and a Gentoo overlay aren't auto-PR'd (they need human review / store login);
their pinned manifests are attached to the release for a one-command submit.
