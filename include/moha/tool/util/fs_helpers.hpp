#pragma once
// Shared filesystem helpers. Tool implementations need normalized paths,
// binary detection, and a predictable "which directories to skip during
// traversal" list — centralised here so the rules are consistent across
// grep / glob / list_dir / find_definition.

#include <filesystem>
#include <string>
#include <string_view>

namespace moha::tools::util {

namespace fs = std::filesystem;

// Read an entire file as a binary blob. Returns "" on open failure (callers
// that need to distinguish missing vs empty should stat first).
[[nodiscard]] std::string read_file(const fs::path& p);

// Write content atomically-ish (truncate + write + flush). Returns the
// empty string on success, or a human-readable error otherwise. Keeps tool
// lambdas terse while still forcing callers to surface failures.
[[nodiscard]] std::string write_file(const fs::path& p, const std::string& content);

// Normalise a user-supplied path. Accepts forward slashes on Windows
// (the model frequently produces them), strips surrounding whitespace and
// quotes, and returns an absolute path relative to cwd when not already
// absolute — so error messages name an unambiguous location.
[[nodiscard]] fs::path normalize_path(std::string s);

// True for directory names we want recursive traversals (grep / glob /
// list_dir) to skip by default. Keeps the skip list in one place so tools
// stay in sync (e.g. adding `_deps` to every tool at once).
[[nodiscard]] bool should_skip_dir(std::string_view name) noexcept;

// Heuristic: scan the first 512 bytes for a NUL. Good enough to avoid
// grep'ing PNGs / executables / model weights into the prompt.
[[nodiscard]] bool is_binary_file(const fs::path& p);

} // namespace moha::tools::util
