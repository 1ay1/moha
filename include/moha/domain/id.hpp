#pragma once
// moha::Id<Tag> — zero-overhead strong newtype for string-shaped IDs.
//
// Rust's rule-of-thumb: never pass a raw `String` across an API boundary
// when the caller could plausibly confuse it with another `String`. Same
// rule here — a ThreadId and a ToolCallId are both "some hex" at runtime,
// but the compiler keeps them apart.

#include <compare>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace moha {

template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}

    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

    [[nodiscard]] bool operator==(std::string_view sv) const noexcept { return value == sv; }

    friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
    friend void from_json(const nlohmann::json& j, Id& id) { j.get_to(id.value); }
};

struct ThreadIdTag     {};
struct ToolCallIdTag   {};
struct ModelIdTag      {};
struct CheckpointIdTag {};
struct ToolNameTag     {};

using ThreadId     = Id<ThreadIdTag>;
using ToolCallId   = Id<ToolCallIdTag>;
using ModelId      = Id<ModelIdTag>;
using CheckpointId = Id<CheckpointIdTag>;
using ToolName     = Id<ToolNameTag>;

} // namespace moha
