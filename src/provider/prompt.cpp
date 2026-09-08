// agentty::provider — the SHARED system-prompt + memory assembly (SSOT for
// every provider). Moved out of provider::anthropic, where it was misfiled as
// a Claude-only asset. Pure string building: CLAUDE.md tiers, learned-memory
// blocks, the OS/shell environment stanza, and the full vs lean (subagent)
// instruction text, plus the per-provider overlay seam. Baked into the binary.

#include "agentty/provider/prompt.hpp"

#include "agentty/provider/msg_shared.hpp"   // wire::home_dir / read_capped_file
#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // util::workspace_root/project_root — AGENTS.md anchor + walk start
#include "agentty/util/dbglog.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentty::provider {

namespace {

// Read a text file, swallowing any I/O error — returns empty string on
// missing file or unreadable. Capped at 64 KiB (shared wire::read_capped_file,
// which also trims trailing whitespace so the wrapper tag isn't jammed against
// a blank line).
[[nodiscard]] std::string read_optional_memory(const std::filesystem::path& p) noexcept {
    return wire::read_capped_file(p);
}

// mtime-keyed cache wrapper around read_optional_memory. The CLAUDE.md
// hierarchy is read on every turn (3 files: ~/CLAUDE.md, ./CLAUDE.md,
// ./CLAUDE.local.md), each capped at 64 KiB. Per-turn that's:
//
//   • on a local SSD: ~3 × stat+open+read = sub-millisecond
//   • on an NFS home: ~3 × roundtrip ≈ 30-100 ms before the worker
//     even starts building the request
//
// The contents change between turns at most rarely (the user editing
// their CLAUDE.md mid-session). Cache by filesystem mtime: stat once
// to get last_write_time; if it matches the cached entry, hand back
// the cached body. Process-lifetime cache; bounded by the 3 paths
// collect_memory_blocks looks at and the 64 KiB-per-file content cap,
// so worst case ~192 KiB of process memory.
//
// Concurrency: the system prompt is built on the stream worker thread
// (Cmd::stream's task body). Two concurrent stream calls against the
// same Anthropic endpoint are serialized at the connection-pool layer,
// so there's effectively one writer in practice — but the mutex
// removes "in practice" from the contract.
[[nodiscard]] std::string read_memory_cached(const std::filesystem::path& p) {
    struct Entry {
        std::filesystem::file_time_type mtime{};
        std::string                     content;
    };
    static std::unordered_map<std::string, Entry> cache;
    static std::mutex                              mu;

    const std::string key = p.string();
    std::error_code   ec;
    auto              now_mtime = std::filesystem::last_write_time(p, ec);

    if (ec) {
        // File missing / unreadable — drop any previous cache entry so
        // a re-creation later is observed (the next call will re-stat
        // and miss into the read path).
        std::lock_guard lk(mu);
        cache.erase(key);
        return {};
    }

    {
        std::lock_guard lk(mu);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.mtime == now_mtime) {
            return it->second.content;
        }
    }

    // Cache miss or stale — read outside the lock so a slow NFS read
    // doesn't block other paths' cache hits.
    std::string body = read_optional_memory(p);
    {
        std::lock_guard lk(mu);
        cache[key] = Entry{now_mtime, body};
    }
    return body;
}

// Resolve the user's home directory portably — shared wire::home_dir.
[[nodiscard]] std::filesystem::path home_dir() noexcept {
    return wire::home_dir();
}

