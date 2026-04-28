#include "moha/runtime/view/composer.hpp"

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

namespace {

// Map moha runtime state → widget State enum. Pure data translation;
// the widget owns all visual decisions (border color, prompt boldness,
// placeholder text, height pin).
maya::Composer::State composer_state(const Model& m) {
    if (m.s.is_awaiting_permission()) return maya::Composer::State::AwaitingPermission;
    if (m.s.is_executing_tool())      return maya::Composer::State::ExecutingTool;
    if (m.s.is_streaming())           return maya::Composer::State::Streaming;
    return maya::Composer::State::Idle;
}

} // namespace

maya::Composer::Config composer_config(const Model& m) {
    maya::Composer::Config cfg;
    cfg.text            = m.ui.composer.text;
    cfg.cursor          = m.ui.composer.cursor;
    cfg.state           = composer_state(m);
    cfg.active_color    = phase_color(m.s.phase);
    cfg.text_color      = fg;
    cfg.accent_color    = accent;
    cfg.warn_color      = warn;
    cfg.highlight_color = highlight;
    cfg.queued          = m.ui.composer.queued.size();
    cfg.profile         = {.label = std::string{profile_label(m.d.profile)},
                           .color = profile_color(m.d.profile)};
    cfg.expanded        = m.ui.composer.expanded;
    return cfg;
}

} // namespace moha::ui
