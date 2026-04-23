#pragma once
// moha::tools::effects — capability-set tags that describe a tool's
// observable impact on the world. The permission policy reads ONLY
// these tags; tool implementations don't get to decide their own
// trust level. This is the typeful inversion of the previous design,
// where each tool carried a bespoke `needs_permission(Profile)`
// lambda — easy to forget, easy to make inconsistent (read-only git_*
// historically said "never ask" while bash said "always ask in non-
// Write", even though both spawn subprocesses).
//
// Pick effects by what the tool DOES to the world, not how it's
// implemented. `git_status` is `ReadFs` even though it shells out to
// `git`; `bash` is `Exec` because the *model* picks the command. The
// distinction between "we know what this subprocess does" and "the
// model picked it" is what the permission flow actually cares about.

#include <cstdint>
#include <initializer_list>
#include <string>

namespace moha::tools {

// Single capability tag. Bit-encoded so EffectSet is a trivial uint8_t.
//
// ReadFs   — reads filesystem state (open, stat, readdir, scan).
// WriteFs  — mutates filesystem state (create/modify/delete files,
//            write to .git, anything that survives the process).
// Net      — sends bytes to or receives bytes from the network.
// Exec     — runs an arbitrary, model-chosen program (`bash`,
//            `diagnostics`). Strictly more dangerous than `WriteFs`
//            because the model controls *what* runs, not just what
//            files change.
// Pure     — no observable effect (no IO of any kind). Today only
//            `todo` qualifies; reserve the tag so the policy can
//            short-circuit it cleanly.
enum class Effect : std::uint8_t {
    ReadFs   = 1u << 0,
    WriteFs  = 1u << 1,
    Net      = 1u << 2,
    Exec     = 1u << 3,
};

// A set of effects, packed into a uint8_t. Trivially copyable, all
// operations constexpr — usable as a value member of constexpr-
// constructed ToolDef-likes and as an immediate operand in the policy.
class EffectSet {
    std::uint8_t bits_ = 0;
public:
    constexpr EffectSet() noexcept = default;
    constexpr EffectSet(std::initializer_list<Effect> es) noexcept {
        for (auto e : es) bits_ |= static_cast<std::uint8_t>(e);
    }
    constexpr explicit EffectSet(std::uint8_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool has(Effect e) const noexcept {
        return (bits_ & static_cast<std::uint8_t>(e)) != 0;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }

    [[nodiscard]] constexpr EffectSet operator|(EffectSet o) const noexcept {
        return EffectSet{static_cast<std::uint8_t>(bits_ | o.bits_)};
    }
    constexpr EffectSet& operator|=(EffectSet o) noexcept {
        bits_ |= o.bits_; return *this;
    }
    constexpr bool operator==(const EffectSet&) const noexcept = default;
};

// Convenience: a fully-pure effect set (no side effects). Use as the
// default for tools that only manipulate in-memory state visible to
// the model (currently just `todo`).
inline constexpr EffectSet pure_effects{};

// ── Parallel-safety rule ────────────────────────────────────────────────
// Governs whether a tool with `want` effects may start while a set of
// `active` effects is in flight. Expressed in terms of effect tags so
// the scheduling policy is derived from the capability model, not
// sprinkled through the runtime.
//
//   WriteFs / Exec demand *exclusive* access:
//     * a WriteFs-tagged tool may mutate filesystem state that any
//       sibling could be reading (ReadFs), writing (WriteFs), or
//       shelling out against (Exec). Two edits to different files
//       look independent until the model picks overlapping paths.
//     * Exec is strictly more powerful — the *model* chooses what the
//       subprocess does, so we must assume the worst and serialise.
//
//   Pure / ReadFs / Net compose freely. Read-read never races; Net
//   touches neither FS nor process state; in-memory Pure tools (todo)
//   operate on data the model cannot observe concurrently.
//
// Reflected in the tool spec as a compile-time invariant (see
// moha::tools::spec::proofs::parallel_rule_is_well_founded).
[[nodiscard]] constexpr bool is_parallel_safe(EffectSet active, EffectSet want) noexcept {
    constexpr auto exclusive = [](EffectSet e) noexcept {
        return e.has(Effect::WriteFs) || e.has(Effect::Exec);
    };
    if (exclusive(active) || exclusive(want)) {
        // One side needs exclusive access; only safe if the other side is
        // empty. `active == {}` covers "nothing is running yet".
        return active.empty();
    }
    return true;
}

// Compile-time spot-checks of the rule — clearer than a prose comment
// and fail the build if someone rewrites the implementation wrong.
namespace proofs {
static_assert( is_parallel_safe(EffectSet{}, EffectSet{{Effect::ReadFs}}));
static_assert( is_parallel_safe(EffectSet{{Effect::ReadFs}}, EffectSet{{Effect::ReadFs}}));
static_assert( is_parallel_safe(EffectSet{{Effect::ReadFs}}, EffectSet{{Effect::Net}}));
static_assert( is_parallel_safe(EffectSet{{Effect::Net}},    EffectSet{{Effect::Net}}));
static_assert( is_parallel_safe(EffectSet{},                 EffectSet{{Effect::WriteFs}}));
static_assert( is_parallel_safe(EffectSet{},                 EffectSet{{Effect::Exec}}));
static_assert(!is_parallel_safe(EffectSet{{Effect::WriteFs}}, EffectSet{{Effect::ReadFs}}));
static_assert(!is_parallel_safe(EffectSet{{Effect::ReadFs}},  EffectSet{{Effect::WriteFs}}));
static_assert(!is_parallel_safe(EffectSet{{Effect::Exec}},    EffectSet{{Effect::ReadFs}}));
static_assert(!is_parallel_safe(EffectSet{{Effect::WriteFs}}, EffectSet{{Effect::WriteFs}}));
static_assert(!is_parallel_safe(EffectSet{{Effect::Exec}},    EffectSet{{Effect::Exec}}));
} // namespace proofs

// "ReadFs, Net" — sorted-by-bit, comma-separated. For permission UI
// and logging; not parsed anywhere.
[[nodiscard]] std::string to_string(EffectSet e);

// Single-effect label, e.g. "ReadFs". Capital-cased for prose use
// ("Tool needs WriteFs permission"); the lower-case "read", "exec"
// forms aren't used anywhere because they collide with verbs.
[[nodiscard]] std::string_view to_string(Effect e) noexcept;

} // namespace moha::tools
