// providers.cpp — the provider picker (^P) + accounts drill-down.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {

Element providers_panel(const Model& m) {
    auto* picker = m.ui.panel.get<pn::Providers>();
    if (!picker) return nothing();

    Panel::Config cfg;
    cfg.title      = " Providers ";
    cfg.accent     = highlight;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.provider_picker_scroll;
    cfg.selected   = picker->index;

    const std::string active_id = active_provider_id();

    auto env_has = [](std::string_view name) -> bool {
        if (name.empty()) return false;
        const char* v = std::getenv(std::string{name}.c_str());
        return v && *v;
    };

    // The one ordered, query-filtered row list — the SAME list the reducer
    // resolves a selection against (see build_provider_rows). The cursor is a
    // plain index into it; there is no offset math on either side.
    auto settings = app::deps().load_settings();
    const std::vector<std::string> saved_custom_hosts =
        provider::saved_custom_hosts(settings.provider_keys);
    const auto rows = ui::build_provider_rows(saved_custom_hosts, picker->query);

    // Live search header (mirrors the model picker). Backspace trims; typing
    // narrows. Hidden ACP/custom rows return the moment the query is cleared.
    cfg.header.push_back(h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
        text(picker->query.empty() ? "type to filter providers\xe2\x80\xa6"
                                   : picker->query,
             picker->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    // Trailing auth-status column for a built-in preset. One place, so every
    // provider's badge stays consistent.
    auto preset_note = [&](const provider::ProviderPreset& p, bool active)
        -> std::pair<std::string, maya::Color> {
        auto signed_badge = [&](std::string signed_label) {
            return std::pair<std::string, maya::Color>{
                active ? std::string{"\xe2\x9c\x93 signed in \xc2\xb7 accounts"}
                       : std::move(signed_label),
                active ? success : muted};
        };
        // OAuth providers whose token lives in their own transport: one
        // uniform status line, answered by the auth vault. Was three
        // near-identical name-keyed blocks calling three different
        // signed_in() probes; vault::signed_in dispatches over the same
        // table, so a new OAuth provider gets its status row for free.
        if (p.token_in_transport) {
            if (auth::vault::signed_in(std::string{p.id}))
                return signed_badge(std::string{p.label} + " (signed in)");
            return {"\xe2\x9a\xa0 sign in with " + std::string{p.label}, warn};
        }
        if (p.is_local || p.auth == provider::AuthStyle::None)
            return {"\xe2\x97\x8f local", info};
        if (p.kind() == provider::Kind::Anthropic) {
            // On-disk credential store is authoritative and independent of the
            // currently-active provider (do NOT read deps().auth here).
            if (auth::anthropic_signed_in())
                return {active ? "\xe2\x9c\x93 signed in \xc2\xb7 accounts" : "\xe2\x9c\x93 signed in", success};
            return {"\xe2\x9a\xa0 sign in", warn};
        }
        // Hosted API-key provider: distinguish a SAVED (pasted) key — which
        // ^D signs out — from an ENV key, which can't be removed in-app, so
        // the badge must not imply ^D will work on it.
        switch (provider::auth_source(p, settings)) {
            case provider::AuthSource::Saved:
                return {active ? "\xe2\x9c\x93 signed in \xc2\xb7 key"
                               : "\xe2\x9c\x93 key saved", success};
            case provider::AuthSource::Env: {
                std::string ev;
                for (auto e : p.auth_env) if (env_has(e)) { ev = e; break; }
                return {"\xe2\x97\x8f key from " + ev, info};
            }
            case provider::AuthSource::Local:
                return {"\xe2\x97\x8f local", info};
            case provider::AuthSource::None:
            default: break;
        }
        std::string_view want = p.auth_env.front();
        return {want.empty() ? "\xe2\x9a\xa0 no key" : "\xe2\x9a\xa0 " + std::string{want}, warn};
    };

    cfg.items.reserve(rows.size());
    for (const auto& r : rows) {
        Panel::Item row;

        if (const auto* p = r.preset()) {
            const bool active = (p->id == active_id);
            const bool confirming = (picker->confirm_remove == std::string{p->id});
            auto [note, note_color] = preset_note(*p, active);
            row.leading        = std::string{p->label} + "  " + std::string{p->blurb};
            // Primary content renders at full foreground (same rule as the
            // model picker): dimming EVERY non-active row to make one stand
            // out inverts the hierarchy — it makes the whole list read as
            // unavailable while the trailing status chip out-shouts the name.
            // Bold + the ● marker is enough to find the active row.
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            if (confirming) {
                // Del/d armed on a preset that has a saved key — second press
                // signs out (clears the key), the preset itself stays.
                row.trailing       = "\xe2\x9c\x97 press again to sign out";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = note;
                row.trailing_style = fg_of(note_color);
            }
            row.active         = active;
        } else if (const auto* agent = r.acp()) {
            const bool active = (agent->id == active_id);
            row.leading        = agent->id + "  external ACP agent (" + agent->command + ")";
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            row.trailing       = "\xe2\x97\x8f agent";
            row.trailing_style = fg_of(info);
            row.active         = active;
        } else if (const auto* spec = r.custom_host()) {
            const bool active = (*spec == active_id);
            const bool confirming = (picker->confirm_remove == *spec);
            row.leading        = *spec + "  custom OpenAI-compatible host";
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            if (confirming) {
                row.trailing       = "\xe2\x9c\x97 press again to remove";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = "\xe2\x9c\x93 ready";
                row.trailing_style = fg_of(success);
            }
            row.active         = active;
        } else {   // NewCustomHost sentinel
            row.leading        = std::string{"Custom host\xe2\x80\xa6  "}
                               + "any OpenAI-compatible server (host:port)";
            row.leading_style  = fg_of(muted);
            row.trailing       = "\xe2\x9c\x8e edit";
            row.trailing_style = fg_of(info);
        }
        // Under width pressure the STATUS chip gives way, not the provider
        // name — the name is what you select; "✓ signed in · accounts" is
        // reference data. Same policy as the command palette and the model
        // picker; maya's default (leading yields first) is for rows where the
        // trailing cell matters more, like the file picker's diffstat.
        row.trailing_secondary = true;
        cfg.items.push_back(std::move(row));
    }

    // Enter opens the accounts drill-down on an account-capable OAuth
    // provider, otherwise it switches. Read straight off the highlighted row.
    const bool row_has_accounts = [&] {
        if (picker->index < 0 || picker->index >= static_cast<int>(rows.size()))
            return false;
        const auto& row = rows[static_cast<std::size_t>(picker->index)];
        // Custom hosts hold multiple keys (accounts) — Enter drills in.
        if (const auto* spec = row.custom_host())
            return provider::credentials::add_method(*spec)
                   != provider::credentials::AddMethod::None;
        const auto* p = row.preset();
        if (!p) return false;
        // Every account-capable provider (OAuth + hosted API key) shows the
        // account drill-down on Enter; only keyless local servers don't.
        return provider::credentials::add_method(p->id)
               != provider::credentials::AddMethod::None;
    }();

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("\xe2\x9c\x93", fg_of(success)), text(" ready  ", fg_dim(muted)),
        text("\xe2\x9a\xa0", fg_of(warn)),    text(" set the named key first  ", fg_dim(muted))
    ).build());
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"type", "filter", 4},
        {"Enter", row_has_accounts ? "accounts" : "switch", 5},
        {"^D", picker->confirm_remove.empty() ? "remove" : "confirm", 2},
        {"^/", "models", 3},                       // cross-hint: model picker
        {"Esc", "close", 4},
    }));

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
