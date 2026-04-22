#pragma once
// moha::Profile — the permission tier a user is running under.
// Read by both the tool domain (`needs_permission(Profile)`) and the
// provider domain (when surfacing permission-requiring tools).

#include <cstdint>
#include <string_view>

namespace moha {

enum class Profile : std::uint8_t { Write, Ask, Minimal };

[[nodiscard]] constexpr std::string_view to_string(Profile p) noexcept {
    switch (p) {
        case Profile::Write:   return "Write";
        case Profile::Ask:     return "Ask";
        case Profile::Minimal: return "Minimal";
    }
    return "?";
}

} // namespace moha
