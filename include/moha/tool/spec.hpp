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
#include <string_view>

#include "moha/tool/effects.hpp"

namespace moha::tools::spec {

struct ToolSpec {
    std::string_view name;            // wire identifier — must be unique
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

inline constexpr std::array kCatalog = {
    //         name              effects                              eager   timeout
    ToolSpec{"read",            {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"edit",            {Effect::ReadFs, Effect::WriteFs},    true,    detail::sec{30}},
    ToolSpec{"write",           {Effect::WriteFs},                    true,    detail::sec{30}},
    ToolSpec{"bash",            {Effect::Exec},                       true,    detail::sec{0}},   // subprocess-managed
    ToolSpec{"grep",            {Effect::ReadFs},                     false,   detail::sec{90}},  // tree walks can be deep
    ToolSpec{"glob",            {Effect::ReadFs},                     false,   detail::sec{60}},
    ToolSpec{"list_dir",        {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"todo",            {} /* pure */,                        true,    detail::sec{5}},   // in-memory only
    ToolSpec{"web_fetch",       {Effect::Net},                        false,   detail::sec{30}},  // matches http total-timeout
    ToolSpec{"web_search",      {Effect::Net},                        false,   detail::sec{20}},
    ToolSpec{"find_definition", {Effect::ReadFs},                     false,   detail::sec{60}},
    ToolSpec{"diagnostics",     {Effect::Exec},                       false,   detail::sec{0}},   // subprocess-managed
    ToolSpec{"git_status",      {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"git_diff",        {Effect::ReadFs},                     false,   detail::sec{30}},
    ToolSpec{"git_log",         {Effect::ReadFs},                     false,   detail::sec{20}},
    ToolSpec{"git_commit",      {Effect::WriteFs},                    true,    detail::sec{30}},
};

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

// Network tools must be exactly the web ones.
consteval bool only_web_is_net() {
    for (const auto& s : kCatalog) {
        if (!s.effects.has(Effect::Net)) continue;
        if (s.name != "web_fetch" && s.name != "web_search") return false;
    }
    return true;
}
static_assert(only_web_is_net(),
              "Only web_fetch/web_search may carry Effect::Net");

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
