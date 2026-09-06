// model_picker.cpp — the model surfaces: the fused cross-provider model
// picker (fused_picker) and the provider picker (provider_picker). Split out
// of the former monolithic pickers.cpp; shared scaffolding lives in
// pickers_prologue.hpp / pickers_common.hpp.
//
// Pure adapter: builds maya::Panel::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements and the typed cursor index.

#include "pickers_prologue.hpp"

namespace agentty::ui {


// ── Fused cross-provider model picker ───────────────────────────────────
// One list over EVERY authed provider (docs/design/unified-model-picker.md).
// Rows are `provider · model` with a context-window trailing cell; sections
// (recent / all providers / sign in) render as is_header dividers. Selecting
// switches provider+model atomically; a dim "sign in to X" row routes to login.
Element fused_picker(const Model& m) {
    auto* picker = m.ui.overlay.get<ov::FusedPicker>();
    if (!picker) return text("");

    // Read the reducer-maintained cache — never rebuild per frame.
    const auto& rows = m.d.fused_rows;

    Panel::Config cfg;
    // Slot-assign mode: retitle so it's clear the pick fills a Smart Mode
    // role rather than switching the model you're chatting with, and say
    // which provider the list is scoped to (see fused_rows_for_model).
    // role_display_name is the single spelling of these labels — this used
    // to be a fourth positional copy (`slot == 0 ? "Strategic" : ...`).
    cfg.title = m.ui.smart_assign_slot
        ? std::string{" Smart Mode \xc2\xb7 pick "}
          + std::string{smart::role_display_name(*m.ui.smart_assign_slot)}
          + " model "
        : (!picker->provider_scope.empty()
               // ^/ scope active: name the one provider the list is pinned
               // to, using the label off any row that carries it.
               ? [&] {
                     std::string label = picker->provider_scope;
                     for (const auto& r : rows)
                         if (r.provider_id == picker->provider_scope
                             && !r.label.empty()) { label = r.label; break; }
                     return " Models \xc2\xb7 " + label + " only ";
                 }()
               : std::string{" Models \xc2\xb7 all providers "});
    cfg.accent   = accent;
    // The active-row edge bar shares the picker's accent, so "you are here"
    // reads as one visual language with the title and query caret instead of
    // introducing a third hue. The cursor bar (bright cyan) still wins on
    // overlap, which is correct — where you ARE outranks where you were.
    cfg.active_color = accent;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.fused_picker_scroll;

    cfg.header.push_back(
        picker->query.empty()
            ? h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
                query_caret(accent),
                text(std::string{m.ui.smart_assign_slot
                                     ? "type to filter this provider"
                                     : "type to filter across providers"},
                     fg_italic(muted))
              ).build()
            : h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
                text(picker->query, fg_of(fg)),
                query_caret(accent)
              ).build());
    cfg.header.push_back(sep);

    // Lazy-load hint: while any provider's catalog is still streaming in,
    // show a dim spinner-ish note so the (initially active-provider-only)
    // list visibly reads as "more coming", not "that's all". Failed providers
    // are surfaced separately so a network blip doesn't look like "that's all".
    {
        int failed = 0;
        std::vector<std::string_view> pending;
        for (const auto& c : m.d.provider_catalogs) {
            if (c.state == ProviderCatalog::State::Loading)
                pending.push_back(c.label);
            else if (c.state == ProviderCatalog::State::Failed) ++failed;
        }
        if (!pending.empty()) {
            // Name who we're waiting on — "⋯ loading Groq, Mistral…" answers
            // "where's X?" precisely, where a bare count made the user guess.
            std::string who;
            const std::size_t shown = std::min<std::size_t>(pending.size(), 3);
            for (std::size_t i = 0; i < shown; ++i) {
                if (i) who += ", ";
                who += pending[i];
            }
            if (pending.size() > shown)
                who += " +" + std::to_string(pending.size() - shown);
            cfg.header.push_back(text(
                "  " + std::string{m.s.spinner.current_frame()}
                    + " loading " + who + "\xe2\x80\xa6",
                fg_italic(muted)));
        }
        if (failed > 0)
            cfg.header.push_back(text(
                "  \xe2\x9a\xa0 " + std::to_string(failed)
                    + (failed == 1 ? " provider failed to refresh"
                                   : " providers failed to refresh")
                    + " \xc2\xb7 reopen to retry",
                fg_of(warn)));
    }

    if (rows.empty()) {
        // A dead end should say WHY it is empty and what to do next. The bare
        // "no models match" was the same message whether you had mistyped,
        // whether every catalog was still loading, or whether you were signed
        // out of everything — three very different situations with three
        // different next actions. It also returned before the footer was
        // built, so the picker offered no hint that Esc even worked.
        const bool loading = std::any_of(
            m.d.provider_catalogs.begin(), m.d.provider_catalogs.end(),
            [](const ProviderCatalog& c) {
                return c.state == ProviderCatalog::State::Loading;
            });
        const bool any_authed = std::any_of(
            m.d.provider_catalogs.begin(), m.d.provider_catalogs.end(),
            [](const ProviderCatalog& c) { return !c.models.empty(); });

        Panel::Row nr;
        if (loading) {
            nr.leading = "  " + std::string{m.s.spinner.current_frame()}
                       + " loading model catalogs\xe2\x80\xa6";
        } else if (!any_authed) {
            nr.leading = "  no providers signed in \xc2\xb7 "
                         "^P to add one";
        } else if (!picker->query.empty()) {
            nr.leading = "  no model matches \xe2\x80\x9c" + picker->query
                       + "\xe2\x80\x9d \xc2\xb7 Backspace to widen";
        } else {
            nr.leading = "  no models available";
        }
        nr.leading_style = fg_italic(muted);
        cfg.rows.push_back(std::move(nr));
        cfg.selected = 0;
        // Keep the footer: an empty list is exactly when the user most needs
        // to be told how to leave or how to reach the provider picker.
        cfg.footer.push_back(key_hints({
            {"^P", "providers", 1},
            {"Esc", "close", 4},
        }));
        return Panel{std::move(cfg)}.build();
    }

    // Section dividers: RECENT vs ALL PROVIDERS, plus a query-gated SIGN IN
    // section — an un-authed provider whose name matches the query renders as
    // one dim actionable row so searching for it is never a dead end.
    enum class Section { Recent, Others, SignIn };
    auto section_of = [](const FusedRow& r) {
        if (r.is_signin_offer()) return Section::SignIn;
        if (r.recent) return Section::Recent;
        return Section::Others;
    };
    std::optional<Section> cur;
    int visual_selected = 0;
    // Per-section sizes, for the right-aligned count shown in each header.
    int recent_count = 0, all_count = 0, signin_count = 0;
    for (const auto& r : rows) {
        switch (section_of(r)) {
            case Section::Recent: ++recent_count; break;
            case Section::Others: ++all_count;    break;
            case Section::SignIn: ++signin_count;  break;
        }
    }

    // ── Badge column width ──────────────────────────────────────────
    // maya's Picker asks callers to "pad badges to a common width" for column
    // alignment, and this picker never did — so with provider labels running
    // 3..14 chars ("Groq" .. "GitHub Copilot") every model NAME started at a
    // different column and the list could not be scanned vertically. The
    // provider badge is the grouping signal in a flat cross-provider list, so
    // a ragged column defeats the whole layout.
    //
    // Measured in DISPLAY COLUMNS via maya::string_width, not bytes: registry
    // labels are ASCII today, but a custom host is user-named and may hold
    // CJK or emoji, where a byte count would over-pad and re-break the very
    // alignment this exists to create. (The same helper the provider picker
    // and the hints strip already use.)
    int badge_w = 0;
    for (const auto& r : rows)
        if (!r.is_signin_offer())
            badge_w = std::max(badge_w, maya::string_width(r.label));
    badge_w = std::min(badge_w, picker_badge_max_cols());
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[static_cast<std::size_t>(i)];
        const Section sec = section_of(r);
        if (!cur || *cur != sec) {
            cur = sec;

            // When scoped to one provider (^/), the browse section is that
            // provider's models — title it "<Provider> models" instead of the
            // cross-provider "all providers" divider.
            std::string others_label = "all providers";
            if (!picker->provider_scope.empty()) {
                std::string plabel = picker->provider_scope;
                for (const auto& rr : rows)
                    if (rr.provider_id == picker->provider_scope
                        && !rr.label.empty()) { plabel = rr.label; break; }
                others_label = plabel + " models";
            }

            // Uniform section divider — label + hue + right-pinned count. See
            // picker_detail::section_header (shared by every grouped picker).
            SectionHeader sh;
            sh.label = sec == Section::Recent ? "recent"
                     : sec == Section::Others ? others_label
                                              : "not signed in";
            sh.hue   = sec == Section::Recent ? info
                     : sec == Section::Others ? accent
                                              : warn;
            sh.count = sec == Section::Recent ? recent_count
                     : sec == Section::Others ? all_count
                                              : signin_count;
            cfg.rows.push_back(section_header(std::move(sh)));
        }
        if (i == picker->index)
            visual_selected = static_cast<int>(cfg.rows.size());
        const bool selected = (i == picker->index);

        // Sign-in offer row: "<Provider>  — Enter to sign in". Dim, single
        // action, no trailing chips (there's no model yet).
        if (r.is_signin_offer()) {
            Panel::Row row;
            row.leading       = "  " + r.label;
            row.leading_style = selected ? fg_bold(fg) : fg_of(muted);
            row.trailing       = "Enter to sign in \xe2\x86\x92";   // →
            row.trailing_style = fg_dim(muted);
            cfg.rows.push_back(std::move(row));
            continue;
        }

        Panel::Row row;
        const bool active = r.active;
        const int bw = maya::string_width(r.label);
        row.badge         = bw < badge_w
                              ? r.label + std::string(
                                    static_cast<std::size_t>(badge_w - bw), ' ')
                              : r.label;
        // Active row keeps the brand accent (it is the "you are here" marker);
        // every other row is hued by capability tier, so the strongest-first
        // browse ordering is legible at a glance instead of unexplained.
        row.badge_style   = fg_dim(
            active ? accent
                   : tier_hue(static_cast<ModelCapabilities::Tier>(r.tier)));
        row.leading       = "  " + r.model_label;
        // The ACTIVE row is marked by maya's native edge bar (a coloured `▎`
        // in column 0, `active_color`) rather than a `● ` text prefix. Two
        // reasons: the bar costs no name width — it lives in chrome the
        // widget already reserves for the cursor — and it keeps every model
        // name starting at the SAME column, so the list scans vertically.
        // A text prefix indented exactly one row out of alignment.
        row.active        = active;
        // The model NAME is the primary content — the thing you are choosing
        // between — so it renders at full foreground. It used to be `muted` for
        // every non-active row, which dimmed the entire list to make ONE row
        // stand out; that inverted the hierarchy (reference chips out-shouted
        // the names) and made a long list read as uniformly unavailable. The
        // active row keeps bold plus the edge bar, which is enough to find it.
        row.leading_style = active ? fg_bold(fg) : fg_of(fg);
        // fzf-style match highlight: paint the query's matched chars in the
        // name so a big filtered list shows WHY each row is here. Use the same
        // `highlight` theme hue as the classic model picker (not `info`) so
        // the two pickers read identically. match_positions are offsets into
        // the NAME; shift them past the uniform two-space indent. (This used
        // to branch on `active`, which carried a wider "● " prefix — the
        // edge bar replaced it, so every row now has the same offset.)
        if (!r.match_positions.empty()) {
            constexpr int kPrefix = 2;
            row.highlight.reserve(r.match_positions.size());
            for (int p : r.match_positions) row.highlight.push_back(kPrefix + p);
            row.highlight_fg = highlight;   // same hue as the other pickers
        }
        // Trailing cell: context window, then the two marks. RIGHT-ALIGNED as
        // a fixed-width column — the number is a MEASUREMENT, and measurements
        // that don't share a decimal column can't be compared at a glance
        // ("1M" next to "200k" next to "8k" read as noise when ragged).
        //
        // The marks sit in their own two fixed slots after it, so ★ and ✦
        // always land in the same place: a row without a favourite leaves a
        // hole rather than sliding its ✦ left into the ★ column, which is what
        // made the old list look jittery as you scrolled.
        std::string ctx;
        if (const int win = r.model.context_window; win > 0) {
            if (win >= 1'000'000) {
                ctx = std::to_string(win / 1'000'000) + "M";
                if (win % 1'000'000 != 0) ctx += "+";        // 1.x M → "1M+"
            } else if (win >= 1000) {
                ctx = std::to_string(win / 1'000) + "k";
            } else {
                ctx = std::to_string(win);
            }
        }
        // Widest realistic context label is 5 columns ("200k", "1M+").
        std::string trailing = ctx.size() < 5
            ? std::string(5 - ctx.size(), ' ') + ctx
            : ctx;
        trailing += r.model.favorite ? "  \xe2\x98\x85" : "   ";      // ★
        // Reasoning badge (precomputed in build_fused_rows) — marks models
        // that can think, so "which of these reason" is legible across
        // providers.
        trailing += r.reasons ? " \xe2\x9c\xa6" : "  ";               // ✦
        row.trailing       = std::move(trailing);
        // Dim by default: the trailing cell is REFERENCE data, not the thing
        // you are choosing between. It used to share the composer's warm
        // accent with the model name's own emphasis, so every row shouted.
        // The active row keeps the accent so the current model still reads at
        // a glance.
        row.trailing_style = active ? fg_of(accent) : fg_dim(muted);
        // NO-TOOLS: a WORD appended to the NAME, not another glyph in the
        // trailing cell. Three reasons: (1) it is a disqualifier, not
        // reference data, and the trailing cell is dim-grey precisely
        // because it IS reference — a warning painted as reference reads as
        // decoration; (2) an unexplained glyph is a puzzle and this picker
        // has no room for a legend; (3) the trailing cell is the FIRST
        // thing dropped under width pressure (yields_trailing below), so
        // the one mark that decides whether the model works at all would
        // vanish on a narrow terminal. "chat only" is self-explanatory,
        // needs no key, and travels with the name it disqualifies.
        if (!r.tool_capable) row.leading += " \xc2\xb7 chat only";
        // Under width pressure the CHIPS give way, not the model name — the
        // name is what you are selecting; the context window and marks are
        // reference data. Without this the default policy (leading yields
        // first) truncated "Claude Sonnet 4.6" to keep "200k ★ ✦" intact on a
        // narrow split, which is exactly backwards.
        row.trailing_secondary = true;
        cfg.rows.push_back(std::move(row));
    }
    cfg.selected = visual_selected;

    // Reasoning-effort control for the highlighted model — the shared
    // reasoning_effort_footer. ←/→ mutates the global m.d.effort live (no
    // staged tier), so the chip, the footer and the wire can't disagree.
    if (picker->index >= 0 && picker->index < static_cast<int>(rows.size())) {
        const auto& hl = rows[static_cast<std::size_t>(picker->index)];
        if (!hl.is_signin_offer())
            for (auto& row : reasoning_effort_footer(m, hl.model.id.value,
                                                     hl.provider_id))
                cfg.footer.push_back(std::move(row));
    }

    // Footer hints follow the mode: in slot-assign Enter PINS a role and Esc
    // goes BACK to Smart Mode, so promising "switch"/"close" would misstate
    // what the keys do. ^/ and ^Tab are switch-only affordances.
    // Written as two branches rather than one `slot >= 0 ? key_hints({...})
    // : key_hints({...})` ternary, which made GCC's -Wdangling-pointer fire.
    // That warning is a FALSE POSITIVE, not a lifetime bug worth preserving:
    // key_hints takes its vector BY VALUE and moves it into the component's
    // capture, so the braced temp is consumed before the full expression
    // ends and nothing outlives it. GCC just loses track of the temp's
    // ownership across the two ternary arms. The branches are equivalent
    // code, read better, and match the surrounding footer style — so this is
    // a quieting rewrite, not a fix. Restoring the ternary would be correct
    // C++ and would only bring the noise back.
    if (m.ui.smart_assign_slot)
        cfg.footer.push_back(key_hints({
            {"\xe2\x86\x91\xe2\x86\x93", "move", 5},
            {"Enter", "pin to role", 5},
            {"^F", "favorite", 1},
            {"^/", "provider", 2},
            {"^L", "refresh", 2},
            {"Esc", "back", 4},
          }));
    else
        cfg.footer.push_back(key_hints({
            {"\xe2\x86\x91\xe2\x86\x93", "move", 5},
            {"Enter", "switch", 5},
            {"^F", "favorite", 1},
            {"^/", "provider", 2},
            {"^L", "refresh", 2},
            {"^Tab", "prev", 2},
            {"Esc", "close", 4},
          }));
    return Panel{std::move(cfg)}.build();
}

// ── Provider picker helpers ──
// (active_provider_id + reasoning_effort_footer live in pickers_common.hpp.)

Element provider_picker(const Model& m) {
    auto* picker = m.ui.overlay.get<ov::ProviderPicker>();
    if (!picker) return nothing();

    Panel::Config cfg;
    cfg.title      = " Providers ";
    cfg.accent     = highlight;
    cfg.min_width  = 52;
    cfg.viewport_h = picker_viewport_h();
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

    cfg.rows.reserve(rows.size());
    for (const auto& r : rows) {
        Panel::Row row;

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
        cfg.rows.push_back(std::move(row));
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
