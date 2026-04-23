#include "moha/runtime/view/statusbar.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <maya/widget/model_badge.hpp>
#include <maya/widget/token_stream.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

constexpr int kCtxBarCells = 10;    // width of the mini context bar, in cells

std::string format_tokens(int n) {
    char buf[16];
    if (n >= 1'000'000) {
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1'000'000.0);
    } else if (n >= 1000) {
        std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%d", n);
    }
    return buf;
}

Color ctx_color(int pct) {
    if (pct < 60)       return success;
    if (pct <= 80)      return warn;
    return danger;
}

// Build a smooth 1/8-gradation progress bar. `pct` in [0, 100], rendered
// across `cells` columns using the unicode "left-aligned" block glyphs
// ▏▎▍▌▋▊▉█ for sub-cell resolution. Empty track uses U+2591 (░) so the
// bar reads as a channel rather than blank space.
std::string ctx_bar_glyphs(int pct, int cells) {
    static constexpr std::string_view kPartials[8] = {
        "", "\u258F", "\u258E", "\u258D",
        "\u258C", "\u258B", "\u258A", "\u2589"
    };
    pct = std::clamp(pct, 0, 100);
    int total_eighths = pct * cells * 8 / 100;
    std::string out;
    out.reserve(static_cast<std::size_t>(cells) * 3);
    for (int i = 0; i < cells; ++i) {
        int filled = std::max(0, total_eighths - i * 8);
        if      (filled >= 8) out += "\u2588";   // full block
        else if (filled >  0) out += kPartials[filled];
        else                  out += "\u2591";   // light shade track
    }
    return out;
}

// Full-width divider that separates the composer from the status row.
// Uses a thin horizontal rule whose ends fade out via partial-block bleed
// so the bar reads as "anchored" without a hard line cutting the screen.
// The middle is a continuous ─ at half-luminance; the leading/trailing
// few cells bleed via short ╴/╶ stubs so the eye sees a soft frame, not a
// guillotine.
Element divider_line() {
    return Element{ComponentElement{
        .render = [](int w, int /*h*/) -> Element {
            if (w <= 0) return text("", {});
            std::string line;
            line.reserve(static_cast<std::size_t>(w) * 3);
            // Fade-in/out: short stubs at each end, ─ in the middle.
            const int fade = std::min(3, w / 8);
            for (int i = 0; i < w; ++i) {
                if      (i == 0 && fade > 0)               line += "\xe2\x95\xb6";   // ╶
                else if (i == w - 1 && fade > 0)           line += "\xe2\x95\xb4";   // ╴
                else                                       line += "\xe2\x94\x80";   // ─
            }
            return text(std::move(line), Style{}.with_fg(muted).with_dim());
        },
        .layout = {},
    }};
}

// All separators are dim middle-dots with breathing room. Pro tools
// don't mix separator characters — one symbol, one rhythm.
Element sep_dot()  { return text("   \xc2\xb7   ", fg_dim(muted)); }  // ·
Element sep_thin() { return text(" \xc2\xb7 ",     fg_dim(muted)); }  // ·

// Helix / Lazygit / k9s style: bold key in default fg, dim label, no
// chip background, no per-key color. The inverse-video keycaps looked
// like stickers; pro TUIs lean on typography weight (bold vs dim) for
// hierarchy, not saturated chips. `cat` is unused now — kept on the
// signature so call sites don't need rewriting.
Element shortcut(const char* key, const char* label, Color /*cat*/) {
    return h(
        text(key, fg_bold(fg)),
        text(" ", {}),
        text(label, fg_dim(muted))
    ).build();
}

// Generous gap between key+label units; reads as discrete bindings
// without needing visual containers around them.
Element shortcut_gap() { return text("   ", {}); }

// Truncate a breadcrumb to a fixed width with a trailing ellipsis. Works
// on UTF-8 strings only approximately (assumes 1 column per byte for
// non-ASCII heuristics), which is fine for thread titles.
std::string truncate_middle(std::string_view s, std::size_t max_chars) {
    if (s.size() <= max_chars) return std::string{s};
    if (max_chars <= 1) return "\u2026";
    return std::string{s.substr(0, max_chars - 1)} + "\u2026";
}

// Profile indicator — plain colored text, no chip / no inverse video.
// Pro tools surface mode/profile as a labelled fragment in the status
// line (vim-modeline style), not as a sticker.
Element profile_tag(Profile p) {
    return text(std::string{profile_label(p)},
                Style{}.with_fg(profile_color(p)).with_bold());
}

