// rag_settings_view.cpp — the RAG mode picker overlay.
//
// Three rows: On / First turn only / Off — how proactive (pre-turn) retrieval
// behaves. The current mode is marked; Enter/Space selects. A deliberately
// tiny pane: one decision, not a wall of toggles.

#include "agentty/runtime/view/panels.hpp"

#include "agentty/runtime/view/form_panel.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "panels/panels_common.hpp"

#include <maya/widget/panel.hpp>
#include <maya/widget/panel.hpp>
#include <maya/platform/io.hpp>

#include <algorithm>
#include <string>

namespace pn = agentty::ui::panel;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace rs = agentty::rag_settings;

Element rag_settings_picker(const Model& m) {
    const auto* o = m.ui.panel.get<pn::Rag>();
    if (!o) return nothing();

    // ONE pane, always the form. Mode and embedder are two rows of the same
    // question; every glyph belongs to maya::Panel and this host does nothing
    // but project state onto its Config.
    //
    // The registry makes this pane taller than a short terminal, so it
    // scrolls — same viewport arithmetic every other picker uses.
    return maya::Panel{form_config(o->embed.form, info,
                                  &m.ui.rag_settings_scroll,
                                  panel_detail::panel_viewport_h(),
                                  panel_detail::panel_terminal_cols())}.build();
}

} // namespace agentty::ui
