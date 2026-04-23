#pragma once
// Partial-JSON helpers for the Anthropic `input_json_delta` hot path.
//
// The SSE stream delivers tool args as fragments that form incomplete JSON
// until the tool_use block closes. Two utilities cover the common needs:
//
//   close_partial_json(raw)
//     Walks the buffer and emits a string nlohmann::json can parse. Closes
//     open strings, fills `"key":` with `null`, strips trailing `,`, and
//     closes unbalanced `{` / `[`. C++ equivalent of Zed's
//     `partial-json-fixer` crate.
//
//   sniff_string(raw, key)
//     Hand-walks the buffer for `"key": "<value>"`. Returns the decoded
//     value only once the closing quote has been seen; std::nullopt until
//     then. Useful when the full close+parse is too heavy to run per tick
//     and you only need one scalar.
//
//   sniff_string_progressive(raw, key)
//     Same as sniff_string but returns whatever bytes have accumulated so
//     far, stopping at a half-escape on the buffer edge. Needed for fields
//     whose value dwarfs every other arg (write's `content`, edit's
//     `old_string` / `new_string`) so the UI doesn't wait for the closing
//     quote on an 800-line file to show anything.
//
// All three are safe on empty / malformed input.

#include <optional>
#include <string>
#include <string_view>

namespace moha::tools::util {

std::string close_partial_json(std::string_view raw);

[[nodiscard]] std::optional<std::string>
sniff_string(std::string_view raw, std::string_view key);

[[nodiscard]] std::optional<std::string>
sniff_string_progressive(std::string_view raw, std::string_view key);

} // namespace moha::tools::util
