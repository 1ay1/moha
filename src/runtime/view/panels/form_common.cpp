// form_view.cpp — project a form::Form onto maya::Panel::Config.
//
// This file is deliberately dull, and that is the point: it maps fields to
// controls one-for-one and makes NO appearance decisions. Every glyph, colour,
// column, caret, bar and the floating menu belong to maya::Panel. If a change
// here starts reaching for a Unicode escape or a Color, it belongs in the
// widget instead.
//
// The one substantive rule enforced here: a Secret is projected as a LENGTH,
// never as its value. maya::panel::Secret has no field that could hold the
// plaintext, so the credential does not cross the rendering boundary at all.

#include "agentty/runtime/view/form_panel.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace agentty::ui {

namespace {

namespace ff = agentty::form::field;

// Probe a path field so the widget can badge it exists/missing. Done here (the
// view builds once per frame, off the hot wire path) rather than in the
// reducer, which must stay pure.
[[nodiscard]] maya::panel::Path::State probe_path(const ff::Path& p) {
    if (p.value.empty()) return maya::panel::Path::State::Unknown;
    std::error_code ec;
    namespace fs = std::filesystem;
    const bool ok = p.want_dir ? fs::is_directory(p.value, ec)
                               : fs::is_regular_file(p.value, ec);
    return ok ? maya::panel::Path::State::Exists
              : maya::panel::Path::State::Missing;
}

[[nodiscard]] maya::PanelControl control_for(const agentty::form::Field& f,
                                            bool editing_this_row) {
    const std::size_t caret = agentty::form::caret_chars(f.value, editing_this_row);

    return std::visit([&](const auto& v) -> maya::PanelControl {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, ff::Toggle>) {
            return maya::panel::Toggle{v.on};
        }
        else if constexpr (std::is_same_v<T, ff::Choice>) {
            return maya::panel::Choice{std::string{v.label()}};
        }
        else if constexpr (std::is_same_v<T, ff::Number>) {
            return maya::panel::Number{v.value};
        }
        else if constexpr (std::is_same_v<T, ff::Slider>) {
            return maya::panel::Slider{v.value, v.min, v.max, v.decimals};
        }
        else if constexpr (std::is_same_v<T, ff::Text>) {
            return maya::panel::Text{v.value, caret, {}};
        }
        else if constexpr (std::is_same_v<T, ff::Secret>) {
            // LENGTH ONLY — the plaintext never reaches the widget.
            return maya::panel::Secret{agentty::form::secret_filled(v), caret};
        }
        else if constexpr (std::is_same_v<T, ff::Path>) {
            return maya::panel::Path{v.value, caret, probe_path(v), {}};
        }
        else if constexpr (std::is_same_v<T, ff::Pick>) {
            return maya::panel::Pick{v.label, v.placeholder};
        }
        else if constexpr (std::is_same_v<T, ff::Header>) {
            return maya::panel::Header{};
        }
        else if constexpr (std::is_same_v<T, ff::Action>) {
            maya::panel::Action a;
            a.status = v.status;
            a.hint   = v.hint;
            switch (v.tone) {
                case ff::Action::Tone::Busy:    a.tone = maya::panel::Action::Tone::Busy;  break;
                case ff::Action::Tone::Good:    a.tone = maya::panel::Action::Tone::Good;  break;
                case ff::Action::Tone::Bad:     a.tone = maya::panel::Action::Tone::Bad;   break;
                case ff::Action::Tone::Neutral: a.tone = maya::panel::Action::Tone::Neutral; break;
            }
            return a;
        }
        else {
            return maya::panel::Toggle{false};
        }
    }, f.value);
}

} // namespace

maya::Panel::Config form_config(const agentty::form::Form& f, maya::Color accent,
                               maya::ScrollState* scroll, int viewport_h,
                               int max_width) {
    maya::Panel::Config cfg;
    cfg.title    = f.title;
    cfg.subtitle = f.subtitle;
    cfg.note     = f.note;
    cfg.selected = f.cursor;
    cfg.accent   = accent;
    cfg.scroll   = scroll;
    if (viewport_h > 0) cfg.viewport_h = viewport_h;
    // A min-width wider than the terminal is not a minimum, it is an overflow:
    // the overlay centres the panel, so the excess is split off BOTH edges and
    // the label column disappears. Leave room for the frame + the overlay's
    // own inset, and never go below a floor that can still show a row.
    if (max_width > 0)
        cfg.min_width = std::max(24, std::min(cfg.min_width, max_width - 6));

    const bool editing = f.editing();
    cfg.items.reserve(f.fields.size());
    for (std::size_t i = 0; i < f.fields.size(); ++i) {
        const auto& src = f.fields[i];
        const bool on_row = (static_cast<int>(i) == f.cursor);

        maya::Panel::Item row;
        row.leading       = src.label;
        row.help          = src.help;
        row.origin        = src.origin;
        row.locked        = src.locked;
        row.locked_reason = src.locked_reason;
        row.error         = src.error;
        row.control       = control_for(src, editing && on_row);
        cfg.items.push_back(std::move(row));
    }

    // The open dropdown, if any. Its contents come from the SAME function the
    // reducer resolves a commit against, so what is shown and what Enter
    // selects cannot drift apart.
    if (const auto* d = f.dropdown()) {
        if (const auto* row = f.focused(); row && row->is_choice()) {
            const auto& c = std::get<ff::Choice>(row->value);
            const auto opts = agentty::form::visible_options(c);

            maya::Panel::Menu menu;
            menu.options     = opts.labels;
            menu.hints       = opts.hints;
            menu.highlighted = d->highlighted;
            // The COMMITTED value, distinct from the cursor: the list marks
            // "what is saved" with ◉ and "where you are" with ❯, and they
            // diverge the moment the user moves.
            menu.current     = c.normalized(c.index);
            menu.scroll      = d->scroll;
            menu.viewport    = agentty::form::kDropdownViewport;

            cfg.menu     = std::move(menu);
            cfg.menu_row = f.cursor;
        }
    }

    return cfg;
}

} // namespace agentty::ui
