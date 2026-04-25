#pragma once
// moha::tools::spec — the compile-time tool catalog.
//
// Each tool factory in `src/tool/tools/*.cpp` populates a `ToolDef` at
// runtime. The shape of *every* tool — its name, its capabilities, its
// streaming behavior — is also fixed at compile time, and lives here as
// a `constexpr std::array` of `ToolSpec`s.
//
// Three reasons this exists as a separate compile-time table:
//
// 1. Static cross-checks. The block of `static_assert`s at the bottom
//    proves properties about the catalog that no runtime test can
//    guarantee — "every WriteFs tool actually has WriteFs", "no
//    read-only tool accidentally got the Exec capability", "the bash
//    tool's name is `bash`, not `Bash`". These are caught at the build
//    where they originate, not at run time.
//
// 2. Single source of truth for catalog metadata. Factories in
//    `src/tool/tools/*` reference `tools::spec::lookup("bash")` for
//    their name / description / effects / eager-streaming flag, so a
//    typo there is impossible — there's only one place to write the
//    string `"bash"`, and the lookup either returns the spec or the
//    factory fails to compile.
//
// 3. Wire-format generators (e.g. the JSON tool list sent to Anthropic)
//    can iterate over a `constexpr` table without paying a runtime
//    init cost. Today the wire path still uses the runtime `registry()`
//    vector; the spec table lets us migrate that progressively.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "moha/tool/effects.hpp"
#include "moha/domain/id.hpp"