// CLAUDE.md memory hierarchy — mirrors Claude Code's resolution
// (binary near offset 134900):
//
//   User    ~/CLAUDE.md             personal, all projects
//   Project <cwd>/CLAUDE.md         committed, project-specific
//   Local   <cwd>/CLAUDE.local.md   gitignored, personal-to-this-project
//
// Each tier is wrapped in its own tag so the model can tell them apart.
// Empty / missing tiers are silently elided. The wire cost is paid once
// per ~5 min cache_control TTL window regardless (the system-prompt
// cache_control breakpoint catches the result); the disk cost is
// memoized through read_memory_cached so the per-turn footprint is
// 3× stat() + memcpy of the cached body, not 3× full read.
[[nodiscard]] std::string collect_memory_blocks() {
    std::string user    = read_memory_cached(home_dir() / "CLAUDE.md");
    std::string project = read_memory_cached(std::filesystem::path{"CLAUDE.md"});
    std::string local   = read_memory_cached(std::filesystem::path{"CLAUDE.local.md"});

    // Agent-authored memory (written by the `remember` tool, removed by
    // `forget`). Loaded as tail-N from the per-scope JSONL stores so the
    // prompt stays bounded even if the on-disk files grow.
    auto learned_user    = tools::memory::load_recent_user();
    auto learned_project = tools::memory::load_recent_project();

    if (user.empty() && project.empty() && local.empty()
        && learned_user.empty() && learned_project.empty()) return {};

    std::ostringstream m;
    m << "\n\n<memory>\n"
      << "Project-specific guidance the user has authored. Treat these "
         "as persistent context for THIS workspace and user; lower tiers "
         "(local, then project, then user) win on conflicting rules.\n";
    if (!user.empty())    m << "<user-memory>\n"    << user    << "\n</user-memory>\n";
    if (!project.empty()) m << "<project-memory>\n" << project << "\n</project-memory>\n";
    if (!local.empty())   m << "<local-memory>\n"   << local   << "\n</local-memory>\n";
    auto emit_learned = [&](const char* tag, std::vector<tools::memory::Record> rs) {
        if (rs.empty()) return;
        // Budget the block so a large memory store can't inflate every
        // system prompt. select_for_prompt keeps all pinned records +
        // the highest-signal remainder within kPromptByteBudget, clipping
        // any over-long record; the rest stay on disk (recallable, still
        // editable) and we note the elided count so the model knows the
        // store holds more than what's shown.
        auto picked = tools::memory::select_for_prompt(std::move(rs));
        if (picked.records.empty() && picked.dropped == 0) return;
        m << "<learned-memory scope=\"" << tag << "\">\n"
          << "Facts you previously stored via the `remember` tool. Each "
             "line is prefixed with the record id — pass that id to "
             "`forget` if the fact is no longer true.\n";
        for (const auto& r : picked.records)
            m << tools::memory::render_for_prompt(r) << "\n";
        if (picked.dropped > 0)
            m << "[+" << picked.dropped << " more stored fact(s) not shown "
                 "here to keep the prompt small — they remain on disk; ask "
                 "about a topic and recall surfaces them, or pin the ones "
                 "that should always be visible.]\n";
        m << "</learned-memory>\n";
    };
    emit_learned("user",    std::move(learned_user));
    emit_learned("project", std::move(learned_project));
    m << "</memory>";
    return m.str();
}

// AGENTS.md — the open AAIF/Linux-Foundation standard for project-scoped
// agent guidance (https://agents.md). Project-scoped only per the published
// spec: no user tier, no local tier. The root file lives at
// <project_root>/AGENTS.md.
//
// This is a thin adapter over the shared wire::agents_md_block helper — the
// SINGLE source of truth for the 3-tier resolution (global → root → nearest
// nested) and the monorepo walk, shared with the OpenAI/Ollama prompts. The
// only Anthropic-specific bit is the file-reader we inject: read_memory_cached
// (a generic mtime-cached reader — the cache key is the path string, so each
// AGENTS.md tier gets its own entry independent of the CLAUDE.md tiers). That
// keeps every tier served from cache, so rebuilding the system prompt on every
// turn costs a handful of stat()s + a memcpy of the cached body rather than N
// full file reads.
//
// Wire shape (precedence low → high): <agents-md-global> (~/.agentty/AGENTS.md,
// optional) → <agents-md> (<workspace_root>/AGENTS.md) → <agents-md-package>
// (nearest nested file, optional; skipped when it is the root file). All are
// injected BEFORE the <memory> block so the standardized public project
// guidance stays visually distinct from the personal CLAUDE.md tiers and the
// model can apply precedence correctly. Sits behind the same Anthropic
// cache_control breakpoint as collect_memory_blocks.
[[nodiscard]] std::string collect_agents_md_block() {
    // workspace_root() is the fixed access boundary (where the root AGENTS.md
    // lives); project_root() is the agent's cwd clamped inside that boundary
    // — the walk start point for finding the nearest nested AGENTS.md.
    return wire::agents_md_block(
        "Project guidance following the open AGENTS.md standard "
        "(agents.md, stewarded by the Agentic AI Foundation under the "
        "Linux Foundation). Treat as authoritative public project "
        "conventions.",
        tools::util::workspace_root(),
        tools::util::project_root(),
        wire::resolve_global_agents_md(),
        &read_memory_cached);
}

} // namespace

