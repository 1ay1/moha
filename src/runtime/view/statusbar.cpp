#include "moha/runtime/view/statusbar.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <maya/widget/model_badge.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

namespace {

// Walk the last assistant message and return the name of the tool call
// currently in `Running` state, if any. Lets the phase chip render
// "▌ ⠋ bash ▐" when ExecutingTool is active.
std::string_view running_tool_name(const Model& m) {
    if (m.d.current.messages.empty()) return {};
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return {};
    for (const auto& tc : last.tool_calls) {
        if (tc.is_running()) return tc.name.value;
    }
    return {};
}

// Pull the rate ring buffer out in chronological (oldest → newest) order.
std::vector<float> ordered_rate_history(const StreamState& s) {
    std::vector<float> out;
    out.reserve(StreamState::kRateSamples);
    if (s.rate_history_full) {
        for (std::size_t i = 0; i < StreamState::kRateSamples; ++i) {
            const auto idx = (s.rate_history_pos + i) % StreamState::kRateSamples;
            out.push_back(s.rate_history[idx]);
        }
    } else {
        for (std::size_t i = 0; i < s.rate_history_pos; ++i)
            out.push_back(s.rate_history[i]);
    }
    return out;
}

} // namespace

maya::StatusBar::Config status_bar_config(const Model& m) {
    const bool is_streaming = m.s.is_streaming() && m.s.active();
    const bool is_executing = m.s.is_executing_tool();
    const bool active       = is_streaming || is_executing;

    // ── Phase spec.
    maya::Color pcolor = phase_color(m.s.phase);
    std::string phase_icon = active
        ? std::string{m.s.spinner.current_frame()}
        : std::string{phase_glyph(m.s.phase)};
    std::string phase_label{phase_verb(m.s.phase)};
    if (is_executing) {
        if (auto tn = running_tool_name(m); !tn.empty())
            phase_label = std::string{tn};
    }
    float phase_elapsed = -1.0f;
    if (active && m.s.started.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m.s.started).count();
        phase_elapsed = static_cast<float>(ms) / 1000.0f;
    }

    // ── Token-stream spec — resolve rate from live deltas / ring buffer.
    auto hist = ordered_rate_history(m.s);
    auto now  = std::chrono::steady_clock::now();
    bool ever_streamed =
        m.s.first_delta_at.time_since_epoch().count() != 0;
    long long ts_ms = ever_streamed
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              now - m.s.first_delta_at).count()
        : 0;
    double sec        = std::max(0.001, static_cast<double>(ts_ms) / 1000.0);
    double approx_tok = static_cast<double>(m.s.live_delta_bytes) / 4.0;
    float disp_rate;
    if (is_streaming && ts_ms >= 250) {
        disp_rate = static_cast<float>(approx_tok / sec);
    } else if (!hist.empty()) {
        disp_rate = hist.back();
    } else {
        disp_rate = 0.0f;
    }

    // ── Model badge.
    maya::ModelBadge mb;
    mb.set_model(m.d.model_id.value);
    mb.set_compact(true);

    // ── Status banner — treat expired toasts as absent.
    bool has_status = !m.s.status.empty() && m.s.status != "ready"
                      && m.s.status_active(now);
    bool is_err = has_status && m.s.status.rfind("error:", 0) == 0;

    // ── Shortcut bindings — high priority survives narrow widths first.
    std::vector<maya::ShortcutRow::Binding> shortcuts = {
        {.key="^K",    .label="palette", .key_color=highlight, .priority=10},
        {.key="^J",    .label="threads", .key_color=highlight, .priority=10},
        {.key="^T",    .label="todo",    .key_color=highlight, .priority=10},
        {.key="S-Tab", .label="profile", .key_color=highlight, .priority=4},
        {.key="^/",    .label="models",  .key_color=highlight, .priority=4},
        {.key="^N",    .label="new",     .key_color=success,   .priority=10},
        {.key="^C",    .label="quit",    .key_color=danger,    .priority=10},
    };

    maya::StatusBar::Config cfg;
    cfg.phase = {
        .glyph        = std::move(phase_icon),
        .verb         = std::move(phase_label),
        .color        = pcolor,
        .breathing    = active,
        .frame        = m.s.spinner.frame_index(),
        .elapsed_secs = phase_elapsed,
    };
    cfg.breadcrumb_title = m.d.current.title;
    cfg.token_stream = {
        .show    = true,
        .rate    = disp_rate,
        .total   = static_cast<int>(approx_tok),
        .history = std::move(hist),
        .color   = highlight,
        .live    = is_streaming,
    };
    cfg.model_badge = mb.build();
    cfg.context = {
        .used = m.s.tokens_in,
        .max  = m.s.context_max,
    };
    cfg.status_text             = has_status ? m.s.status : std::string{};
    cfg.status_is_error         = is_err;
    cfg.shortcuts               = std::move(shortcuts);
    cfg.breadcrumb_min_width    = is_streaming ? 160 : 130;
    cfg.token_stream_min_width  = 110;
    cfg.ctx_bar_min_width       = 55;
    cfg.text_color              = fg;
    return cfg;
}

} // namespace moha::ui
