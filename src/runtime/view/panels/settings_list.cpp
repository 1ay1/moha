// settings_list_view.cpp — the settings pickers (Ctrl+K →
// Plugins/Commands/Agents/Hooks). One list overlay, built on the house
// Picker widget so it frames/scrolls exactly like every other picker.
//
// Two modes, both rendered here:
//   • list  — rows from settings::items_for(concern), each led by a HEALTH
//             badge (● connected / ◌ connecting / ⚠ attention) or, for
//             navigation rows, an affordance arrow; the Enter action lives in
//             the right-aligned hint + footer, never the badge.
//   • add   — a one-line prompt (header) with a live caret; footer shows
//             the format hint + submit/cancel keys.

#include "agentty/runtime/view/panels.hpp"
#include "panels_common.hpp"   // kPanel* widths, panel_viewport_h

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/hints.hpp"   // key_hints — the shared footer idiom
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/panel/settings/list.hpp"
#include "agentty/runtime/panel/settings/items.hpp"

#include <maya/widget/panel.hpp>
#include <maya/widget/panel/caret.hpp>

#include <algorithm>
#include <string>

namespace pn = agentty::ui::panel;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace se = agentty::settings;

namespace {

struct Badge { std::string glyph; Color color; };

// Health badge — the LEADING glyph on a top-level row. A row shows its STATE
// at a glance (connected / connecting / failed), the way every agentty status
// surface does. The Enter action (remove / approve) is communicated by the
// footer + the right-aligned hint, NOT the badge — so a healthy connected
// server never wears a scary red ✕.
Badge status_badge(se::Item::Status s) {
    switch (s) {
        case se::Item::Status::Ok:      return {"\xe2\x97\x8f", success};  // ● connected/healthy
        case se::Item::Status::Pending: return {"\xe2\x97\x8c", muted};    // ◌ connecting…
        case se::Item::Status::Bad:     return {"\xe2\x9a\xa0", warn};     // ⚠ error/attention
        case se::Item::Status::Neutral:
        default:                        return {" ", muted};
    }
}

// Navigation rows (RAG / Smart / profile) that jump elsewhere show a subtle
// affordance arrow instead of a health dot — they have no health, they're
// doors. Returns std::nullopt for rows that should use the status badge.
std::optional<Badge> nav_badge(se::Action a) {
    switch (a) {
        case se::Action::CycleProfile: return Badge{"\xe2\x86\xbb", info};      // ↻
        case se::Action::OpenRag:
        case se::Action::OpenSmart:    return Badge{"\xe2\x86\x92", highlight};  // →
        default:                       return std::nullopt;
    }
}

// Whether this concern supports the inline `a`dd flow.
bool can_add(se::Category c) {
    return c == se::Category::Plugins
        || c == se::Category::Commands
        || c == se::Category::Agents;
}

// The prompt shown while typing a new entry, per concern.
const char* add_prompt(se::Category c) {
    switch (c) {
        case se::Category::Plugins:
            return "name command args\xe2\x80\xa6  (or --http url / --python f.py / "
                   "--uvx pkg / --npx pkg; add --project for the repo)";
        case se::Category::Commands: return "new command name (creates .agentty/commands/<name>.md)";
        case se::Category::Agents:   return "new subagent name (creates .agentty/agents/<name>.md)";
        default:                     return "";
    }
}

} // namespace

