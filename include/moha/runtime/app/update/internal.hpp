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
// paint stays bounded. The old 60/20 pair let tool-heavy turns (e.g. a
// coding session with 40+ bash/read/edit calls inside a single assistant
// message) push the live canvas past ~3000 rows, at which point render
// latency spiked enough to visibly stall the status bar & composer even
// though the worker thread was still pumping deltas. 40/15 keeps the hot
// set small enough that one render pass fits comfortably inside a Tick
// interval on modest hardware.
inline constexpr int kViewWindow = 40;
inline constexpr int kSliceChunk = 15;

// ── update_stream.cpp ────────────────────────────────────────────────────
void update_stream_preview(ToolUse& tc);
bool guard_truncated_tool_args(ToolUse& tc);
nlohmann::json salvage_args(const ToolUse& tc);
maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason = StopReason::Unspecified);

// ── update_modal.cpp ─────────────────────────────────────────────────────
Step           submit_message(Model m);
maya::Cmd<Msg> maybe_virtualize(Model& m);
void           persist_settings(const Model& m);

// ── update_tool.cpp ──────────────────────────────────────────────────────
void apply_tool_output(Model& m, const ToolCallId& id,
                       std::expected<std::string, tools::ToolError>&& result);
void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason);

} // namespace detail
} // namespace moha::app
