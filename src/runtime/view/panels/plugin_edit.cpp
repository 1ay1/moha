// plugin_edit.cpp — the plugin detail/add form panel (view).
//
// Pure adapter, same shape as smart_mode.cpp: project the pane's form onto
// maya::Panel via the shared form_config. Every chrome decision belongs to
// the widget; this file only says which scroll slot and accent it uses.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/form_panel.hpp"

namespace agentty::ui {

Element plugin_edit_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::PluginEdit>();
    if (!o) return nothing();
    return maya::Panel{form_config(o->form, info,
                                  &m.ui.plugin_edit_scroll,
                                  panel_viewport_h(),
                                  panel_terminal_cols())}.build();
}

} // namespace agentty::ui
