// rag_settings_view.cpp — the RAG mode picker overlay.
//
// Three rows: On / First turn only / Off — how proactive (pre-turn) retrieval
// behaves. The current mode is marked; Enter/Space selects. A deliberately
// tiny pane: one decision, not a wall of toggles.

#include "agentty/runtime/view/pickers.hpp"

#include "agentty/runtime/view/form_view.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/rag_settings.hpp"
#include "pickers/pickers_common.hpp"

#include <maya/widget/panel.hpp>
#include <maya/widget/panel.hpp>
#include <maya/platform/io.hpp>

#include <algorithm>
#include <string>

namespace ov = agentty::ui::overlay;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace rs = agentty::rag_settings;

Element rag_settings_picker(const Model& m) {
    const auto* o = m.ui.overlay.get<ov::RagSettings>();
    if (!o) return nothing();

    // ONE pane, always the form. Mode and embedder are two rows of the same
    // question; every glyph belongs to maya::Panel and this host does nothing
    // but project state onto its Config.
    //
    // The registry makes this pane taller than a short terminal, so it
    // scrolls — same viewport arithmetic every other picker uses.
    return maya::Panel{form_config(o->embed.form, info,
                                  &m.ui.rag_settings_scroll,
                                  picker_detail::picker_viewport_h(),
                                  picker_detail::picker_terminal_cols())}.build();
}

} // namespace agentty::ui
