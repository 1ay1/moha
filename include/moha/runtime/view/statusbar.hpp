#pragma once
#include <maya/widget/status_bar.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::StatusBar::Config status_bar_config(const Model& m);

} // namespace moha::ui
