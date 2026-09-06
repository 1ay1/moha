// smart_form.cpp — Smart Mode's row model.
//
// Pure: no catalogue lookups, no IO, no chrome. The pane resolves each role
// (which needs the model catalogue and the active provider) and hands the
// resulting labels in; this file only decides what the rows ARE.

#include "agentty/runtime/smart_form.hpp"

#include "agentty/runtime/settings_registry.hpp"   // row ranges

namespace agentty::smart_form {

namespace {

void add_slot(form::Builder& b, const char* id, std::string label,
              const SlotView& v, std::string help) {
    b.pick(id, std::move(label), v.label, std::move(help));
    // Provenance, the same column the embeddings pane uses for
    // "settings.json" / "env: …". A pinned slot is a deliberate user choice;
    // an auto one is the catalogue's pick. Saying which is the difference
    // between "why is it using that model?" and an answer.
    b.origin(v.pinned ? "pinned" : "auto");
}

} // namespace

form::Form build_form(const Inputs& in) {
    form::Builder b{" Smart Mode "};
    // Group-header bookkeeping for the registry walk below. The slot rows are
    // hand-written (they are model pickers, not table rows), so the walk starts
    // fresh and emits its own header.
    bool first = true;
    auto last_group = settings::registry::Group::Sources;
    b.subtitle(in.enabled
        ? "routing each turn to the cheapest model that can do it"
        : "every turn goes to the main model");

    b.toggle(kFieldEnabled, "Smart Mode", in.enabled,
             "route by role instead of sending everything to one model");
    if (!in.enabled_lock.empty()) b.lock(in.enabled_lock);

    // The slots only mean something while routing is on. Locked rather than
    // hidden: a pane whose contents appear and vanish is disorienting, and the
    // rows are exactly what explains what the switch above would DO.
    const bool live = in.enabled;

    add_slot(b, kFieldStrategic, "Strategic", in.strategic,
             "planning, architecture, hard debugging");
    if (!live) b.lock("Smart Mode off");

    add_slot(b, kFieldImpl, "Implementation", in.implementation,
             "writing and editing code");
    if (!live) b.lock("Smart Mode off");

    add_slot(b, kFieldUtility, "Utility", in.utility,
             "summaries, titles, cheap mechanical work");
    if (!live) b.lock("Smart Mode off");

    // ── Advanced: the numeric policy ────────────────────────────────
    // WALKED from the settings registry, exactly as the Retrieval pane walks
    // its half. Label, help, control kind, range, group header, env lock and
    // provenance all come from the row — this pane contributes only "the
    // Settings-owned rows are mine".
    //
    // These were hand-rolled here first: three labels, three help strings and
    // three ranges retyped out of the table that already declared them, plus a
    // second implementation of the env-lock rule. Everything the registry
    // exists to prevent.
    settings::registry::add_rows(b, in.smart,
                                 settings::registry::Owner::Smart,
                                 in.advanced, first, last_group);

    // The affordance has to be ON SCREEN. Advanced rows are hidden by default,
    // and a key nobody can see is exactly the discoverability failure these
    // knobs were moved out of an env var to fix.
    b.note(in.advanced ? "a  hide advanced" : "a  advanced");

    return b.build();
}

std::optional<smart::ModelRole> role_of_field(std::string_view id) noexcept {
    if (id == kFieldStrategic) return smart::ModelRole::Strategic;
    if (id == kFieldImpl)      return smart::ModelRole::Implementation;
    if (id == kFieldUtility)   return smart::ModelRole::Utility;
    return std::nullopt;
}

std::string_view field_of_role(smart::ModelRole r) noexcept {
    switch (r) {
        case smart::ModelRole::Strategic:      return kFieldStrategic;
        case smart::ModelRole::Implementation: return kFieldImpl;
        case smart::ModelRole::Utility:        return kFieldUtility;
    }
    return kFieldStrategic;
}

void focus_role(form::Form& f, smart::ModelRole r) noexcept {
    const auto id = field_of_role(r);
    for (std::size_t i = 0; i < f.fields.size(); ++i)
        if (f.fields[i].id == id) { f.cursor = static_cast<int>(i); return; }
}

bool enabled_from_form(const form::Form& f, bool fallback) noexcept {
    if (const auto* row = f.find(kFieldEnabled))
        if (const auto* t = std::get_if<form::field::Toggle>(&row->value))
            return t->on;
    return fallback;
}

} // namespace agentty::smart_form