// Phase indicator — colored glyph + bold verb, no chip background.
// Glyph carries the urgency (●/⠋/⚠), color reinforces it, weight tells
// the eye it's a state label vs free text.
Element phase_chip(std::string_view glyph, std::string_view verb, Color c) {
    return h(
        text(std::string{glyph}, fg_of(c)),
        text(" ", {}),
        text(std::string{verb}, Style{}.with_fg(c).with_bold())
    ).build();
}

// Subtle leading marker for the breadcrumb — a thin left edge bar, like
// a folder spine. Reads as "this is a labelled section" without taking
// horizontal space from the title itself.
Element edge_mark(Color c) { return text("\xe2\x96\x8e", fg_of(c)); } // ▎

// Build the sparkline samples in chronological (oldest → newest) order
// from the StreamState ring buffer. Empty entries (not yet sampled) are
// dropped so the bar starts narrow and grows up to kRateSamples wide as
// the stream produces more data.
std::vector<float> ordered_rate_history(const StreamState& s) {
    std::vector<float> out;
    out.reserve(StreamState::kRateSamples);
    if (s.rate_history_full) {
        // Wrap from pos to end, then 0 to pos.
        for (std::size_t i = 0; i < StreamState::kRateSamples; ++i) {
            const auto idx = (s.rate_history_pos + i) % StreamState::kRateSamples;
            out.push_back(s.rate_history[idx]);
        }
    } else {
        for (std::size_t i = 0; i < s.rate_history_pos; ++i)
            out.push_back(s.rate_history[i]);
    }
    return out;
}

// Walk the last assistant message and return the name of the tool call
// currently in `Running` state, if any. Lets the phase chip render
// "▌ ⠋ bash ▐" when ExecutingTool is active instead of the generic
// "Running" label — more useful at a glance.
std::string_view running_tool_name(const Model& m) {
    if (m.d.current.messages.empty()) return {};
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return {};
    for (const auto& tc : last.tool_calls) {
        if (tc.is_running()) return tc.name.value;
    }
    return {};
}


} // namespace

