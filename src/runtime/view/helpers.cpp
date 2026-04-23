#include "moha/runtime/view/helpers.hpp"

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <variant>

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

maya::Color profile_color(Profile p) noexcept {
    switch (p) {
        case Profile::Write:   return accent;
        case Profile::Ask:     return info;
        case Profile::Minimal: return muted;
    }
    return fg;
}

std::string_view phase_glyph(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return "●";
        else if constexpr (std::same_as<T, phase::Streaming>)          return "◐";
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return "⚠";
        else                                                           return "▶";
    }, p);
}

std::string_view phase_verb(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return "Ready";
        else if constexpr (std::same_as<T, phase::Streaming>)          return "Streaming";
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return "Awaiting";
        else                                                           return "Running";
    }, p);
}

maya::Color phase_color(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> maya::Color {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return muted;
        else if constexpr (std::same_as<T, phase::Streaming>)          return highlight;
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return warn;
        else                                                           return success;
    }, p);
}

std::string small_caps(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        out.push_back(static_cast<char>(
            (c >= 'a' && c <= 'z') ? (c - 32) : c));
        if (i + 1 < s.size()) out.push_back(' ');
    }
    return out;
}

std::string tabular_int(int n, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%*d", width, n);
    return buf;
}

std::string format_elapsed_5(float secs) {
    // EVERY branch must produce EXACTLY 5 display columns — this label
    // lives in the status bar where any width change per frame reads as
    // jitter. Budgets by magnitude:
    //   <   10 s  → " 3.4s"   ( 1 + 3 + 1 = 5 )
    //   <  100 s  → "12.3s"   ( 4 + 1     = 5 )
    //   <  600 s  → " 234s"   ( 4 + 1     = 5 )
    //   < 600 m   → "59m9s" … needs "mm'ss" style — pick " 9m5s" /
    //                 "59m5s" (always 5 chars, seconds as single digit
    //                 rounded down, minutes 1–2 digits).
    //   else      → " >1hr"
    char buf[16];
    if (secs < 0.0f) secs = 0.0f;
    if      (secs <   10.0f)  std::snprintf(buf, sizeof(buf), " %.1fs", static_cast<double>(secs));
    else if (secs <  100.0f)  std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
    else if (secs <  600.0f)  std::snprintf(buf, sizeof(buf), "%4ds", static_cast<int>(secs));
    else if (secs < 3600.0f) {
        int m = static_cast<int>(secs) / 60;
        int s = (static_cast<int>(secs) / 10) % 6;   // tens of seconds, 0–5
        // "%2dm%ds" → e.g. " 9m3s" or "59m4s" — always 5 cols.
        std::snprintf(buf, sizeof(buf), "%2dm%ds", m, s);
    }
    else                      std::snprintf(buf, sizeof(buf), " >1hr");
    return buf;
}

std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string utf8_encode(char32_t cp) {
    std::string out;
    auto u = static_cast<uint32_t>(cp);
    if (u < 0x80) {
        out.push_back(static_cast<char>(u));
    } else if (u < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else if (u < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (u >> 12)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (u >> 18)));
        out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
    return out;
}

int context_max_for_model(std::string_view model_id) noexcept {
    // The `[1m]` tag is moha's internal marker for the 1 M-context window
    // beta (`context-1m-2025-08-07`). Sonnet 4.6 / opus-4-7 with the tag
    // get a 1 M window; without it they're standard 200 K. Haiku stays
    // 200 K. If new models with different windows ship, extend here.
    if (model_id.find("[1m]") != std::string_view::npos) return 1'000'000;
    return 200'000;
}

int utf8_prev(std::string_view s, int byte_pos) noexcept {
    if (byte_pos <= 0) return 0;
    int p = byte_pos - 1;
    while (p > 0 && (static_cast<uint8_t>(s[p]) & 0xC0) == 0x80) --p;
    return p;
}

int utf8_next(std::string_view s, int byte_pos) noexcept {
    int n = static_cast<int>(s.size());
    if (byte_pos >= n) return n;
    int p = byte_pos + 1;
    while (p < n && (static_cast<uint8_t>(s[p]) & 0xC0) == 0x80) ++p;
    return p;
}

} // namespace moha::ui
