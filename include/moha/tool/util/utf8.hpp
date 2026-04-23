#pragma once
// UTF-8 validation + repair. Subprocess output on Windows is whatever code
// page cmd.exe/PowerShell picked (usually OEM/CP1252), but nlohmann::json
// throws type_error.316 on any non-UTF-8 byte. Every string we hand the API
// must pass through to_valid_utf8 at the capture boundary — and ideally
// once more at serialization (belt-and-suspenders for paths that bypass us).

#include <string>
#include <string_view>

namespace moha::tools::util {

// RFC 3629 scan. True iff every byte sequence in `s` decodes to a valid
// code point (no overlong, no surrogates, no > U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view s) noexcept;

// Replace every invalid byte sequence in `s` with U+FFFD (0xEF 0xBF 0xBD).
// Valid bytes pass through unchanged.
[[nodiscard]] std::string sanitize_utf8(std::string_view s);

// Return a copy of `s` guaranteed to be valid UTF-8. Try in order: already
// valid → transcode from GetConsoleOutputCP() → transcode from CP_ACP →
// byte-level scrub. On non-Windows only the first and last steps apply.
[[nodiscard]] std::string to_valid_utf8(std::string s);

// Return the largest position `cut` in [0, max_bytes] such that `s[0, cut)`
// does NOT split a multi-byte UTF-8 sequence. When `s.size() <= max_bytes`
// returns `s.size()` (no truncation). Never throws, never allocates. Used
// at every byte-granularity truncation site (size caps on file reads,
// HTTP bodies, captured stdout, thread titles, tool output limits) so
// the resulting prefix is still safe to hand to nlohmann::json::dump()
// without triggering its UTF-8 type_error.316.
[[nodiscard]] std::size_t safe_utf8_cut(std::string_view s, std::size_t max_bytes) noexcept;

} // namespace moha::tools::util
