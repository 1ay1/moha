#include "moha/runtime/view/statusbar.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

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
Element divider_line() {
    return Element{ComponentElement{
        .render = [](int w, int /*h*/) -> Element {
            std::string line;
            line.reserve(static_cast<std::size_t>(w) * 3);
            for (int i = 0; i < w; ++i) line += "\u2500";
            return text(std::move(line), Style{}.with_fg(muted).with_dim());
        },
        .layout = {},
    }};
}

// Subtle separator between items within a group.
Element sep_dot() { return text("  \u00B7  ", fg_dim(muted)); }

Element shortcut(const char* key, const char* label) {
    return h(
        text(key, fg_bold(fg)),
        text(" ", {}),
        text(label, fg_dim(muted))
    ).build();
}

Element shortcut_gap() { return text("   ", {}); }

// Truncate a breadcrumb to a fixed width with a trailing ellipsis. Works
// on UTF-8 strings only approximately (assumes 1 column per byte for
// non-ASCII heuristics), which is fine for thread titles.
std::string truncate_middle(std::string_view s, std::size_t max_chars) {
    if (s.size() <= max_chars) return std::string{s};
    if (max_chars <= 1) return "\u2026";
    return std::string{s.substr(0, max_chars - 1)} + "\u2026";
}

// Profile pill rendered as an inverse-filled tag so it reads as a chip.
// Using with_inverse preserves the terminal theme's foreground/background
// pair rather than imposing a background color that may clash.
Element profile_tag(Profile p) {
    Color c = profile_color(p);
    auto body = Style{}.with_fg(c).with_inverse().with_bold();
    auto rim  = Style{}.with_fg(c);
    return h(
        text("\u258C", rim),                                // ▌ left rim
        text(" " + std::string{profile_label(p)} + " ", body),
        text("\u2590", rim)                                 // ▐ right rim
    ).build();
}

} // namespace

Element status_bar(const Model& m) {
    bool is_streaming = m.stream.is_streaming() && m.stream.active;

    // ── Left group: breadcrumb + state ───────────────────────────────────
    auto phase_style = Style{}.with_fg(phase_color(m.stream.phase)).with_bold();

    // Animate the phase glyph with the shared spinner while streaming, so
    // the user sees that the backend is alive; other phases keep their
    // semantic glyph (⚠ for permission, ▶ for running, ● for idle).
    std::string phase_icon = is_streaming
        ? std::string{m.stream.spinner.current_frame()}
        : std::string{phase_glyph(m.stream.phase)};

    auto phase_pill = h(
        text(phase_icon, phase_style),
        text(" ", {}),
        text(std::string{phase_verb(m.stream.phase)}, phase_style)
    ).build();

    // Thread breadcrumb — truncated so it never pushes the resources off
    // the right edge. Omitted entirely for unnamed threads.
    Element breadcrumb = text("", {});
    bool has_breadcrumb = !m.current.title.empty();
    if (has_breadcrumb) {
        breadcrumb = h(
            text(truncate_middle(m.current.title, 28), fg_dim(fg)),
            sep_dot()
        ).build();
    }

    auto left = h(
        text(" ", {}),
        breadcrumb,
        phase_pill,
        sep_dot(),
        profile_tag(m.profile)
    ).build();

    // ── Right group: live rate + resources ───────────────────────────────
    int total_tok = m.stream.tokens_in + m.stream.tokens_out;
    bool has_tokens = total_tok > 0;
    int pct = (m.stream.context_max > 0)
                  ? (total_tok * 100 / m.stream.context_max)
                  : 0;

    std::vector<Element> right_parts;

    // Live tokens/sec during streaming — acts as a speedometer. Driven by
    // local delta-byte accumulation (~4 B/token for Claude tokenizer)
    // because Anthropic only emits message_delta.usage rarely, often once
    // before message_stop. Divisor is time since the FIRST delta (not since
    // message_start) so TTFT doesn't drag the rate down. Shown the moment
    // we have ~250 ms of post-first-token data.
    if (is_streaming && m.stream.first_delta_at.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m.stream.first_delta_at).count();
        if (ms >= 250) {
            double sec = static_cast<double>(ms) / 1000.0;
            // 4 B/token: middle of Claude tokenizer range (~3.5–4.5 for
            // English/code). Conservative — slightly underestimates speed.
            double approx_tok = static_cast<double>(m.stream.live_delta_bytes) / 4.0;
            double rate = approx_tok / sec;
            char buf[24];
            // Show one decimal under 10 tok/s so slow streams don't read as
            // a flat "2 tok/s" — that's where precision matters most.
            if (rate < 10.0)
                std::snprintf(buf, sizeof(buf), "%.1f tok/s", rate);
            else
                std::snprintf(buf, sizeof(buf), "%.0f tok/s", rate);
            right_parts.push_back(text(buf, fg_of(success).with_bold()));
            right_parts.push_back(sep_dot());
        }
    }

    right_parts.push_back(text(m.model_id.value, fg_of(accent).with_bold()));

    if (has_tokens) {
        right_parts.push_back(sep_dot());
        std::string tok_str = "\u2191" + format_tokens(m.stream.tokens_in)
                            + " \u2193" + format_tokens(m.stream.tokens_out);
        right_parts.push_back(text(tok_str, fg_dim(muted)));
    }

    // Mini context bar — smooth 1/8-gradation blocks, colored by threshold.
    // Always show when we have a max; reads as "headroom" rather than a
    // single dimensionless percentage.
    if (m.stream.context_max > 0) {
        Color c = ctx_color(pct);
        right_parts.push_back(sep_dot());
        right_parts.push_back(text("ctx ", fg_dim(muted)));
        right_parts.push_back(text(ctx_bar_glyphs(pct, kCtxBarCells), fg_of(c)));
        char pbuf[8];
        std::snprintf(pbuf, sizeof(pbuf), " %d%%", pct);
        right_parts.push_back(text(pbuf, fg_of(c).with_bold()));
    }

    right_parts.push_back(text(" ", {}));

    auto right = hstack()(std::move(right_parts));

    auto activity_row = h(left, spacer(), right).build();

    // ── Error / transient status banner ──────────────────────────────────
    Element status_row;
    bool has_status = !m.stream.status.empty() && m.stream.status != "ready";
    if (has_status) {
        bool is_err = m.stream.status.rfind("error:", 0) == 0;
        status_row = h(
            text(" ", {}),
            text(is_err ? "\u26A0 " : "", fg_of(is_err ? danger : muted)),
            text(m.stream.status, fg_of(is_err ? danger : muted))
        ).build();
    }

    // ── Shortcut row ─────────────────────────────────────────────────────
    auto shortcuts = h(
        text(" ", {}),
        shortcut("^K",    "palette"),  shortcut_gap(),
        shortcut("^J",    "threads"),  shortcut_gap(),
        shortcut("S-Tab", "profile"),  shortcut_gap(),
        shortcut("^/",    "models"),   shortcut_gap(),
        shortcut("^N",    "new"),      shortcut_gap(),
        shortcut("^C",    "quit")
    ).build();

    if (has_status) {
        return v(divider_line(), activity_row, status_row, shortcuts).build();
    }
    return v(divider_line(), activity_row, shortcuts).build();
}

} // namespace moha::ui
