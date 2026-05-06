#include "moha/runtime/view/thread/turn/turn.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/markdown.hpp>

#include "moha/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"
#include "moha/runtime/view/cache.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/thread/turn/permission.hpp"

namespace moha::ui {

namespace {

// ── Cached markdown render. The ONE Element-returning helper kept in
//    moha — strictly because cross-frame cache state lives in the
//    StreamingMarkdown widget instance, which we keep alive across
//    frames so its block cache survives.
maya::Element cached_markdown_for(const Message& msg, const ThreadId& tid,
                                  std::size_t msg_idx) {
    auto& cache = message_md_cache(tid, msg_idx);

    // Fully settled — no in-flight tail.  Use the finalized full-markdown
    // parse and drop any streaming widget we might have been holding.
    if (msg.streaming_text.empty()) {
        if (!cache.finalized) {
            cache.finalized = std::make_shared<maya::Element>(
                maya::markdown(msg.text));
            cache.streaming.reset();
        }
        return *cache.finalized;
    }

    // Streaming, possibly with prior settled text from earlier rounds of
    // the same agent turn.  Feed the combined buffer through
    // StreamingMarkdown — its block cache survives across rounds, and
    // set_content with a strict prefix-extension is internally just a
    // feed() of the new tail (no whole-source re-parse).  The "\n\n"
    // separator matches what message_stop will commit, so the view
    // shows the same paragraph break the settled message will have.
    if (!cache.streaming) {
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
    }
    cache.finalized.reset();

    std::string combined;
    combined.reserve(msg.text.size() + 2 + msg.streaming_text.size());
    combined.append(msg.text);
    if (!msg.text.empty()) combined.append("\n\n");
    combined.append(msg.streaming_text);
    cache.streaming->set_content(combined);
    return cache.streaming->build();
}

// ── Per-speaker visual identity: rail color + glyph + display name.
//    Centralized so the rail color, the header glyph, and the bottom
//    streaming indicator stay in lockstep.
struct SpeakerStyle {
    maya::Color color;
    std::string glyph;
    std::string label;
};

SpeakerStyle speaker_style_for(Role role, const Model& m) {
    if (role == Role::User) {
        return {highlight, "\xe2\x9d\xaf", "You"};                   // ❯
    }
    const auto& id = m.d.model_id.value;
    maya::Color c;
    std::string label;
    if      (id.find("opus")   != std::string::npos) { c = accent;    label = "Opus";   }
    else if (id.find("sonnet") != std::string::npos) { c = info;      label = "Sonnet"; }
    else if (id.find("haiku")  != std::string::npos) { c = success;   label = "Haiku";  }
    else                                              { c = highlight; label = id;       }
    for (std::size_t i = 0; i + 2 < id.size(); ++i) {
        char ch = id[i];
        if (ch >= '0' && ch <= '9') {
            char delim = id[i + 1];
            if ((delim == '-' || delim == '.') && id[i + 2] >= '0' && id[i + 2] <= '9') {
                std::size_t end = i + 3;
                while (end < id.size() && id[end] >= '0' && id[end] <= '9') ++end;
                auto ver = id.substr(i, end - i);
                for (auto& v : ver) if (v == '-') v = '.';
                label += " " + ver;
                break;
            }
        }
    }
    return {c, "\xe2\x9c\xa6", std::move(label)};                    // ✦
}

// ── Trailing meta strip for the turn header — `12:34 · 4.2s · turn N`.
std::string format_turn_meta(const Message& msg, int turn_num,
                             std::optional<float> elapsed_secs) {
    std::string meta = timestamp_hh_mm(msg.timestamp);
    if (elapsed_secs && *elapsed_secs > 0.0f)
        meta += "  \xc2\xb7  " + format_duration_compact(*elapsed_secs);
    if (turn_num > 0)
        meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    return meta;
}

// ── Compute the assistant turn's wall-clock elapsed: from previous
//    user message timestamp to this one.
std::optional<float> assistant_elapsed(const Message& msg, const Model& m) {
    if (msg.role != Role::Assistant) return std::nullopt;
    for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
        if (&m.d.current.messages[i] == &msg) continue;
        if (m.d.current.messages[i].role == Role::User) {
            auto dt = std::chrono::duration<float>(
                msg.timestamp - m.d.current.messages[i].timestamp).count();
            if (dt > 0.0f && dt < 3600.0f) return dt;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

maya::Turn::Config turn_config(const Message& msg, std::size_t msg_idx,
                               int turn_num, const Model& m) {
    // Settled-turn cache.  A message that has a successor in the messages
    // vector is by construction fully resolved — moha only appends a new
    // message once the current turn's text is final, all tools terminal,
    // and any permission prompt resolved.  Reusing the prior frame's
    // built Config skips per-frame rebuilding of the turn header, the
    // entire agent_timeline (every tool card), and the permission /
    // markdown wiring — which is the dominant cost as a session grows.
    const bool can_cache = (msg_idx + 1 < m.d.current.messages.size());
    if (can_cache) {
        auto& slot = turn_config_cache(m.d.current.id, msg_idx);
        if (slot.cfg) return *slot.cfg;
    }

    auto style = speaker_style_for(msg.role, m);

    maya::Turn::Config cfg;
    cfg.glyph      = style.glyph;
    cfg.label      = style.label;
    cfg.rail_color = style.color;
    cfg.meta       = format_turn_meta(msg, turn_num,
                         msg.role == Role::Assistant
                             ? assistant_elapsed(msg, m)
                             : std::nullopt);
    cfg.checkpoint_above = (msg.role == Role::User && msg.checkpoint_id.has_value());
    cfg.checkpoint_color = warn;

    if (msg.role == Role::User) {
        cfg.body.emplace_back(maya::Turn::PlainText{.content = msg.text, .color = fg});
    } else if (msg.role == Role::Assistant) {
        const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            // Cross-frame StreamingMarkdown cache requires holding the
            // widget instance; feed its built Element via the typed
            // Element variant of BodySlot.
            cfg.body.emplace_back(cached_markdown_for(msg, m.d.current.id, msg_idx));
        }
        if (!msg.tool_calls.empty()) {
            cfg.body.emplace_back(
                agent_timeline_config(msg, m.s.spinner.frame_index(), style.color));
            // In-flight permission card under the timeline.
            for (const auto& tc : msg.tool_calls) {
                if (m.d.pending_permission && m.d.pending_permission->id == tc.id) {
                    cfg.body.emplace_back(inline_permission_config(
                        *m.d.pending_permission, tc));
                }
            }
        }
        if (msg.error) cfg.error = *msg.error;
    }

    if (can_cache) {
        auto& slot = turn_config_cache(m.d.current.id, msg_idx);
        slot.cfg = std::make_shared<maya::Turn::Config>(cfg);
    }
    return cfg;
}

maya::Turn::Config turn_config_assistant_group(std::size_t start_idx,
                                               std::size_t end_idx,
                                               int turn_num,
                                               const Model& m)
{
    // Caller invariants: all messages in [start, end) are Assistant and
    // contiguous.  end > start.
    const auto& msgs = m.d.current.messages;
    const Message& first = msgs[start_idx];
    const Message& last  = msgs[end_idx - 1];

    // The group is fully settled iff there's a non-assistant successor
    // after end_idx — i.e., the user spoke again, ending the run.  When
    // settled, cache under the LAST message's idx (its presence pins
    // the group's identity and its successor's existence pins
    // settledness).
    const bool can_cache = (end_idx < msgs.size());
    if (can_cache) {
        auto& slot = turn_config_cache(m.d.current.id, end_idx - 1);
        if (slot.cfg) return *slot.cfg;
    }

    auto style = speaker_style_for(Role::Assistant, m);

    maya::Turn::Config cfg;
    cfg.glyph      = style.glyph;
    cfg.label      = style.label;
    cfg.rail_color = style.color;
    // Meta: timestamp of the FIRST message (when the group started),
    // elapsed = first user→last assistant span (already what
    // assistant_elapsed computes for `last`).
    cfg.meta = format_turn_meta(first, turn_num, assistant_elapsed(last, m));
    cfg.checkpoint_above = false;   // assistant turns never carry checkpoints
    cfg.checkpoint_color = warn;

    // Body composition:
    //   - For each message in the group: emit its text body slot (if any).
    //   - Emit ONE merged agent_timeline holding every tool_call.
    //   - Emit any in-flight permission cards.
    //
    // This is the visual fusion: three API responses with one tool
    // each render as three text blocks above ONE actions panel with
    // three rows.  The user sees "the agent did three things" instead
    // of three indistinguishable headers.

    for (std::size_t i = start_idx; i < end_idx; ++i) {
        const auto& msg = msgs[i];
        const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            cfg.body.emplace_back(cached_markdown_for(msg, m.d.current.id, i));
        }
    }

    // Merge every tool_call from every message in the group into ONE
    // synthetic Message and feed it through agent_timeline_config.
    // We hand-build the timeline to avoid allocating a Message copy:
    // collect tool_calls, sum elapsed, count categories, etc.
    std::size_t total_tools = 0;
    for (std::size_t i = start_idx; i < end_idx; ++i)
        total_tools += msgs[i].tool_calls.size();

    if (total_tools > 0) {
        // Build a synthetic Message holding the concatenated tool_calls.
        // Cheap: ToolUse is movable, but we only need a const view —
        // use an indirect helper rather than copying.  Here we just
        // build a thin Message instance with copies; the cost is
        // bounded by total_tools (typically < 50 per group).
        Message merged;
        merged.role = Role::Assistant;
        merged.timestamp = last.timestamp;
        merged.tool_calls.reserve(total_tools);
        for (std::size_t i = start_idx; i < end_idx; ++i) {
            for (const auto& tc : msgs[i].tool_calls) {
                merged.tool_calls.push_back(tc);
            }
        }
        cfg.body.emplace_back(
            agent_timeline_config(merged, m.s.spinner.frame_index(),
                                  style.color));

        // In-flight permission cards: if any tool_call across the
        // group is awaiting permission, surface it under the timeline.
        if (m.d.pending_permission) {
            for (std::size_t i = start_idx; i < end_idx; ++i) {
                for (const auto& tc : msgs[i].tool_calls) {
                    if (m.d.pending_permission->id == tc.id) {
                        cfg.body.emplace_back(inline_permission_config(
                            *m.d.pending_permission, tc));
                    }
                }
            }
        }
    }

    // Surface the first error in the group (later messages are usually
    // refinements; the first error is the most informative root).
    for (std::size_t i = start_idx; i < end_idx; ++i) {
        if (msgs[i].error) { cfg.error = *msgs[i].error; break; }
    }

    if (can_cache) {
        auto& slot = turn_config_cache(m.d.current.id, end_idx - 1);
        slot.cfg = std::make_shared<maya::Turn::Config>(cfg);
    }
    return cfg;
}

} // namespace moha::ui