Element settings_list_panel(const Model& m) {
    const auto* o = m.ui.panel.get<pn::SettingsList>();
    if (!o) return nothing();

    const bool adding = o->input_active;
    auto rows = se::items_for(m, o->concern);

    Panel::Config cfg;
    cfg.title      = std::string{" "} + se::label(o->concern) + " ";
    cfg.accent     = highlight;   // cyan, matching the command palette
    cfg.min_width  = panel_detail::kPanelWide;   // value + provenance column
    // The SHARED responsive height, not a hardcoded 14. A fixed list taller
    // than the terminal scrolls the base's top rows into native scrollback
    // that cannot be reclaimed — which is the whole reason
    // panel_viewport_h() exists (see pickers_common.hpp).
    cfg.viewport_h = panel_detail::panel_viewport_h();
    // Scroll like every other picker — the Plugins pane can exceed any
    // viewport (a server advertising 125 tools is 135+ rows), and a
    // scroll-less panel paints the overflow past the modal border.
    cfg.scroll     = &m.ui.settings_list_scroll;
    cfg.selected   = adding ? -1 : o->index;

    // ── Header ─────────────────────────────────────────────────
    if (adding) {
        // The live add prompt. Make it visually own the pane: a bright
        // prompt line with a REAL caret, the format hint above it dim.
        // cfg.selected == -1 already removes the list highlight, so the
        // eye lands here.
        //
        // with_caret, not a glyph appended at the end: the caret renders
        // AT o->cursor (←/→ work mid-string), and a long pasted command
        // line windows under it (‹/› markers) instead of walking off the
        // right edge — same machinery as every panel text field.
        cfg.header.push_back(h(
            text("  "), text(add_prompt(o->concern), fg_dim(muted))
        ).build());
        cfg.header.push_back(h(
            text("  \xe2\x9c\x8e  ", fg_of(highlight)),         // ✎ pencil
            text(maya::panel::detail::with_caret(
                     o->input,
                     static_cast<std::size_t>(std::max(0, o->cursor)),
                     panel_detail::panel_terminal_cols() - 12),
                 fg_bold(fg))
        ).build());
    } else {
        cfg.header.push_back(h(
            text("  "), text(se::subtitle(o->concern), fg_dim(muted))
        ).build());
    }
    cfg.header.push_back(text(""));

    // ── Rows (dimmed while adding, to focus the prompt) ──────────
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& it = rows[static_cast<std::size_t>(i)];
        // Top-level rows lead with a HEALTH badge (status), except pure
        // navigation rows (RAG/Smart/profile) which show an affordance arrow.
        const Badge b = nav_badge(it.action).value_or(status_badge(it.status));

        Panel::Item row;
        // A plugin's tool rows are indented under their server with a tree
        // connector (├─ / └─ for the last one), a dim on/off checkbox, and
        // dimmed text — so the server→tools hierarchy reads at a glance
        // instead of a flat list of same-looking dotted rows. Top-level rows
        // lead with their health/nav badge and a brighter name.
        if (it.indented) {
            // Last child = the next row is a top-level row (or the end).
            const bool last =
                (i + 1 >= static_cast<int>(rows.size()))
                || !rows[static_cast<std::size_t>(i + 1)].indented;
            const char* elbow = last ? "\xe2\x94\x94\xe2\x94\x80 "   // └─
                                     : "\xe2\x94\x9c\xe2\x94\x80 ";  // ├─
            const char* box   = it.on ? "\xe2\x97\x89" : "\xe2\x97\x8b"; // ◉ / ○
            row.badge       = std::string("  ") + elbow + box;
            // Connector + box are structural — dim; the tool NAME carries the
            // on/off emphasis. But when the tool is INACTIVE (its plugin is
            // disabled/not connected) the WHOLE subtree greys out — name and
            // all — so it reads as "present but can't run," no matter each
            // tool's individual state. `adding` also dims everything.
            const bool dim = adding || it.inactive;
            row.badge_style = fg_dim(muted);
            row.leading       = it.primary;
            row.leading_style = fg_of(dim ? muted : (it.on ? fg : muted));
        } else {
            row.badge       = b.glyph;
            row.badge_style = fg_of(adding ? muted : b.color);
            row.leading       = it.primary;
            row.leading_style = fg_of(adding ? muted : fg);
        }
        row.trailing       = it.secondary;
        row.trailing_style = fg_dim(muted);
        // Armed for removal (first `d` pressed on this row) — paint it as a
        // clear, reversible danger prompt: the second `d` commits, anything
        // else disarms.
        if (!adding && !it.indented && !it.arg.empty()
            && o->confirm_remove == it.arg) {
            row.badge       = "\xe2\x9a\xa0";           // ⚠
            row.badge_style = fg_of(warn);
            row.leading_style = fg_bold(warn);
            row.trailing       = "press d again to remove";
            row.trailing_style = fg_bold(warn);
        }
        cfg.items.push_back(std::move(row));
    }

    // ── Footer ─────────────────────────────────────────────────
    if (adding) {
        // Mode hint in the note line — the same slot the panel's derived
        // editing hint uses, so the add prompt reads like every other
        // editing mode (armed-delete gets the same treatment below).
        cfg.note = "typing \xc2\xb7 \xe2\x86\xb5 create \xc2\xb7 esc cancel";
    } else if (!o->confirm_remove.empty()) {
        cfg.note = "d confirm remove \xc2\xb7 any other key cancels";
    } else {
        cfg.footer.push_back(text(""));
        // Action hint for the focused row, then keys (+ `a add`).
        std::string act_hint;
        if (o->index >= 0 && o->index < static_cast<int>(rows.size()))
            act_hint = rows[static_cast<std::size_t>(o->index)].hint;
        if (!act_hint.empty())
            cfg.footer.push_back(h(
                text("  \xe2\x86\xb5 ", fg_of(highlight)),
                text(act_hint, fg_of(fg))
            ).build());

        // The shared key_hints idiom (hints.hpp), not hand-built runs —
        // every other picker's footer goes through it, and the drift this
        // file had (its own spacing, its own hue choices) is exactly what
        // the helper exists to prevent. Conditional verbs stay conditional.
        std::vector<Hint> keys{{"\xe2\x86\x91\xe2\x86\x93", "move", 5},
                               {"\xe2\x86\xb5", "act", 5}};
        if (can_add(o->concern))                     keys.push_back({"a", "add", 2, success});
        if (o->concern == se::Category::Plugins)     keys.push_back({"d", "remove", 3, warn});
        keys.push_back({"esc", "close", 5});
        cfg.footer.push_back(key_hints(std::move(keys)));
    }

    return Panel{std::move(cfg)}.build();
}

} // namespace agentty::ui
