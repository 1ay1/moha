#pragma once
// Zed-style bash guards. The model occasionally tries an interactive command
// (vim, psql, bare `python`) and the TUI stalls on the spinner until timeout.
// Reject these up front with an explicit hint so the model retries with a
// non-interactive form, and reject a handful of flagrantly destructive
// patterns (rm -rf /, fork bombs, mkfs, `curl | sh`).
//
// Returns an empty string if the command is acceptable, otherwise a
// human-readable rejection reason suitable for surfacing as a ToolError.

#include <string>
#include <string_view>

namespace moha::tools::util {

[[nodiscard]] std::string validate_bash_command(std::string_view cmd);

} // namespace moha::tools::util
