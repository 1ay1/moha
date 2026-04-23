#pragma once
// Small, pure helpers shared between the thread renderer and the tool-card
// renderer. Kept separate from helpers.hpp because these all operate on
// ToolUse/args shape — they don't belong with generic color/UTF-8 stuff.

#include <initializer_list>
#include <string>

#include <nlohmann/json.hpp>

#include "moha/domain/conversation.hpp"

namespace moha::ui {

// Safe string read — empty when the key is missing or non-string.
[[nodiscard]] std::string safe_arg(const nlohmann::json& args, const char* key);

// Pick the first non-empty string under any of the listed keys. Mirrors
// the alias-tolerant parsing the tool implementations do (write/edit accept
// `path | file_path | filepath | filename`, `display_description | description`
// etc.). Without this the view reads one canonical key and a model that
// picks an alias renders as a blank card even though the tool ran fine.
[[nodiscard]] std::string pick_arg(const nlohmann::json& args,
                                   std::initializer_list<const char*> keys);

// Int read with default when missing / wrong type.
[[nodiscard]] int safe_int_arg(const nlohmann::json& args, const char* key, int def);

// Newline count + 1 for trailing non-newline line. Zero for empty input.
[[nodiscard]] int count_lines(const std::string& s);

// Seconds spent on this tool call so far. Running → now - started;
// terminal → finished - started; returns 0 when started_at is unset.
// Called every Tick while a tool runs so the card updates live.
[[nodiscard]] float tool_elapsed(const ToolUse& tc);

// Strip the ```…``` fence and trailing metadata bash wraps its captured
// stdout/stderr in. The fence lets the model see an unambiguous "literal
// output" boundary but is visual noise inside the widget's own frame.
[[nodiscard]] std::string strip_bash_output_fence(const std::string& s);

// Pull the exit code out of bash/diagnostics tool output — recognizes
// both "failed with exit code N" (Zed-style) and "[exit code N]" (legacy).
// Returns 124 for a timeout marker, 0 when none of those patterns match.
[[nodiscard]] int parse_exit_code(const std::string& output);

} // namespace moha::ui
