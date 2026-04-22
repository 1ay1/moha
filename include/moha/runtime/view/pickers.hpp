#pragma once
#include <maya/maya.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element model_picker(const Model& m);
[[nodiscard]] maya::Element thread_list(const Model& m);
[[nodiscard]] maya::Element command_palette(const Model& m);
[[nodiscard]] maya::Element todo_modal(const Model& m);

} // namespace moha::ui
