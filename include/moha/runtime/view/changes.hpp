#pragma once
#include <maya/widget/changes_strip.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::ChangesStrip::Config changes_strip_config(const Model& m);

} // namespace moha::ui
