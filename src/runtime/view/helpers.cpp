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
