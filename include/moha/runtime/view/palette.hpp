#pragma once
// moha::ui — terminal palette using only named ANSI colors.
//
// Named-ANSI-only is a deliberate constraint: the user's terminal theme
// always wins.  We pick semantic names; the terminal decides the hue.

#include <maya/style/color.hpp>
#include <maya/style/style.hpp>

namespace moha::ui {

// ── Semantic palette (named ANSI only — terminal theme wins) ──────────────
inline constexpr auto fg          = maya::Color::white();
inline constexpr auto muted       = maya::Color::bright_black();
inline constexpr auto accent      = maya::Color::magenta();   // brand / Write profile
inline constexpr auto info        = maya::Color::blue();      // Ask profile / threads
inline constexpr auto success     = maya::Color::green();     // accepted / running OK
inline constexpr auto warn        = maya::Color::yellow();    // pending / amber
inline constexpr auto danger      = maya::Color::red();       // errors / rejected
inline constexpr auto highlight   = maya::Color::cyan();      // command palette / mentions

// ── Style presets — terminal default fg unless overridden ─────────────────
inline maya::Style dim()    { return maya::Style{}.with_dim(); }
inline maya::Style bold()   { return maya::Style{}.with_bold(); }
inline maya::Style italic() { return maya::Style{}.with_italic(); }

inline maya::Style fg_of(maya::Color c)         { return maya::Style{}.with_fg(c); }
inline maya::Style fg_bold(maya::Color c)       { return maya::Style{}.with_fg(c).with_bold(); }
inline maya::Style fg_dim(maya::Color c)        { return maya::Style{}.with_fg(c).with_dim(); }
inline maya::Style fg_italic(maya::Color c)     { return maya::Style{}.with_fg(c).with_italic(); }

} // namespace moha::ui
