#pragma once
#include <maya/maya.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element diff_review(const Model& m);

} // namespace moha::ui
