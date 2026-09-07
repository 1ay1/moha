#pragma once
// agentty::ui — project a form::Form onto maya::Panel::Config.
//
// The whole rendering surface for every configuration pane is this ONE
// function. A pane's view becomes:
//
//   maya::Form{form_config(pane.form, info)}.build()
//
// and the pane itself contributes no chrome — appearance is entirely
// maya::Form's, so panes cannot drift from each other the way five
// hand-written pickers did.

#include <maya/style/color.hpp>
#include <maya/widget/panel.hpp>

#include "agentty/runtime/panel/form.hpp"

namespace agentty::ui {

// A settings pane grows with the registry, so it WILL outgrow a short
// terminal. `scroll` must outlive the returned Config's Element (it lives in
// Model::UI); passing null disables scrolling, which is only correct when the
// caller can guarantee the rows fit.
//
// `max_width` clamps the panel's min-width to what the terminal actually has.
// A fixed 60 columns in a narrower terminal makes the panel wider than the
// screen and the overlay centres it, so the left half — every label — slides
// off-screen. 0 means "no clamp". This lives on the HOST side because the
// host is what knows the terminal size; the widget would have to learn it
// from a layout callback, and those must declare a height they cannot know.
[[nodiscard]] maya::Panel::Config form_config(const agentty::form::Form& f,
                                             maya::Color accent,
                                             maya::ScrollState* scroll = nullptr,
                                             int viewport_h = 0,
                                             int max_width = 0);

} // namespace agentty::ui
