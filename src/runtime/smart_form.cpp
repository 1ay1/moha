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
    // Hidden behind ^A. These decide HOW Smart Mode routes, so they belong
    // beside the switch that turns routing on — but they are the kind of knob
    // a user cannot judge at a glance, and four rows that matter should not
    // compete with three that mostly should not be touched.
    //
    // Ranges come from the settings registry, which is also what clamps them
    // on the way in, so the row and the store agree by construction.
    if (in.advanced) {
        namespace reg = settings::registry;
        b.header("Routing policy");

        const auto row = [&](const char* id, std::string label,
                             int value, std::string help,
                             const std::string& lock) {
            const auto* d = reg::find(id);
            if (!d) return;              // unreachable: ids are registry ids
            b.number(id, std::move(label), value,
                     static_cast<std::int64_t>(d->min),
                     static_cast<std::int64_t>(d->max), std::move(help));
            if (!lock.empty()) {
                b.lock(lock);
                b.origin(lock);
            } else if (!live) {
                b.lock("Smart Mode off");
            }
        };

        row(kFieldComplexCut, "Complexity cut", in.complex_threshold,
            "score at which a turn routes as Complex; lower spends more",
            in.complex_threshold_lock);
        row(kFieldDeepMargin, "Deep-band margin", in.deep_margin,
            "how far into a tier a turn must sit to earn the extra effort step",
            in.deep_margin_lock);
        row(kFieldBiasClamp, "Self-correction cap", in.bias_clamp,
            "how far this session's own corrections may drift effort",
            in.bias_clamp_lock);
    }

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
