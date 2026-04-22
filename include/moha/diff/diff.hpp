#pragma once
// moha diff domain — value types for a structured edit, plus the pure
// functions that compute and render them. No filesystem, no I/O.

#include <cstdint>
#include <string>
#include <vector>

namespace moha {

struct Hunk {
    enum class Status : std::uint8_t { Pending, Accepted, Rejected };
    int old_start = 0, old_len = 0, new_start = 0, new_len = 0;
    std::string patch;
    Status status = Status::Pending;
};

struct FileChange {
    std::string path;
    int added   = 0;
    int removed = 0;
    std::vector<Hunk> hunks;
    std::string original_contents;
    std::string new_contents;
};

namespace diff {

// Compute unified diff and structured hunks from before/after text.
[[nodiscard]] FileChange compute(const std::string& path,
                                 const std::string& before,
                                 const std::string& after);

// Render a unified diff string (for display / for passing to DiffView widget).
[[nodiscard]] std::string render_unified(const FileChange& c);

// Apply accepted hunks on top of `original_contents`, skipping rejected ones.
[[nodiscard]] std::string apply_accepted(const FileChange& c);

} // namespace diff
} // namespace moha
