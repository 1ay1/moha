#pragma once
// moha::ui — small pure helpers shared by view modules.

#include <chrono>
#include <string>
#include <string_view>

#include <maya/style/color.hpp>

#include "moha/runtime/model.hpp"

namespace moha::ui {

// Enum reflection — delegates to moha::to_string().
[[nodiscard]] inline std::string_view profile_label(Profile p) noexcept { return to_string(p); }
[[nodiscard]] maya::Color profile_color(Profile p) noexcept;
[[nodiscard]] inline std::string_view phase_label(const Phase& p) noexcept { return to_string(p); }

// Status-bar styling — glyph + verb form for the current phase, and a
// terminal color picked to communicate urgency at a glance.
[[nodiscard]] std::string_view phase_glyph(const Phase& p) noexcept;
[[nodiscard]] std::string_view phase_verb(const Phase& p) noexcept;
[[nodiscard]] maya::Color      phase_color(const Phase& p) noexcept;

[[nodiscard]] std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp);

// Context window size for a given model id. Defaults to 200 K but bumps
// to 1 M when the model id carries the moha-internal `[1m]` tag (which
// triggers the `context-1m-2025-08-07` beta on the wire). Used by the
// status-bar ctx % calculation so the bar doesn't read "180 %" after
// switching to a 1 M-window model with the old 200 K cap baked in.
[[nodiscard]] int context_max_for_model(std::string_view model_id) noexcept;

// UTF-8 helpers.
[[nodiscard]] std::string utf8_encode(char32_t cp);
[[nodiscard]] int utf8_prev(std::string_view s, int byte_pos) noexcept;
[[nodiscard]] int utf8_next(std::string_view s, int byte_pos) noexcept;

} // namespace moha::ui
