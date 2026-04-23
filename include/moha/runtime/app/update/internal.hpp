#pragma once
// Shared internals for the update/* translation units. Not part of the public
// moha::app interface — external callers go through moha::app::update() in
// update.hpp. Lives under include/ rather than a private src/ header so the
// three update/*.cpp files and update.cpp can all see the same declarations.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <maya/maya.hpp>
#include <nlohmann/json.hpp>

#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::app {

using Step = std::pair<Model, maya::Cmd<Msg>>;
inline Step done(Model m) { return {std::move(m), maya::Cmd<Msg>::none()}; }

namespace detail {

// Hard cap on per-message live buffers. A misbehaving server (or adversarial
// proxy) emitting unbounded `text_delta`/`input_json_delta` would otherwise
// grow `streaming_text` / `args_streaming` until the process OOMs. 8 MiB is
// far above any realistic single-message body — hitting this cap means
// something genuinely broken upstream, not a real workload.
inline constexpr std::size_t kMaxStreamingBytes = 8 * 1024 * 1024;

// View virtualization thresholds — when the transcript exceeds kViewWindow
// messages, slice kSliceChunk of the oldest into terminal scrollback so Yoga
// paint stays bounded.
inline constexpr int kViewWindow = 60;
inline constexpr int kSliceChunk = 20;

// ── update_stream.cpp ────────────────────────────────────────────────────
void update_stream_preview(ToolUse& tc);
bool guard_truncated_tool_args(ToolUse& tc);
nlohmann::json salvage_args(const ToolUse& tc);
maya::Cmd<Msg> finalize_turn(Model& m, std::string_view stop_reason = {});

// ── update_modal.cpp ─────────────────────────────────────────────────────
Step           submit_message(Model m);
maya::Cmd<Msg> maybe_virtualize(Model& m);
void           persist_settings(const Model& m);

// ── update_tool.cpp ──────────────────────────────────────────────────────
void apply_tool_output(Model& m, const ToolCallId& id,
                       std::string&& output, bool error);
void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason);

} // namespace detail
} // namespace moha::app
