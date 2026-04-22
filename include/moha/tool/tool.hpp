#pragma once
// moha::tool — Tool concept + DynamicDispatch adapter.

#include <concepts>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "moha/model.hpp"
#include "moha/tool/registry.hpp"

namespace moha::tool {

using tools::ToolOutput;
using tools::ToolError;
using tools::ExecResult;

template <class T>
concept Tool = requires(T& t, const nlohmann::json& args, Profile profile) {
    { T::name() }              -> std::convertible_to<std::string_view>;
    { T::description() }       -> std::convertible_to<std::string_view>;
    { T::input_schema() }      -> std::convertible_to<nlohmann::json>;
    { t.needs_permission(profile) } -> std::convertible_to<bool>;
    { t.execute(args) }        -> std::convertible_to<ExecResult>;
};

struct DynamicDispatch {
    [[nodiscard]] static const tools::ToolDef* find(std::string_view name) noexcept {
        return tools::find(name);
    }

    [[nodiscard]] static ExecResult execute(std::string_view name,
                                            const nlohmann::json& args) noexcept {
        const auto* td = tools::find(name);
        if (!td) return std::unexpected(ToolError::not_found("unknown tool: " + std::string{name}));
        auto safe_args = args.is_object() ? args : nlohmann::json::object();
        try {
            return td->execute(safe_args);
        } catch (const std::exception& e) {
            return std::unexpected(ToolError::unknown(std::string{"tool crashed: "} + e.what()));
        }
    }

    [[nodiscard]] static bool needs_permission(std::string_view name,
                                               Profile profile) noexcept {
        const auto* td = tools::find(name);
        return td ? td->needs_permission(profile) : true;
    }
};

} // namespace moha::tool
