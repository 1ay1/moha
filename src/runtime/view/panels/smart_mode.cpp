// smart_mode.cpp — the Smart Mode config panel (^S), via the shared form.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/form_panel.hpp"

namespace agentty::ui {

// Smart Mode config overlay: a master Enabled toggle + the three role slots,
// each showing its RESOLVED model (pinned, or the auto-fill). See
// docs/design/smart-mode.md.
Element smart_mode_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::SmartMode>();
    if (!o) return nothing();
    // Every glyph belongs to maya::Panel; this host only projects state onto
    // its Config, exactly as the Retrieval pane does.
    return maya::Panel{form_config(o->form, success,
                                  &m.ui.smart_mode_scroll,
                                  panel_viewport_h(),
                                  panel_terminal_cols())}.build();
}


} // namespace agentty::ui
