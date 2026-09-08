#pragma once
#include <string_view>
#include <vector>

namespace agentty::ui {

// Shared host-owned rotating word pool for the ActivityIndicator.
// Returned by const-ref; lifetime is static. The in-Turn placeholder
// indicator (conversation.cpp) wires this into
// ActivityIndicator::Config::words so the widget stays content-agnostic.
// (A bottom-of-thread activity_indicator_config(Model) builder also
// lived here once — dead, removed; see the .cpp note.)
[[nodiscard]] const std::vector<std::string_view>& activity_indicator_words();

} // namespace agentty::ui