Element status_bar(const Model& m) {
    bool is_streaming = m.s.is_streaming() && m.s.active;

    // Phase chip colors + glyph resolved up front; used inside the
    // width-aware builder below.
    Color pcolor = phase_color(m.s.phase);
    bool spin = is_streaming || m.s.is_executing_tool();
    std::string phase_icon = spin
        ? std::string{m.s.spinner.current_frame()}
        : std::string{phase_glyph(m.s.phase)};
    std::string phase_label{phase_verb(m.s.phase)};
    if (m.s.is_executing_tool()) {
        if (auto tn = running_tool_name(m); !tn.empty())
            phase_label = std::string{tn};
    }

    // Context fullness uses `tokens_in` (includes cache_read + creation).
    // Value is the cumulative prefix size from the most recent request.
    int ctx_used = m.s.tokens_in;
    bool has_tokens = ctx_used > 0;
    int pct = (m.s.context_max > 0)
                  ? std::min(100, ctx_used * 100 / m.s.context_max)
                  : 0;
    bool has_breadcrumb = !m.d.current.title.empty();

    // ── Responsive activity row ─────────────────────────────────────────
    // Sections are dropped progressively as width shrinks so the row never
    // wraps to a second line. Priority (kept longest → dropped first):
    //   phase_pill, model_badge, ctx (always)
    //   profile, ctx_bar + absolute count (>= 60)
    //   ↑↓ token counts (>= 90)
    //   breadcrumb (>= 100)
    //   live TokenStream sparkline (>= 120)
    auto activity_row = Element{ComponentElement{
        .render = [=, &m](int w, int /*h*/) -> Element {
            if (w <= 0) return text("", {});

            auto phase_pill = phase_chip(phase_icon, phase_label, pcolor);

            // ── Left group ─────────────────────────────────────────────
            std::vector<Element> lparts;
            lparts.push_back(text(" ", {}));
            if (has_breadcrumb && w >= 100) {
                // Title budget scales with width so we don't elbow out the
                // right group on medium terminals.
                std::size_t title_budget = (w >= 140) ? 28
                                         : (w >= 120) ? 20
                                                      : 12;
                lparts.push_back(h(
                    edge_mark(pcolor),
                    text(" " + truncate_middle(m.d.current.title, title_budget),
                         fg_of(fg).with_bold()),
                    sep_dot()
                ).build());
            }
            lparts.push_back(phase_pill);
            if (w >= 60) {
                lparts.push_back(sep_thin());
                lparts.push_back(profile_tag(m.d.profile));
            }
            auto left = hstack()(std::move(lparts));

            // ── Right group ────────────────────────────────────────────
            std::vector<Element> right_parts;

            // Live tok/s + sparkline — most expensive section visually, so
            // gate it on wide terminals only.
            if (is_streaming && w >= 120
                && m.s.first_delta_at.time_since_epoch().count() != 0) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - m.s.first_delta_at).count();
                if (ms >= 250) {
                    double sec = static_cast<double>(ms) / 1000.0;
                    double approx_tok = static_cast<double>(
                        m.s.live_delta_bytes) / 4.0;
                    float  rate = static_cast<float>(approx_tok / sec);

                    TokenStream ts;
                    ts.set_compact(true);
                    ts.set_tokens_per_sec(rate);
                    ts.set_total_tokens(static_cast<int>(approx_tok));
                    ts.set_elapsed(static_cast<float>(sec));
                    ts.set_rate_history(ordered_rate_history(m.s));
                    ts.set_color(highlight);
                    right_parts.push_back(ts.build());
                    right_parts.push_back(sep_dot());
                }
            }

            // Model badge — always shown.
            {
                ModelBadge mb;
                mb.set_model(m.d.model_id.value);
                mb.set_compact(true);
                right_parts.push_back(mb.build());
            }

            // Per-direction tokens.
            if (has_tokens && w >= 90) {
                right_parts.push_back(sep_thin());
                std::string tok_str = "\xe2\x86\x91"
                    + format_tokens(m.s.tokens_in) + " \xe2\x86\x93"
                    + format_tokens(m.s.tokens_out);
                right_parts.push_back(text(tok_str, fg_dim(muted)));
            }

            // Context indicator — always when we have a usage event; the
            // visual bar + absolute count drop away below 60 cols,
            // leaving just "ctx 32%".
            if (m.s.context_max > 0 && has_tokens) {
                Color c = ctx_color(pct);
                right_parts.push_back(sep_thin());
                right_parts.push_back(text("ctx ", fg_dim(muted)));
                if (w >= 60) {
                    std::string used_str = format_tokens(ctx_used) + "/"
                                         + format_tokens(m.s.context_max) + " ";
                    right_parts.push_back(text(used_str, fg_dim(muted)));
                    right_parts.push_back(
                        text(ctx_bar_glyphs(pct, kCtxBarCells), fg_of(c)));
                }
                char pbuf[8];
                std::snprintf(pbuf, sizeof(pbuf), " %d%%", pct);
                right_parts.push_back(text(pbuf, fg_of(c).with_bold()));
            }

            right_parts.push_back(text(" ", {}));
            auto right = hstack()(std::move(right_parts));

            return h(left, spacer(), right).build();
        },
        .layout = {},
    }};

    // ── Error / transient status banner ──────────────────────────────────
    Element status_row;
    bool has_status = !m.s.status.empty() && m.s.status != "ready";
    if (has_status) {
        bool is_err = m.s.status.rfind("error:", 0) == 0;
        Color bc = is_err ? danger : muted;
        // Error banner: leading edge mark + italic text, no bg — same
        // reasoning as the activity row above.
        status_row = h(
            text(" ", {}),
            edge_mark(bc),
            text(is_err ? " \xe2\x9a\xa0  " : "  ", fg_of(bc)),
            text(m.s.status, fg_of(bc).with_italic())
        ).build();
    }

    // Bottom divider — twins the top one so the activity row sits
    // bracketed between two thin rules, reading as a discrete panel
    // without bg color trickery. The shortcut row below is then
    // visually clear it's a *separate* hint strip, not part of the
    // status panel.
    auto bottom_divider = divider_line();

    // ── Shortcut row ─────────────────────────────────────────────────────
    // Single-color caps (highlight) for everything except quit, which is
    // danger red so the affordance is unambiguous. Category-coloring every
    // key looked rainbow-busy; one neutral color + one warning color is
    // the standard polished TUI treatment (see htop F-keys, fzf, lazygit).
    auto shortcuts = h(
        text(" ", {}),
        shortcut("^K",    "palette", highlight),  shortcut_gap(),
        shortcut("^J",    "threads", highlight),  shortcut_gap(),
        shortcut("S-Tab", "profile", highlight),  shortcut_gap(),
        shortcut("^/",    "models",  highlight),  shortcut_gap(),
        shortcut("^N",    "new",     highlight),  shortcut_gap(),
        shortcut("^C",    "quit",    danger)
    ).build();

    if (has_status) {
        return v(divider_line(), activity_row, status_row,
                 std::move(bottom_divider), shortcuts).build();
    }
    return v(divider_line(), activity_row,
             std::move(bottom_divider), shortcuts).build();
}

} // namespace moha::ui
