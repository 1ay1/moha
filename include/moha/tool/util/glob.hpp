#pragma once
// fnmatch-style glob matcher. Supports `*` (any run, incl. empty),
// `?` (any single char), `[abc]` / `[a-z]` / `[!abc]` character classes.
// Matching is case-insensitive on Windows (cmd.exe semantics — `*.CPP`
// should find `.cpp`) and case-sensitive elsewhere. `**` collapses to `*`
// (no path-spanning); callers match against just the filename segment.

#include <string_view>

namespace moha::tools::util {

[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view name) noexcept;

} // namespace moha::tools::util