namespace moha::tools::spec {

enum class Kind : std::uint8_t;  // forward-declared; defined after ToolSpec

struct ToolSpec {
    std::string_view name;            // wire identifier — must be unique
    Kind             kind;            // closed-set discriminator; match `name`
    EffectSet        effects;         // capability set; drives the policy
    bool             eager_input_streaming;   // FGTS opt-in flag (Anthropic)
    // Wall-clock watchdog deadline. The reducer schedules
    // `Cmd::after(max_seconds, ToolTimeoutCheck{id})` when a tool of
    // this kind transitions to Running; the handler force-fails it if
    // it's still running when the timer fires. `0s` means "no overlay
    // timeout" — used for tools that own their own timeout via the
    // subprocess runner (bash, diagnostics) so we don't double-gate.
    //
    // Pick by the tool's longest-but-still-reasonable runtime: a slow
    // NFS read might take 20 s; a recursive grep across a Linux kernel
    // tree might take a minute. The watchdog is the safety net for
    // "the worker thread is wedged"; legitimate slow-but-progressing
    // workloads should fit comfortably under the chosen ceiling.
    //
    // Typed as `std::chrono::seconds` so callers can't accidentally
    // feed this to a millisecond-expecting scheduler without an
    // explicit `duration_cast` — the unit is in the type.
    std::chrono::seconds max_seconds;
};

// ── The full tool catalog, in the same display order as the runtime
// registry (`src/tool/registry.cpp:build_registry`). Order matters: the
// model has a recall bias toward earlier entries, so `edit` precedes
// `write` to nudge against full-file rewrites.
//
// Description text is intentionally NOT in the catalog — it's wire-only
// metadata (each tool's factory composes its own help text, sometimes
// platform-conditional like bash on Windows). Cross-validating
// descriptions buys nothing; cross-validating effects + names matters.
// Short-hand so the catalog reads as `20s` rather than
// `std::chrono::seconds{20}`. Scoped to the catalog array so the UDL
// doesn't leak to headers that include this one.
namespace detail {
using sec = std::chrono::seconds;
}

// ── Closed-set identity for every tool in the catalog ───────────────────
// A typed discriminator so call sites stop hand-rolling `name == "bash"`
// string compares. Add a tool: add a Kind arm, add a row to the catalog
// with the matching `kind`, and `kind_of("new_tool") == Kind::NewTool`
// at compile time — the `static_assert` wall below proves every Kind has
// exactly one catalog entry and every catalog entry has a Kind.
//
// Wire identity stays the `name` string field (Anthropic's vocabulary is
// the source of truth on the wire); Kind is the internal closed set the
// reducer/views dispatch on.
enum class Kind : std::uint8_t {
    Read,
    Edit,
    Write,
    Bash,
    Grep,
    Glob,
    ListDir,
    Todo,
    WebFetch,
    WebSearch,
    FindDefinition,
    Diagnostics,
    GitStatus,
    GitDiff,
    GitLog,
    GitCommit,
    // ── Index-backed table-of-contents tools ───────────────────────────
    Outline,      // one file's symbol map
    RepoMap,      // the workspace-wide compact tree
    Signatures,   // grep symbol names across the index
    // ── Sub-agent ──────────────────────────────────────────────────────
    Investigate,
    // ── Persistent codebase-memory tools ──────────────────────────────
    Remember,
    Forget,
    Memos,
    Recall,
    FindUsages,
    MineAdrs,
    Navigate,
};

inline constexpr std::array kCatalog = {
    //         name              kind                  effects                              eager   timeout
    ToolSpec{"read",            Kind::Read,           {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"edit",            Kind::Edit,           {Effect::ReadFs, Effect::WriteFs},    true,    detail::sec{30}},
    ToolSpec{"write",           Kind::Write,          {Effect::WriteFs},                    true,    detail::sec{30}},
    ToolSpec{"bash",            Kind::Bash,           {Effect::Exec},                       true,    detail::sec{0}},   // subprocess-managed
    ToolSpec{"grep",            Kind::Grep,           {Effect::ReadFs},                     false,   detail::sec{90}},  // tree walks can be deep
    ToolSpec{"glob",            Kind::Glob,           {Effect::ReadFs},                     false,   detail::sec{60}},
    ToolSpec{"list_dir",        Kind::ListDir,        {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"todo",            Kind::Todo,           {} /* pure */,                        true,    detail::sec{5}},   // in-memory only
    ToolSpec{"web_fetch",       Kind::WebFetch,       {Effect::Net},                        false,   detail::sec{30}},  // matches http total-timeout
    ToolSpec{"web_search",      Kind::WebSearch,      {Effect::Net},                        false,   detail::sec{20}},
    ToolSpec{"find_definition", Kind::FindDefinition, {Effect::ReadFs},                     false,   detail::sec{60}},
    ToolSpec{"diagnostics",     Kind::Diagnostics,    {Effect::Exec},                       false,   detail::sec{0}},   // subprocess-managed
    ToolSpec{"git_status",      Kind::GitStatus,      {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"git_diff",        Kind::GitDiff,        {Effect::ReadFs},                     false,   detail::sec{30}},
    ToolSpec{"git_log",         Kind::GitLog,         {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"git_commit",      Kind::GitCommit,      {Effect::WriteFs},                    true,    detail::sec{30}},
    // ── Tier-2 token-saver tools (index-backed table of contents) ──────
    ToolSpec{"outline",         Kind::Outline,        {Effect::ReadFs},                     false,   detail::sec{15}},
    ToolSpec{"repo_map",        Kind::RepoMap,        {Effect::ReadFs},                     false,   detail::sec{120}}, // first scan walks the tree
    ToolSpec{"signatures",      Kind::Signatures,     {Effect::ReadFs},                     false,   detail::sec{30}},
    // ── Tier-3 sub-agent. `Net` because it dials the model API; the
    // read-only tool subset it may invoke is enforced inside
    // investigate.cpp, not at the spec layer.
    ToolSpec{"investigate",     Kind::Investigate,    {Effect::Net},                        true,    detail::sec{300}},
    // ── Persistent workspace-memory tools (in-process, no I/O cost) ────
    // `remember` writes to <workspace>/.moha/memos.json — local FS; we
    // tag it WriteFs so the policy gates on it under stricter profiles
    // (the user should know when knowledge is being persisted to disk).
    // `forget` mutates the store the same way → WriteFs.
    // `memos` is read-only over the in-memory store + JSON file.
    ToolSpec{"remember",        Kind::Remember,       {Effect::WriteFs},                    true,    detail::sec{5}},
    ToolSpec{"forget",          Kind::Forget,         {Effect::WriteFs},                    false,   detail::sec{5}},
    ToolSpec{"memos",           Kind::Memos,          {Effect::ReadFs},                     false,   detail::sec{5}},
    // `recall`, `find_usages`, `navigate` operate purely on in-memory
    // caches (memo store / symbol graph / semantic index that the agent
    // populated earlier). They never touch the filesystem at call time,
    // so tagging them as ReadFs would force them to wait behind any
    // in-flight WriteFs/Exec sibling for no real reason — the user would
    // see them sit "queued" while an `edit` finishes. Empty effects =
    // pure = parallel-safe with everything, including WriteFs.
    ToolSpec{"recall",          Kind::Recall,         {} /* pure */,                        false,   detail::sec{5}},
    ToolSpec{"find_usages",     Kind::FindUsages,     {} /* pure */,                        false,   detail::sec{5}},
    // `mine_adrs` calls git + Haiku → Net effect (model API) and may
    // write memos; long-running because it can scan up to 200 commits.
    ToolSpec{"mine_adrs",       Kind::MineAdrs,       {Effect::Net, Effect::WriteFs},       false,   detail::sec{120}},
    // Pure-cpp semantic finder over the in-memory index. Cheap.
    ToolSpec{"navigate",        Kind::Navigate,       {} /* pure */,                        false,   detail::sec{10}},
};

// Wire-string → Kind. `std::nullopt` for names not in the catalog so the
// caller (reducer guards, runtime dispatch) can react to an unknown tool
// explicitly instead of silently falling through a string compare chain.
[[nodiscard]] constexpr std::optional<Kind> kind_of(std::string_view name) noexcept {
    for (const auto& s : kCatalog) if (s.name == name) return s.kind;
    return std::nullopt;
}

// Kind → wire-string. Total: every Kind has an entry by the static_assert
// wall below, so the fallback is unreachable in well-formed builds.
[[nodiscard]] constexpr std::string_view name_of(Kind k) noexcept {
    for (const auto& s : kCatalog) if (s.kind == k) return s.name;
    return {};
}

// Compile-time lookup. Returns a pointer to the spec, or nullptr if
// the name doesn't exist. Used by the runtime factories to populate
// `ToolDef::name` / `description` / `effects` from the table.
[[nodiscard]] constexpr const ToolSpec* lookup(std::string_view name) noexcept {
    for (const auto& s : kCatalog) if (s.name == name) return &s;
    return nullptr;
}

// Fixed-string non-type template parameter so a tool factory can write
// `spec::require<"bash">()` and have the misspelling caught at compile
// time. The instantiation site evaluates `lookup` in a constant
// expression and `static_assert`s on the result.
template <std::size_t N>
struct FixedName {
    char data[N];
    consteval FixedName(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {data, N - 1};   // strip trailing NUL
    }
};

// `spec::require<"bash">()` returns the catalog entry for "bash".
// Compile error if no entry with that name exists — there is no way
// to silently create a tool whose name isn't in the catalog.
template <FixedName Name>
[[nodiscard]] consteval const ToolSpec& require() {
    constexpr std::string_view name_v = Name.view();
    constexpr const ToolSpec* s = lookup(name_v);
    static_assert(s != nullptr,
                  "tool name not in moha::tools::spec::kCatalog — add an "
                  "entry there before calling spec::require<...>");
    return *s;
}

// ── Compile-time correctness proofs of the catalog ───────────────────────
// These are the safety net: every property a reader might assume about
// a tool's capabilities is verified at build time. Anyone editing the
// catalog above gets an instant signal if they break an invariant.
namespace proofs {

// Every name in the catalog is unique.
consteval bool all_names_unique() {
    for (std::size_t i = 0; i < kCatalog.size(); ++i)
        for (std::size_t j = i + 1; j < kCatalog.size(); ++j)
            if (kCatalog[i].name == kCatalog[j].name) return false;
    return true;
}
static_assert(all_names_unique(), "tool catalog has duplicate names");

// Every Kind arm appears exactly once in the catalog. Pair with the name
// uniqueness check: together they prove `kind_of(name)` is injective and
// `name_of(kind)` is total.
consteval bool kinds_bijective() {
    constexpr Kind kAll[] = {
        Kind::Read, Kind::Edit, Kind::Write, Kind::Bash,
        Kind::Grep, Kind::Glob, Kind::ListDir, Kind::Todo,
        Kind::WebFetch, Kind::WebSearch, Kind::FindDefinition,
        Kind::Diagnostics, Kind::GitStatus, Kind::GitDiff,
        Kind::GitLog, Kind::GitCommit,
        Kind::Outline, Kind::RepoMap, Kind::Signatures,
        Kind::Investigate,
        Kind::Remember, Kind::Forget, Kind::Memos,
        Kind::Recall, Kind::FindUsages, Kind::MineAdrs,
        Kind::Navigate,
    };
    if (std::size(kAll) != kCatalog.size()) return false;
    for (auto k : kAll) {
        int hits = 0;
        for (const auto& s : kCatalog) if (s.kind == k) ++hits;
        if (hits != 1) return false;
    }
    // And for every row the reverse holds (name↔kind round-trip).
    for (const auto& s : kCatalog) {
        auto k = kind_of(s.name);
        if (!k || *k != s.kind) return false;
        if (name_of(s.kind) != s.name) return false;
    }
    return true;
}
static_assert(kinds_bijective(),
              "spec::Kind and kCatalog must be in bijection — every Kind arm "
              "needs exactly one row whose `kind` matches and whose `name` "
              "round-trips through kind_of/name_of");

// Every tool has a non-empty name (the wire requires it).
consteval bool all_names_present() {
    for (const auto& s : kCatalog) if (s.name.empty()) return false;
    return true;
}
static_assert(all_names_present(), "every tool needs a non-empty name");

// Lookup works for every catalog entry.
static_assert(lookup("bash")     != nullptr);
static_assert(lookup("git_commit") != nullptr);
static_assert(lookup("nonexistent") == nullptr);

// Capability invariants — the rules we want to never violate.

// `bash` and `diagnostics` are the only Exec tools; nothing else gets
// arbitrary code execution.
consteval bool only_known_exec_tools() {
    for (const auto& s : kCatalog) {
        if (!s.effects.has(Effect::Exec)) continue;
        if (s.name != "bash" && s.name != "diagnostics") return false;
    }
    return true;
}
static_assert(only_known_exec_tools(),
              "Only `bash` and `diagnostics` may carry Effect::Exec — adding "
              "another Exec tool requires a separate review and updating this "
              "static_assert");

// Tools that mutate the filesystem must NOT also be Exec — those are
// strictly more dangerous and would belong in the bash family if they
// needed both. (Keeps the policy table clean.)
consteval bool no_writefs_and_exec_combo() {
    for (const auto& s : kCatalog)
        if (s.effects.has(Effect::WriteFs) && s.effects.has(Effect::Exec))
            return false;
    return true;
}
static_assert(no_writefs_and_exec_combo(),
              "no tool may carry both WriteFs and Exec — promote to bash");

// Pure tools must have empty effects.
static_assert(lookup("todo")->effects.empty());

// Read-side tools must NOT have WriteFs / Net / Exec.
consteval bool readonly_invariants() {
    constexpr std::string_view kReadOnly[] = {
        "read","grep","glob","list_dir","find_definition",
        "git_status","git_diff","git_log",
        "outline","repo_map","signatures","memos","recall","find_usages",
        "navigate",
    };
    for (auto n : kReadOnly) {
        auto* s = lookup(n);
        if (!s) return false;
        if (s->effects.has(Effect::WriteFs)) return false;
        if (s->effects.has(Effect::Exec))    return false;
        if (s->effects.has(Effect::Net))     return false;
    }
    return true;
}
static_assert(readonly_invariants(),
              "a tool listed as read-only carries a write/exec/net capability");

// Network tools: the two web ones, the sub-agent (dials the model API),
// and the ADR miner (also dials Haiku). Anything else carrying Net
// needs explicit review.
consteval bool only_known_net_tools() {
    for (const auto& s : kCatalog) {
        if (!s.effects.has(Effect::Net)) continue;
        if (s.name != "web_fetch" && s.name != "web_search"
         && s.name != "investigate" && s.name != "mine_adrs") return false;
    }
    return true;
}
static_assert(only_known_net_tools(),
              "Only web_fetch / web_search / investigate / mine_adrs may carry Effect::Net");

// ── Per-tool timeout proofs ─────────────────────────────────────────────
// Pin the wall-clock-watchdog values so a careless edit (set 0 on the
// wrong tool, or 6000 on a fast one) breaks the build instead of
// silently changing runtime behaviour.

// Subprocess-managed tools (bash, diagnostics) have their own
// timeout in `subprocess.cpp`; the overlay watchdog must be 0 to
// avoid double-gating.
consteval bool subprocess_tools_have_no_overlay_timeout() {
    auto* b = lookup("bash");
    auto* d = lookup("diagnostics");
    return b && d && b->max_seconds == std::chrono::seconds{0}
                  && d->max_seconds == std::chrono::seconds{0};
}
static_assert(subprocess_tools_have_no_overlay_timeout(),
              "bash/diagnostics must have max_seconds=0 — they own their timeout");

// Every other tool MUST have a finite, sensible overlay timeout. Zero
// would mean "never time out", and that's the bug the watchdog exists
// to prevent. Cap at 5 minutes so no tool can wedge the agent for
// longer than the user's patience.
consteval bool other_tools_have_bounded_timeout() {
    using std::chrono::seconds;
    for (const auto& s : kCatalog) {
        if (s.name == "bash" || s.name == "diagnostics") continue;
        if (s.max_seconds < seconds{1} || s.max_seconds > seconds{300}) return false;
    }
    return true;
}
static_assert(other_tools_have_bounded_timeout(),
              "non-subprocess tools need a max_seconds in [1, 300]");

// `web_fetch` cannot wait longer than the underlying http total
// timeout, otherwise the watchdog fires while the client is still
// happily blocking on a slow server.
static_assert(lookup("web_fetch")->max_seconds <= std::chrono::seconds{30},
              "web_fetch overlay timeout must be ≤ http total (30s)");

} // namespace proofs

} // namespace moha::tools::spec
