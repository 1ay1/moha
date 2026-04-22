#pragma once
// Robust reader for Claude tool-use args. Tolerates the common shapes a
// streaming model emits when its JSON drifts:
//   • missing fields       → default
//   • nulls                → default or nullopt
//   • numbers/bools in a string slot → coerce via dump()
//   • "42" in an int slot  → parse as 42
//   • arrays of strings in a string slot → newline-join (write-tool idiom)
// The goal is to avoid red error cards for recoverable model slips while
// still surfacing "genuinely required field missing" as an explicit error.

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace moha::tools::util {

class ArgReader {
public:
    explicit ArgReader(const nlohmann::json& args) noexcept : args_(args) {}

    [[nodiscard]] bool is_object() const noexcept { return args_.is_object(); }

    [[nodiscard]] bool has(std::string_view key) const {
        return args_.is_object() && args_.contains(std::string{key});
    }

    // Missing / null -> def. number/bool -> coerced. array -> newline-join
    // of elements (matching write-tool's idiom). `note` is set to a
    // non-empty human-readable string when a coercion happened, so callers
    // can echo it to the model without showing an error.
    [[nodiscard]] std::string str(std::string_view key,
                                  std::string def = {},
                                  std::string* note = nullptr) const;

    // Like str(), but returns nullopt if the key is missing, null, or
    // produces an empty string after coercion. For genuinely required
    // fields.
    [[nodiscard]] std::optional<std::string> require_str(std::string_view key) const;

    // Parses "42" strings, truncates doubles. Returns def on missing/null
    // or parse failure.
    [[nodiscard]] int integer(std::string_view key, int def) const;

    // Accepts "true"/"false" / "1"/"0" strings.
    [[nodiscard]] bool boolean(std::string_view key, bool def) const;

    [[nodiscard]] const nlohmann::json* raw(std::string_view key) const noexcept;

private:
    const nlohmann::json& args_;
};

} // namespace moha::tools::util
