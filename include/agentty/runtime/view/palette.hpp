#pragma once
// agentty::ui — terminal palette using only named ANSI colors.
//
// Named-ANSI-only is a deliberate constraint: the user's terminal theme
// always wins.  We pick semantic names; the terminal decides the hue.

#include <maya/style/color.hpp>
#include <maya/style/style.hpp>

#include "agentty/runtime/panel/palette.hpp"   // PaletteContext, Command
#include "agentty/runtime/model.hpp"            // Model

namespace agentty::ui {

// ── Semantic palette (named ANSI only — terminal theme wins) ──────────────
//
// Two layers below: legacy "brand" names (back-compat with existing widget
// configs) and a new token layer organised by axis. The discipline is
// "one hue = one axis": status colors (green/yellow/red) ONLY mean status
// (ok/warn/error), code-reference cyan ONLY means "filesystem path or
// code identifier," brand magenta ONLY means agentty identity (Write
// profile, queue chips), info blue ONLY means information / context-shift
// (threads, vcs). Tool categories compress to a tonal palette so a long
// run of inspect-class tools reads as a calm gray list, with magenta
// edits and cyan bashes standing out as the meaningful actions.
//
// `fg` is the primary body-text color. ANSI 7 ("white") renders as a mid-
// gray in most modern terminal themes (Catppuccin, Solarized, One Dark,
// Gruvbox) — readable, but not the brightest the terminal offers. Prose
// that the user is actually *reading* (their own typed message, the
// assistant's reply paragraphs) maps to ANSI 15 ("bright_white") so it
// pops against the theme's background at maximum contrast. Chrome and
// metadata still use `muted` / `with_dim()` to recede.
inline constexpr auto fg          = maya::Color::bright_white();
inline constexpr auto muted       = maya::Color::bright_black();
inline constexpr auto accent      = maya::Color::magenta();   // brand / Write profile
inline constexpr auto info        = maya::Color::blue();      // Ask profile / threads
inline constexpr auto success     = maya::Color::green();     // accepted / running OK
inline constexpr auto warn        = maya::Color::yellow();    // pending / amber
inline constexpr auto danger      = maya::Color::red();       // errors / rejected
inline constexpr auto highlight   = maya::Color::cyan();      // command palette / mentions

// ── Token layer (axis-disciplined). Prefer these in new code. ────────────

// Text hierarchy — three tiers from primary prose down to chrome.
inline constexpr auto text_primary   = maya::Color::bright_white();  // prose, headlines
inline constexpr auto text_secondary = maya::Color::white();         // mid-tone metadata (inspect tools, neutral chrome)
inline constexpr auto text_tertiary  = maya::Color::bright_black();  // footers, hints, blanks

// Status — severity / outcome ONLY. Never a category color.
inline constexpr auto status_ok      = maya::Color::bright_green();
inline constexpr auto status_info    = maya::Color::bright_blue();
inline constexpr auto status_warn    = maya::Color::bright_yellow();
inline constexpr auto status_error   = maya::Color::bright_red();

// Code references — file paths, identifiers, command args.
inline constexpr auto code_path      = maya::Color::bright_cyan();   // file paths, identifiers
inline constexpr auto code_text      = maya::Color::cyan();          // code-block body content

// Role / identity — persistent identifier colors. One hue per role.
inline constexpr auto role_brand     = maya::Color::magenta();          // agentty brand, Write profile, queue chips
inline constexpr auto role_brand_alt = maya::Color::bright_magenta();   // queued-message chips, alt brand
inline constexpr auto role_info      = maya::Color::blue();             // Ask profile, threads, vcs context

// ── Style presets — terminal default fg unless overridden ─────────────────
inline maya::Style dim()    { return maya::Style{}.with_dim(); }
inline maya::Style bold()   { return maya::Style{}.with_bold(); }
inline maya::Style italic() { return maya::Style{}.with_italic(); }

inline maya::Style fg_of(maya::Color c)         { return maya::Style{}.with_fg(c); }
inline maya::Style fg_bold(maya::Color c)       { return maya::Style{}.with_fg(c).with_bold(); }

// `fg_dim` returns "dim color" — but `with_dim()` on an already-muted
// color (bright_black / gray) collapses below the readable floor on
// dark / low-contrast themes (true-black backgrounds, OLED palettes,
// some Solarized variants). The intent of `fg_dim(muted)` is "subdued
// secondary text," and bright_black ALONE already carries that role
// on every reasonable theme — stacking the SGR `dim` attribute on top
// just trades readability for nothing. So suppress the `with_dim()`
// when the color is bright_black; keep it for everything else, where
// dimming a bright color is exactly the meaningful signal we want
// (a muted form of the brand color, etc.).
inline maya::Style fg_dim(maya::Color c) {
    const bool is_already_muted =
        c.kind() == maya::Color::Kind::Named
        && c.index() == static_cast<uint8_t>(maya::AnsiColor::BrightBlack);
    return is_already_muted
        ? maya::Style{}.with_fg(c)
        : maya::Style{}.with_fg(c).with_dim();
}
inline maya::Style fg_italic(maya::Color c)     { return maya::Style{}.with_fg(c).with_italic(); }

// ── Palette row visibility ─────────────────────────────────────
//
// Rows are GATED — Review/Accept-all need pending changes, Run-code-block a
// fenced reply, Update a release — and THREE places must agree on which are
// visible: the VIEW that renders the list, the REDUCER that resolves a cursor
// to a command, and back_to() restoring a cursor after Esc. When they
// disagree the palette shows one list while the cursor indexes another, so
// Enter fires a neighbour and Esc restores the wrong row.
//
// It was written out twice — in palette.cpp and in nav_pickers.cpp — each
// commented "the SAME predicate" as the other. One definition makes that
// comment true rather than aspirational.
//
// Lives here rather than beside the command table because it needs the Model,
// and command_palette.hpp is deliberately Model-free.
[[nodiscard]] PaletteContext palette_context(const Model& m);

} // namespace agentty::ui