std::string default_system_prompt(bool lean) {
#if defined(_WIN32)
    constexpr const char* os_name  = "Windows";
    constexpr const char* shell    = "cmd.exe (Windows Command Prompt)";
    constexpr const char* shell_hint =
        "Prefer native Windows equivalents: `dir` / `where` / `systeminfo` / "
        "`type` / `findstr` / `powershell -c`. Do NOT use POSIX-only tools "
        "like `uname`, `cat /etc/os-release`, `sw_vers`, `ls`, `grep`, `sed`, "
        "`awk`, or shell heredocs (`<<EOF`) — they will fail. "
        "Commands chain with `&&` and `||` under cmd.exe, but path separators "
        "are backslashes and paths with spaces must be quoted.";
#elif defined(__APPLE__)
    constexpr const char* os_name  = "macOS (Darwin)";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `sw_vers` gives macOS version, `uname -a` gives kernel.";
#else
    constexpr const char* os_name  = "Linux";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `/etc/os-release` gives distro info, `uname -a` gives kernel.";
#endif

    std::string cwd;
    // current_path().string() narrows the wide Windows path through the active
    // code page (ANSI), so a non-ASCII path turns into invalid UTF-8 and would
    // poison the whole system prompt on the JSON wire. u8string() converts the
    // wide path to UTF-8 directly — no lossy ANSI round-trip. (No-op on POSIX,
    // where the native encoding is already UTF-8.)
    try {
        auto u8 = std::filesystem::current_path().u8string();
        cwd.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (const std::exception& e) {
        util::dbglog("anthropic.system_prompt.cwd", e.what());
    } catch (...) {
        util::dbglog("anthropic.system_prompt.cwd", "non-std exception");
    }

    std::ostringstream oss;
    oss << "You are agentty, a terminal coding assistant. Act, don't ask. "
        << "When the user says something vague (\"edit it\", \"make it "
        << "better\", \"improve it\", \"make it interesting\", \"fix it\"), "
        << "make a reasonable improvement yourself with `edit` — do NOT "
        << "respond with a list of options or clarifying questions. Keep "
        << "prose short; let tool cards speak for themselves.\n\n"
        << "<file-editing>\n"
        << "  - For ANY change to a file that already exists, use `edit`. "
        << "If the file is in conversation history (you wrote it, or you "
        << "read it earlier), construct `edit.old_text` from memory — do "
        << "NOT re-read it.\n"
        << "  - `write` is for creating NEW files. If the file exists, "
        << "use `edit` — calling `write` on an existing file dumps the "
        << "entire body over the wire and stalls the stream; it is the "
        << "single worst latency choice available to you.\n"
        << "  - If a `write` fails with \"Output blocked by content "
        << "filtering policy\" (Anthropic's safety classifier — more "
        << "aggressive on OAuth / Pro / Max paths than on direct API "
        << "keys), you can: (a) retry once — the filter is "
        << "probabilistic, (b) write a short stub file first, then "
        << "build it up via successive `edit` calls. Don't loop on the "
        << "same large `write` more than twice.\n"
        << "  - `edit.old_text` must match the file exactly (indentation "
        << "matters; trailing whitespace is tolerated). If unsure, `read` "
        << "the relevant slice first.\n"
        << "  - NEVER shell out (cat/echo/sed/heredoc/printf) for file IO.\n"
        << "  - ALWAYS include a brief `display_description` on `write` "
        << "and `edit`. It paints in the tool card before the long fields "
        << "stream — schemas list `path` and `display_description` first "
        << "for that reason, don't reorder.\n"
        << "</file-editing>\n\n"
        << "<shell>\n"
        << "  - Use the `shell` tool to run commands. It executes each "
        << "command with the operating-system shell named in "
        << "<environment> below — write for THAT shell, not for a shell "
        << "you assume. Explain destructive commands before running "
        << "them.\n"
        << "  - For listing/searching files, prefer the dedicated tools "
        << "(`list_dir`, `glob`, `grep`, `find_definition`) over shelling "
        << "out — they give the UI structured cards.\n"
        << "</shell>\n\n"
        << "<tool-batching>\n"
        << "  - Every model round-trip costs seconds. When your next "
        << "steps are INDEPENDENT (no output of one feeds another), "
        << "emit ALL their tool calls in ONE message \xe2\x80\x94 e.g. "
        << "read 3 files + grep 2 patterns as five parallel calls, not "
        << "five turns. agentty executes safe combinations concurrently "
        << "and serializes the rest, so a wide batch is never wrong.\n"
        << "  - Only sequence when there is a true data dependency "
        << "(read THEN edit what you found). Do not sequence out of "
        << "caution \xe2\x80\x94 the scheduler owns safety, you own width.\n"
        << "</tool-batching>\n\n"
        << "<output-formatting>\n"
        << "  - The TUI renders GFM markdown. A table MUST start its "
        << "header row at the line beginning with `|` and be preceded by "
        << "a blank line. NEVER put lead-in prose on the same line as the "
        << "header (`Layout: | Dir | Role |` renders as a wall of pipes — "
        << "the parser rejects it as a non-table). Write the lead-in as "
        << "its own line, then a blank line, then:\n"
        << "      | Dir | Role |\n"
        << "      |-----|------|\n"
        << "      | a/  | x    |\n"
        << "  - For 2-3 short columns, prefer a simple bulleted list over "
        << "a table — it reads better in a narrow terminal.\n"
        << "  - LaTeX math RENDERS in this terminal (maya's TeX "
        << "typesetter). Use inline `$…$` (or `\\(…\\)`) for symbols/"
        << "variables mid-sentence, and a display block \u2014 `$$…$$` or "
        << "a ```math fence \u2014 for anything 2-D (fractions, \\sqrt, "
        << "\\sum/\\int with limits, matrices, cases). So write real math "
        << "when it helps (`$O(n\\log n)$`, `$$\\frac{-b\\pm\\sqrt{b^2-4ac}}"
        << "{2a}}$$`) instead of ASCII-art or plain-text formulae. Keep "
        << "unknown/exotic macros to a minimum \u2014 they degrade to their "
        << "name rather than rendering.\n"
        << "</output-formatting>\n\n"
        << "<context-economy>\n"
        << "  Every byte you ingest stays in context for the rest of "
        << "the session and pushes the conversation toward auto-"
        << "compaction. Be deliberate:\n"
        << "  - `read` returns up to 2000 lines. For larger files, "
        << "use `offset` + `limit` to page through — read only the "
        << "slice you need, not the whole file. Re-reading the SAME "
        << "(path, offset, limit) returns a 'file unchanged' sentinel "
        << "that you should respect: refer to the earlier tool_result "
        << "instead of re-fetching.\n"
        << "  - Prefer `grep` / `find_definition` over `read` when "
        << "you're looking for a pattern or symbol — they return the "
        << "match + 2 lines of context, not the whole file.\n"
        << "  - `shell` output is capped at 30 KB in your context. "
        << "Outputs larger than that are spilled to a temp file and "
        << "you receive a `<persisted-output>` envelope with a 2 KB "
        << "head + 1 KB tail and the file path. If you need bytes in "
        << "between, `read` the spill path with offset/limit — don't "
        << "re-run the command.\n"
        << "  - `web_fetch` is capped at 20 KB. For long pages, "
        << "fetch ONCE and remember what you saw; don't refetch the "
        << "same URL within a turn.\n"
        << "  - Don't ask for output you don't need. `ls -laR` of a "
        << "deep tree, a 50 K-line build log, or `find . -type f` in "
        << "node_modules will land in your context as one big tool "
        << "result and shorten the session for everyone.\n"
        << "  - Old tool results FADE: once a result is more than a "
        << "handful of tool calls back, only a ~2 KB head+tail of it "
        << "stays on the wire (errors and recent results stay full). "
        << "So when a large read/grep/bash output tells you something "
        << "you'll need later, act on it or note the key fact NOW "
        << "(e.g. `remember`) rather than assuming you can re-scroll the "
        << "full dump ten calls from now.\n"
        << "</context-economy>\n\n"
        << "<big-codebases>\n"
        << "  In a large or unfamiliar repository, call `repo_map` FIRST "
        << "\xe2\x80\x94 one budgeted call returns a PageRank-ranked skeleton "
        << "(top files + definition signatures) that replaces a dozen "
        << "exploratory read/grep rounds. Pass `focus` with your task's "
        << "keywords to re-center the map, then go DIRECT: `read` the "
        << "specific file:line ranges the map surfaced instead of paging "
        << "whole files. For multi-region investigations, fan out "
        << "parallel `task` explorers \xe2\x80\x94 each returns one condensed "
        << "report instead of leaving raw exploration in your context.\n"
        << "</big-codebases>\n\n"
        << "<search-strategy>\n"
        << "  Code search has THREE layers \xe2\x80\x94 pick the cheapest one "
        << "that answers the question, and escalate only when it comes up "
        << "empty. Using a heavier layer than the question needs is the "
        << "main way turns waste context on noise.\n"
        << "  1. LEXICAL (`grep`) \xe2\x80\x94 \"where does this TEXT appear\": an "
        << "exact identifier, string, error message, or token. This is the "
        << "default and the fastest; `grep` is ripgrep-backed and skips "
        << "generated trees. Reach for it first for anything you can name "
        << "literally.\n"
        << "  2. STRUCTURAL (`search_structural`) \xe2\x80\x94 \"where does this "
        << "code SHAPE appear\": a call pattern regardless of arguments "
        << "(`foo($$$)`), a control-flow shape (`if ($$$C) return $X;`), an "
        << "empty catch (`catch ($$$) {}`), a self-assignment (`$X = $X`). "
        << "Metavariables `$X` (exactly ONE node — atom or balanced group) and "
        << "`$$$X` (MANY nodes: arg lists, multi-token conditions) match "
        << "code, and it NEVER matches inside comments or string literals "
        << "\xe2\x80\x94 so it has none of grep's false positives from log "
        << "strings, docstrings, or same-named-but-unrelated tokens. Use it "
        << "the moment a grep is drowning in matches inside comments/strings "
        << "or you're trying to express a pattern, not a literal.\n"
        << "  3. SEMANTIC (`search_code`) \xe2\x80\x94 \"where is the code that DOES "
        << "X\" when you can't name the symbol (\"where is retry backoff "
        << "handled\"). Last resort for in-codebase search: it is the only "
        << "layer that finds code by meaning, but it can go stale and is "
        << "noisier on short keyword queries than grep.\n"
        << "  For a KNOWN symbol: `find_definition` jumps to where it's "
        << "declared; `grep` with word=true finds every whole-word USE (no "
        << "`foo` inside `foobar`); `search_structural 'foo($$$)'` finds "
        << "calls only, skipping comments/strings. Two force-multipliers that "
        << "kill the fetch\xe2\x86\x92" "filter\xe2\x86\x92" "refetch loop: `grep` with "
        << "context:\"block\" returns each hit's WHOLE enclosing function "
        << "(not \xc2\xb1" "2 lines), and `read` with symbol=\"name\" returns "
        << "exactly one function/type's body \xe2\x80\x94 so you almost never "
        << "need line-number math or sed/head/tail. And always let the "
        << "expensive layer NARROW scope, then finish with the cheap one "
        << "\xe2\x80\x94 e.g. `repo_map` to find the 3 files a subsystem lives "
        << "in, then `grep` within them. Never dump whole files when a "
        << "`file:line`, a block, or a symbol read would do.\n"
        << "</search-strategy>\n\n"
        << "<in-house-languages>\n"
        << "  When the repo contains an in-house DSL, config dialect, or "
        << "proprietary framework you don't recognise, do NOT guess its "
        << "syntax from general knowledge \xe2\x80\x94 hallucinated constructs in "
        << "a private language are never caught by your training data. "
        << "Instead: (1) `search_docs` for its documentation \xe2\x80\x94 the "
        << "knowledge index also covers installed skills and remembered "
        << "facts, and a hit on a skill:// path means a skill exists for "
        << "it \xe2\x80\x94 activate it with `skill`. (2) Retrieve REAL examples: "
        << "`grep`/`glob` for existing files in that language and imitate "
        << "their patterns exactly (retrieval-grounded few-shot beats "
        << "recall for low-resource languages). (3) `repo_map` still works "
        << "on DSL files \xe2\x80\x94 identifiers graph even when full parsing "
        << "doesn't. (4) If a grammar/spec file exists (.ebnf, .g4, .proto, "
        << "a SYNTAX.md), read it before writing a single line.\n"
        << "</in-house-languages>\n\n"
        << "<environment>\n"
        << "  os: " << os_name << "\n"
        << "  shell: " << shell << "\n";
    if (!cwd.empty()) oss << "  cwd: " << cwd << "\n";
    oss << "</environment>\n\n"
        << "<shell-notes>\n"
        << shell_hint << "\n"
        << "</shell-notes>\n\n";
    // SUBAGENT lean prompt: stop here. The memory-tools protocol, the
    // user-authored CLAUDE.md tiers, and the skills catalog below are all
    // parent-only concerns — a subagent can't call the memory tools and
    // doesn't drive skills the same way, so shipping them just inflates its
    // billed prefix on every cache miss. Everything above is the operational
    // discipline a subagent genuinely needs to work well.
    if (lean) return oss.str();
    oss << "<memory-tools>\n"
        << "  - If the user asks you to remember something — \"remember "
        << "that...\", \"don't forget X\", \"keep in mind Y\", \"from now "
        << "on...\", \"always do Z\" — you MUST call the `remember` tool. "
        << "Do not just acknowledge in prose; the prose disappears at the "
        << "end of the session, but `remember` persists to "
        << "~/.agentty/memory.jsonl (scope=user) or "
        << "<workspace>/.agentty/memory.jsonl (scope=project) and is "
        << "reloaded into your system prompt on every future turn.\n"
        << "  - Default scope is `project` (this codebase only). Use "
        << "scope=`user` when the fact is about the user themselves "
        << "(\"I prefer fish shell\", \"my name is...\", \"I use vim\") "
        << "and applies across every project.\n"
        << "  - Smart scope: if you omit `scope` (or pick project) for a "
        << "fact that's plainly about the USER \u2014 first-person \"I "
        << "prefer\"/\"my name\", personal tooling \u2014 the tool "
        << "auto-routes it to user scope and notes \"scope\u2192user\" in "
        << "its reply, so a personal preference never bleeds into every "
        << "repo. It only ever corrects project\u2192user, never the "
        << "reverse; pass an explicit `scope` to override.\n"
        << "  - Dedup is automatic: if you `remember` a fact that's "
        << "near-identical to an existing one in the same scope, the "
        << "store refreshes the existing record's timestamp + hit count "
        << "instead of writing a duplicate. Just call `remember` with "
        << "the fact; you don't need to grep <learned-memory> first.\n"
        << "  - Pass `pin=true` for facts the user has explicitly "
        << "emphasised (\"always do X\", \"never do Y\") or that are "
        << "load-bearing for every turn (the build command, a hard "
        << "project convention). Pinned facts survive cap rollover and "
        << "render with ★ in <learned-memory>.\n"
        << "  - Pass `tags=[\"build\", \"picker\"]` when a fact "
        << "belongs to an obvious topic. Tags group facts in the system "
        << "prompt so you can scan by area.\n"
        << "  - When the user CORRECTS a previous fact (\"actually the "
        << "build command is now Z\", \"that's no longer true\"), use "
        << "`remember` with `supersedes=<old-id>` — it atomically writes "
        << "the new record and drops the old one. Cleaner than "
        << "forget-then-remember.\n"
        << "  - Keep each remembered fact short and self-contained: one "
        << "sentence the future-you can act on without re-reading the "
        << "current conversation.\n"
        << "  - If the user asks you to forget something (\"forget X\", "
        << "\"that's no longer true\", \"drop the memory about Y\"), call "
        << "`forget` with either the record id (shown as `[id]` prefix "
        << "in the <learned-memory> block above) or a substring that "
        << "uniquely identifies the fact. Pass `dry_run=true` with a "
        << "substring first when the match might be broad — the tool "
        << "returns the list of records that WOULD be removed.\n"
        << "  - If the user wants a clean slate on this codebase (\"start "
        << "fresh\", \"forget everything you know about this project\", "
        << "\"wipe your memory\"), use `wipe_memory(scope=\"project\")`. "
        << "Call ONCE without `confirm` to preview the count; only after "
        << "the user agrees, re-call with `confirm=true`. `wipe_memory` "
        << "with scope=\"user\" wipes cross-project facts — require "
        << "explicit confirmation before doing that.\n"
        << "  - Do NOT call `remember` proactively for things the user "
        << "didn't ask you to remember. Don't store transient state "
        << "(current file you're editing, today's build error). Store "
        << "durable preferences and project conventions.\n"
        << "  - REFLECTIVE WRITE-BACK is the one exception to the rule "
        << "above: when you FINISH a non-trivial task (root-caused a bug, "
        << "discovered the build/test invocation, learned a hard project "
        << "convention the hard way), distill AT MOST ONE short, durable, "
        << "verified fact from it and store it with `remember` (scope="
        << "project, tag by topic) \xe2\x80\x94 future sessions retrieve it via "
        << "search_docs and proactive retrieval, so today's discovery "
        << "becomes tomorrow's context. The bar: would a fresh session "
        << "waste \xe2\x89\xa5" "10 minutes rediscovering this? If yes, store it; "
        << "if it's routine or speculative, don't. Never store secrets.\n"
        << "</memory-tools>\n";
    // Append AGENTS.md (AAIF standard, project-scoped) BEFORE the CLAUDE.md
    // tiers. Standardized public project guidance lands first, personal
    // memory layers on top — same end-of-prompt position so the always-on
    // rules above still anchor first.
    oss << collect_agents_md_block();
    // Append CLAUDE.md tiers (User + Project + Local) when present.
    // Lives at the END of the prompt so the always-on rules above
    // anchor first; user-authored memory then layers on top.
    oss << collect_memory_blocks();
    // On-demand skills catalog (names + descriptions only). The full
    // bodies load lazily via the `skill` tool — progressive disclosure
    // keeps the per-request cost to one cheap line per skill.
    oss << tools::skills::catalog_block();
    return oss.str();
}

// ---------------------------------------------------------------------------
// Per-provider overlay seam.
//
// The base prompt above is the single source of truth for every provider. When
// a specific model needs a divergent nudge (a "pedantic" model that mishandles
// a construct, a provider whose tool-call dialect wants an extra reminder), add
// its delta here keyed by canonical provider id — do NOT fork the whole base.
// The delta is APPENDED after the base, so it always wins on any conflicting
// instruction (last-writer semantics in a system prompt). Everything stays
// baked into the binary: no file is read from disk, so there is no
// prompt-injection surface.
//
// To add one, return a non-empty block for the id, e.g.:
//
//   if (provider_id == "openai")
//       return "<provider-notes>\n  - Emit tool arguments as a single JSON "
//              "object; never wrap them in a markdown code fence.\n"
//              "</provider-notes>\n";
//
// Keep overlays SMALL and additive. If you find yourself rewriting whole
// sections per provider, that's the signal to promote this to a templated
// base rather than growing the deltas.
// ---------------------------------------------------------------------------
std::string prompt_overlay(std::string_view provider_id) {
    (void)provider_id;
    // No provider currently diverges from the shared base. New overlays plug
    // in here; the seam + composition are already wired so adding one is a
    // one-line change with no call-site churn.
    return {};
}

std::string system_prompt_with_overlay(std::string_view provider_id, bool lean) {
    std::string base = default_system_prompt(lean);
    std::string overlay = prompt_overlay(provider_id);
    if (overlay.empty()) return base;
    // Ensure a clean seam between the base and the appended delta.
    if (!base.empty() && base.back() != '\n') base.push_back('\n');
    base.push_back('\n');
    base += overlay;
    return base;
}

std::vector<ToolSpec> default_tools() {
    std::vector<ToolSpec> out;
    for (const auto& td : tools::wire_tools()) {
        if (!td.advertise) continue;   // dispatch-only (proxy owns the schema)
        out.push_back({td.name.value, td.description, td.input_schema});
    }
    return out;
}

} // namespace agentty::provider
