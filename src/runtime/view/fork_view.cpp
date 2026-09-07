// fork_view.cpp — the fork picker overlay.
//
// Three rows: how proactive RAG behaves in the fork. A fork always starts
// FRESH (near-zero context; the parent transcript is readable on demand),
// so the only choice is RAG behaviour. Enter forks with the highlighted
// row. Own TU, matching rag_settings_view.

#include "agentty/runtime/view/pickers.hpp"
#include "pickers/pickers_common.hpp"   // kPanel* widths, picker_viewport_h

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/fork_picker.hpp"

#include <maya/widget/panel.hpp>

#include <string>

namespace ov = agentty::ui::overlay;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace fp = agentty::fork_picker;

namespace {

struct RowSpec { const char* label; const char* help; };

// Labels keyed BY CHOICE, not by position. A positional table has to be
// kept in the same order as the enum by hand; a total switch cannot drift,
// and adding a Choice makes the compiler demand its label.
[[nodiscard]] constexpr RowSpec row_spec(fp::Choice c) noexcept {
    switch (c) {
        case fp::Choice::RagPerTurn:
            return {"RAG per turn",   "fresh thread · retrieve context before every turn"};
        case fp::Choice::FirstTurnRag:
            return {"First-turn RAG", "fresh thread · retrieve once, up front"};
        case fp::Choice::RagOff:
            return {"RAG off",        "fresh thread · no retrieval"};
    }
    return {"RAG off", "fresh thread · no retrieval"};
}

} // namespace

Element fork_picker_view(const Model& m) {
    const auto* o = m.ui.overlay.get<ov::Fork>();
    if (!o) return nothing();

    Panel::Config cfg;
    cfg.title      = " Fork thread ";
    cfg.accent     = info;
    cfg.min_width  = picker_detail::kPanelStandard;
    // Height is CONTENT-sized, not the shared viewport: this is a fixed,
    // four-choice menu with nothing to scroll, so clamping it to a scrollable
    // list's height would leave dead rows under the last choice.
    cfg.viewport_h = fp::kChoiceCount + 2;
    cfg.scroll     = nullptr;
    cfg.selected   = static_cast<int>(o->choice);

    // Walk the enumeration the cursor moves through — one list, no parallel
    // table to keep in order.
    for (const fp::Choice c : fp::kChoices) {
        const RowSpec spec = row_spec(c);
        Panel::Item row;
        row.leading       = spec.label;
        row.leading_style = fg_of(fg);
        row.trailing      = spec.help;
        row.trailing_style = fg_dim(muted);
        cfg.items.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(text(
        "  A fork starts FRESH (near-zero context) · the old transcript is "
        "readable on demand.",
        fg_dim(muted)));
    cfg.footer.push_back(h(
        text("\xe2\x86\x91\xe2\x86\x93 ", fg_of(fg)),  text("choose   ", fg_dim(muted)),
        text("Enter ", fg_of(fg)), text("fork   ", fg_dim(muted)),
        text("Esc ", fg_of(fg)),   text("close", fg_dim(muted))
    ).build());

    return Panel{std::move(cfg)}.build();
}

} // namespace agentty::ui
