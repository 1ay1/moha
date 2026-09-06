#pragma once
// agentty::smart_form — Smart Mode as a form::Form.
//
// Smart Mode's overlay was a hand-built Picker with its own row enum, its own
// cursor arithmetic, its own key handling and its own "this is pinned by an
// env var" special case. All four now come from the shared form layer, so it
// behaves identically to the embeddings pane and to every future config
// surface — one key map, one renderer, one interaction model.
//
// What stays here is the only Smart-Mode-specific knowledge: which rows exist,
// how a role slot renders, and how rows map back onto a RoleConfig.
//
// ── The three slots are `Pick`, not `Choice` ─────────────────────────────
// A role's candidate set is the whole model catalogue: large, provider-scoped,
// and worth searching. That is precisely the case a dropdown must NOT try to
// serve — so Enter on a slot hands off to the fused model picker, exactly as
// it did before. `Choice` stays reserved for genuine enums.

#include <string>
#include <vector>

#include "agentty/domain/smart_mode.hpp"
#include "agentty/runtime/form.hpp"

namespace agentty::smart_form {

// Field ids — the reducer addresses rows by id, never by index, because the
// row set is derived and an index changes meaning when it does.
inline constexpr const char* kFieldEnabled  = "enabled";
inline constexpr const char* kFieldStrategic = "strategic";
inline constexpr const char* kFieldImpl      = "implementation";
inline constexpr const char* kFieldUtility   = "utility";
// The advanced tuning rows. Ids match the settings-registry row ids exactly,
// so the reducer writes them back by walking the table rather than naming
// each one — the row IS the binding.
inline constexpr const char* kFieldComplexCut = "smart.complex_threshold";
inline constexpr const char* kFieldDeepMargin = "smart.deep_margin";
inline constexpr const char* kFieldBiasClamp  = "smart.bias_clamp";

// The resolved display label for one role, plus whether it was pinned. The
// pane computes these (resolution needs the catalogue and the active
// provider); this header only says what the form needs.
struct SlotView {
    std::string label;     // what would actually run right now
    bool        pinned = false;
};

struct Inputs {
    bool     enabled = false;
    // Non-empty ⇒ the master switch is pinned by AGENTTY_SMART_MODE and the
    // row must render read-only saying so. Previously the toggle looked live
    // and silently refused on Enter with a toast — a row that lies about
    // being editable is worse than one that explains why it isn't.
    std::string enabled_lock;
    SlotView strategic;
    SlotView implementation;
    SlotView utility;

    // Numeric routing policy. These belong HERE, next to the switch and the
    // slots they govern — they are how Smart Mode decides, and a user looking
    // for "how eagerly does it escalate" looks at the Smart Mode pane, not at
    // Retrieval. Advanced: the values are meaningful but not self-explanatory,
    // so they sit behind ^A rather than crowding the four rows that matter.
    int complex_threshold = 3;
    int deep_margin       = 3;
    int bias_clamp        = 2;

    // Non-empty ⇒ an AGENTTY_SMART_* export is overriding that row, which
    // renders read-only naming the variable. Same rule as enabled_lock: a row
    // that looks editable and silently loses the edit is the worst outcome.
    std::string complex_threshold_lock;
    std::string deep_margin_lock;
    std::string bias_clamp_lock;

    // Reveal the advanced rows (^A).
    bool advanced = false;
};

// Build the pane. Pure — unit-testable with no catalogue and no terminal.
[[nodiscard]] form::Form build_form(const Inputs& in);

// The role a row id names, or nullopt for the master switch. Total, so a
// caller cannot silently treat "not a slot" as a slot.
[[nodiscard]] std::optional<smart::ModelRole> role_of_field(std::string_view id) noexcept;

// The field id that configures a role — the inverse of role_of_field. Total,
// so "re-open the pane on the slot I just set" needs no index arithmetic and
// cannot land on the wrong row when the row set changes.
[[nodiscard]] std::string_view field_of_role(smart::ModelRole r) noexcept;

// Place the cursor on the row that configures `r`. A no-op when the row is
// absent, which is the honest behaviour: the caller asked for a row, not an
// index, so a missing row leaves the cursor where it was.
void focus_role(form::Form& f, smart::ModelRole r) noexcept;

// Read the master switch back out of the form.
[[nodiscard]] bool enabled_from_form(const form::Form& f, bool fallback) noexcept;

} // namespace agentty::smart_form
